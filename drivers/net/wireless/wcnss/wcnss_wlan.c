/* Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/wcnss_wlan.h>
#include <linux/platform_data/qcom_wcnss_device.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/gpio.h>
#include <mach/msm_iomap.h>
#include <linux/wakelock.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/clk.h>
#include <linux/ratelimit.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/uaccess.h>

#include <mach/peripheral-loader.h>
#include <mach/msm_smd.h>
#include <mach/msm_iomap.h>
#ifdef CONFIG_WCNSS_MEM_PRE_ALLOC
#include "wcnss_prealloc.h"
#endif
#if defined(CONFIG_MACH_MELIUS_SKT) || defined(CONFIG_MACH_MELIUS_KTT) || \
	defined(CONFIG_MACH_MELIUS_LGT)
#if !defined(CONFIG_RADIO_IRIS) && defined(CONFIG_IR_REMOCON_FPGA)
/* workaround about conflict with qualcomm FM radio gpio */
#include <mach/msm8930-gpio.h>
#endif
#endif

#define DEVICE "wcnss_wlan"
#define VERSION "1.01"
#define WCNSS_PIL_DEVICE "wcnss"

/* module params */
#define WCNSS_CONFIG_UNSPECIFIED (-1)

static int has_48mhz_xo = WCNSS_CONFIG_UNSPECIFIED;
module_param(has_48mhz_xo, int, S_IWUSR | S_IRUGO);
MODULE_PARM_DESC(has_48mhz_xo, "Is an external 48 MHz XO present");

static int has_calibrated_data = WCNSS_CONFIG_UNSPECIFIED;
module_param(has_calibrated_data, int, S_IWUSR | S_IRUGO);
MODULE_PARM_DESC(has_calibrated_data, "whether calibrated data file available");

static DEFINE_SPINLOCK(reg_spinlock);

#define MSM_RIVA_PHYS			0x03204000

#define RIVA_SPARE_OFFSET		0x0b4
#define RIVA_SSR_BIT			BIT(23)
#define RIVA_SUSPEND_BIT		BIT(24)

#define MSM_RIVA_CCU_BASE			0x03200800

#define CCU_INVALID_ADDR_OFFSET		0x100
#define CCU_LAST_ADDR0_OFFSET		0x104
#define CCU_LAST_ADDR1_OFFSET		0x108
#define CCU_LAST_ADDR2_OFFSET		0x10c

#define WCNSS_CTRL_CHANNEL			"WCNSS_CTRL"
#define WCNSS_MAX_FRAME_SIZE		(4*1024)
#define WCNSS_VERSION_LEN			30

/* message types */
#define WCNSS_CTRL_MSG_START	0x01000000
#define	WCNSS_VERSION_REQ             (WCNSS_CTRL_MSG_START + 0)
#define	WCNSS_VERSION_RSP             (WCNSS_CTRL_MSG_START + 1)
#define	WCNSS_NVBIN_DNLD_REQ          (WCNSS_CTRL_MSG_START + 2)
#define	WCNSS_NVBIN_DNLD_RSP          (WCNSS_CTRL_MSG_START + 3)
#define	WCNSS_CALDATA_UPLD_REQ        (WCNSS_CTRL_MSG_START + 4)
#define	WCNSS_CALDATA_UPLD_RSP        (WCNSS_CTRL_MSG_START + 5)
#define	WCNSS_CALDATA_DNLD_REQ        (WCNSS_CTRL_MSG_START + 6)
#define	WCNSS_CALDATA_DNLD_RSP        (WCNSS_CTRL_MSG_START + 7)


#define VALID_VERSION(version) \
	((strncmp(version, "INVALID", WCNSS_VERSION_LEN)) ? 1 : 0)

#define FW_CALDATA_CAPABLE() \
	((penv->fw_major >= 1) && (penv->fw_minor >= 5) ? 1 : 0)

struct smd_msg_hdr {
	unsigned int msg_type;
	unsigned int msg_len;
};

struct wcnss_version {
	struct smd_msg_hdr hdr;
	unsigned char  major;
	unsigned char  minor;
	unsigned char  version;
	unsigned char  revision;
};

#define NVBIN_FILE "wlan/prima/WCNSS_qcom_wlan_nv.bin"

/*
 * On SMD channel 4K of maximum data can be transferred, including message
 * header, so NV fragment size as next multiple of 1Kb is 3Kb.
 */
#define NV_FRAGMENT_SIZE  3072
#define MAX_CALIBRATED_DATA_SIZE  (64*1024)
#define LAST_FRAGMENT        (1 << 0)
#define MESSAGE_TO_FOLLOW    (1 << 1)
#define CAN_RECEIVE_CALDATA  (1 << 15)
#define WCNSS_RESP_SUCCESS   1
#define WCNSS_RESP_FAIL      0


/* Macro to find the total number fragments of the NV bin Image */
#define TOTALFRAGMENTS(x) (((x % NV_FRAGMENT_SIZE) == 0) ? \
	(x / NV_FRAGMENT_SIZE) : ((x / NV_FRAGMENT_SIZE) + 1))

struct nvbin_dnld_req_params {
	/*
	 * Fragment sequence number of the NV bin Image. NV Bin Image
	 * might not fit into one message due to size limitation of
	 * the SMD channel FIFO so entire NV blob is chopped into
	 * multiple fragments starting with seqeunce number 0. The
	 * last fragment is indicated by marking is_last_fragment field
	 * to 1. At receiving side, NV blobs would be concatenated
	 * together without any padding bytes in between.
	 */
	unsigned short frag_number;

	/*
	 * bit 0: When set to 1 it indicates that no more fragments will
	 * be sent.
	 * bit 1: When set, a new message will be followed by this message
	 * bit 2- bit 14:  Reserved
	 * bit 15: when set, it indicates that the sender is capable of
	 * receiving Calibrated data.
	 */
	unsigned short msg_flags;

	/* NV Image size (number of bytes) */
	unsigned int nvbin_buffer_size;

	/*
	 * Following the 'nvbin_buffer_size', there should be
	 * nvbin_buffer_size bytes of NV bin Image i.e.
	 * uint8[nvbin_buffer_size].
	 */
};


struct nvbin_dnld_req_msg {
	/*
	 * Note: The length specified in nvbin_dnld_req_msg messages
	 * should be hdr.msg_len = sizeof(nvbin_dnld_req_msg) +
	 * nvbin_buffer_size.
	 */
	struct smd_msg_hdr hdr;
	struct nvbin_dnld_req_params dnld_req_params;
};

struct cal_data_params {

	/* The total size of the calibrated data, including all the
	 * fragments.
	 */
	unsigned int total_size;
	unsigned short frag_number;
	/*
	 * bit 0: When set to 1 it indicates that no more fragments will
	 * be sent.
	 * bit 1: When set, a new message will be followed by this message
	 * bit 2- bit 15: Reserved
	 */
	unsigned short msg_flags;
	/*
	 * fragment size
	 */
	unsigned int frag_size;
	/*
	 * Following the frag_size, frag_size of fragmented
	 * data will be followed.
	 */
};

struct cal_data_msg {
	/*
	 * The length specified in cal_data_msg should be
	 * hdr.msg_len = sizeof(cal_data_msg) + frag_size
	 */
	struct smd_msg_hdr hdr;
	struct cal_data_params cal_params;
};

static struct {
	struct platform_device *pdev;
	void		*pil;
	struct resource	*mmio_res;
	struct resource	*tx_irq_res;
	struct resource	*rx_irq_res;
	struct resource	*gpios_5wire;
	const struct dev_pm_ops *pm_ops;
	int		triggered;
	int		smd_channel_ready;
	smd_channel_t	*smd_ch;
	unsigned char	wcnss_version[WCNSS_VERSION_LEN];
	unsigned char   fw_major;
	unsigned char   fw_minor;
	unsigned int	serial_number;
	int		thermal_mitigation;
	void		(*tm_notify)(struct device *, int);
	struct wcnss_wlan_config wlan_config;
	struct delayed_work wcnss_work;
	struct work_struct wcnssctrl_version_work;
	struct work_struct wcnssctrl_nvbin_dnld_work;
	struct work_struct wcnssctrl_rx_work;
	struct wake_lock wcnss_wake_lock;
	void __iomem *msm_wcnss_base;
	int	ssr_boot;
	int	nv_downloaded;
	unsigned char *fw_cal_data;
	unsigned char *user_cal_data;
	int	fw_cal_rcvd;
	int	fw_cal_exp_frag;
	int	fw_cal_available;
	int	user_cal_read;
	int	user_cal_available;
	int	user_cal_rcvd;
	int	user_cal_exp_size;
	int	device_opened;
	struct mutex dev_lock;
	wait_queue_head_t read_wait;
} *penv = NULL;

static ssize_t wcnss_serial_number_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (!penv)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%08X\n", penv->serial_number);
}

static ssize_t wcnss_serial_number_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int value;

	if (!penv)
		return -ENODEV;

	if (sscanf(buf, "%08X", &value) != 1)
		return -EINVAL;

	penv->serial_number = value;
	return count;
}

static DEVICE_ATTR(serial_number, S_IRUSR | S_IWUSR,
	wcnss_serial_number_show, wcnss_serial_number_store);


static ssize_t wcnss_thermal_mitigation_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (!penv)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", penv->thermal_mitigation);
}

static ssize_t wcnss_thermal_mitigation_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int value;

	if (!penv)
		return -ENODEV;

	if (sscanf(buf, "%d", &value) != 1)
		return -EINVAL;
	penv->thermal_mitigation = value;
	if (penv->tm_notify)
		(penv->tm_notify)(dev, value);
	return count;
}

static DEVICE_ATTR(thermal_mitigation, S_IRUSR | S_IWUSR,
	wcnss_thermal_mitigation_show, wcnss_thermal_mitigation_store);


static ssize_t wcnss_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (!penv)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%s", penv->wcnss_version);
}

static DEVICE_ATTR(wcnss_version, S_IRUSR,
		wcnss_version_show, NULL);

/* wcnss_reset_intr() is invoked when host drivers fails to
 * communicate with WCNSS over SMD; so logging these registers
 * helps to know WCNSS failure reason */
static void wcnss_log_ccpu_regs(void)
{
	void __iomem *ccu_base;
	void __iomem *ccu_reg;
	u32 reg = 0;

	ccu_base = ioremap(MSM_RIVA_CCU_BASE, SZ_512);
	if (!ccu_base) {
		pr_err("%s: ioremap WCNSS CCU reg failed\n", __func__);
		return;
	}

	ccu_reg = ccu_base + CCU_INVALID_ADDR_OFFSET;
	reg = readl_relaxed(ccu_reg);
	pr_info("%s: CCU_CCPU_INVALID_ADDR %08x\n", __func__, reg);

	ccu_reg = ccu_base + CCU_LAST_ADDR0_OFFSET;
	reg = readl_relaxed(ccu_reg);
	pr_info("%s: CCU_CCPU_LAST_ADDR0 %08x\n", __func__, reg);

	ccu_reg = ccu_base + CCU_LAST_ADDR1_OFFSET;
	reg = readl_relaxed(ccu_reg);
	pr_info("%s: CCU_CCPU_LAST_ADDR1 %08x\n", __func__, reg);

	ccu_reg = ccu_base + CCU_LAST_ADDR2_OFFSET;
	reg = readl_relaxed(ccu_reg);
	pr_info("%s: CCU_CCPU_LAST_ADDR2 %08x\n", __func__, reg);

	iounmap(ccu_base);
}

/* interface to reset Riva by sending the reset interrupt */
void wcnss_reset_intr(void)
{
	wcnss_log_ccpu_regs();
	wmb();
	__raw_writel(1 << 24, MSM_APCS_GCC_BASE + 0x8);
}
EXPORT_SYMBOL(wcnss_reset_intr);

static int wcnss_create_sysfs(struct device *dev)
{
	int ret;

	if (!dev)
		return -ENODEV;

	ret = device_create_file(dev, &dev_attr_serial_number);
	if (ret)
		return ret;

	ret = device_create_file(dev, &dev_attr_thermal_mitigation);
	if (ret)
		goto remove_serial;

	ret = device_create_file(dev, &dev_attr_wcnss_version);
	if (ret)
		goto remove_thermal;

	return 0;

remove_thermal:
	device_remove_file(dev, &dev_attr_thermal_mitigation);
remove_serial:
	device_remove_file(dev, &dev_attr_serial_number);

	return ret;
}

static void wcnss_remove_sysfs(struct device *dev)
{
	if (dev) {
		device_remove_file(dev, &dev_attr_serial_number);
		device_remove_file(dev, &dev_attr_thermal_mitigation);
		device_remove_file(dev, &dev_attr_wcnss_version);
	}
}
static void wcnss_smd_notify_event(void *data, unsigned int event)
{
	int len = 0;

	if (penv != data) {
		pr_err("wcnss: invalid env pointer in smd callback\n");
		return;
	}
	switch (event) {
	case SMD_EVENT_DATA:
		len = smd_read_avail(penv->smd_ch);
		if (len < 0) {
			pr_err("wcnss: failed to read from smd %d\n", len);
			return;
		}
		schedule_work(&penv->wcnssctrl_rx_work);
		break;

	case SMD_EVENT_OPEN:
		pr_debug("wcnss: opening WCNSS SMD channel :%s",
				WCNSS_CTRL_CHANNEL);
		schedule_work(&penv->wcnssctrl_version_work);

		break;

	case SMD_EVENT_CLOSE:
		pr_debug("wcnss: closing WCNSS SMD channel :%s",
				WCNSS_CTRL_CHANNEL);
		/* This SMD is closed only during SSR */
		penv->ssr_boot = true;
		penv->nv_downloaded = 0;
		break;

	default:
		break;
	}
}

static void wcnss_post_bootup(struct work_struct *work)
{
	pr_info("%s: Cancel APPS vote for Iris & Riva\n", __func__);

	/* Since Riva is up, cancel any APPS vote for Iris & Riva VREGs  */
	wcnss_wlan_power(&penv->pdev->dev, &penv->wlan_config,
		WCNSS_WLAN_SWITCH_OFF);

#if defined(CONFIG_MACH_MELIUS_SKT) || defined(CONFIG_MACH_MELIUS_KTT) || \
	defined(CONFIG_MACH_MELIUS_LGT)
#if !defined(CONFIG_RADIO_IRIS) && defined(CONFIG_IR_REMOCON_FPGA)
	/* workaround about conflict with qualcomm FM radio gpio */
	gpio_tlmm_config(GPIO_CFG(GPIO_IRDA_SDA, 0,
		GPIO_CFG_INPUT, GPIO_CFG_NO_PULL, GPIO_CFG_8MA), 1);
	gpio_tlmm_config(GPIO_CFG(GPIO_IRDA_SCL, 0,
		GPIO_CFG_INPUT,	GPIO_CFG_NO_PULL, GPIO_CFG_8MA), 1);
#endif
#endif
}

static int
wcnss_gpios_config(struct resource *gpios_5wire, bool enable)
{
	int i, j;
	int rc = 0;

	for (i = gpios_5wire->start; i <= gpios_5wire->end; i++) {
		if (enable) {
			rc = gpio_request(i, gpios_5wire->name);
			if (rc) {
				pr_err("WCNSS gpio_request %d err %d\n", i, rc);
				goto fail;
			}
		} else
			gpio_free(i);
	}

	return rc;

fail:
	for (j = i-1; j >= gpios_5wire->start; j--)
		gpio_free(j);
	return rc;
}

static int __devinit
wcnss_wlan_ctrl_probe(struct platform_device *pdev)
{
	if (!penv)
		return -ENODEV;

	penv->smd_channel_ready = 1;

	pr_info("%s: SMD ctrl channel up\n", __func__);

	/* Schedule a work to do any post boot up activity */
	INIT_DELAYED_WORK(&penv->wcnss_work, wcnss_post_bootup);
	schedule_delayed_work(&penv->wcnss_work, msecs_to_jiffies(10000));

	return 0;
}

void wcnss_flush_delayed_boot_votes()
{
	flush_delayed_work_sync(&penv->wcnss_work);
}
EXPORT_SYMBOL(wcnss_flush_delayed_boot_votes);

static int __devexit
wcnss_wlan_ctrl_remove(struct platform_device *pdev)
{
	if (penv)
		penv->smd_channel_ready = 0;

	pr_info("%s: SMD ctrl channel down\n", __func__);

	return 0;
}


static struct platform_driver wcnss_wlan_ctrl_driver = {
	.driver = {
		.name	= "WLAN_CTRL",
		.owner	= THIS_MODULE,
	},
	.probe	= wcnss_wlan_ctrl_probe,
	.remove	= __devexit_p(wcnss_wlan_ctrl_remove),
};

static int __devexit
wcnss_ctrl_remove(struct platform_device *pdev)
{
	if (penv && penv->smd_ch)
		smd_close(penv->smd_ch);

	return 0;
}

static int __devinit
wcnss_ctrl_probe(struct platform_device *pdev)
{
	int ret = 0;

	if (!penv)
		return -ENODEV;

	ret = smd_named_open_on_edge(WCNSS_CTRL_CHANNEL, SMD_APPS_WCNSS,
			&penv->smd_ch, penv, wcnss_smd_notify_event);
	if (ret < 0) {
		pr_err("wcnss: cannot open the smd command channel %s: %d\n",
				WCNSS_CTRL_CHANNEL, ret);
		return -ENODEV;
	}
	smd_disable_read_intr(penv->smd_ch);

	return 0;
}

/* platform device for WCNSS_CTRL SMD channel */
static struct platform_driver wcnss_ctrl_driver = {
	.driver = {
		.name	= "WCNSS_CTRL",
		.owner	= THIS_MODULE,
	},
	.probe	= wcnss_ctrl_probe,
	.remove	= __devexit_p(wcnss_ctrl_remove),
};

struct device *wcnss_wlan_get_device(void)
{
	if (penv && penv->pdev && penv->smd_channel_ready)
		return &penv->pdev->dev;
	return NULL;
}
EXPORT_SYMBOL(wcnss_wlan_get_device);

struct platform_device *wcnss_get_platform_device(void)
{
	if (penv && penv->pdev)
		return penv->pdev;
	return NULL;
}
EXPORT_SYMBOL(wcnss_get_platform_device);

struct wcnss_wlan_config *wcnss_get_wlan_config(void)
{
	if (penv && penv->pdev)
		return &penv->wlan_config;
	return NULL;
}
EXPORT_SYMBOL(wcnss_get_wlan_config);

int wcnss_device_ready(void)
{
	if (penv && penv->pdev && penv->nv_downloaded)
		return 1;
	return 0;
}
EXPORT_SYMBOL(wcnss_device_ready);


struct resource *wcnss_wlan_get_memory_map(struct device *dev)
{
	if (penv && dev && (dev == &penv->pdev->dev) && penv->smd_channel_ready)
		return penv->mmio_res;
	return NULL;
}
EXPORT_SYMBOL(wcnss_wlan_get_memory_map);

int wcnss_wlan_get_dxe_tx_irq(struct device *dev)
{
	if (penv && dev && (dev == &penv->pdev->dev) &&
				penv->tx_irq_res && penv->smd_channel_ready)
		return penv->tx_irq_res->start;
	return WCNSS_WLAN_IRQ_INVALID;
}
EXPORT_SYMBOL(wcnss_wlan_get_dxe_tx_irq);

int wcnss_wlan_get_dxe_rx_irq(struct device *dev)
{
	if (penv && dev && (dev == &penv->pdev->dev) &&
				penv->rx_irq_res && penv->smd_channel_ready)
		return penv->rx_irq_res->start;
	return WCNSS_WLAN_IRQ_INVALID;
}
EXPORT_SYMBOL(wcnss_wlan_get_dxe_rx_irq);

void wcnss_wlan_register_pm_ops(struct device *dev,
				const struct dev_pm_ops *pm_ops)
{
	if (penv && dev && (dev == &penv->pdev->dev) && pm_ops)
		penv->pm_ops = pm_ops;
}
EXPORT_SYMBOL(wcnss_wlan_register_pm_ops);

void wcnss_wlan_unregister_pm_ops(struct device *dev,
				const struct dev_pm_ops *pm_ops)
{
	if (penv && dev && (dev == &penv->pdev->dev) && pm_ops) {
		if (pm_ops->suspend != penv->pm_ops->suspend ||
				pm_ops->resume != penv->pm_ops->resume)
			pr_err("PM APIs dont match with registered APIs\n");
		penv->pm_ops = NULL;
	}
}
EXPORT_SYMBOL(wcnss_wlan_unregister_pm_ops);

void wcnss_register_thermal_mitigation(struct device *dev,
				void (*tm_notify)(struct device *, int))
{
	if (penv && dev && tm_notify)
		penv->tm_notify = tm_notify;
}
EXPORT_SYMBOL(wcnss_register_thermal_mitigation);

void wcnss_unregister_thermal_mitigation(
				void (*tm_notify)(struct device *, int))
{
	if (penv && tm_notify) {
		if (tm_notify != penv->tm_notify)
			pr_err("tm_notify doesn't match registered\n");
		penv->tm_notify = NULL;
	}
}
EXPORT_SYMBOL(wcnss_unregister_thermal_mitigation);

unsigned int wcnss_get_serial_number(void)
{
	if (penv)
		return penv->serial_number;
	return 0;
}
EXPORT_SYMBOL(wcnss_get_serial_number);

void wcnss_ssr_boot_notify(void)
{
	void __iomem *pmu_spare_reg;
	u32 reg = 0;
	unsigned long flags;

	pmu_spare_reg = penv->msm_wcnss_base + RIVA_SPARE_OFFSET;

	spin_lock_irqsave(&reg_spinlock, flags);
	reg = readl_relaxed(pmu_spare_reg);
	reg |= RIVA_SSR_BIT;
	writel_relaxed(reg, pmu_spare_reg);
	spin_unlock_irqrestore(&reg_spinlock, flags);
}
EXPORT_SYMBOL(wcnss_ssr_boot_notify);

static int enable_wcnss_suspend_notify;

static int enable_wcnss_suspend_notify_set(const char *val,
				struct kernel_param *kp)
{
	int ret;

	ret = param_set_int(val, kp);
	if (ret)
		return ret;

	if (enable_wcnss_suspend_notify)
		pr_info("Suspend notification activated for wcnss\n");

	return 0;
}
module_param_call(enable_wcnss_suspend_notify, enable_wcnss_suspend_notify_set,
		param_get_int, &enable_wcnss_suspend_notify, S_IRUGO | S_IWUSR);

static void wcnss_suspend_notify(void)
{
	void __iomem *pmu_spare_reg;
	u32 reg = 0;
	unsigned long flags;

	/* For Riva */
	pmu_spare_reg = penv->msm_wcnss_base + RIVA_SPARE_OFFSET;

	spin_lock_irqsave(&reg_spinlock, flags);
	reg = readl_relaxed(pmu_spare_reg);
	reg |= RIVA_SUSPEND_BIT;
	writel_relaxed(reg, pmu_spare_reg);
	spin_unlock_irqrestore(&reg_spinlock, flags);
}

static void wcnss_resume_notify(void)
{
	void __iomem *pmu_spare_reg;
	u32 reg = 0;
	unsigned long flags;

	/* For Riva */
	pmu_spare_reg = penv->msm_wcnss_base + RIVA_SPARE_OFFSET;
	spin_lock_irqsave(&reg_spinlock, flags);
	reg = readl_relaxed(pmu_spare_reg);
	reg &= ~RIVA_SUSPEND_BIT;
	writel_relaxed(reg, pmu_spare_reg);
	spin_unlock_irqrestore(&reg_spinlock, flags);
}

static int wcnss_wlan_suspend(struct device *dev)
{
	int ret = 0;

	if (penv && dev && (dev == &penv->pdev->dev) &&
	    penv->smd_channel_ready &&
	    penv->pm_ops && penv->pm_ops->suspend) {
		ret = penv->pm_ops->suspend(dev);
		if (ret == 0 && enable_wcnss_suspend_notify)
			wcnss_suspend_notify();
		return ret;
	}
	return 0;
}

static int wcnss_wlan_resume(struct device *dev)
{
	int ret = 0;

	if (penv && dev && (dev == &penv->pdev->dev) &&
	    penv->smd_channel_ready &&
	    penv->pm_ops && penv->pm_ops->resume) {
		ret = penv->pm_ops->resume(dev);
		if (ret == 0 && enable_wcnss_suspend_notify)
			wcnss_resume_notify();
		return ret;
	}
	return 0;
}

void wcnss_prevent_suspend()
{
	if (penv)
		wake_lock(&penv->wcnss_wake_lock);
}
EXPORT_SYMBOL(wcnss_prevent_suspend);

void wcnss_allow_suspend()
{
	if (penv)
		wake_unlock(&penv->wcnss_wake_lock);
}
EXPORT_SYMBOL(wcnss_allow_suspend);

int fw_cal_data_available(void)
{
	if (penv)
		return penv->fw_cal_available;
	else
		return -ENODEV;
}

static int wcnss_smd_tx(void *data, int len)
{
	int ret = 0;

	ret = smd_write_avail(penv->smd_ch);
	if (ret < len) {
		pr_err("wcnss: no space available for smd frame\n");
		return -ENOSPC;
	}
	ret = smd_write(penv->smd_ch, data, len);
	if (ret < len) {
		pr_err("wcnss: failed to write Command %d", len);
		ret = -ENODEV;
	}
	return ret;
}

static unsigned char wcnss_fw_status(void)
{
	int len = 0;
	int rc = 0;

	unsigned char fw_status = 0xFF;

	len = smd_read_avail(penv->smd_ch);
	if (len < 1) {
		pr_err("%s: invalid firmware status", __func__);
		return fw_status;
	}

	rc = smd_read(penv->smd_ch, &fw_status, 1);
	if (rc < 0) {
		pr_err("%s: incomplete data read from smd\n", __func__);
		return fw_status;
	}
	return fw_status;
}

static void wcnss_send_cal_rsp(unsigned char fw_status)
{
	struct smd_msg_hdr *rsphdr;
	unsigned char *msg = NULL;
	int rc;

	msg = kmalloc((sizeof(struct smd_msg_hdr) + 1), GFP_KERNEL);
	if (NULL == msg) {
		pr_err("wcnss: %s: failed to get memory\n", __func__);
		return;
	}

	rsphdr = (struct smd_msg_hdr *)msg;
	rsphdr->msg_type = WCNSS_CALDATA_UPLD_RSP;
	rsphdr->msg_len = sizeof(struct smd_msg_hdr) + 1;
	memcpy(msg+sizeof(struct smd_msg_hdr), &fw_status, 1);

	rc = wcnss_smd_tx(msg, rsphdr->msg_len);
	if (rc < 0)
		pr_err("wcnss: smd tx failed\n");
}

/* Collect calibrated data from WCNSS */
void extract_cal_data(int len)
{
	int rc;
	struct cal_data_params calhdr;
	unsigned char fw_status = WCNSS_RESP_FAIL;

	if (len < sizeof(struct cal_data_params)) {
		pr_err("wcnss: incomplete cal header length\n");
		return;
	}

	rc = smd_read(penv->smd_ch, (unsigned char *)&calhdr,
			sizeof(struct cal_data_params));
	if (rc < sizeof(struct cal_data_params)) {
		pr_err("wcnss: incomplete cal header read from smd\n");
		return;
	}

	if (penv->fw_cal_exp_frag != calhdr.frag_number) {
		pr_err("wcnss: Invalid frgament");
		goto exit;
	}

	if (calhdr.frag_size > WCNSS_MAX_FRAME_SIZE) {
		pr_err("wcnss: Invalid fragment size");
		goto exit;
	}

	if (0 == calhdr.frag_number) {
		if (calhdr.total_size > MAX_CALIBRATED_DATA_SIZE) {
			pr_err("wcnss: Invalid cal data size %d",
				calhdr.total_size);
			goto exit;
		}
		kfree(penv->fw_cal_data);
		penv->fw_cal_rcvd = 0;
		penv->fw_cal_data = kmalloc(calhdr.total_size,
				GFP_KERNEL);
		if (penv->fw_cal_data == NULL) {
			smd_read(penv->smd_ch, NULL, calhdr.frag_size);
			goto exit;
		}
	}

	mutex_lock(&penv->dev_lock);
	if (penv->fw_cal_rcvd + calhdr.frag_size >
			MAX_CALIBRATED_DATA_SIZE) {
		pr_err("calibrated data size is more than expected %d",
				penv->fw_cal_rcvd + calhdr.frag_size);
		penv->fw_cal_exp_frag = 0;
		penv->fw_cal_rcvd = 0;
		smd_read(penv->smd_ch, NULL, calhdr.frag_size);
		goto unlock_exit;
	}

	rc = smd_read(penv->smd_ch, penv->fw_cal_data + penv->fw_cal_rcvd,
			calhdr.frag_size);
	if (rc < calhdr.frag_size)
		goto unlock_exit;

	penv->fw_cal_exp_frag++;
	penv->fw_cal_rcvd += calhdr.frag_size;

	if (calhdr.msg_flags & LAST_FRAGMENT) {
		penv->fw_cal_exp_frag = 0;
		penv->fw_cal_available = true;
		pr_info("wcnss: cal data collection completed\n");
	}
	mutex_unlock(&penv->dev_lock);
	wake_up(&penv->read_wait);

	if (penv->fw_cal_available) {
		fw_status = WCNSS_RESP_SUCCESS;
		wcnss_send_cal_rsp(fw_status);
	}
	return;

unlock_exit:
	mutex_unlock(&penv->dev_lock);

exit:
	wcnss_send_cal_rsp(fw_status);
	return;
}


static void wcnssctrl_rx_handler(struct work_struct *worker)
{
	int len = 0;
	int rc = 0;
	unsigned char buf[sizeof(struct wcnss_version)];
	struct smd_msg_hdr *phdr;
	struct wcnss_version *pversion;
	unsigned char fw_status = 0;

	len = smd_read_avail(penv->smd_ch);
	if (len > WCNSS_MAX_FRAME_SIZE) {
		pr_err("wcnss: frame larger than the allowed size\n");
		smd_read(penv->smd_ch, NULL, len);
		return;
	}
	if (len <= 0)
		return;

	rc = smd_read(penv->smd_ch, buf, sizeof(struct smd_msg_hdr));
	if (rc < sizeof(struct smd_msg_hdr)) {
		pr_err("wcnss: incomplete header read from smd\n");
		return;
	}
	len -= sizeof(struct smd_msg_hdr);

	phdr = (struct smd_msg_hdr *)buf;

	switch (phdr->msg_type) {

	case WCNSS_VERSION_RSP:
		if (len != sizeof(struct wcnss_version)
				- sizeof(struct smd_msg_hdr)) {
			pr_err("wcnss: invalid version data from wcnss %d\n",
					len);
			return;
		}
		rc = smd_read(penv->smd_ch, buf+sizeof(struct smd_msg_hdr),
				len);
		if (rc < len) {
			pr_err("wcnss: incomplete data read from smd\n");
			return;
		}
		pversion = (struct wcnss_version *)buf;
		penv->fw_major = pversion->major;
		penv->fw_minor = pversion->minor;
		snprintf(penv->wcnss_version, WCNSS_VERSION_LEN,
			"%02x%02x%02x%02x", pversion->major, pversion->minor,
					pversion->version, pversion->revision);
		pr_info("wcnss: version %s\n", penv->wcnss_version);
		/*
		 * schedule work to download nvbin to riva ccpu,
		 * only if riva major >= 1 and minor >= 4.
		 */
		if ((pversion->major >= 1) && (pversion->minor >= 4)) {
			pr_info("wcnss: schedule dnld work for riva\n");
			schedule_work(&penv->wcnssctrl_nvbin_dnld_work);
		} else {
			penv->nv_downloaded = true;
		}
		break;

	case WCNSS_NVBIN_DNLD_RSP:
		penv->nv_downloaded = true;
		fw_status = wcnss_fw_status();
		pr_debug("wcnss: received WCNSS_NVBIN_DNLD_RSP from ccpu %u\n",
			fw_status);
		break;

	case WCNSS_CALDATA_DNLD_RSP:
		penv->nv_downloaded = true;
		fw_status = wcnss_fw_status();
		pr_debug("wcnss: received WCNSS_CALDATA_DNLD_RSP from ccpu %u\n",
			fw_status);
		break;

	case WCNSS_CALDATA_UPLD_REQ:
		penv->fw_cal_available = 0;
		extract_cal_data(len);
		break;

	default:
		pr_err("wcnss: invalid message type %d\n", phdr->msg_type);
	}
	return;
}

static void wcnss_send_version_req(struct work_struct *worker)
{
	struct smd_msg_hdr smd_msg;
	int ret = 0;

	smd_msg.msg_type = WCNSS_VERSION_REQ;
	smd_msg.msg_len = sizeof(smd_msg);
	ret = wcnss_smd_tx(&smd_msg, smd_msg.msg_len);
	if (ret < 0)
		pr_err("wcnss: smd tx failed\n");

	return;
}


static void wcnss_nvbin_dnld(void)
{
	int ret = 0;
	struct nvbin_dnld_req_msg *dnld_req_msg;
	unsigned short total_fragments = 0;
	unsigned short count = 0;
	unsigned short retry_count = 0;
	unsigned short cur_frag_size = 0;
	unsigned char *outbuffer = NULL;
	const void *nv_blob_addr = NULL;
	unsigned int nv_blob_size = 0;
	const struct firmware *nv = NULL;
	struct device *dev = &penv->pdev->dev;

	ret = request_firmware(&nv, NVBIN_FILE, dev);

	if (ret || !nv || !nv->data || !nv->size) {
		pr_err("wcnss: %s: request_firmware failed for %s\n",
			__func__, NVBIN_FILE);
		return;
	}

	/*
	 * First 4 bytes in nv blob is validity bitmap.
	 * We cannot validate nv, so skip those 4 bytes.
	 */
	nv_blob_addr = nv->data + 4;
	nv_blob_size = nv->size - 4;

	total_fragments = TOTALFRAGMENTS(nv_blob_size);

	pr_info("wcnss: NV bin size: %d, total_fragments: %d\n",
		nv_blob_size, total_fragments);

	/* get buffer for nv bin dnld req message */
	outbuffer = kmalloc((sizeof(struct nvbin_dnld_req_msg) +
		NV_FRAGMENT_SIZE), GFP_KERNEL);

	if (NULL == outbuffer) {
		pr_err("wcnss: %s: failed to get buffer\n", __func__);
		goto err_free_nv;
	}

	dnld_req_msg = (struct nvbin_dnld_req_msg *)outbuffer;

	dnld_req_msg->hdr.msg_type = WCNSS_NVBIN_DNLD_REQ;
	dnld_req_msg->dnld_req_params.msg_flags = 0;

	for (count = 0; count < total_fragments; count++) {
		dnld_req_msg->dnld_req_params.frag_number = count;

		if (count == (total_fragments - 1)) {
			/* last fragment, take care of boundry condition */
			cur_frag_size = nv_blob_size % NV_FRAGMENT_SIZE;
			if (!cur_frag_size)
				cur_frag_size = NV_FRAGMENT_SIZE;

			dnld_req_msg->dnld_req_params.msg_flags |=
				LAST_FRAGMENT;
			dnld_req_msg->dnld_req_params.msg_flags |=
				CAN_RECEIVE_CALDATA;
		} else {
			cur_frag_size = NV_FRAGMENT_SIZE;
			dnld_req_msg->dnld_req_params.msg_flags &=
				~LAST_FRAGMENT;
		}

		dnld_req_msg->dnld_req_params.nvbin_buffer_size =
			cur_frag_size;

		dnld_req_msg->hdr.msg_len =
			sizeof(struct nvbin_dnld_req_msg) + cur_frag_size;

		/* copy NV fragment */
		memcpy((outbuffer + sizeof(struct nvbin_dnld_req_msg)),
			(nv_blob_addr + count * NV_FRAGMENT_SIZE),
			cur_frag_size);

		ret = wcnss_smd_tx(outbuffer, dnld_req_msg->hdr.msg_len);

		retry_count = 0;
		while ((ret == -ENOSPC) && (retry_count <= 3)) {
			pr_debug("wcnss: %s: smd tx failed, ENOSPC\n",
				__func__);
			pr_debug("fragment: %d, len: %d, TotFragments: %d, retry_count: %d\n",
				count, dnld_req_msg->hdr.msg_len,
				total_fragments, retry_count);

			/* wait and try again */
			msleep(20);
			retry_count++;
			ret = wcnss_smd_tx(outbuffer,
					dnld_req_msg->hdr.msg_len);
		}

		if (ret < 0) {
			pr_err("wcnss: %s: smd tx failed\n", __func__);
			pr_err("fragment %d, len: %d, TotFragments: %d, retry_count: %d\n",
				count, dnld_req_msg->hdr.msg_len,
				total_fragments, retry_count);
			goto err_dnld;
		}
	}

err_dnld:
	/* free buffer */
	kfree(outbuffer);

err_free_nv:
	/* release firmware */
	release_firmware(nv);

	return;
}


static void wcnss_caldata_dnld(const void *cal_data,
		unsigned int cal_data_size, bool msg_to_follow)
{
	int ret = 0;
	struct cal_data_msg *cal_msg;
	unsigned short total_fragments = 0;
	unsigned short count = 0;
	unsigned short retry_count = 0;
	unsigned short cur_frag_size = 0;
	unsigned char *outbuffer = NULL;

	total_fragments = TOTALFRAGMENTS(cal_data_size);

	outbuffer = kmalloc((sizeof(struct cal_data_msg) +
		NV_FRAGMENT_SIZE), GFP_KERNEL);

	if (NULL == outbuffer) {
		pr_err("wcnss: %s: failed to get buffer\n", __func__);
		return;
	}

	cal_msg = (struct cal_data_msg *)outbuffer;

	cal_msg->hdr.msg_type = WCNSS_CALDATA_DNLD_REQ;
	cal_msg->cal_params.msg_flags = 0;

	for (count = 0; count < total_fragments; count++) {
		cal_msg->cal_params.frag_number = count;

		if (count == (total_fragments - 1)) {
			cur_frag_size = cal_data_size % NV_FRAGMENT_SIZE;
			if (!cur_frag_size)
				cur_frag_size = NV_FRAGMENT_SIZE;

			cal_msg->cal_params.msg_flags
			    |= LAST_FRAGMENT;
			if (msg_to_follow)
				cal_msg->cal_params.msg_flags |=
					MESSAGE_TO_FOLLOW;
		} else {
			cur_frag_size = NV_FRAGMENT_SIZE;
			cal_msg->cal_params.msg_flags &=
				~LAST_FRAGMENT;
		}

		cal_msg->cal_params.total_size = cal_data_size;
		cal_msg->cal_params.frag_size =
			cur_frag_size;

		cal_msg->hdr.msg_len =
			sizeof(struct cal_data_msg) + cur_frag_size;

		memcpy((outbuffer + sizeof(struct cal_data_msg)),
			(cal_data + count * NV_FRAGMENT_SIZE),
			cur_frag_size);

		ret = wcnss_smd_tx(outbuffer, cal_msg->hdr.msg_len);

		retry_count = 0;
		while ((ret == -ENOSPC) && (retry_count <= 3)) {
			pr_debug("wcnss: %s: smd tx failed, ENOSPC\n",
					__func__);
			pr_debug("fragment: %d, len: %d, TotFragments: %d, retry_count: %d\n",
				count, cal_msg->hdr.msg_len,
				total_fragments, retry_count);

			/* wait and try again */
			msleep(20);
			retry_count++;
			ret = wcnss_smd_tx(outbuffer,
				cal_msg->hdr.msg_len);
		}

		if (ret < 0) {
			pr_err("wcnss: %s: smd tx failed\n", __func__);
			pr_err("fragment %d, len: %d, TotFragments: %d, retry_count: %d\n",
				count, cal_msg->hdr.msg_len,
				total_fragments, retry_count);
			goto err_dnld;
		}
	}


err_dnld:
	/* free buffer */
	kfree(outbuffer);

	return;
}


static void wcnss_nvbin_dnld_main(struct work_struct *worker)
{
	int retry = 0;

	if (!FW_CALDATA_CAPABLE())
		goto nv_download;

	if (!penv->fw_cal_available && WCNSS_CONFIG_UNSPECIFIED
		!= has_calibrated_data && !penv->user_cal_available) {
		while (!penv->user_cal_available && retry++ < 5)
			msleep(500);
	}

	/* only cal data is sent during ssr (if available) */
	if (penv->fw_cal_available && penv->ssr_boot) {
		pr_info_ratelimited("wcnss: cal download during SSR, using fw cal");
		wcnss_caldata_dnld(penv->fw_cal_data, penv->fw_cal_rcvd, false);
		return;

	} else if (penv->user_cal_available && penv->ssr_boot) {
		pr_info_ratelimited("wcnss: cal download during SSR, using user cal");
		wcnss_caldata_dnld(penv->user_cal_data,
		penv->user_cal_rcvd, false);
		return;

	} else if (penv->user_cal_available) {
		pr_info_ratelimited("wcnss: cal download during cold boot, using user cal");
		wcnss_caldata_dnld(penv->user_cal_data,
		penv->user_cal_rcvd, true);
	}

nv_download:
	pr_info_ratelimited("wcnss: NV download");
	wcnss_nvbin_dnld();

	return;
}



static int
wcnss_trigger_config(struct platform_device *pdev)
{
	int ret;
	struct qcom_wcnss_opts *pdata;

	/* make sure we are only triggered once */
	if (penv->triggered)
		return 0;
	penv->triggered = 1;

	/* initialize the WCNSS device configuration */
	pdata = pdev->dev.platform_data;
	if (WCNSS_CONFIG_UNSPECIFIED == has_48mhz_xo)
		has_48mhz_xo = pdata->has_48mhz_xo;
	penv->wlan_config.use_48mhz_xo = has_48mhz_xo;

	penv->thermal_mitigation = 0;
	strlcpy(penv->wcnss_version, "INVALID", WCNSS_VERSION_LEN);

	penv->gpios_5wire = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"wcnss_gpios_5wire");

	/* allocate 5-wire GPIO resources */
	if (!penv->gpios_5wire) {
		dev_err(&pdev->dev, "insufficient IO resources\n");
		ret = -ENOENT;
		goto fail_gpio_res;
	}

	/* Configure 5 wire GPIOs */
	ret = wcnss_gpios_config(penv->gpios_5wire, true);
	if (ret) {
		dev_err(&pdev->dev, "WCNSS gpios config failed.\n");
		goto fail_gpio_res;
	}

	/* power up the WCNSS */
	ret = wcnss_wlan_power(&pdev->dev, &penv->wlan_config,
					WCNSS_WLAN_SWITCH_ON);
	if (ret) {
		dev_err(&pdev->dev, "WCNSS Power-up failed.\n");
		goto fail_power;
	}

	/* trigger initialization of the WCNSS */
	penv->pil = pil_get(WCNSS_PIL_DEVICE);
	if (IS_ERR(penv->pil)) {
		dev_err(&pdev->dev, "Peripheral Loader failed on WCNSS.\n");
		ret = PTR_ERR(penv->pil);
		penv->pil = NULL;
		goto fail_pil;
	}

	/* allocate resources */
	penv->mmio_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"wcnss_mmio");
	penv->tx_irq_res = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
							"wcnss_wlantx_irq");
	penv->rx_irq_res = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
							"wcnss_wlanrx_irq");

	if (!(penv->mmio_res && penv->tx_irq_res && penv->rx_irq_res)) {
		dev_err(&pdev->dev, "insufficient resources\n");
		ret = -ENOENT;
		goto fail_res;
	}

	INIT_WORK(&penv->wcnssctrl_rx_work, wcnssctrl_rx_handler);
	INIT_WORK(&penv->wcnssctrl_version_work, wcnss_send_version_req);
	INIT_WORK(&penv->wcnssctrl_nvbin_dnld_work, wcnss_nvbin_dnld_main);

	wake_lock_init(&penv->wcnss_wake_lock, WAKE_LOCK_SUSPEND, "wcnss");

	penv->msm_wcnss_base = ioremap(MSM_RIVA_PHYS, SZ_256);
	if (!penv->msm_wcnss_base) {
		pr_err("%s: ioremap wcnss physical failed\n", __func__);
		goto fail_wake;
	}

	return 0;

fail_wake:
	wake_lock_destroy(&penv->wcnss_wake_lock);

fail_res:
	if (penv->pil)
		pil_put(penv->pil);
fail_pil:
	wcnss_wlan_power(&pdev->dev, &penv->wlan_config,
				WCNSS_WLAN_SWITCH_OFF);
fail_power:
	wcnss_gpios_config(penv->gpios_5wire, false);
fail_gpio_res:
	kfree(penv);
	penv = NULL;
	return ret;
}

static int wcnss_node_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev;

	if (!penv)
		return -EFAULT;

	/* first open is only to trigger WCNSS platform driver */
	if (!penv->triggered) {
		pr_info(DEVICE " triggered by userspace\n");
		pdev = penv->pdev;
		return wcnss_trigger_config(pdev);

	} else if (penv->device_opened) {
		pr_info(DEVICE " already opened\n");
		return -EBUSY;
	}

	mutex_lock(&penv->dev_lock);
	penv->user_cal_rcvd = 0;
	penv->user_cal_read = 0;
	penv->user_cal_available = false;
	penv->user_cal_data = NULL;
	penv->device_opened = 1;
	mutex_unlock(&penv->dev_lock);

	return 0;
}

static ssize_t wcnss_wlan_read(struct file *fp, char __user
			*buffer, size_t count, loff_t *position)
{
	int rc = 0;

	if (!penv || !penv->device_opened)
		return -EFAULT;

	rc = wait_event_interruptible(penv->read_wait, penv->fw_cal_rcvd
			> penv->user_cal_read || penv->fw_cal_available);

	if (rc < 0)
		return rc;

	mutex_lock(&penv->dev_lock);

	if (penv->fw_cal_available && penv->fw_cal_rcvd
			== penv->user_cal_read) {
		rc = 0;
		goto exit;
	}

	if (count > penv->fw_cal_rcvd - penv->user_cal_read)
		count = penv->fw_cal_rcvd - penv->user_cal_read;

	rc = copy_to_user(buffer, penv->fw_cal_data +
			penv->user_cal_read, count);
	if (rc == 0) {
		penv->user_cal_read += count;
		rc = count;
	}

exit:
	mutex_unlock(&penv->dev_lock);
	return rc;
}

/* first (valid) write to this device should be 4 bytes cal file size */
static ssize_t wcnss_wlan_write(struct file *fp, const char __user
			*user_buffer, size_t count, loff_t *position)
{
	int rc = 0;
	int size = 0;

	if (!penv || !penv->device_opened || penv->user_cal_available)
		return -EFAULT;

	if (penv->user_cal_rcvd == 0 && count >= 4
			&& !penv->user_cal_data) {
		rc = copy_from_user((void *)&size, user_buffer, 4);
		if (size > MAX_CALIBRATED_DATA_SIZE) {
			pr_err(DEVICE " invalid size to write %d\n", size);
			return -EFAULT;
		}

		rc += count;
		count -= 4;
		penv->user_cal_exp_size =  size;
		penv->user_cal_data = kmalloc(size, GFP_KERNEL);
		if (penv->user_cal_data == NULL) {
			pr_err(DEVICE " no memory to write\n");
			return -ENOMEM;
		}
		if (0 == count)
			goto exit;

	} else if (penv->user_cal_rcvd == 0 && count < 4)
		return -EFAULT;

	if (MAX_CALIBRATED_DATA_SIZE < count + penv->user_cal_rcvd) {
		pr_err(DEVICE " invalid size to write %d\n", count +
				penv->user_cal_rcvd);
		rc = -ENOMEM;
		goto exit;
	}
	rc = copy_from_user((void *)penv->user_cal_data +
			penv->user_cal_rcvd, user_buffer, count);
	if (0 == rc) {
		penv->user_cal_rcvd += count;
		rc += count;
	}
	if (penv->user_cal_rcvd == penv->user_cal_exp_size) {
		penv->user_cal_available = true;
		pr_info_ratelimited("wcnss: user cal written");
	}

exit:
	return rc;
}


static const struct file_operations wcnss_node_fops = {
	.owner = THIS_MODULE,
	.open = wcnss_node_open,
	.read = wcnss_wlan_read,
	.write = wcnss_wlan_write,
};

static struct miscdevice wcnss_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE,
	.fops = &wcnss_node_fops,
};

static int __devinit
wcnss_wlan_probe(struct platform_device *pdev)
{
	int ret = 0;

	/* verify we haven't been called more than once */
	if (penv) {
		dev_err(&pdev->dev, "cannot handle multiple devices.\n");
		return -ENODEV;
	}

	/* create an environment to track the device */
	penv = kzalloc(sizeof(*penv), GFP_KERNEL);
	if (!penv) {
		dev_err(&pdev->dev, "cannot allocate device memory.\n");
		return -ENOMEM;
	}
	penv->pdev = pdev;

	/* register sysfs entries */
	ret = wcnss_create_sysfs(&pdev->dev);
	if (ret)
		return -ENOENT;

	mutex_init(&penv->dev_lock);
	init_waitqueue_head(&penv->read_wait);

	/*
	 * Since we were built into the kernel we'll be called as part
	 * of kernel initialization.  We don't know if userspace
	 * applications are available to service PIL at this time
	 * (they probably are not), so we simply create a device node
	 * here.  When userspace is available it should touch the
	 * device so that we know that WCNSS configuration can take
	 * place
	 */
	pr_info(DEVICE " probed in built-in mode\n");
	return misc_register(&wcnss_misc);

}

static int __devexit
wcnss_wlan_remove(struct platform_device *pdev)
{
	wcnss_remove_sysfs(&pdev->dev);
	return 0;
}


static const struct dev_pm_ops wcnss_wlan_pm_ops = {
	.suspend	= wcnss_wlan_suspend,
	.resume		= wcnss_wlan_resume,
};

static struct platform_driver wcnss_wlan_driver = {
	.driver = {
		.name	= DEVICE,
		.owner	= THIS_MODULE,
		.pm	= &wcnss_wlan_pm_ops,
	},
	.probe	= wcnss_wlan_probe,
	.remove	= __devexit_p(wcnss_wlan_remove),
};

static int __init wcnss_wlan_init(void)
{
	int ret = 0;

	platform_driver_register(&wcnss_wlan_driver);
	platform_driver_register(&wcnss_wlan_ctrl_driver);
	platform_driver_register(&wcnss_ctrl_driver);

#ifdef CONFIG_WCNSS_MEM_PRE_ALLOC
	ret = wcnss_prealloc_init();
	if (ret < 0)
		pr_err("wcnss: pre-allocation failed\n");
#endif

	return ret;
}

static void __exit wcnss_wlan_exit(void)
{
	if (penv) {
		if (penv->pil)
			pil_put(penv->pil);


		kfree(penv);
		penv = NULL;
	}

	platform_driver_unregister(&wcnss_ctrl_driver);
	platform_driver_unregister(&wcnss_wlan_ctrl_driver);
	platform_driver_unregister(&wcnss_wlan_driver);
#ifdef CONFIG_WCNSS_MEM_PRE_ALLOC
	wcnss_prealloc_deinit();
#endif
}

module_init(wcnss_wlan_init);
module_exit(wcnss_wlan_exit);

MODULE_LICENSE("GPL v2");
MODULE_VERSION(VERSION);
MODULE_DESCRIPTION(DEVICE "Driver");

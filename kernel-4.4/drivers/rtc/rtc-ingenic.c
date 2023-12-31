/*
 * Copyright (C) 2015 Ingenic Semiconductor Co., Ltd.
 * Author: cli <chen.li@ingenic.com>
 *
 * Real Time Clock interface for Ingenic's SOC, such as X1000,
 * and so on. (kernel.4.4)
 *
 * Base on:rtc-generic: RTC driver using the generic RTC abstraction
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

/*#define DEBUG*/
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <soc/xburst/reboot.h>

#include "rtc-ingenic.h"

#if defined(CONFIG_DEBUG_FS) && defined(DEBUG)
#include <linux/slab.h>
#include <linux/debugfs.h>
#endif

struct ingenic_rtc_device {
	struct rtc_device *rtc;
	struct device *dev;
	struct mutex  reg_mutex;
	bool pmic_vailed;	/*power manage ic is vailed*/
	void __iomem *reg_base;
	int irq;
	uint32_t lprs_pwon_ms;	/*long press power ms time on Wakeup pin*/
	uint32_t hr_assert_ms;	/*HIBERNATE Reset assert time n*125ms*/
	struct clk *rtc_gate;
	int rtc_clk_rate;
	unsigned int hwrsr;
	struct notifier_block restart_handler;
#if defined(CONFIG_DEBUG_FS) && defined(DEBUG)
	struct dentry	*debugfs;
#endif
#ifdef CONFIG_SUSPEND_TEST
	unsigned int sleep_count;
	unsigned int os_alarm_time;
	unsigned int save_rtccr;
#endif
};

static struct ingenic_rtc_device *m_rtc;

static bool ingenic_rtc_clk_disconnect = false;

static int ingenic_rtc_read(struct ingenic_rtc_device* rtc, int reg)
{
	return readl(rtc->reg_base + reg);
}

static int ingenic_rtc_write(struct ingenic_rtc_device* rtc, int reg, int val)
{
	int timeout = 1000000;

	mutex_lock(&rtc->reg_mutex);

	writel(WENR_WENPAT_WRITABLE, rtc->reg_base + RTC_WENR);

	while (!(readl(rtc->reg_base + RTC_WENR) & WENR_WEN) &&
			--timeout);
	if (!timeout) {
		dev_warn(rtc->dev, "wait rtc wenr timeout\n");
		mutex_unlock(&rtc->reg_mutex);
		return -EIO;
	}
	timeout = 1000000;
	while(!(readl(rtc->reg_base + RTC_RTCCR) & RTCCR_WRDY) &&
			--timeout);
	if (!timeout) {
		dev_warn(rtc->dev, "wait rtc write ready timeout 1 %x:%x (rtccr:%x)\n",
				reg, val,
				readl(rtc->reg_base + RTC_RTCCR));
		mutex_unlock(&rtc->reg_mutex);
		return -EIO;
	}

	writel(val, rtc->reg_base + reg);

	timeout = 1000000;
	while(!(readl(rtc->reg_base + RTC_RTCCR) & RTCCR_WRDY) &&
			--timeout);
	if (!timeout) {
		dev_warn(rtc->dev, "wait rtc write ready timeout 2 %x:%x (rtccr:%x)\n",
				reg, val,
				readl(rtc->reg_base + RTC_RTCCR));
		mutex_unlock(&rtc->reg_mutex);
		return -EIO;
	}

	mutex_unlock(&rtc->reg_mutex);
	return 0;
}

static int ingenic_rtc_prepare_enable(struct ingenic_rtc_device *rtc)
{
	if (ingenic_rtc_read(rtc, RTC_HSPR) != HSPR_RTCV) { /*rtc power on*/
		unsigned int rtccr, tmp;
		int ret;
		dev_info(rtc->dev, "usb rtc clk, system power on first time\n");

		/*Try use rtc clk*/
		rtccr = RTCCR_RTCE;
		//writel(rtccr, rtc->reg_base + RTC_RTCCR);
		ingenic_rtc_clk_disconnect = false;
		ret = ingenic_rtc_write(rtc, RTC_RTCGR, (rtc->rtc_clk_rate - 1) & RTCGR_NC1HZ_MASK);
		if (!ret && (ingenic_rtc_read(rtc, RTC_RTCGR) & RTCGR_NC1HZ_MASK) ==
				(rtc->rtc_clk_rate - 1))
			goto rtc_power_on;

		/*Try use ext clk / 512*/
		dev_info(rtc->dev, "use (extern clk)/512\n");
		rtccr = RTCCR_SELEXC | RTCCR_RTCE;
		writel(rtccr, rtc->reg_base + RTC_RTCCR);
		rtc->rtc_clk_rate = 24000000/512;
		ingenic_rtc_clk_disconnect = true;
		ret = ingenic_rtc_write(rtc, RTC_RTCGR, (rtc->rtc_clk_rate - 1) & RTCGR_NC1HZ_MASK);
		if (ret || (ingenic_rtc_read(rtc, RTC_RTCGR) & RTCGR_NC1HZ_MASK) !=
				(rtc->rtc_clk_rate - 1))
			return -ENODEV;
rtc_power_on:
		ingenic_rtc_write(rtc, RTC_HWFCR, HWFCR_WAIT_TIME(rtc->lprs_pwon_ms, rtc->rtc_clk_rate));
		ingenic_rtc_write(rtc, RTC_HRCR, HRCR_WAIT_TIME(rtc->hr_assert_ms, rtc->rtc_clk_rate));
		ingenic_rtc_write(rtc, RTC_RTCSR, 0);
		ingenic_rtc_write(rtc, RTC_RTCSAR, 0);
		tmp = ingenic_rtc_read(rtc, RTC_WKUPPINCR);
		tmp &= ~(WKUPPINCR_P_JUD_EN);
		ingenic_rtc_write(rtc, RTC_WKUPPINCR, tmp);
		ingenic_rtc_write(rtc, RTC_RTCCR, rtccr);
		ingenic_rtc_write(rtc, RTC_HSPR, HSPR_RTCV);
	} else {
		int temp, temp_new;

		temp = HWFCR_WAIT_TIME(rtc->lprs_pwon_ms, rtc->rtc_clk_rate);
		if (temp != (ingenic_rtc_read(rtc, RTC_HWFCR) & HWFCR_MASK))
			ingenic_rtc_write(rtc, RTC_HWFCR, temp);

		temp = HRCR_WAIT_TIME(rtc->hr_assert_ms, rtc->rtc_clk_rate);
		if (temp != (ingenic_rtc_read(rtc, RTC_HRCR) & HRCR_MASK))
			ingenic_rtc_write(rtc, RTC_HRCR, temp);

		temp = ingenic_rtc_read(rtc, RTC_WKUPPINCR);
		temp_new = temp & (~WKUPPINCR_P_JUD_EN);
		if (temp_new != temp)
			ingenic_rtc_write(rtc, RTC_WKUPPINCR, temp_new);

		rtc->hwrsr = ingenic_rtc_read(rtc, RTC_HWRSR);
	}
	ingenic_rtc_write(rtc, RTC_HWRSR, 0x0);
	ingenic_rtc_write(rtc, RTC_RTCGR, RTCGR_LOCK | ingenic_rtc_read(rtc, RTC_RTCGR));
	ingenic_rtc_write(rtc, RTC_HWCR, HWCR_EALM | EPDET_ENABLE);

	return 0;
}

static irqreturn_t ingenic_rtc_interrupt_thread_handler(int irq, void *dev_id)
{
	struct ingenic_rtc_device *rtc = dev_id;
	unsigned int rtccr;

	mutex_lock(&rtc->rtc->ops_lock);
	rtccr = ingenic_rtc_read(rtc, RTC_RTCCR);
	rtccr &= ~(RTCCR_1HZ | RTCCR_AF);
	ingenic_rtc_write(rtc, RTC_RTCCR, rtccr);
	mutex_unlock(&rtc->rtc->ops_lock);

	rtc_update_irq(rtc->rtc, 0/*uncare*/, 0/*uncare*/);

	return IRQ_HANDLED;
}

static int ingenic_rtc_get_time(struct device *dev, struct rtc_time *tm)
{
	struct ingenic_rtc_device *rtc = dev_get_drvdata(dev);
	uint32_t secs, secs2;
	int timeout = 10;

	/* If the seconds register is read while it is updated, it can contain a
	 * bogus value. This can be avoided by making sure that two consecutive
	 * reads have the same value.
	 */
	secs = readl(rtc->reg_base + RTC_RTCSR);
	secs = readl(rtc->reg_base + RTC_RTCSR);

	while (secs != secs2 && --timeout) {
		secs = secs2;
		secs2 = readl(rtc->reg_base + RTC_RTCSR);
	}

	if (timeout == 0)
		return -EIO;

	rtc_time_to_tm(secs, tm);

	return rtc_valid_tm(tm);
}

static int ingenic_rtc_set_mmss(struct device *dev, unsigned long secs)
{
	struct ingenic_rtc_device *rtc = dev_get_drvdata(dev);
	return ingenic_rtc_write(rtc, RTC_RTCSR, secs);
}

static int ingenic_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct ingenic_rtc_device *rtc = dev_get_drvdata(dev);
	unsigned int rtccr_new, rtccr;

	rtccr = ingenic_rtc_read(rtc, RTC_RTCCR);
	/*WARN_ON(!(enabled && (rtccr & (RTCCR_1HZIE|RTCCR_AIE))));*/

	if (!enabled) {
		rtccr_new = rtccr;
		if (!rtc->rtc->uie_rtctimer.enabled)
			rtccr_new &= ~(RTCCR_1HZIE);
		rtccr_new &= ~(RTCCR_AIE | RTCCR_AE);
		if (rtccr_new != rtccr)
			ingenic_rtc_write(rtc, RTC_RTCCR, rtccr_new);
	}
	pr_debug("=====> %s %d enabled %d, %x\n", __func__, __LINE__,
			enabled,
			ingenic_rtc_read(rtc, RTC_RTCCR));
	return 0;
}

static int ingenic_rtc_read_alarm(struct device * dev, struct rtc_wkalrm *alrm)
{
	struct ingenic_rtc_device *rtc = dev_get_drvdata(dev);
	uint32_t secs;
	uint32_t ctrl;

	secs = ingenic_rtc_read(rtc, RTC_RTCSAR);
	ctrl = ingenic_rtc_read(rtc, RTC_RTCCR);

	alrm->enabled = !!(ctrl & RTCCR_AIE);
	alrm->pending = !!(ctrl & RTCCR_AF);

	rtc_time_to_tm(secs, &alrm->time);

	return rtc_valid_tm(&alrm->time);
}

static int ingenic_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct ingenic_rtc_device *rtc = dev_get_drvdata(dev);
	struct timerqueue_node *next = timerqueue_getnext(&rtc->rtc->timerqueue);
	struct rtc_timer *timer;
	unsigned long secs;
	unsigned int rtccr_new, rtccr;
	int ret = 0;

	BUG_ON(!next);
	timer = container_of(next, struct rtc_timer, node);
	rtc_tm_to_time(&alrm->time, &secs);

	if (&rtc->rtc->uie_rtctimer == timer) {
		/*1HZ peroid interrupt*/
		rtccr = rtccr_new = ingenic_rtc_read(rtc, RTC_RTCCR);
		rtccr_new &= ~(RTCCR_AIE | RTCCR_AE);
		rtccr_new |= RTCCR_1HZIE;
		if (rtccr_new != rtccr)
			ret = ingenic_rtc_write(rtc, RTC_RTCCR, rtccr_new);
	} else {
		/*alarm interrupt*/
		ret = ingenic_rtc_write(rtc, RTC_RTCSAR, secs);
		if (!ret) {
			rtccr = rtccr_new = ingenic_rtc_read(rtc, RTC_RTCCR);
			rtccr_new &= ~(RTCCR_1HZIE);
			rtccr_new |= RTCCR_AIE | RTCCR_AE;
			if (rtccr_new != rtccr) {
				ret = ingenic_rtc_write(rtc, RTC_RTCCR, rtccr_new);
			}
		}
	}
	pr_debug("=====> %s %d %x:%x:%x\n", __func__, __LINE__,
			ingenic_rtc_read(rtc, RTC_RTCSR),
			ingenic_rtc_read(rtc, RTC_RTCSAR),
			ingenic_rtc_read(rtc, RTC_RTCCR));
	return ret;
}

static int ingenic_rtc_rtc_proc(struct device *dev, struct seq_file *seq)
{
	struct ingenic_rtc_device *rtc = dev_get_drvdata(dev);

	seq_printf(seq, "RTC regulator\t: 0x%08x\n",
			ingenic_rtc_read(rtc, RTC_RTCGR));
	seq_printf(seq, "update_IRQ\t: %s\n",
			(ingenic_rtc_read(rtc, RTC_RTCCR) & RTCCR_1HZIE) ? "yes" : "no");

	if (rtc->hwrsr & HWRSR_APD)
		seq_printf(seq, "Accident power down\n");
	if (rtc->hwrsr & HWRSR_HR)
		seq_printf(seq, "Hibernate Reset: \n");
	if (rtc->hwrsr & HWRSR_PIN)
		seq_printf(seq, "\tWakeup Pin wakeup system\n");
	if (rtc->hwrsr & HWRSR_ALM)
		seq_printf(seq, "\tAlarm wakeup system\n");
	if (rtc->hwrsr & HWRSR_PPR)
		seq_printf(seq, "PAD PIN Reset\n");
	return 0;
}

static const struct rtc_class_ops ingenic_rtc_ops = {
	.read_time = ingenic_rtc_get_time,
	.set_mmss = ingenic_rtc_set_mmss,
	.alarm_irq_enable = ingenic_rtc_alarm_irq_enable,
	.read_alarm = ingenic_rtc_read_alarm,
	.set_alarm = ingenic_rtc_set_alarm,
	.proc = ingenic_rtc_rtc_proc,
};

#define INGENIC_RTC_DEFUALT_PWON_PRESS_MS (2000)
#define INGENIC_RTC_DEFUALT_HIBERNATE_RESET_ASSERT_MS (60) /*copy from old rtc driver*/

static void ingenic_rtc_hibernate(void)
{
	struct ingenic_rtc_device *rtc = m_rtc;

	mutex_lock(&rtc->rtc->ops_lock);
	ingenic_rtc_write(rtc, RTC_HCR, HCR_PD);
	mdelay(2000);
	mutex_unlock(&rtc->rtc->ops_lock);
	pr_err("RTC hibernate has been run, but extern power not down RTC_HCR(%x)\n",
			ingenic_rtc_read(rtc, RTC_HCR));
	while(1);
}

static int ingenic_rtc_reset_handler(struct notifier_block *this, unsigned long mode,
		void *cmd)
{
	struct ingenic_rtc_device *rtc = m_rtc;
	uint32_t rtc_rtcsr,rtc_rtccr;

	/*
	 * Use setup params "reboot=h" (mode == REBOOT_HARD) ,
	 * enable the hibernate reset function
	 */
	if (mode != REBOOT_HARD || (cmd && (!strcmp(cmd, REBOOT_CMD_RECOVERY) ||
					!strcmp(cmd, REBOOT_CMD_SOFTBURN)))) {
		pr_debug("%s %d mode %lu cmd %s\n", __func__, __LINE__, mode, cmd ? (char*)cmd : "null");
		return NOTIFY_DONE;
	}
	pr_info("hibernate reset");
	mutex_lock(&rtc->rtc->ops_lock);
	rtc_rtcsr = ingenic_rtc_read(rtc, RTC_RTCSR);
	ingenic_rtc_write(rtc, RTC_RTCSAR, rtc_rtcsr + 5);	/*5s delay*/
	rtc_rtccr = ingenic_rtc_read(rtc, RTC_RTCCR);
	rtc_rtccr &= ~RTCCR_AF;
	rtc_rtccr |= RTCCR_AIE | RTCCR_AE;
	ingenic_rtc_write(rtc, RTC_RTCCR, rtc_rtccr);
	ingenic_rtc_write(rtc, RTC_HWRSR, 0x0);
	ingenic_rtc_write(rtc, RTC_HWCR, HWCR_EALM|EPDET_ENABLE);
	ingenic_rtc_write(rtc, RTC_HCR, HCR_PD);
	while(1) {
		mdelay(200);
		printk("%s:We should NOT come here.%08x\n",
				__func__, ingenic_rtc_read(rtc, RTC_HCR));
	}
	mutex_unlock(&rtc->rtc->ops_lock);
	return NOTIFY_STOP;
}

#if defined(CONFIG_DEBUG_FS) && defined(DEBUG)
static ssize_t ingenic_rtc_show_regs(struct file *file, char __user *user_buf,
				size_t count, loff_t *ppos)
{
	struct ingenic_rtc_device *rtc = file->private_data;
	char *buf;
	int len = 0;
	ssize_t ret;

#define REGS_BUFSIZE	1024
	buf = kzalloc(REGS_BUFSIZE, GFP_KERNEL);
	if (!buf)
		return 0;
	len += snprintf(buf + len, REGS_BUFSIZE - len, "rtc register: %p\n", rtc->reg_base);
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_RTCCR    : 0x%08x\n", ingenic_rtc_read(rtc, RTC_RTCCR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_RTCSR    : 0x%08x\n", ingenic_rtc_read(rtc, RTC_RTCSR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_RTCSAR   : 0x%08x\n", ingenic_rtc_read(rtc, RTC_RTCSAR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_RTCGR    : 0x%08x\n", ingenic_rtc_read(rtc, RTC_RTCGR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_HCR      : 0x%08x\n", ingenic_rtc_read(rtc, RTC_HCR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_HWFCR    : 0x%08x\n", ingenic_rtc_read(rtc, RTC_HWFCR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_HRCR     : 0x%08x\n", ingenic_rtc_read(rtc, RTC_HRCR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_HWCR     : 0x%08x\n", ingenic_rtc_read(rtc, RTC_HWCR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_HWRSR    : 0x%08x\n", ingenic_rtc_read(rtc, RTC_HWRSR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_HSPR     : 0x%08x\n", ingenic_rtc_read(rtc, RTC_HSPR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_WENR     : 0x%08x\n", ingenic_rtc_read(rtc, RTC_WENR));
	len += snprintf(buf + len, REGS_BUFSIZE - len, "RTC_WKUPPINCR: 0x%08x\n", ingenic_rtc_read(rtc, RTC_WKUPPINCR));
	ret =  simple_read_from_buffer(user_buf, count, ppos, buf, len);
	kfree(buf);
#undef REGS_BUFSIZE
	return ret;
}

static struct file_operations ingenic_rtc_reg_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = ingenic_rtc_show_regs,
	.llseek = default_llseek,
};
#endif

static int __init ingenic_rtc_probe(struct platform_device *pdev)
{
	struct ingenic_rtc_device *ingenic_rtc;
	struct resource *res;
	struct clk *rtc_clk;
	struct clk *rtc_gate;
	int ret;

	ingenic_rtc = devm_kzalloc(&pdev->dev, sizeof(struct ingenic_rtc_device), GFP_KERNEL);
	if (!ingenic_rtc)
		return -ENOMEM;

	ingenic_rtc->pmic_vailed = of_property_read_bool(pdev->dev.of_node,
			"system-power-controller");
	if (ingenic_rtc->pmic_vailed) {
		ret = of_property_read_u32(pdev->dev.of_node,
				"power-on-press-ms",
				&ingenic_rtc->lprs_pwon_ms);
		if (ret)
			ingenic_rtc->lprs_pwon_ms = INGENIC_RTC_DEFUALT_PWON_PRESS_MS;
		ingenic_rtc->hr_assert_ms = INGENIC_RTC_DEFUALT_HIBERNATE_RESET_ASSERT_MS;
		if (!pm_power_off) {
			m_rtc = ingenic_rtc;
			pm_power_off = ingenic_rtc_hibernate;
		}
	}
	ingenic_rtc->dev = &pdev->dev;
	mutex_init(&ingenic_rtc->reg_mutex);

	rtc_gate = clk_get(&pdev->dev, "gate_rtc");
	if (IS_ERR(rtc_gate))
		return PTR_ERR(rtc_gate);
	clk_prepare_enable(rtc_gate);
	clk_put(rtc_gate);

	rtc_clk = clk_get(&pdev->dev, "rtc");
	if (IS_ERR_OR_NULL(rtc_clk) ||
			(ingenic_rtc->rtc_clk_rate = clk_get_rate(rtc_clk)) <= 0) {
		dev_warn(&pdev->dev, "Impossible: can not find fin_rtc(use 32768)\n");
		ingenic_rtc->rtc_clk_rate = 32768;
	}
	if (!IS_ERR_OR_NULL(rtc_clk))
		clk_put(rtc_clk);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOMEM;

	ingenic_rtc->reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ingenic_rtc->reg_base))
		return PTR_ERR(ingenic_rtc->reg_base);

	ingenic_rtc->irq = platform_get_irq(pdev, 0);
	if (ingenic_rtc->irq < 0)
		return ingenic_rtc->irq;

	ret = ingenic_rtc_prepare_enable(ingenic_rtc);
	if (ret)
		return ret;

	device_init_wakeup(&pdev->dev, true);
	platform_set_drvdata(pdev, ingenic_rtc);

	ingenic_rtc->rtc = devm_rtc_device_register(&pdev->dev, "rtc-ingenic",
					&ingenic_rtc_ops, THIS_MODULE);
	if (IS_ERR(ingenic_rtc->rtc))
		return PTR_ERR(ingenic_rtc->rtc);

	ret = devm_request_threaded_irq(&pdev->dev, ingenic_rtc->irq, NULL,
			ingenic_rtc_interrupt_thread_handler,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT,
			"rtc 1HZ and alarm",
			(void *)ingenic_rtc);
	if (ret)
		return ret;

	ingenic_rtc->restart_handler.notifier_call = ingenic_rtc_reset_handler;
	ingenic_rtc->restart_handler.priority = RTC_HIBERNATE_RESET_PROR;
	ret = register_restart_handler(&ingenic_rtc->restart_handler);
	if (ret)
		dev_warn(&pdev->dev,
				 "cannot register rtc restart\
				 handler (err=%d)\n", ret);


#if defined(CONFIG_DEBUG_FS) && defined(DEBUG)
	ingenic_rtc->debugfs = debugfs_create_file("rtc_reg", 0x444, NULL,
			ingenic_rtc, &ingenic_rtc_reg_ops);
#endif
	return 0;
}

static void __exit ingenic_rtc_remove(struct platform_device *pdev)
{
#if defined(CONFIG_DEBUG_FS) && defined(DEBUG)
	struct ingenic_rtc_device *ingenic_rtc = platform_get_drvdata(pdev);
	if (ingenic_rtc->debugfs)
		debugfs_remove(ingenic_rtc->debugfs);
#endif
}

static const struct of_device_id ingenic_rtc_of_match[] = {
	{ .compatible = "ingenic,rtc", .data = NULL, },
	{},
};
MODULE_DEVICE_TABLE(of, ingenic_rtc_of_match);

#ifdef CONFIG_PM
static int ingenic_rtc_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct ingenic_rtc_device *rtc = platform_get_drvdata(pdev);
#ifdef CONFIG_SUSPEND_TEST
	unsigned int val;
	unsigned int test_alarm_time, sr_time;

	val = ingenic_rtc_read(rtc, RTC_RTCCR);
	if(val & RTCCR_AE) {
		rtc->save_rtccr = val;
		rtc->os_alarm_time = ingenic_rtc_read(rtc, RTC_RTCSAR);
	}
	val |= RTCCR_AIE | RTCCR_AE;
	ingenic_rtc_write(rtc, RTC_RTCCR, val);

	sr_time = ingenic_rtc_read(rtc, RTC_RTCSR);
	test_alarm_time = sr_time + CONFIG_SUSPEND_ALARM_TIME;
	if(rtc->os_alarm_time && rtc->os_alarm_time > sr_time \
	   && rtc->os_alarm_time < test_alarm_time)
		test_alarm_time =  rtc->os_alarm_time;
	ingenic_rtc_write(rtc, RTC_RTCSAR, test_alarm_time);

	printk("-------suspend count = %d\n", rtc->sleep_count++);
#endif
	if (device_may_wakeup(&pdev->dev))
		enable_irq_wake(rtc->irq);
	return 0;
}

static int ingenic_rtc_resume(struct platform_device *pdev)
{
	struct ingenic_rtc_device *rtc = platform_get_drvdata(pdev);
#ifdef CONFIG_SUSPEND_TEST
	if(rtc->save_rtccr & RTCCR_AE) {
		ingenic_rtc_write(rtc, RTC_RTCSAR, rtc->os_alarm_time);
		ingenic_rtc_write(rtc, RTC_RTCCR, rtc->save_rtccr);
		rtc->os_alarm_time = 0;
		rtc->save_rtccr = 0;
	} else {
		unsigned int val;
		val = ingenic_rtc_read(rtc, RTC_RTCCR);
		val &= ~ (RTCCR_AF |RTCCR_AIE | RTCCR_AE);
		ingenic_rtc_write(rtc, RTC_RTCCR, val);
	}
#endif
	if (device_may_wakeup(&pdev->dev))
		disable_irq_wake(rtc->irq);
	return 0;
}

#else
#define ingenic_rtc_suspend NULL
#define ingenic_rtc_resume NULL
#endif

static struct platform_driver ingenic_rtc_driver = {
	.driver	= {
		.name = "rtc-ingenic",
		.of_match_table = ingenic_rtc_of_match,
	},
	.remove		= __exit_p(ingenic_rtc_remove),
	.suspend	= ingenic_rtc_suspend,
	.resume		= ingenic_rtc_resume,
};
module_platform_driver_probe(ingenic_rtc_driver, ingenic_rtc_probe);

MODULE_AUTHOR("Cli <chen.li@ingenic.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ingenic RTC driver");
MODULE_ALIAS("platform:rtc-ingenic");

// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha/BCM4775 GNSS power + pinctrl control driver.
 * Ported from LineageOS android_kernel_xiaomi_sm8250
 * drivers/airoha/airoha_gps_driver.c for mainline 6.18.
 *
 * Provides /dev/airoha_gps.  Userspace writes:
 *   OPEN  -> LDO on
 *   CLOSE -> LDO off
 *   DF    -> select "gps_enable_active" pinctrl state
 *   DI    -> select "gps_enable_suspend" pinctrl state
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define AIROHA_GPS_NAME "airoha_gps"

struct airoha_gps {
	dev_t devt;
	struct class *class;
	struct cdev cdev;
	int ldo_gpio;
	struct pinctrl *pctrl;
	struct pinctrl_state *active;
	struct pinctrl_state *idle;
	struct semaphore lock;
};

static int airoha_gps_open(struct inode *inode, struct file *file)
{
	struct airoha_gps *gps = container_of(inode->i_cdev, struct airoha_gps, cdev);
	file->private_data = gps;
	return 0;
}

static int airoha_gps_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t airoha_gps_write(struct file *file, const char __user *user,
				size_t len, loff_t *off)
{
	struct airoha_gps *gps = file->private_data;
	char buf[32] = {0};
	int ret = 0;

	if (len > sizeof(buf) - 1)
		len = sizeof(buf) - 1;
	if (copy_from_user(buf, user, len))
		return -EFAULT;

	if (down_interruptible(&gps->lock))
		return -ERESTARTSYS;

	if (strstr(buf, "OPEN"))
		gpio_direction_output(gps->ldo_gpio, 1);
	else if (strstr(buf, "CLOSE"))
		gpio_direction_output(gps->ldo_gpio, 0);
	else if (strstr(buf, "DF") && gps->active)
		ret = pinctrl_select_state(gps->pctrl, gps->active);
	else if (strstr(buf, "DI") && gps->idle)
		ret = pinctrl_select_state(gps->pctrl, gps->idle);

	up(&gps->lock);
	pr_info("airoha_gps: cmd=%s ldo=%d\n", buf, gpio_get_value(gps->ldo_gpio));
	return ret ? ret : (ssize_t)len;
}

static ssize_t airoha_gps_read(struct file *file, char __user *user,
			       size_t len, loff_t *off)
{
	const char msg[] = "INTERRUPT\n";
	size_t n = min(len, sizeof(msg) - 1);
	if (copy_to_user(user, msg, n))
		return -EFAULT;
	return n;
}

static const struct file_operations airoha_gps_fops = {
	.owner = THIS_MODULE,
	.open = airoha_gps_open,
	.release = airoha_gps_release,
	.read = airoha_gps_read,
	.write = airoha_gps_write,
};

static int airoha_gps_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_gps *gps;
	int ret;

	gps = devm_kzalloc(dev, sizeof(*gps), GFP_KERNEL);
	if (!gps)
		return -ENOMEM;
	sema_init(&gps->lock, 1);

	gps->ldo_gpio = of_get_named_gpio(dev->of_node, "gps-ldo", 0);
	if (!gpio_is_valid(gps->ldo_gpio))
		return dev_err_probe(dev, -ENOENT, "no gps-ldo GPIO\n");
	ret = devm_gpio_request_one(dev, gps->ldo_gpio, GPIOD_OUT_LOW, "gps-ldo");
	if (ret)
		return ret;

	gps->pctrl = devm_pinctrl_get(dev);
	if (IS_ERR(gps->pctrl)) {
		dev_warn(dev, "no pinctrl: %ld\n", PTR_ERR(gps->pctrl));
		gps->pctrl = NULL;
	} else {
		gps->active = pinctrl_lookup_state(gps->pctrl, "gps_enable_active");
		if (IS_ERR(gps->active)) {
			dev_warn(dev, "no gps_enable_active state\n");
			gps->active = NULL;
		}
		gps->idle = pinctrl_lookup_state(gps->pctrl, "gps_enable_suspend");
		if (IS_ERR(gps->idle)) {
			dev_warn(dev, "no gps_enable_suspend state\n");
			gps->idle = NULL;
		}
	}

	ret = alloc_chrdev_region(&gps->devt, 0, 1, AIROHA_GPS_NAME);
	if (ret)
		return ret;

	cdev_init(&gps->cdev, &airoha_gps_fops);
	gps->cdev.owner = THIS_MODULE;
	ret = cdev_add(&gps->cdev, gps->devt, 1);
	if (ret)
		goto err_region;

	gps->class = class_create(AIROHA_GPS_NAME);
	if (IS_ERR(gps->class)) {
		ret = PTR_ERR(gps->class);
		goto err_cdev;
	}
	if (!device_create(gps->class, NULL, gps->devt, gps, AIROHA_GPS_NAME)) {
		ret = -ENODEV;
		goto err_class;
	}

	platform_set_drvdata(pdev, gps);
	dev_info(dev, "ready: /dev/%s (LDO off, pinctrl %s)\n",
		 AIROHA_GPS_NAME, gps->pctrl ? "ok" : "missing");
	return 0;

err_class:
	class_destroy(gps->class);
err_cdev:
	cdev_del(&gps->cdev);
err_region:
	unregister_chrdev_region(gps->devt, 1);
	return ret;
}

static void airoha_gps_remove(struct platform_device *pdev)
{
	struct airoha_gps *gps = platform_get_drvdata(pdev);
	device_destroy(gps->class, gps->devt);
	class_destroy(gps->class);
	cdev_del(&gps->cdev);
	unregister_chrdev_region(gps->devt, 1);
}

static const struct of_device_id airoha_gps_of_match[] = {
	{ .compatible = "bcm4775" },
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_gps_of_match);

static struct platform_driver airoha_gps_driver = {
	.probe = airoha_gps_probe,
	.remove = airoha_gps_remove,
	.driver = {
		.name = "airoha_gps",
		.of_match_table = airoha_gps_of_match,
	},
};
module_platform_driver(airoha_gps_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha/BCM4775 GNSS power and pinctrl control");

/* drivers/misc/touch_wake.c
*
* Copyright 2011  Ezekeel
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* --------------------------------------------------------------------------------------
*
* Fixed issues with random misbehaving when powering off device via Powerkey
*
* Bumped version to 1.1
*
*                                         Jean-Pierre Rasquin <yank555.lu@gmail.com>
*
* --------------------------------------------------------------------------------------
*
* Fixed proxy detection, only disable touchwake when proxy sensor is active and
* proximity is really detected
*
* Bumped version to 1.1a
*
*  		                           Jean-Pierre Rasquin <yank555.lu@gmail.com>
*
* --------------------------------------------------------------------------------------
*
* Modified and improved.
*
*
*  		                           Voronin Igor <Voron.exe@gmail.com>
*/

#include <linux/init.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/gp2a.h>
#include <linux/touch_wake.h>
#include <linux/workqueue.h>
#include <linux/earlysuspend.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/wakelock.h>
#include <linux/input.h>
#include <linux/kmod.h>

extern void touchscreen_enable(void);
extern void touchscreen_disable(void);

extern void enable_for_touchwake(void);
extern void restore_for_touchwake(void);

static bool touchwake_enabled = false;
static bool touch_disabled = false;
static bool device_suspended = false;

static bool tw_debug_on = false;

static bool timed_out = true;

enum
{
	TOUCH_WAKE_BIT = 0x01,
	PROXIMITY_WAKE_BIT = 0x02,
	LONGTOUCH_SLEEP_WAKE_BIT = 0x04,
	PROXIMITY_TOUCH_BIT = 0x08
};

static unsigned int mode = TOUCH_WAKE_BIT | PROXIMITY_WAKE_BIT | LONGTOUCH_SLEEP_WAKE_BIT;

static bool keep_wake_lock = false;
// Keep device awakened, may be needed on some devices but not on Galaxy Nexus. Consumes power.

static unsigned int touchoff_delay = (30 * 1000);

static bool prox_near = false;
static bool first_touch = false;

static void touchwake_touchoff(struct work_struct * touchoff_work);
static DECLARE_DELAYED_WORK(touchoff_work, touchwake_touchoff);
static void press_wakeupkey(struct work_struct * presswakeupkey_work);
static DECLARE_WORK(presswakeupkey_work, press_wakeupkey);
static void press_sleepkey(struct work_struct * presssleepkey_work);
static DECLARE_WORK(presssleepkey_work, press_sleepkey);
static DEFINE_MUTEX(lock);
static void presskey(unsigned int key);


static struct input_dev * powerkey_device;
static struct wake_lock touchwake_wake_lock;
static struct timeval last_powerkeypress;
static struct timeval touch_begin;

#define TOUCHWAKE_VERSION "1.1a"
#define TIME_LONGPRESS 500
#define TIME_LONGTOUCH 300
#define POWERPRESS_DELAY 50

static void touchwake_disable_touch(void)
{
	tw_debug("[TOUCHWAKE]: Disable touch controls\n");

	touchscreen_disable();
	touch_disabled = true;
	return;
}

static void touchwake_enable_touch(void)
{
	tw_debug("[TOUCHWAKE] Enable touch controls\n");

	touchscreen_enable();
	touch_disabled = false;
	return;
}

static void touchwake_early_suspend(struct early_suspend * h)
{
	tw_debug("[TOUCHWAKE] Enter early suspend\n");

	if (touchwake_enabled) {

		if (timed_out && (mode & TOUCH_WAKE_BIT)) {
			tw_debug("[TOUCHWAKE] Early suspend - keep touch enabled indefinately\n");

			if (keep_wake_lock)
				wake_lock(&touchwake_wake_lock);
		}
		else {
			tw_debug("[TOUCHWAKE] Early suspend - disable touch immediately (indefinate mode)\n");

			touchwake_disable_touch();
		}


		if ((mode & PROXIMITY_WAKE_BIT)) {
			enable_for_touchwake();
		}
	}
	else {
		tw_debug("[TOUCHWAKE] Early suspend - disable touch immediately (TouchWake disabled)\n");

		touchwake_disable_touch();
	}

	device_suspended = true;
	return;
}

static void touchwake_late_resume(struct early_suspend * h)
{
	tw_debug("[TOUCHWAKE] Enter late resume\n");

	cancel_delayed_work(&touchoff_work);
	flush_scheduled_work();

	if (keep_wake_lock)
		wake_unlock(&touchwake_wake_lock);

	if (touch_disabled)
		touchwake_enable_touch();

	if ((mode & PROXIMITY_WAKE_BIT))
		restore_for_touchwake();

	timed_out = true;
	device_suspended = false;
	return;
}

static struct early_suspend touchwake_suspend_data =
{
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = touchwake_early_suspend,
	.resume = touchwake_late_resume,
};

static void touchwake_touchoff(struct work_struct * touchoff_work)
{
	touchwake_disable_touch();
	if (keep_wake_lock)
		wake_unlock(&touchwake_wake_lock);
	return;
}

static void presskey(unsigned int key)
{
	mutex_lock(&lock);
	tw_debug("[TOUCHWAKE] Emulating %d key press\n", key);

	input_event(powerkey_device, EV_KEY, key, 1);
	input_event(powerkey_device, EV_SYN, 0, 0);
	msleep(POWERPRESS_DELAY);

	tw_debug("[TOUCHWAKE] Emulating %d key release\n", key);

	input_event(powerkey_device, EV_KEY, key, 0);
	input_event(powerkey_device, EV_SYN, 0, 0);
	msleep(POWERPRESS_DELAY);

	mutex_unlock(&lock);

	return;
}

static void press_wakeupkey(struct work_struct * presswakeupkey_work)
{
	presskey(KEY_WAKEUP);
	return;
}

static void press_sleepkey(struct work_struct * presssleepkey_work)
{
	presskey(KEY_SLEEP);
	return;
}

static ssize_t touchwake_status_read(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%u\n", (touchwake_enabled ? 1 : 0));
}

static ssize_t touchwake_status_write(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) == 1) {
		pr_devel("%s: %u \n", __FUNCTION__, data);

		if (data == 1) {
			tw_debug("[TOUCHWAKE] %s: TOUCHWAKE function enabled\n", __FUNCTION__);

			touchwake_enabled = true;
		}
		else if (data == 0) {
			tw_debug("[TOUCHWAKE] %s: TOUCHWAKE function disabled\n", __FUNCTION__);

			touchwake_enabled = false;
		}
		else {
			tw_debug("[TOUCHWAKE] %s: invalid input range %u\n", __FUNCTION__, data);
		}
	}
	else {
		tw_debug("[TOUCHWAKE] %s: invalid input\n", __FUNCTION__);

	}

	return size;
}

static ssize_t touchwake_delay_read(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%u\n", mode * 1000);
}

static ssize_t touchwake_delay_write(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) == 1) {
		mode = data / 1000;
		tw_debug("[TOUCHWAKE] Mode set to %u\n", mode);

	}
	else {
		tw_debug("[TOUCHWAKE] %s: invalid input\n", __FUNCTION__);

	}

	return size;
}

int get_touchoff_delay()
{
	return touchoff_delay;
}
EXPORT_SYMBOL(get_touchoff_delay);

static ssize_t touchwake_version(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%s\n", TOUCHWAKE_VERSION);
}

static ssize_t touchwake_debug_read(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%u\n", (tw_debug_on ? 1 : 0));
}

static ssize_t touchwake_debug_write(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) == 1) {
		pr_devel("%s: %u \n", __FUNCTION__, data);

		if (data == 1) {
			tw_debug("[TOUCHWAKE] %s: tw_debug function enabled\n", __FUNCTION__);

			tw_debug_on = true;
		}
		else if (data == 0) {
			tw_debug("[TOUCHWAKE] %s: tw_debug function disabled\n", __FUNCTION__);

			tw_debug_on = false;
		}
		else {
			tw_debug("[TOUCHWAKE] %s: invalid input range %u\n", __FUNCTION__, data);
		}
	}
	else {
		tw_debug("[TOUCHWAKE] %s: invalid input\n", __FUNCTION__);

	}

	return size;
}

static DEVICE_ATTR(enabled, S_IRUGO | S_IWUGO, touchwake_status_read, touchwake_status_write);
static DEVICE_ATTR(delay, S_IRUGO | S_IWUGO, touchwake_delay_read, touchwake_delay_write);
static DEVICE_ATTR(version, S_IRUGO, touchwake_version, NULL);
static DEVICE_ATTR(debug, S_IRUGO, touchwake_debug_read, touchwake_debug_write);

static struct attribute *touchwake_notification_attributes[] =
{
	&dev_attr_enabled.attr,
	&dev_attr_delay.attr,
	&dev_attr_version.attr,
	&dev_attr_debug.attr,
	NULL
};

static struct attribute_group touchwake_notification_group =
{
	.attrs = touchwake_notification_attributes,
};

static struct miscdevice touchwake_device =
{
	.minor = MISC_DYNAMIC_MINOR,
	.name = "touchwake",
};

void proximity_detected(void)
{
	prox_near = true;
	tw_debug("[TOUCHWAKE] Proximity near event\n");

	return;
}
EXPORT_SYMBOL(proximity_detected);

void proximity_off(void)
{

	tw_debug("[TOUCHWAKE] Proximity far event\n");

	if (likely(touchwake_enabled) && prox_near && (mode & PROXIMITY_WAKE_BIT) && device_suspended)
	{
		tw_debug("[TOUCHWAKE] Waking by proximitor\n");

		device_suspended = false;
		schedule_work(&presswakeupkey_work);
	}

	prox_near = false;
	return;
}
EXPORT_SYMBOL(proximity_off);

void powerkey_pressed(void)
{
	tw_debug("[TOUCHWAKE] Powerkey pressed\n");

	do_gettimeofday(&last_powerkeypress);
	//timed_out = false; // Yank555 : consider user is indeed turning off the device

	return;
}
EXPORT_SYMBOL(powerkey_pressed);

void powerkey_released(void)
{
	tw_debug("[TOUCHWAKE] Powerkey released\n");

	struct timeval now;
	int time_pressed;

	do_gettimeofday(&now);
	time_pressed = (now.tv_sec - last_powerkeypress.tv_sec) * MSEC_PER_SEC +
		(now.tv_usec - last_powerkeypress.tv_usec) / USEC_PER_MSEC;

	if (unlikely(time_pressed > TIME_LONGPRESS || device_suspended)) {
		timed_out = true; // Yank555 : OK, user is not turning off device, but long-pressing Powerkey, or turing on device, so back to normal
		tw_debug("[TOUCHWAKE] Powerkey longpress detected released\n");

	}
	else {
		tw_debug("[TOUCHWAKE] Device shortpress detected released\n");

	}

	return;
}
EXPORT_SYMBOL(powerkey_released);

void touch_press(bool up)
{
	tw_debug("[TOUCHWAKE] Touch event! Up = %d\n", up);

	if (likely(touchwake_enabled) && (mode & TOUCH_WAKE_BIT)
		&& (prox_near || !(mode & PROXIMITY_TOUCH_BIT))) {

		if (unlikely(device_suspended)) {
			device_suspended = false;
			tw_debug("[TOUCHWAKE] Got toch in suspended, awakening, Up = %d\n", up);

			if (!up && (mode & LONGTOUCH_SLEEP_WAKE_BIT))
				first_touch = true;
			do_gettimeofday(&touch_begin);
			schedule_work(&presswakeupkey_work);
		}
		else if (up && first_touch) {
			first_touch = false;

			struct timeval now;
			int time_pressed;

			do_gettimeofday(&now);
			time_pressed = (now.tv_sec - touch_begin.tv_sec) * MSEC_PER_SEC +
				(now.tv_usec - touch_begin.tv_usec) / USEC_PER_MSEC;

			if (time_pressed > TIME_LONGTOUCH)
			{
				tw_debug("[TOUCHWAKE] Got long first touch up, resleeping, touchtime = %d\n", time_pressed);

				schedule_work(&presssleepkey_work);
			}
			else {
				tw_debug("[TOUCHWAKE] Got short first touch up, touchtime = %d\n", time_pressed);

			}
		}
	}

	return;
}
EXPORT_SYMBOL(touch_press);


void set_powerkeydev(struct input_dev * input_device)
{
	tw_debug("[TOUCHWAKE] Powerkey device set to: %p\n", input_device);

	powerkey_device = input_device;
	return;
}
EXPORT_SYMBOL(set_powerkeydev);

bool device_is_suspended(void)
{
	return device_suspended;
}
EXPORT_SYMBOL(device_is_suspended);

static int __init touchwake_control_init(void)
{
	int ret;

	pr_info("%s misc_register(%s)\n", __FUNCTION__, touchwake_device.name);
	ret = misc_register(&touchwake_device);

	if (ret) {
		pr_err("%s misc_register(%s) fail\n", __FUNCTION__, touchwake_device.name);

		return 1;
	}

	if (sysfs_create_group(&touchwake_device.this_device->kobj, &touchwake_notification_group) < 0) {
		pr_err("%s sysfs_create_group fail\n", __FUNCTION__);
		pr_err("Failed to create sysfs group for device (%s)!\n", touchwake_device.name);
	}

	register_early_suspend(&touchwake_suspend_data);
	do_gettimeofday(&last_powerkeypress);

	if (keep_wake_lock)
		wake_lock_init(&touchwake_wake_lock, WAKE_LOCK_SUSPEND, "touchwake_wake");
	return 0;
}

device_initcall(touchwake_control_init);

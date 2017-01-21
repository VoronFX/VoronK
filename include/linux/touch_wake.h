/* include/linux/touch_wake.h */

#ifndef _LINUX_TOUCH_WAKE_H
#define _LINUX_TOUCH_WAKE_H

#include <linux/input.h>
#include <linux/gp2a.h>

void powerkey_pressed(void);
void powerkey_released(void);
void proximity_detected(void);
void proximity_off(void);
void touch_press(void);
int get_touchoff_delay(void);
bool device_is_suspended(void);
void set_powerkeydev(struct input_dev * input_device);
static gp2a_platform_data *touch_wake_proximity_dev;

#endif

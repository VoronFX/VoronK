/* include/linux/touch_wake.h */

#ifndef _LINUX_TOUCH_WAKE_H
#define _LINUX_TOUCH_WAKE_H

#include <linux/kmod.h>
#include <linux/input.h>

void powerkey_pressed(void);
void powerkey_released(void);
void proximity_detected(void);
void proximity_off(void);
void touch_press(bool up);
int get_touchoff_delay(void);
bool device_is_suspended(void);
void set_powerkeydev(struct input_dev * input_device);

extern bool tw_debug_on;
#define tw_debug(str, args...) if (unlikely(tw_debug_on)) pr_info(str, ##args)


#endif

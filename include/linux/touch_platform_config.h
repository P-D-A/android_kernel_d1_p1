/*
 * include/linux/touch_platfrom_config.h - platform data structure for touchscreen
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _TOUCH_PLATFORM_CONFIG_H
#define _TOUCH_PLATFORM_CONFIG_H

#define TOUCH_OMAP4430_INT_PIN 35
#define TOUCH_OMAP4430_RESET_PIN  36

#ifdef CONFIG_TOUCHSCREEN_MXT224E_ATMEL
#include <linux/atmel_qt602240.h>
#endif

#ifdef CONFIG_TOUCHSCREEN_RMI4_SYNAPTICS

#include <linux/synaptics_i2c_rmi_1564.h>

/*define some tp type*/
#define LCD_X_QVGA         320
#define LCD_Y_QVGA         240
#define LCD_X_HVGA         320
#define LCD_Y_HVGA         480
#define LCD_X_WVGA         480
#define LCD_Y_WVGA         800
#define LCD_JS_WVGA        882
#define LCD_JS_HVGA        510


struct tp_resolution_conversion{
    int lcd_x;
    int lcd_y;
    int jisuan;
};

struct synaptics_i2c_platform_data {
    int (*touch_power)(int on);	/* Only valid in first array entry */
    int (*touch_gpio_config_interrupt)(void);/*it will config the gpio*/
    void (*set_touch_probe_flag)(int detected);/*we use this to detect the probe is detected*/
    int (*read_touch_probe_flag)(void);/*when the touch is find we return a value*/
    int (*touch_reset)(void);
    int (*get_touch_reset_pin)(void);
    int (*get_phone_version)(struct tp_resolution_conversion *tp_resolution_type);/*add this function for judge the tp type*/
};

#endif

#endif /*_TOUCH_PLATFORM_CONFIG_H */

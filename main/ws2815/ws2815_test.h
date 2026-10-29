#pragma once

#include <stdbool.h>

/* Runs the GPIO13 WS2815B test and keeps every configured LED at full yellow. */
void ws2815_full_yellow_test(void);

/* Starts the 8-LED RPM bar; rpm_source is updated by the CAN receive path. */
void ws2815_rpm_bar_start(volatile int *rpm_source);

/* AMI 首页待机时，8 颗转速灯保持暗绿色；进入驾驶页后恢复 RPM 灯条。 */
void ws2815_rpm_bar_set_home_standby(bool enabled);

/* Holds GPIO13 at a steady 3.3 V for board-level wiring verification. */
void gpio13_high_level_test(void);

#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_AMI = 1,
    SCREEN_ID_AUTONOMOUS = 2,
    SCREEN_ID_DRIVER_VIEW = 3,
    SCREEN_ID_INSPECT = 4,
    _SCREEN_ID_LAST = 4
};

typedef struct _objects_t {
    lv_obj_t *ami;
    lv_obj_t *autonomous;
    lv_obj_t *driver_view;
    lv_obj_t *inspect;
    lv_obj_t *line_acc;
    lv_obj_t *eight;
    lv_obj_t *high_foll;
    lv_obj_t *ebs_1;
    lv_obj_t *check;
    lv_obj_t *driving;
    lv_obj_t *obj0;
    lv_obj_t *bfp_bar;
    lv_obj_t *batt;
    lv_obj_t *km_label_2;
    lv_obj_t *km_label_1;
    lv_obj_t *lv_bar_1;
    lv_obj_t *km_label_3;
    lv_obj_t *hv_bar_3;
    lv_obj_t *ts;
    lv_obj_t *motor_tem;
    lv_obj_t *__;
    lv_obj_t *km_label_5;
    lv_obj_t *v_3;
    lv_obj_t *motor;
    lv_obj_t *lv_volt;
    lv_obj_t *rotating;
    lv_obj_t *rotating_speed;
    lv_obj_t *v_4;
    lv_obj_t *ts_1;
    lv_obj_t *ts_volt;
    lv_obj_t *motor_1;
    lv_obj_t *lv_a;
    lv_obj_t *imd;
    lv_obj_t *obj1;
    lv_obj_t *speed_label_3;
    lv_obj_t *km_label_6;
    lv_obj_t *ts_6;
    lv_obj_t *ts_a;
    lv_obj_t *v;
    lv_obj_t *v_1;
    lv_obj_t *autonomous_task_label;
    lv_obj_t *v_2;
    lv_obj_t *v_5;
    lv_obj_t *v_6;
    lv_obj_t *v_7;
    lv_obj_t *ebs;
    lv_obj_t *obj3;
    lv_obj_t *temp;
    lv_obj_t *obj4;
    lv_obj_t *volt;
    lv_obj_t *obj5;
    lv_obj_t *ams;
    lv_obj_t *obj6;
    lv_obj_t *as;
    lv_obj_t *obj7;
    lv_obj_t *r2d;
    lv_obj_t *r2d_1;
    lv_obj_t *sbw;
    lv_obj_t *obj8;
    lv_obj_t *ebh;
    lv_obj_t *obj9;
    lv_obj_t *ts_2;
    lv_obj_t *obj10;
    lv_obj_t *ready_label;
    lv_obj_t *middle_container;
    lv_obj_t *temp_motor_container;
    lv_obj_t *tempmotor_label;
    lv_obj_t *lap_times_container;
    lv_obj_t *laptime_label;
    lv_obj_t *lastlap_label;
    lv_obj_t *temp_inv_container;
    lv_obj_t *temp_inv_label;
    lv_obj_t *speed_container;
    lv_obj_t *speed_label;
    lv_obj_t *km_label;
    lv_obj_t *speed_label_2;
    lv_obj_t *km_label_4;
    lv_obj_t *brake_acell_presure_container;
    lv_obj_t *brake_presure_bar;
    lv_obj_t *accellerator_presure_bar;
    lv_obj_t *hv_bar;
    lv_obj_t *lv_bar;
    lv_obj_t *hv_label;
    lv_obj_t *lv_label;
    lv_obj_t *obj11;
    lv_obj_t *obj12;
    lv_obj_t *obj13;
    lv_obj_t *hv_bar_2;
    lv_obj_t *obj14;
    lv_obj_t *obj15;
    lv_obj_t *brake_presure_container_1;
    lv_obj_t *brake_presure_bar_5;
    lv_obj_t *brake_presure_bar_6;
    lv_obj_t *obj16;
    lv_obj_t *inspect_header;
    lv_obj_t *obj17;
    lv_obj_t *obj18;
    lv_obj_t *obj19;
    lv_obj_t *inspect_as_state;
    lv_obj_t *inspect_can_status;
    lv_obj_t *inspect_test_timer;
    lv_obj_t *inspect_ready_panel;
    lv_obj_t *obj20;
    lv_obj_t *obj21;
    lv_obj_t *obj22;
    lv_obj_t *obj23;
    lv_obj_t *obj24;
    lv_obj_t *obj25;
    lv_obj_t *obj26;
    lv_obj_t *obj27;
    lv_obj_t *obj28;
    lv_obj_t *obj29;
    lv_obj_t *obj30;
    lv_obj_t *obj31;
    lv_obj_t *obj32;
    lv_obj_t *obj33;
    lv_obj_t *obj34;
    lv_obj_t *obj35;
    lv_obj_t *obj36;
    lv_obj_t *obj37;
    lv_obj_t *obj38;
    lv_obj_t *inspect_center_panel;
    lv_obj_t *obj39;
    lv_obj_t *obj40;
    lv_obj_t *obj41;
    lv_obj_t *obj42;
    lv_obj_t *obj43;
    lv_obj_t *obj44;
    lv_obj_t *obj45;
    lv_obj_t *obj46;
    lv_obj_t *obj47;
    lv_obj_t *obj48;
    lv_obj_t *obj49;
    lv_obj_t *obj50;
    lv_obj_t *obj51;
    lv_obj_t *obj52;
    lv_obj_t *obj53;
    lv_obj_t *obj54;
    lv_obj_t *obj55;
    lv_obj_t *obj56;
    lv_obj_t *obj57;
    lv_obj_t *obj58;
    lv_obj_t *inspect_main_action;
    lv_obj_t *inspect_main_detail;
    lv_obj_t *inspect_main_note;
    lv_obj_t *inspect_ebs_tile;
    lv_obj_t *obj59;
    lv_obj_t *inspect_ebs_value;
    lv_obj_t *inspect_ts_tile;
    lv_obj_t *obj60;
    lv_obj_t *inspect_ts_value;
    lv_obj_t *inspect_brake_tile;
    lv_obj_t *obj61;
    lv_obj_t *inspect_brake_value;
    lv_obj_t *inspect_steering_tile;
    lv_obj_t *obj62;
    lv_obj_t *inspect_steering_value;
    lv_obj_t *inspect_event_panel;
    lv_obj_t *obj63;
    lv_obj_t *obj64;
    lv_obj_t *inspect_last_command;
    lv_obj_t *obj65;
    lv_obj_t *inspect_trigger_source;
    lv_obj_t *obj66;
    lv_obj_t *inspect_reset_required;
    lv_obj_t *inspect_reset_instruction;
    lv_obj_t *inspect_data_valid;
    lv_obj_t *inspect_footer;
    lv_obj_t *inspect_footer_status;
    lv_obj_t *inspect_state_led;
    lv_obj_t *inspect_amoy_logo;
    lv_obj_t *inspect_data_age;
} objects_t;

extern objects_t objects;

void create_screen_ami();
void tick_screen_ami();

void create_screen_autonomous();
void tick_screen_autonomous();

void create_screen_driver_view();
void tick_screen_driver_view();

void create_screen_inspect();
void tick_screen_inspect();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/

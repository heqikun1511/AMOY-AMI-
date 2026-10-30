#pragma once

#include <stdbool.h>
#include <stdint.h>

/* AMI display gateway protocol from docs/ami_display.dbc.
 * All frames are 11-bit standard CAN, Intel/little-endian and DLC 8. */
#define AMI_DISPLAY_CAN_ID_MOTOR              0x620U
#define AMI_DISPLAY_CAN_ID_PEDALS             0x621U
#define AMI_DISPLAY_CAN_ID_AS_STATUS          0x622U
#define AMI_DISPLAY_CAN_ID_ELECTRICAL         0x623U
#define AMI_DISPLAY_CAN_ID_CONTROL_STATUS     0x624U
#define AMI_DISPLAY_CAN_ID_ANGLES_AND_SOURCES 0x625U

/* K2 motor-controller protocol compatibility frames (29-bit extended CAN). */
#define K2_CAN_ID_VCU_TO_MCU_COMMAND 0x08C1EF21U
#define K2_CAN_ID_MCU_TO_VCU_STATUS1 0x0CFFC6EFU

typedef struct {
    bool valid;
    uint32_t rx_count;
    uint32_t last_rx_ms;
} ami_display_can_frame_meta_t;

typedef struct {
    ami_display_can_frame_meta_t meta;
    float motor_speed_rpm;
    float motor_power_kw;
    float controller_temperature_c;
    float motor_temperature_c;
} ami_display_can_motor_t;

typedef struct {
    ami_display_can_frame_meta_t meta;
    float throttle_percent;
    float brake_percent;
    uint8_t low_voltage_battery_v;
    bool as_positive;
    bool air_positive;
} ami_display_can_pedals_t;

typedef struct {
    ami_display_can_frame_meta_t meta;
    uint8_t ebs_state, res_state, sbw_state, ehb_state;
    float brake_pressure_1_mpa, brake_pressure_2_mpa;
    float air_pressure_1_mpa, air_pressure_2_mpa;
} ami_display_can_as_status_t;

typedef struct {
    ami_display_can_frame_meta_t meta;
    float drive_bus_voltage_v;
    float drive_bus_current_a;
} ami_display_can_electrical_t;

typedef struct {
    ami_display_can_frame_meta_t meta;
    uint8_t drive_control_owner, steering_control_owner, brake_control_owner;
    uint8_t actual_gear, commanded_gear, bms_discharge_available;
    bool sdc_closed, rc_link_connected;
} ami_display_can_control_status_t;

typedef struct {
    ami_display_can_frame_meta_t meta;
    float steering_target_angle_deg;
    float steering_actual_angle_deg;
    uint8_t source_state[16];
} ami_display_can_angles_and_sources_t;

typedef struct {
    ami_display_can_frame_meta_t meta;
    float speed_rpm;
    float torque_permille;
    uint8_t control_mode;
    uint8_t gear;
    bool main_contactor_closed;
    float dc_bus_voltage_v;
} ami_display_can_k2_motor_t;

typedef struct {
    ami_display_can_motor_t motor;
    ami_display_can_pedals_t pedals;
    ami_display_can_as_status_t as_status;
    ami_display_can_electrical_t electrical;
    ami_display_can_control_status_t control_status;
    ami_display_can_angles_and_sources_t angles_and_sources;
    ami_display_can_k2_motor_t k2_command;
    ami_display_can_k2_motor_t k2_status;
    uint32_t rejected_dlc_count;
} ami_display_can_snapshot_t;

void ami_display_can_init(void);
bool ami_display_can_receive(uint32_t can_id, bool is_extended,
                             const uint8_t *data, uint8_t len);
void ami_display_can_get_snapshot(ami_display_can_snapshot_t *snapshot);
bool ami_display_can_frame_is_fresh(const ami_display_can_frame_meta_t *meta,
                                    uint32_t timeout_ms);
bool ami_display_can_any_frame_is_fresh(const ami_display_can_snapshot_t *snapshot,
                                        uint32_t timeout_ms);

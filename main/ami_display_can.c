#include "ami_display_can.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static ami_display_can_snapshot_t s_snapshot;
static portMUX_TYPE s_ami_display_can_lock = portMUX_INITIALIZER_UNLOCKED;

static uint16_t le_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t le_i16(const uint8_t *p)
{
    return (int16_t)le_u16(p);
}

static void update_meta(ami_display_can_frame_meta_t *meta, uint32_t now_ms)
{
    meta->valid = true;
    meta->rx_count++;
    meta->last_rx_ms = now_ms;
}

static void decode_k2_motor(ami_display_can_k2_motor_t *motor,
                            const uint8_t *data, uint32_t now_ms)
{
    /* K2 uses unsigned raw speed with 0.5 rpm/bit and -10000 rpm offset. */
    motor->speed_rpm = (float)le_u16(&data[0]) * 0.5f - 10000.0f;
    motor->torque_permille = (float)le_u16(&data[2]);
    motor->control_mode = data[4];
    motor->gear = data[5] & 0x03U;
    motor->main_contactor_closed = (data[5] & 0x08U) != 0;
    motor->dc_bus_voltage_v = (float)le_u16(&data[6]) * 0.1f;
    update_meta(&motor->meta, now_ms);
}

void ami_display_can_init(void)
{
    taskENTER_CRITICAL(&s_ami_display_can_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    taskEXIT_CRITICAL(&s_ami_display_can_lock);
}

bool ami_display_can_receive(uint32_t can_id, bool is_extended,
                             const uint8_t *data, uint8_t len)
{
    if (!data) {
        return false;
    }

    const bool is_k2_command = is_extended && can_id == K2_CAN_ID_VCU_TO_MCU_COMMAND;
    const bool is_k2_status = is_extended && can_id == K2_CAN_ID_MCU_TO_VCU_STATUS1;
    const bool is_ami_standard = !is_extended &&
                                 can_id >= AMI_DISPLAY_CAN_ID_MOTOR &&
                                 can_id <= AMI_DISPLAY_CAN_ID_ANGLES_AND_SOURCES;

    if (!is_k2_command && !is_k2_status && !is_ami_standard) {
        return false;
    }

    if (len != 8) {
        taskENTER_CRITICAL(&s_ami_display_can_lock);
        s_snapshot.rejected_dlc_count++;
        taskEXIT_CRITICAL(&s_ami_display_can_lock);
        return false;
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    taskENTER_CRITICAL(&s_ami_display_can_lock);
    if (is_k2_command || is_k2_status) {
        decode_k2_motor(is_k2_command ? &s_snapshot.k2_command
                                      : &s_snapshot.k2_status,
                        data, now_ms);
        taskEXIT_CRITICAL(&s_ami_display_can_lock);
        return true;
    }

    switch (can_id) {
    case AMI_DISPLAY_CAN_ID_MOTOR:
        s_snapshot.motor.motor_speed_rpm = (float)le_i16(&data[0]);
        s_snapshot.motor.motor_power_kw = (float)le_i16(&data[2]) * 0.1f;
        s_snapshot.motor.controller_temperature_c = (float)le_i16(&data[4]) * 0.1f;
        s_snapshot.motor.motor_temperature_c = (float)le_i16(&data[6]) * 0.1f;
        update_meta(&s_snapshot.motor.meta, now_ms);
        break;
    case AMI_DISPLAY_CAN_ID_PEDALS:
        s_snapshot.pedals.throttle_percent = (float)le_u16(&data[0]) * 0.01f;
        s_snapshot.pedals.brake_percent = (float)le_u16(&data[2]) * 0.01f;
        s_snapshot.pedals.low_voltage_battery_v = data[4];
        s_snapshot.pedals.as_positive = data[5] != 0;
        s_snapshot.pedals.air_positive = data[6] != 0;
        update_meta(&s_snapshot.pedals.meta, now_ms);
        break;
    case AMI_DISPLAY_CAN_ID_AS_STATUS:
        s_snapshot.as_status.ebs_state = data[0];
        s_snapshot.as_status.res_state = data[1];
        s_snapshot.as_status.sbw_state = data[2];
        s_snapshot.as_status.ehb_state = data[3];
        s_snapshot.as_status.brake_pressure_1_mpa = (float)data[4] * 0.02f;
        s_snapshot.as_status.brake_pressure_2_mpa = (float)data[5] * 0.02f;
        s_snapshot.as_status.air_pressure_1_mpa = (float)data[6] * 0.02f;
        s_snapshot.as_status.air_pressure_2_mpa = (float)data[7] * 0.02f;
        update_meta(&s_snapshot.as_status.meta, now_ms);
        break;
    case AMI_DISPLAY_CAN_ID_ELECTRICAL:
        s_snapshot.electrical.drive_bus_voltage_v = (float)le_u16(&data[0]) * 0.1f;
        s_snapshot.electrical.drive_bus_current_a = (float)le_i16(&data[2]) * 0.1f;
        update_meta(&s_snapshot.electrical.meta, now_ms);
        break;
    case AMI_DISPLAY_CAN_ID_CONTROL_STATUS:
        s_snapshot.control_status.drive_control_owner = data[0];
        s_snapshot.control_status.steering_control_owner = data[1];
        s_snapshot.control_status.brake_control_owner = data[2];
        s_snapshot.control_status.actual_gear = data[3];
        s_snapshot.control_status.commanded_gear = data[4];
        s_snapshot.control_status.bms_discharge_available = data[5];
        s_snapshot.control_status.sdc_closed = data[6] != 0;
        s_snapshot.control_status.rc_link_connected = data[7] != 0;
        update_meta(&s_snapshot.control_status.meta, now_ms);
        break;
    case AMI_DISPLAY_CAN_ID_ANGLES_AND_SOURCES:
        s_snapshot.angles_and_sources.steering_target_angle_deg =
            (float)le_i16(&data[0]) * 0.1f;
        s_snapshot.angles_and_sources.steering_actual_angle_deg =
            (float)le_i16(&data[2]) * 0.1f;
        for (unsigned i = 0; i < 16; ++i) {
            s_snapshot.angles_and_sources.source_state[i] =
                (data[4 + i / 4] >> ((i % 4) * 2)) & 0x03U;
        }
        update_meta(&s_snapshot.angles_and_sources.meta, now_ms);
        break;
    default:
        taskEXIT_CRITICAL(&s_ami_display_can_lock);
        return false;
    }
    taskEXIT_CRITICAL(&s_ami_display_can_lock);
    return true;
}

void ami_display_can_get_snapshot(ami_display_can_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    taskENTER_CRITICAL(&s_ami_display_can_lock);
    *snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_ami_display_can_lock);
}

bool ami_display_can_frame_is_fresh(const ami_display_can_frame_meta_t *meta,
                                    uint32_t timeout_ms)
{
    if (!meta || !meta->valid) {
        return false;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return now_ms - meta->last_rx_ms <= timeout_ms;
}

bool ami_display_can_any_frame_is_fresh(const ami_display_can_snapshot_t *snapshot,
                                        uint32_t timeout_ms)
{
    if (!snapshot) {
        return false;
    }
    return ami_display_can_frame_is_fresh(&snapshot->motor.meta, timeout_ms) ||
           ami_display_can_frame_is_fresh(&snapshot->pedals.meta, timeout_ms) ||
           ami_display_can_frame_is_fresh(&snapshot->as_status.meta, timeout_ms) ||
           ami_display_can_frame_is_fresh(&snapshot->electrical.meta, timeout_ms) ||
           ami_display_can_frame_is_fresh(&snapshot->control_status.meta, timeout_ms) ||
           ami_display_can_frame_is_fresh(&snapshot->angles_and_sources.meta, timeout_ms) ||
           ami_display_can_frame_is_fresh(&snapshot->k2_command.meta, timeout_ms) ||
           ami_display_can_frame_is_fresh(&snapshot->k2_status.meta, timeout_ms);
}

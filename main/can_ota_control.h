#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CAN_OTA_VCU_TO_AMI_ID 0x6E0U
#define CAN_OTA_AMI_TO_VCU_ID 0x6E1U

/* Process the authenticated enter-Recovery request in the normal dashboard.
 * Returns true when the frame belongs to the CAN update protocol. */
bool can_ota_control_handle_frame(uint32_t can_id, bool is_extended,
                                  const uint8_t *data, uint8_t len,
                                  bool vehicle_safe);

/* Confirm a newly installed ota_0 image once the dashboard reached the end of
 * its hardware/UI initialisation. Safe to call on every normal boot. */
void can_ota_control_confirm_running_app(void);

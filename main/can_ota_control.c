#include "can_ota_control.h"

#include "bsp_can.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define CAN_OTA_OP_ENTER_RECOVERY 0x01U
#define CAN_OTA_OP_ENTER_ACK      0x81U
#define CAN_OTA_STATUS_OK         0x00U
#define CAN_OTA_STATUS_BUSY       0x0BU
#define CAN_OTA_STATUS_UNSAFE     0x09U
#define CAN_OTA_REQUEST_MAGIC     0x43414E55UL /* "CANU" */

static const char *TAG = "CAN_OTA_CTRL";
static bool s_reboot_pending;

static bool enter_key_valid(const uint8_t *data, uint8_t len)
{
    static const uint8_t key[6] = { 0x42, 0x4F, 0x4F, 0x54, 0x21, 0xA5 };
    if (!data || len != 8 || data[0] != CAN_OTA_OP_ENTER_RECOVERY) {
        return false;
    }
    for (size_t i = 0; i < sizeof(key); ++i) {
        if (data[i + 2] != key[i]) {
            return false;
        }
    }
    return true;
}

static esp_err_t save_update_request(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("can_ota", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(nvs, "request", CAN_OTA_REQUEST_MAGIC);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void switch_to_recovery_task(void *arg)
{
    const uint8_t session = (uint8_t)(uintptr_t)arg;
    uint8_t reply[8] = { CAN_OTA_OP_ENTER_ACK, session, CAN_OTA_STATUS_OK,
                         0x01, 0, 0, 0, 0 };

    esp_err_t err = save_update_request();
    const esp_partition_t *recovery = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "can_recovery");
    if (err == ESP_OK && recovery) {
        err = esp_ota_set_boot_partition(recovery);
    } else if (!recovery && err == ESP_OK) {
        err = ESP_ERR_NOT_FOUND;
    }

    if (err != ESP_OK) {
        reply[2] = 0x05; /* flash/partition error */
        ESP_LOGE(TAG, "Cannot enter Recovery: %s", esp_err_to_name(err));
        (void)bsp_can_send_standard(CAN_OTA_AMI_TO_VCU_ID, reply, sizeof(reply));
        s_reboot_pending = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "Authenticated CAN update request; rebooting to Recovery");
    (void)bsp_can_send_standard(CAN_OTA_AMI_TO_VCU_ID, reply, sizeof(reply));
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

bool can_ota_control_handle_frame(uint32_t can_id, bool is_extended,
                                  const uint8_t *data, uint8_t len,
                                  bool vehicle_safe)
{
    if (is_extended || can_id != CAN_OTA_VCU_TO_AMI_ID) {
        return false;
    }

    if (!enter_key_valid(data, len)) {
        return true;
    }

    if (!vehicle_safe) {
        uint8_t reply[8] = { CAN_OTA_OP_ENTER_ACK, data[1],
                             CAN_OTA_STATUS_UNSAFE, 0, 0, 0, 0, 0 };
        ESP_LOGW(TAG, "Rejected CAN update request: vehicle is not safe");
        (void)bsp_can_send_standard(CAN_OTA_AMI_TO_VCU_ID, reply, sizeof(reply));
        return true;
    }

    if (s_reboot_pending) {
        uint8_t reply[8] = { CAN_OTA_OP_ENTER_ACK, data[1],
                             CAN_OTA_STATUS_BUSY, 0, 0, 0, 0, 0 };
        (void)bsp_can_send_standard(CAN_OTA_AMI_TO_VCU_ID, reply, sizeof(reply));
        return true;
    }

    s_reboot_pending = true;
    if (xTaskCreate(switch_to_recovery_task, "can_ota_reboot", 4096,
                    (void *)(uintptr_t)data[1], 8, NULL) != pdPASS) {
        s_reboot_pending = false;
        ESP_LOGE(TAG, "Failed to create Recovery switch task");
    }
    return true;
}

void can_ota_control_confirm_running_app(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running || running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        return;
    }

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "ota_0 startup confirmed; rollback cancelled");
        } else {
            ESP_LOGE(TAG, "Failed to confirm ota_0: %s", esp_err_to_name(err));
        }
    }
}

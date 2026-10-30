#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/twai.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CAN_TX_PIN GPIO_NUM_15
#define CAN_RX_PIN GPIO_NUM_14
#define CAN_OTA_RX_ID 0x6E0U
#define CAN_OTA_TX_ID 0x6E1U

#define OP_START       0x10U
#define OP_END         0x11U
#define OP_ACTIVATE    0x12U
#define OP_ABORT       0x13U
#define OP_QUERY       0x14U
#define OP_DATA_MIN    0x80U
#define OP_READY       0x90U
#define OP_START_ACK   0x91U
#define OP_DATA_ACK    0x92U
#define OP_VERIFY_ACK  0x93U
#define OP_STATUS      0x94U
#define OP_ACTIVATE_ACK 0x95U
#define OP_ABORT_ACK   0x96U
#define OP_ERROR       0xFFU

#define STATUS_OK             0x00U
#define STATUS_COMMAND        0x01U
#define STATUS_SESSION        0x02U
#define STATUS_LENGTH         0x03U
#define STATUS_SEQUENCE       0x04U
#define STATUS_FLASH          0x05U
#define STATUS_CRC            0x06U
#define STATUS_IMAGE          0x07U
#define STATUS_UNSAFE_STATE   0x09U
#define STATUS_TIMEOUT        0x0AU
#define STATUS_BUSY           0x0BU

#define PROTOCOL_VERSION 1U
#define TARGET_MAIN      1U
#define FLOW_WINDOW      32U
#define SESSION_TIMEOUT_MS 30000U
#define CAN_OTA_REQUEST_MAGIC 0x43414E55UL

typedef enum {
    UPDATE_IDLE = 0,
    UPDATE_RECEIVING = 1,
    UPDATE_VERIFIED = 2,
} update_state_t;

typedef struct {
    update_state_t state;
    uint8_t session;
    uint8_t next_sequence;
    uint8_t frames_in_window;
    uint8_t last_error;
    uint32_t image_size;
    uint32_t received;
    uint32_t crc32;
    TickType_t last_activity;
    esp_ota_handle_t ota_handle;
    bool ota_handle_valid;
    const esp_partition_t *target;
    uint8_t write_buffer[FLOW_WINDOW * 7U];
    size_t write_buffer_length;
} update_context_t;

static const char *TAG = "CAN_RECOVERY";
static update_context_t s_update;

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32_le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static esp_err_t can_send(const uint8_t data[8])
{
    twai_message_t message = {
        .identifier = CAN_OTA_TX_ID,
        .data_length_code = 8,
    };
    memcpy(message.data, data, 8);
    return twai_transmit(&message, pdMS_TO_TICKS(100));
}

static void send_error(uint8_t session, uint8_t status, uint8_t opcode)
{
    uint8_t reply[8] = { OP_ERROR, session, status, opcode, 0, 0, 0, 0 };
    put_u32_le(&reply[4], s_update.received);
    s_update.last_error = status;
    (void)can_send(reply);
}

static void send_ready(void)
{
    uint8_t reply[8] = { OP_READY, 0, STATUS_OK, PROTOCOL_VERSION,
                         0, 0, 0, 0 };
    uint32_t size = s_update.target ? s_update.target->size : 0;
    put_u32_le(&reply[4], size);
    (void)can_send(reply);
}

static void send_progress(void)
{
    uint8_t reply[8] = { OP_DATA_ACK, s_update.session, STATUS_OK,
                         s_update.next_sequence, 0, 0, 0, 0 };
    put_u32_le(&reply[4], s_update.received);
    (void)can_send(reply);
    s_update.frames_in_window = 0;
}

static esp_err_t set_request_marker(bool present)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("can_ota", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (present) {
        err = nvs_set_u32(nvs, "request", CAN_OTA_REQUEST_MAGIC);
    } else {
        err = nvs_erase_key(nvs, "request");
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static bool request_marker_is_set(void)
{
    nvs_handle_t nvs;
    uint32_t value = 0;
    if (nvs_open("can_ota", NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u32(nvs, "request", &value);
    nvs_close(nvs);
    return err == ESP_OK && value == CAN_OTA_REQUEST_MAGIC;
}

static void abort_transfer(void)
{
    if (s_update.ota_handle_valid) {
        (void)esp_ota_abort(s_update.ota_handle);
    }
    s_update.ota_handle_valid = false;
    s_update.state = UPDATE_IDLE;
    s_update.received = 0;
    s_update.image_size = 0;
    s_update.frames_in_window = 0;
    s_update.next_sequence = 0;
    s_update.write_buffer_length = 0;
}

static void process_start(const uint8_t data[8])
{
    uint8_t session = data[1];
    uint8_t target = data[2];
    uint8_t flags = data[3];
    uint32_t image_size = get_u32_le(&data[4]);

    if (s_update.state == UPDATE_RECEIVING) {
        send_error(session, STATUS_BUSY, OP_START);
        return;
    }
    if (target != TARGET_MAIN || flags != 0 || !s_update.target) {
        send_error(session, STATUS_COMMAND, OP_START);
        return;
    }
    if (image_size < 1024 || image_size > s_update.target->size) {
        send_error(session, STATUS_LENGTH, OP_START);
        return;
    }

    abort_transfer();
    esp_err_t err = esp_ota_begin(s_update.target, image_size,
                                  &s_update.ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        send_error(session, STATUS_FLASH, OP_START);
        return;
    }

    s_update.ota_handle_valid = true;
    s_update.state = UPDATE_RECEIVING;
    s_update.session = session;
    s_update.image_size = image_size;
    s_update.received = 0;
    s_update.crc32 = 0;
    s_update.next_sequence = 0;
    s_update.frames_in_window = 0;
    s_update.write_buffer_length = 0;
    s_update.last_error = STATUS_OK;
    s_update.last_activity = xTaskGetTickCount();
    (void)set_request_marker(true);

    uint8_t reply[8] = { OP_START_ACK, session, STATUS_OK, FLOW_WINDOW,
                         0, 0, 0, 0 };
    put_u32_le(&reply[4], s_update.target->size);
    (void)can_send(reply);
    ESP_LOGI(TAG, "Receiving session %u, image %lu bytes", session,
             (unsigned long)image_size);
}

static esp_err_t flush_write_buffer(void)
{
    if (s_update.write_buffer_length == 0) {
        return ESP_OK;
    }
    esp_err_t err = esp_ota_write(s_update.ota_handle, s_update.write_buffer,
                                  s_update.write_buffer_length);
    if (err == ESP_OK) {
        s_update.write_buffer_length = 0;
    }
    return err;
}

static void process_data(const uint8_t data[8])
{
    if (s_update.state != UPDATE_RECEIVING) {
        send_error(s_update.session, STATUS_COMMAND, data[0]);
        return;
    }

    uint8_t sequence = data[0] & 0x7FU;
    if (sequence != s_update.next_sequence) {
        send_error(s_update.session, STATUS_SEQUENCE, data[0]);
        return;
    }

    uint32_t remaining = s_update.image_size - s_update.received;
    size_t length = remaining < 7U ? remaining : 7U;
    if (length == 0) {
        send_error(s_update.session, STATUS_LENGTH, data[0]);
        return;
    }

    memcpy(&s_update.write_buffer[s_update.write_buffer_length], &data[1], length);
    s_update.write_buffer_length += length;
    s_update.crc32 = esp_crc32_le(s_update.crc32, &data[1], length);
    s_update.received += length;
    s_update.next_sequence = (uint8_t)((s_update.next_sequence + 1U) & 0x7FU);
    s_update.frames_in_window++;
    s_update.last_activity = xTaskGetTickCount();

    if (s_update.frames_in_window >= FLOW_WINDOW ||
        s_update.received == s_update.image_size) {
        esp_err_t err = flush_write_buffer();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %lu: %s",
                     (unsigned long)s_update.received, esp_err_to_name(err));
            uint8_t session = s_update.session;
            abort_transfer();
            send_error(session, STATUS_FLASH, data[0]);
            return;
        }
        send_progress();
    }
}

static void process_end(const uint8_t data[8])
{
    uint8_t session = data[1];
    uint32_t expected_crc = get_u32_le(&data[2]);
    if (s_update.state != UPDATE_RECEIVING || session != s_update.session) {
        send_error(session, STATUS_SESSION, OP_END);
        return;
    }
    if (s_update.received != s_update.image_size) {
        send_error(session, STATUS_LENGTH, OP_END);
        return;
    }
    if (s_update.crc32 != expected_crc) {
        ESP_LOGE(TAG, "CRC mismatch: received %08lx expected %08lx",
                 (unsigned long)s_update.crc32, (unsigned long)expected_crc);
        abort_transfer();
        send_error(session, STATUS_CRC, OP_END);
        return;
    }

    esp_err_t err = esp_ota_end(s_update.ota_handle);
    s_update.ota_handle_valid = false;
    if (err != ESP_OK) {
        s_update.state = UPDATE_IDLE;
        ESP_LOGE(TAG, "Image validation failed: %s", esp_err_to_name(err));
        send_error(session, STATUS_IMAGE, OP_END);
        return;
    }

    s_update.state = UPDATE_VERIFIED;
    uint8_t reply[8] = { OP_VERIFY_ACK, session, STATUS_OK, UPDATE_VERIFIED,
                         0, 0, 0, 0 };
    put_u32_le(&reply[4], s_update.received);
    (void)can_send(reply);
    ESP_LOGI(TAG, "Image CRC and ESP image validation passed");
}

static void process_activate(const uint8_t data[8])
{
    static const uint8_t key[4] = { 0xA5, 0x5A, 0xC3, 0x3C };
    uint8_t session = data[1];
    if (s_update.state != UPDATE_VERIFIED || session != s_update.session ||
        memcmp(&data[2], key, sizeof(key)) != 0) {
        send_error(session, STATUS_UNSAFE_STATE, OP_ACTIVATE);
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(s_update.target);
    if (err == ESP_OK) {
        err = set_request_marker(false);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Activation failed: %s", esp_err_to_name(err));
        send_error(session, STATUS_FLASH, OP_ACTIVATE);
        return;
    }

    uint8_t reply[8] = { OP_ACTIVATE_ACK, session, STATUS_OK, data[6],
                         0, 0, 0, 0 };
    put_u32_le(&reply[4], s_update.received);
    (void)can_send(reply);
    uint32_t delay_ms = data[6] ? (uint32_t)data[6] * 100U : 200U;
    ESP_LOGW(TAG, "Activating ota_0 in %lu ms", (unsigned long)delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

static void process_abort(const uint8_t data[8])
{
    uint8_t session = data[1];
    abort_transfer();
    uint8_t reply[8] = { OP_ABORT_ACK, session, STATUS_OK, 0, 0, 0, 0, 0 };
    (void)can_send(reply);
}

static void process_query(const uint8_t data[8])
{
    uint8_t reply[8] = { OP_STATUS, data[1], (uint8_t)s_update.state,
                         s_update.next_sequence, 0, 0, 0, 0 };
    put_u32_le(&reply[4], s_update.received);
    (void)can_send(reply);
}

static void process_frame(const twai_message_t *message)
{
    if (message->extd || message->rtr || message->identifier != CAN_OTA_RX_ID ||
        message->data_length_code != 8) {
        return;
    }

    const uint8_t *data = message->data;
    if (data[0] >= OP_DATA_MIN) {
        process_data(data);
        return;
    }
    switch (data[0]) {
    case OP_START: process_start(data); break;
    case OP_END: process_end(data); break;
    case OP_ACTIVATE: process_activate(data); break;
    case OP_ABORT: process_abort(data); break;
    case OP_QUERY: process_query(data); break;
    default: send_error(data[1], STATUS_COMMAND, data[0]); break;
    }
}

static void handle_alerts(void)
{
    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) != ESP_OK) {
        return;
    }
    if (alerts & TWAI_ALERT_BUS_OFF) {
        ESP_LOGE(TAG, "CAN bus-off, starting recovery");
        (void)twai_initiate_recovery();
    }
    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        ESP_LOGW(TAG, "CAN bus recovered");
        (void)twai_start();
        send_ready();
    }
}

static void recovery_loop(void)
{
    for (;;) {
        twai_message_t message;
        if (twai_receive(&message, pdMS_TO_TICKS(100)) == ESP_OK) {
            process_frame(&message);
        }
        handle_alerts();

        if (s_update.state == UPDATE_RECEIVING &&
            (xTaskGetTickCount() - s_update.last_activity) >
                pdMS_TO_TICKS(SESSION_TIMEOUT_MS)) {
            uint8_t session = s_update.session;
            ESP_LOGE(TAG, "Session %u timed out at %lu bytes", session,
                     (unsigned long)s_update.received);
            abort_transfer();
            send_error(session, STATUS_TIMEOUT, OP_DATA_MIN);
        }
    }
}

static void maybe_boot_existing_main(bool update_requested)
{
    if (update_requested || !s_update.target) {
        return;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t state_err = esp_ota_get_state_partition(s_update.target, &state);
    if (state_err == ESP_OK && state != ESP_OTA_IMG_UNDEFINED &&
        state != ESP_OTA_IMG_VALID) {
        ESP_LOGW(TAG, "ota_0 state %d is not auto-bootable; staying in Recovery",
                 (int)state);
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(s_update.target);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Existing ota_0 is valid; booting dashboard");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    ESP_LOGW(TAG, "No valid ota_0 image; staying in Recovery (%s)",
             esp_err_to_name(err));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_update.target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "main");
    if (!s_update.target) {
        ESP_LOGE(TAG, "Fixed ota_0 partition 'main' not found");
    }

    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
        CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    general.rx_queue_len = 64;
    general.tx_queue_len = 16;
    general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                             TWAI_ALERT_RX_QUEUE_FULL;
    twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    ESP_ERROR_CHECK(twai_driver_install(&general, &timing, &filter));
    ESP_ERROR_CHECK(twai_start());

    bool requested = request_marker_is_set();
    ESP_LOGW(TAG, "CAN Recovery ready: RX=0x%03X TX=0x%03X requested=%d",
             CAN_OTA_RX_ID, CAN_OTA_TX_ID, requested);
    send_ready();
    maybe_boot_existing_main(requested);
    recovery_loop();
}

/*
 * Based on Espressif's ESP-IDF bt_pan_panu example.
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_event.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pan_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "nvs_flash.h"

#include "bt_app_core_utils.h"
#include "pan_netif.h"

#define TAG "PANU"
#define ETH_ADDR_LEN 6

static const char s_iphone_name[] = CONFIG_PAN_PEER_DEVICE_NAME;
static esp_bd_addr_t s_iphone_address;
static char s_discovered_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static uint8_t s_discovered_name_length;
static uint8_t s_local_mac[ETH_ADDR_LEN];
static bool s_iphone_found;
static bool s_iphone_address_known;
static TimerHandle_t s_retry_timer;

static const esp_bt_inq_mode_t s_inquiry_mode = ESP_BT_INQ_MODE_GENERAL_INQUIRY;
static const uint8_t s_inquiry_length = 5;
static const uint8_t s_inquiry_response_limit = 0;

static void start_inquiry(const char *reason);
static void connect_to_iphone(void);

static char *format_bda(const uint8_t *bda, char *buffer, size_t size)
{
    if (bda == NULL || buffer == NULL || size < 18) {
        return NULL;
    }
    snprintf(buffer, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return buffer;
}

static void bdaddr_to_eth_mac(const uint8_t *bda, uint8_t *mac)
{
    /* iPhone PAN identifies the client by its BD_ADDR; keep it unchanged. */
    memcpy(mac, bda, ETH_ADDR_LEN);
}

static bool read_name_from_eir(uint8_t *eir, char *name, uint8_t *name_length)
{
    if (eir == NULL) {
        return false;
    }

    uint8_t length = 0;
    uint8_t *value = esp_bt_gap_resolve_eir_data(
        eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &length);
    if (value == NULL) {
        value = esp_bt_gap_resolve_eir_data(
            eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &length);
    }
    if (value == NULL) {
        return false;
    }

    if (length > ESP_BT_GAP_MAX_BDNAME_LEN) {
        length = ESP_BT_GAP_MAX_BDNAME_LEN;
    }
    if (name != NULL) {
        memcpy(name, value, length);
        name[length] = '\0';
    }
    if (name_length != NULL) {
        *name_length = length;
    }
    return true;
}

static bool read_name_from_property(const esp_bt_gap_dev_prop_t *property,
                                    char *name, uint8_t *name_length)
{
    if (property == NULL || property->val == NULL || name == NULL ||
        name_length == NULL) {
        return false;
    }

    if (property->type == ESP_BT_GAP_DEV_PROP_EIR) {
        return read_name_from_eir(property->val, name, name_length);
    }
    if (property->type != ESP_BT_GAP_DEV_PROP_BDNAME || property->len <= 0) {
        return false;
    }

    size_t length = strnlen(property->val, property->len);
    if (length > ESP_BT_GAP_MAX_BDNAME_LEN) {
        length = ESP_BT_GAP_MAX_BDNAME_LEN;
    }
    memcpy(name, property->val, length);
    name[length] = '\0';
    *name_length = (uint8_t)length;
    return true;
}

static void retry_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (s_iphone_address_known) {
        ESP_LOGI(TAG, "retry delay elapsed; reconnecting to paired iPhone");
        connect_to_iphone();
    } else {
        ESP_LOGI(TAG, "retry delay elapsed; looking for iPhone again");
        start_inquiry(NULL);
    }
}

static void schedule_retry(const char *reason)
{
    ESP_LOGW(TAG,
             "%s; ACTION REQUIRED: enable iPhone Personal Hotspot > "
             "Allow Others to Join. Retrying in %d seconds",
             reason, CONFIG_PAN_RETRY_DELAY_SECONDS);
    s_iphone_found = false;

    if (xTimerChangePeriod(s_retry_timer,
                           pdMS_TO_TICKS(CONFIG_PAN_RETRY_DELAY_SECONDS * 1000),
                           0) != pdPASS) {
        ESP_LOGE(TAG, "failed to schedule PAN retry");
    }
}

static void start_inquiry(const char *reason)
{
    if (reason != NULL) {
        ESP_LOGW(TAG, "%s; scanning for iPhone \"%s\"", reason, s_iphone_name);
    } else {
        ESP_LOGI(TAG, "scanning for iPhone \"%s\"", s_iphone_name);
    }
    s_iphone_found = false;
    esp_err_t err = esp_bt_gap_start_discovery(s_inquiry_mode, s_inquiry_length,
                                                s_inquiry_response_limit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start Bluetooth inquiry: %s", esp_err_to_name(err));
        schedule_retry("Bluetooth inquiry could not start");
    }
}

static void connect_to_iphone(void)
{
    char address[18] = {0};
    ESP_LOGI(TAG, "connecting PANU -> iPhone NAP %s",
             format_bda(s_iphone_address, address, sizeof(address)));

    esp_err_t err = esp_pan_connect(s_iphone_address, ESP_PAN_ROLE_PANU,
                                    ESP_PAN_ROLE_NAP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PAN connection request failed: %s", esp_err_to_name(err));
        schedule_retry("PAN connection request rejected");
    }
}

static void load_single_bonded_device(void)
{
    int bonded_count = esp_bt_gap_get_bond_device_num();
    if (bonded_count != 1) {
        if (bonded_count > 1) {
            ESP_LOGW(TAG, "%d paired devices found; scanning by configured name",
                     bonded_count);
        }
        return;
    }

    int requested_count = 1;
    esp_bd_addr_t bonded_address;
    esp_err_t err = esp_bt_gap_get_bond_device_list(&requested_count,
                                                     &bonded_address);
    if (err != ESP_OK || requested_count != 1) {
        ESP_LOGW(TAG, "failed to read paired iPhone address: %s",
                 esp_err_to_name(err));
        return;
    }

    memcpy(s_iphone_address, bonded_address, ESP_BD_ADDR_LEN);
    s_iphone_address_known = true;
    char address[18] = {0};
    ESP_LOGI(TAG, "using the only paired Bluetooth device %s as iPhone",
             format_bda(s_iphone_address, address, sizeof(address)));
}

static void handle_pan_event(uint16_t event, void *event_data)
{
    esp_pan_cb_param_t *param = event_data;
    char address[18] = {0};

    switch (event) {
    case ESP_PAN_INIT_EVT:
        if (param->init.status == ESP_PAN_SUCCESS) {
            ESP_ERROR_CHECK(esp_bt_gap_set_device_name(CONFIG_PAN_LOCAL_DEVICE_NAME));
            ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                      ESP_BT_GENERAL_DISCOVERABLE));
            if (s_iphone_address_known) {
                connect_to_iphone();
            } else {
                start_inquiry(NULL);
            }
        } else {
            ESP_LOGE(TAG, "PAN initialization failed, status=%d", param->init.status);
        }
        break;
    case ESP_PAN_SET_ROLE_EVT:
        ESP_LOGI(TAG, "PANU role registered, status=%d", param->set_role.status);
        break;
    case ESP_PAN_OPENING_EVT:
        ESP_LOGI(TAG, "PAN opening, handle=%u, peer=%s", param->opening.handle,
                 format_bda(param->opening.remote_bda, address, sizeof(address)));
        break;
    case ESP_PAN_OPEN_EVT:
        if (param->open.status == ESP_PAN_SUCCESS) {
            xTimerStop(s_retry_timer, 0);
            ESP_LOGI(TAG, "connected to iPhone PAN, handle=%u, peer=%s",
                     param->open.handle,
                     format_bda(param->open.remote_bda, address, sizeof(address)));
            pan_netif_on_connected(param->open.handle);
        } else {
            ESP_LOGE(TAG, "PAN connection failed, status=%d", param->open.status);
            schedule_retry("iPhone PAN service is unavailable");
        }
        break;
    case ESP_PAN_CLOSE_EVT:
        ESP_LOGW(TAG, "iPhone PAN disconnected, handle=%u", param->close.handle);
        pan_netif_on_disconnected();
        schedule_retry("iPhone PAN disconnected");
        break;
    case ESP_PAN_CONG_EVT:
        pan_netif_on_congestion(param->cong.cong);
        break;
    case ESP_PAN_WRITE_EVT:
        if (param->write.status != ESP_PAN_SUCCESS) {
            ESP_LOGE(TAG, "PAN write failed, handle=%u, status=%d",
                     param->write.handle, param->write.status);
        }
        break;
    case ESP_PAN_DEINIT_EVT:
        ESP_LOGI(TAG, "PAN deinitialized, status=%d", param->deinit.status);
        break;
    default:
        break;
    }
}

static void pan_callback(esp_pan_cb_event_t event, esp_pan_cb_param_t *param)
{
    if (event == ESP_PAN_DATA_IND_EVT) {
        esp_err_t err = pan_netif_input(param->data_ind.dst, param->data_ind.src,
                                        param->data_ind.protocol, param->data_ind.data,
                                        param->data_ind.len);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "dropped PAN RX frame: %s", esp_err_to_name(err));
        }
        return;
    }

    bt_app_work_dispatch(handle_pan_event, event, param, sizeof(*param), NULL, NULL);
}

static void handle_gap_event(uint16_t event, void *event_data)
{
    esp_bt_gap_cb_param_t *param = event_data;

    switch (event) {
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            if (s_iphone_found) {
                connect_to_iphone();
            } else {
                schedule_retry("iPhone not found or not discoverable");
            }
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "pairing failed, status=%d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
        ESP_LOGI(TAG, "legacy pairing PIN: 1234");
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "confirm the same number on iPhone: %06" PRIu32,
                 param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "enter this passkey on iPhone if requested: %06" PRIu32,
                 param->key_notif.passkey);
        break;
    default:
        break;
    }
}

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        for (int i = 0; i < param->disc_res.num_prop; ++i) {
            if (!read_name_from_property(&param->disc_res.prop[i],
                                         s_discovered_name,
                                         &s_discovered_name_length)) {
                continue;
            }

            if (!s_iphone_found &&
                strlen(s_iphone_name) == s_discovered_name_length &&
                memcmp(s_discovered_name, s_iphone_name,
                       s_discovered_name_length) == 0) {
                s_iphone_found = true;
                memcpy(s_iphone_address, param->disc_res.bda, ESP_BD_ADDR_LEN);
                s_iphone_address_known = true;
                ESP_LOGI(TAG, "found iPhone \"%s\"; stopping discovery",
                         s_discovered_name);
                esp_err_t err = esp_bt_gap_cancel_discovery();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "failed to stop discovery: %s",
                             esp_err_to_name(err));
                }
                break;
            }
        }
        return;
    }

    switch (event) {
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
    case ESP_BT_GAP_AUTH_CMPL_EVT:
    case ESP_BT_GAP_PIN_REQ_EVT:
    case ESP_BT_GAP_CFM_REQ_EVT:
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        bt_app_work_dispatch(handle_gap_event, event, param, sizeof(*param), NULL, NULL);
        break;
    default:
        break;
    }
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    if (strcmp(s_iphone_name, "CHANGE_ME_IPHONE_NAME") == 0) {
        ESP_LOGE(TAG, "set CONFIG_PAN_PEER_DEVICE_NAME to the iPhone name first");
        return;
    }

    s_retry_timer = xTimerCreate("pan_retry",
                                 pdMS_TO_TICKS(CONFIG_PAN_RETRY_DELAY_SECONDS * 1000),
                                 pdFALSE, NULL, retry_timer_callback);
    if (s_retry_timer == NULL) {
        ESP_LOGE(TAG, "failed to create PAN retry timer");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&controller_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bluedroid_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_config));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    load_single_bonded_device();

    bt_app_task_start_up();

    esp_bt_io_cap_t io_capability = ESP_BT_IO_CAP_IO;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,
                                                  &io_capability,
                                                  sizeof(uint8_t)));
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_callback));
    ESP_ERROR_CHECK(esp_pan_register_callback(pan_callback));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code = {0};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(pin_type, 0, pin_code));

    const uint8_t *bt_address = esp_bt_dev_get_address();
    bdaddr_to_eth_mac(bt_address, s_local_mac);
    ESP_ERROR_CHECK(pan_netif_init(s_local_mac));

    esp_pan_cfg_t pan_config = ESP_PAN_DEFAULT_CONFIG();
    pan_config.role = ESP_PAN_ROLE_PANU;
    ESP_ERROR_CHECK(esp_pan_init(&pan_config));

    char address[18] = {0};
    ESP_LOGI(TAG, "ESP32 Bluetooth address %s",
             format_bda(bt_address, address, sizeof(address)));
    ESP_LOGI(TAG, "target iPhone: %s", s_iphone_name);
}

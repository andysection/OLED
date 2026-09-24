/*
 * Based on Espressif's bt_app_core_utils component.
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bt_app_core_utils.h"

#define TAG "BT_APP_CORE"

static QueueHandle_t s_queue;

static bool send_message(bt_app_msg_t *message)
{
    if (message == NULL || s_queue == NULL) {
        ESP_LOGE(TAG, "dispatch queue is not ready");
        return false;
    }
    if (xQueueSend(s_queue, message, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGE(TAG, "dispatch queue is full");
        return false;
    }
    return true;
}

static void task_handler(void *arg)
{
    (void)arg;
    bt_app_msg_t message;

    for (;;) {
        if (xQueueReceive(s_queue, &message, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (message.sig == BT_APP_SIG_WORK_DISPATCH && message.cb != NULL) {
            message.cb(message.event, message.param);
        }
        if (message.param != NULL) {
            if (message.free_cb != NULL) {
                message.free_cb(message.param);
            }
            free(message.param);
        }
    }
}

bool bt_app_work_dispatch(bt_app_cb_t callback, uint16_t event, void *params, int param_len,
                          bt_app_copy_cb_t copy_callback, bt_app_free_cb_t free_callback)
{
    bt_app_msg_t message = {
        .sig = BT_APP_SIG_WORK_DISPATCH,
        .event = event,
        .cb = callback,
        .free_cb = free_callback,
    };

    if (param_len == 0) {
        return send_message(&message);
    }
    if (params == NULL || param_len < 0) {
        ESP_LOGE(TAG, "invalid dispatch arguments");
        return false;
    }

    message.param = malloc(param_len);
    if (message.param == NULL) {
        ESP_LOGE(TAG, "dispatch allocation failed, free heap=%" PRIu32,
                 esp_get_free_heap_size());
        return false;
    }

    memcpy(message.param, params, param_len);
    if (copy_callback != NULL) {
        copy_callback(message.param, params, param_len);
    }
    if (!send_message(&message)) {
        if (free_callback != NULL) {
            free_callback(message.param);
        }
        free(message.param);
        return false;
    }
    return true;
}

void bt_app_task_start_up(void)
{
    s_queue = xQueueCreate(10, sizeof(bt_app_msg_t));
    configASSERT(s_queue != NULL);

    BaseType_t created = xTaskCreate(task_handler, "bt_app", 4096, NULL, 10, NULL);
    configASSERT(created == pdPASS);
}

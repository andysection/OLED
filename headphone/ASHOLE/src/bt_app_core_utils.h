/* SPDX-License-Identifier: Unlicense OR CC0-1.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BT_APP_SIG_WORK_DISPATCH 0x01

typedef void (*bt_app_cb_t)(uint16_t event, void *param);
typedef void (*bt_app_free_cb_t)(void *param);
typedef void (*bt_app_copy_cb_t)(void *dest, void *src, int len);

typedef struct {
    uint16_t sig;
    uint16_t event;
    bt_app_cb_t cb;
    bt_app_free_cb_t free_cb;
    void *param;
} bt_app_msg_t;

bool bt_app_work_dispatch(bt_app_cb_t callback, uint16_t event, void *params, int param_len,
                          bt_app_copy_cb_t copy_callback, bt_app_free_cb_t free_callback);
void bt_app_task_start_up(void);

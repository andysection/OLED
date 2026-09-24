/* SPDX-License-Identifier: Unlicense OR CC0-1.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t pan_netif_init(const uint8_t *mac);
void pan_netif_on_connected(uint16_t handle);
void pan_netif_on_disconnected(void);
void pan_netif_on_congestion(bool congested);
esp_err_t pan_netif_input(const uint8_t *dst, const uint8_t *src, uint16_t protocol,
                          const uint8_t *payload, uint16_t len);

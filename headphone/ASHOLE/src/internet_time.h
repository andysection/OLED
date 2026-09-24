/* SPDX-License-Identifier: Unlicense OR CC0-1.0 */

#pragma once

#include "esp_err.h"

esp_err_t internet_time_init(void);
void internet_time_on_network_up(void);
void internet_time_on_network_down(void);

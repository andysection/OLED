/* SPDX-License-Identifier: Unlicense OR CC0-1.0 */

#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "lwip/apps/sntp.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

#include "internet_time.h"

#define TAG "INTERNET_TIME"

static TimerHandle_t s_print_timer;
static bool s_network_up;
static ip_addr_t s_resolved_server;

static void start_sntp_on_tcpip(void *arg)
{
    (void)arg;
    if (!s_network_up) {
        return;
    }

    sntp_stop();
    sntp_setserver(0, &s_resolved_server);
    ESP_LOGI(TAG, "requesting time from %s (%s)", CONFIG_PAN_SNTP_SERVER,
             ipaddr_ntoa(&s_resolved_server));
    sntp_init();
}

static void stop_sntp_on_tcpip(void *arg)
{
    (void)arg;
    sntp_stop();
}

static void time_server_resolved(const char *name, const ip_addr_t *address,
                                 void *arg)
{
    (void)arg;
    if (!s_network_up) {
        return;
    }
    if (address == NULL) {
        ESP_LOGE(TAG, "failed to resolve time server %s", name);
        return;
    }

    ip_addr_copy(s_resolved_server, *address);
    if (tcpip_callback(start_sntp_on_tcpip, NULL) != ERR_OK) {
        ESP_LOGE(TAG, "failed to schedule SNTP start");
    }
}

static void print_current_time(void)
{
    time_t now = 0;
    struct tm local_time = {0};
    char buffer[48] = {0};

    time(&now);
    if (localtime_r(&now, &local_time) == NULL ||
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S %Z", &local_time) == 0) {
        ESP_LOGE(TAG, "failed to format local time");
        return;
    }

    ESP_LOGI(TAG, "current time: %s", buffer);
}

static void print_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (s_network_up) {
        print_current_time();
    }
}

static void time_sync_callback(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "time synchronized from %s", CONFIG_PAN_SNTP_SERVER);
    print_current_time();

    if (s_network_up &&
        xTimerChangePeriod(s_print_timer,
                           pdMS_TO_TICKS(CONFIG_PAN_TIME_PRINT_INTERVAL_SECONDS *
                                        1000),
                           0) != pdPASS) {
        ESP_LOGE(TAG, "failed to start time print timer");
    }
}

esp_err_t internet_time_init(void)
{
    if (setenv("TZ", CONFIG_PAN_TIME_ZONE, 1) != 0) {
        return ESP_FAIL;
    }
    tzset();

    s_print_timer = xTimerCreate(
        "time_print",
        pdMS_TO_TICKS(CONFIG_PAN_TIME_PRINT_INTERVAL_SECONDS * 1000),
        pdTRUE, NULL, print_timer_callback);
    if (s_print_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_PAN_SNTP_SERVER);
    config.start = false;
    config.wait_for_sync = false;
    config.sync_cb = time_sync_callback;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        xTimerDelete(s_print_timer, 0);
        s_print_timer = NULL;
    }
    return err;
}

void internet_time_on_network_up(void)
{
    s_network_up = true;
    xTimerStop(s_print_timer, 0);
    ESP_LOGI(TAG, "resolving time server %s with IPv6/DNS64 preference",
             CONFIG_PAN_SNTP_SERVER);

    ip_addr_t address = {0};
    err_t err = dns_gethostbyname_addrtype(
        CONFIG_PAN_SNTP_SERVER, &address, time_server_resolved, NULL,
        LWIP_DNS_ADDRTYPE_IPV6_IPV4);
    if (err == ERR_OK) {
        time_server_resolved(CONFIG_PAN_SNTP_SERVER, &address, NULL);
    } else if (err != ERR_INPROGRESS) {
        ESP_LOGE(TAG, "time server DNS request failed: %d", err);
    }
}

void internet_time_on_network_down(void)
{
    s_network_up = false;
    if (s_print_timer != NULL) {
        xTimerStop(s_print_timer, 0);
    }
    if (tcpip_callback(stop_sntp_on_tcpip, NULL) != ERR_OK) {
        ESP_LOGE(TAG, "failed to schedule SNTP stop");
    }
}

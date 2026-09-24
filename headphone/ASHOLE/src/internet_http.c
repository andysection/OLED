/* SPDX-License-Identifier: Unlicense OR CC0-1.0 */

#include <stdbool.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "internet_http.h"

#define TAG "INTERNET_HTTP"

static TaskHandle_t s_http_task;
static volatile bool s_network_up;
static volatile bool s_rerun_requested;

static void perform_request(const char *label, const char *url,
                            esp_http_client_method_t method, const char *body)
{
    if (!s_network_up) {
        return;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = CONFIG_PAN_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "%s: failed to create HTTP client", label);
        return;
    }

    if (body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    ESP_LOGI(TAG, "%s %s", label, url);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s succeeded: HTTP %d, content length=%lld", label,
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "%s failed: %s", label, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void http_test_task(void *arg)
{
    (void)arg;
    do {
        s_rerun_requested = false;
        perform_request("GET API", CONFIG_PAN_HTTP_GET_URL,
                        HTTP_METHOD_GET, NULL);
        perform_request("POST API", CONFIG_PAN_HTTP_POST_URL,
                        HTTP_METHOD_POST,
                        "{\"source\":\"esp32-bluetooth-pan\"}");
    } while (s_network_up && s_rerun_requested);
    s_http_task = NULL;
    vTaskDelete(NULL);
}

void internet_http_on_network_up(void)
{
    s_network_up = true;
    if (s_http_task != NULL) {
        s_rerun_requested = true;
        ESP_LOGI(TAG, "HTTP test already running; another pass is queued");
        return;
    }
    if (xTaskCreate(http_test_task, "pan_http", 6144, NULL, 5,
                    &s_http_task) != pdPASS) {
        s_http_task = NULL;
        ESP_LOGE(TAG, "failed to create HTTP test task");
    }
}

void internet_http_on_network_down(void)
{
    s_network_up = false;
}

/*
 * Based on Espressif's ESP-IDF bt_pan_panu example.
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_pan_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#include "internet_http.h"
#include "internet_time.h"
#include "pan_netif.h"

#define TAG "PAN_NETIF"
#define ETH_HEADER_LEN 14
#define ETH_TYPE_IPV6 0x86DD

typedef struct {
    esp_netif_driver_base_t base;
    uint16_t pan_handle;
    uint8_t mac[6];
    bool congested;
    uint32_t tx_frames;
    uint32_t tx_errors;
    uint32_t rx_frames;
    uint32_t rx_loopback_frames;
} pan_netif_glue_t;

static esp_netif_t *s_pan_netif;
static pan_netif_glue_t *s_glue;
static esp_ping_handle_t s_ping;
static TimerHandle_t s_dhcp_timer;
static TimerHandle_t s_ping_timer;
static bool s_link_up;
static bool s_has_ipv4;
static bool s_has_ipv6;
static bool s_network_up;

static void start_ping(void);

static bool has_usable_address(void)
{
    return s_has_ipv4 || s_has_ipv6;
}

static uint16_t read_be16(const uint8_t *value)
{
    return ((uint16_t)value[0] << 8) | value[1];
}

static void log_pan_frame(const char *direction, uint32_t sequence,
                          uint16_t protocol, const uint8_t *payload, size_t len,
                          const uint8_t *dst, const uint8_t *src)
{
    if (protocol == ESP_PAN_PROTO_IP && payload != NULL && len >= 28 &&
        (payload[0] >> 4) == 4) {
        size_t ip_header_len = (payload[0] & 0x0f) * 4;
        if (ip_header_len >= 20 && len >= ip_header_len + 8 && payload[9] == 17) {
            uint16_t src_port = read_be16(payload + ip_header_len);
            uint16_t dst_port = read_be16(payload + ip_header_len + 2);
            if ((src_port == 68 && dst_port == 67) ||
                (src_port == 67 && dst_port == 68)) {
                ESP_LOGI(TAG,
                         "PAN %s #%" PRIu32 " DHCP UDP %u -> %u, len=%u, "
                         "MAC %02x:%02x:%02x:%02x:%02x:%02x -> "
                         "%02x:%02x:%02x:%02x:%02x:%02x",
                         direction, sequence, src_port, dst_port, (unsigned)len,
                         src[0], src[1], src[2], src[3], src[4], src[5],
                         dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
                return;
            }
        }
    }

    if (protocol == ETH_TYPE_IPV6 && payload != NULL && len >= 41 &&
        (payload[0] >> 4) == 6 && payload[6] == 58) {
        ESP_LOGI(TAG,
                 "PAN %s #%" PRIu32 " IPv6 ICMP type=%u, len=%u, "
                 "MAC %02x:%02x:%02x:%02x:%02x:%02x -> "
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 direction, sequence, payload[40], (unsigned)len,
                 src[0], src[1], src[2], src[3], src[4], src[5],
                 dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
        return;
    }

    if (sequence <= 8) {
        ESP_LOGI(TAG,
                 "PAN %s #%" PRIu32 " protocol=0x%04x, len=%u, "
                 "MAC %02x:%02x:%02x:%02x:%02x:%02x -> "
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 direction, sequence, protocol, (unsigned)len,
                 src[0], src[1], src[2], src[3], src[4], src[5],
                 dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
    }
}

static esp_err_t pan_transmit(void *handle, void *data, size_t len)
{
    pan_netif_glue_t *glue = handle;
    if (glue == NULL || glue->pan_handle == ESP_PAN_INVALID_HANDLE ||
        glue->congested || data == NULL || len < ETH_HEADER_LEN) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *frame = data;
    uint16_t protocol = ((uint16_t)frame[12] << 8) | frame[13];
    uint16_t payload_len = (uint16_t)(len - ETH_HEADER_LEN);

    uint32_t sequence = ++glue->tx_frames;
    log_pan_frame("TX", sequence, protocol, frame + ETH_HEADER_LEN, payload_len,
                  frame, frame + 6);

    esp_err_t err = esp_pan_write(glue->pan_handle, frame, frame + 6, protocol,
                                  payload_len, frame + ETH_HEADER_LEN, false);
    if (err != ESP_OK) {
        ++glue->tx_errors;
        ESP_LOGE(TAG, "PAN TX #" PRIu32 " rejected: %s", sequence,
                 esp_err_to_name(err));
    }
    return err;
}

static void free_rx_buffer(void *handle, void *buffer)
{
    (void)handle;
    free(buffer);
}

static esp_err_t post_attach(esp_netif_t *netif, void *args)
{
    pan_netif_glue_t *glue = args;
    glue->base.netif = netif;

    esp_netif_driver_ifconfig_t driver_config = {
        .handle = glue,
        .transmit = pan_transmit,
        .driver_free_rx_buffer = free_rx_buffer,
    };

    ESP_ERROR_CHECK(esp_netif_set_driver_config(netif, &driver_config));
    ESP_ERROR_CHECK(esp_netif_set_mac(netif, glue->mac));
    ESP_LOGI(TAG, "PAN interface MAC %02x:%02x:%02x:%02x:%02x:%02x",
             glue->mac[0], glue->mac[1], glue->mac[2],
             glue->mac[3], glue->mac[4], glue->mac[5]);
    return ESP_OK;
}

static void ping_success(esp_ping_handle_t handle, void *args)
{
    (void)args;
    uint8_t ttl;
    uint16_t sequence;
    uint32_t elapsed_ms;
    uint32_t received_length;
    ip_addr_t target;

    esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &sequence, sizeof(sequence));
    esp_ping_get_profile(handle, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(handle, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    esp_ping_get_profile(handle, ESP_PING_PROF_SIZE, &received_length, sizeof(received_length));
    esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    ESP_LOGI(TAG, "%" PRIu32 " bytes from %s: seq=%u ttl=%u time=%" PRIu32 " ms",
             received_length, ipaddr_ntoa(&target), sequence, ttl, elapsed_ms);
}

static void ping_timeout(esp_ping_handle_t handle, void *args)
{
    (void)args;
    uint16_t sequence;
    ip_addr_t target;

    esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &sequence, sizeof(sequence));
    esp_ping_get_profile(handle, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    ESP_LOGW(TAG, "ping %s seq=%u timed out", ipaddr_ntoa(&target), sequence);
}

static void ping_end(esp_ping_handle_t handle, void *args)
{
    (void)args;
    ip_addr_t target;
    uint32_t transmitted;
    uint32_t received;
    uint32_t duration_ms;

    esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(handle, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(handle, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    esp_ping_get_profile(handle, ESP_PING_PROF_DURATION, &duration_ms, sizeof(duration_ms));

    uint32_t loss = transmitted == 0 ? 100 : 100 - (received * 100 / transmitted);
    ESP_LOGI(TAG, "%s: sent=%" PRIu32 ", received=%" PRIu32
             ", loss=%" PRIu32 "%%, time=%" PRIu32 " ms",
             ipaddr_ntoa(&target), transmitted, received, loss, duration_ms);

    if (received > 0) {
        ESP_LOGI(TAG, "Internet test PASSED; next test in %d seconds",
                 CONFIG_PAN_NETWORK_TEST_INTERVAL_SECONDS);
    } else {
        ESP_LOGW(TAG,
                 "Internet test FAILED; check iPhone cellular data and Personal "
                 "Hotspot. Retrying in %d seconds",
                 CONFIG_PAN_NETWORK_TEST_INTERVAL_SECONDS);
    }

    if (has_usable_address() &&
        xTimerChangePeriod(s_ping_timer,
                           pdMS_TO_TICKS(CONFIG_PAN_NETWORK_TEST_INTERVAL_SECONDS *
                                        1000),
                           0) != pdPASS) {
        ESP_LOGE(TAG, "failed to schedule the next Internet test");
    }
}

static void ping_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (!has_usable_address() || s_ping == NULL) {
        return;
    }

    ESP_LOGI(TAG, "running scheduled Internet test");
    esp_err_t err = esp_ping_start(s_ping);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to restart ping: %s", esp_err_to_name(err));
    }
}

static void dhcp_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (s_has_ipv4 || !s_link_up || s_glue == NULL ||
        s_glue->pan_handle == ESP_PAN_INVALID_HANDLE) {
        return;
    }

#if CONFIG_PAN_STATIC_IPV4_FALLBACK
    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_str_to_ip4(CONFIG_PAN_STATIC_IPV4_ADDRESS, &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_PAN_STATIC_IPV4_GATEWAY, &ip_info.gw) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_PAN_STATIC_IPV4_NETMASK, &ip_info.netmask) != ESP_OK) {
        ESP_LOGE(TAG, "invalid static IPv4 fallback configuration");
        return;
    }

    esp_err_t stop_err = esp_netif_dhcpc_stop(s_pan_netif);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "failed to stop DHCP before static IPv4 fallback: %s",
                 esp_err_to_name(stop_err));
        return;
    }

    esp_err_t static_err = esp_netif_set_ip_info(s_pan_netif, &ip_info);
    if (static_err == ESP_OK) {
        esp_netif_dns_info_t dns_info = {0};
        if (esp_netif_str_to_ip4(CONFIG_PAN_IPV4_DNS_SERVER,
                                 &dns_info.ip.u_addr.ip4) == ESP_OK) {
            dns_info.ip.type = ESP_IPADDR_TYPE_V4;
            esp_err_t dns_err = esp_netif_set_dns_info(
                s_pan_netif, ESP_NETIF_DNS_MAIN, &dns_info);
            if (dns_err != ESP_OK) {
                ESP_LOGE(TAG, "failed to configure static IPv4 DNS: %s",
                         esp_err_to_name(dns_err));
            }
        }
        ESP_LOGW(TAG,
                 "no DHCP offer after %d seconds; testing iPhone hotspot with "
                 "static IPv4 " IPSTR ", gateway " IPSTR,
                 CONFIG_PAN_DHCP_TIMEOUT_SECONDS, IP2STR(&ip_info.ip),
                 IP2STR(&ip_info.gw));
        return;
    }
    ESP_LOGE(TAG, "failed to apply static IPv4 fallback: %s",
             esp_err_to_name(static_err));
#endif

    if (has_usable_address()) {
        ESP_LOGW(TAG, "IPv4 unavailable; continuing with IPv6 only");
        return;
    }

    uint16_t handle = s_glue->pan_handle;
    ESP_LOGW(TAG,
             "no usable IPv4/IPv6 address after %d seconds; ACTION REQUIRED: "
             "enable iPhone Personal Hotspot > Allow Others to Join. Frames: TX=%" PRIu32
             ", TX errors=%" PRIu32 ", RX=%" PRIu32 ". Reconnecting",
             CONFIG_PAN_DHCP_TIMEOUT_SECONDS, s_glue->tx_frames,
             s_glue->tx_errors, s_glue->rx_frames);
    esp_err_t err = esp_pan_disconnect(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to close PAN after DHCP timeout: %s",
                 esp_err_to_name(err));
    }
}

static void stop_ping(void)
{
    if (s_ping_timer != NULL) {
        xTimerStop(s_ping_timer, 0);
    }
    if (s_ping == NULL) {
        return;
    }

    esp_ping_handle_t handle = s_ping;
    s_ping = NULL;
    esp_ping_stop(handle);
    esp_ping_delete_session(handle);
}

static bool resolve_ping_target(const char *target_text, ip_addr_t *target)
{
    if (ipaddr_aton(target_text, target)) {
        return true;
    }

    struct addrinfo hints = {
        .ai_family = s_has_ipv4 ? AF_INET : AF_INET6,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *result = NULL;
    ESP_LOGI(TAG, "resolving Internet test host %s as %s", target_text,
             s_has_ipv4 ? "IPv4" : "IPv6/DNS64");
    int err = getaddrinfo(target_text, NULL, &hints, &result);
    if (err != 0 || result == NULL) {
        ESP_LOGE(TAG, "failed to resolve %s (getaddrinfo=%d)", target_text, err);
        return false;
    }

    if (result->ai_family == AF_INET) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)result->ai_addr;
        ip4_addr_t ipv4;
        inet_addr_to_ip4addr(&ipv4, &address->sin_addr);
        ip_addr_copy_from_ip4(*target, ipv4);
    } else if (result->ai_family == AF_INET6) {
        const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)result->ai_addr;
        ip6_addr_t ipv6;
        inet6_addr_to_ip6addr(&ipv6, &address->sin6_addr);
        ip_addr_copy_from_ip6(*target, ipv6);
    } else {
        freeaddrinfo(result);
        ESP_LOGE(TAG, "resolver returned unsupported address family for %s", target_text);
        return false;
    }

    freeaddrinfo(result);
    ESP_LOGI(TAG, "resolved %s to %s", target_text, ipaddr_ntoa(target));
    return true;
}

static void start_ping(void)
{
    if (s_ping != NULL || s_pan_netif == NULL) {
        return;
    }

    const char *target_text = s_has_ipv4 ? CONFIG_PAN_PING_TARGET
                                         : CONFIG_PAN_PING_TARGET_IPV6;
    ip_addr_t target = {0};
    if (!resolve_ping_target(target_text, &target)) {
        return;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target;
    config.count = CONFIG_PAN_PING_COUNT;
    config.timeout_ms = CONFIG_PAN_PING_TIMEOUT_MS;
    config.interface = esp_netif_get_netif_impl_index(s_pan_netif);

    esp_ping_callbacks_t callbacks = {
        .on_ping_success = ping_success,
        .on_ping_timeout = ping_timeout,
        .on_ping_end = ping_end,
    };

    esp_ping_handle_t handle = NULL;
    if (esp_ping_new_session(&config, &callbacks, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create ping session");
        return;
    }

    s_ping = handle;
    ESP_LOGI(TAG, "testing Internet access with %s", target_text);
    esp_err_t err = esp_ping_start(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start ping: %s", esp_err_to_name(err));
        s_ping = NULL;
        esp_ping_delete_session(handle);
    }
}

static void network_became_available(void)
{
    if (s_has_ipv4) {
        xTimerStop(s_dhcp_timer, 0);
    }
    if (s_network_up) {
        return;
    }

    s_network_up = true;
    internet_time_on_network_up();
    internet_http_on_network_up();
    start_ping();
}

static void network_became_unavailable(void)
{
    if (!s_network_up) {
        return;
    }

    s_network_up = false;
    stop_ping();
    internet_time_on_network_down();
    internet_http_on_network_down();
}

static void got_ip_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                           void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip_t *event = event_data;
    if (event == NULL || event->esp_netif != s_pan_netif || !s_link_up) {
        return;
    }

    bool first_ipv4 = !s_has_ipv4;
    s_has_ipv4 = true;
    xTimerStop(s_dhcp_timer, 0);
    ESP_LOGI(TAG, "iPhone assigned IP " IPSTR ", mask " IPSTR ", gateway " IPSTR,
             IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
             IP2STR(&event->ip_info.gw));
    if (s_network_up && first_ipv4) {
        stop_ping();
        start_ping();
        internet_http_on_network_up();
        return;
    }
    network_became_available();
}

static void lost_ip_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                            void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip_t *event = event_data;
    if (event == NULL || event->esp_netif != s_pan_netif || !s_link_up) {
        return;
    }

    s_has_ipv4 = false;
    if (has_usable_address()) {
        ESP_LOGW(TAG, "PAN IPv4 address lost; continuing with IPv6");
        return;
    }

    network_became_unavailable();
    xTimerChangePeriod(s_dhcp_timer,
                       pdMS_TO_TICKS(CONFIG_PAN_DHCP_TIMEOUT_SECONDS * 1000), 0);
    ESP_LOGW(TAG,
             "PAN IPv4 address lost; waiting %d seconds for a usable address",
             CONFIG_PAN_DHCP_TIMEOUT_SECONDS);
}

static const char *ipv6_type_name(esp_ip6_addr_type_t type)
{
    switch (type) {
    case ESP_IP6_ADDR_IS_GLOBAL:
        return "global";
    case ESP_IP6_ADDR_IS_LINK_LOCAL:
        return "link-local";
    case ESP_IP6_ADDR_IS_SITE_LOCAL:
        return "site-local";
    case ESP_IP6_ADDR_IS_UNIQUE_LOCAL:
        return "unique-local";
    case ESP_IP6_ADDR_IS_IPV4_MAPPED_IPV6:
        return "IPv4-mapped";
    default:
        return "unknown";
    }
}

static void got_ipv6_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip6_t *event = event_data;
    if (event == NULL || event->esp_netif != s_pan_netif || !s_link_up) {
        return;
    }

    esp_ip6_addr_type_t type = esp_netif_ip6_get_addr_type(&event->ip6_info.ip);
    ESP_LOGI(TAG, "iPhone assigned IPv6 " IPV6STR " (%s)",
             IPV62STR(event->ip6_info.ip), ipv6_type_name(type));

    if (type != ESP_IP6_ADDR_IS_GLOBAL &&
        type != ESP_IP6_ADDR_IS_UNIQUE_LOCAL) {
        return;
    }

    esp_netif_dns_info_t dns_info = {0};
    if (esp_netif_str_to_ip6(CONFIG_PAN_IPV6_DNS_SERVER,
                             &dns_info.ip.u_addr.ip6) == ESP_OK) {
        dns_info.ip.type = ESP_IPADDR_TYPE_V6;
        esp_err_t dns_err = esp_netif_set_dns_info(
            s_pan_netif, ESP_NETIF_DNS_FALLBACK, &dns_info);
        if (dns_err == ESP_OK) {
            ESP_LOGI(TAG, "IPv6 fallback DNS configured: %s",
                     CONFIG_PAN_IPV6_DNS_SERVER);
        } else {
            ESP_LOGE(TAG, "failed to configure fallback DNS: %s",
                     esp_err_to_name(dns_err));
        }
    } else {
        ESP_LOGE(TAG, "invalid IPv6 fallback DNS: %s",
                 CONFIG_PAN_IPV6_DNS_SERVER);
    }

    s_has_ipv6 = true;
    network_became_available();
}

esp_err_t pan_netif_init(const uint8_t *mac)
{
    if (s_pan_netif != NULL || mac == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_dhcp_timer = xTimerCreate(
        "pan_dhcp", pdMS_TO_TICKS(CONFIG_PAN_DHCP_TIMEOUT_SECONDS * 1000),
        pdFALSE, NULL, dhcp_timer_callback);
    s_ping_timer = xTimerCreate(
        "pan_ping", pdMS_TO_TICKS(CONFIG_PAN_NETWORK_TEST_INTERVAL_SECONDS * 1000),
        pdFALSE, NULL, ping_timer_callback);
    if (s_dhcp_timer == NULL || s_ping_timer == NULL) {
        if (s_dhcp_timer != NULL) {
            xTimerDelete(s_dhcp_timer, 0);
            s_dhcp_timer = NULL;
        }
        if (s_ping_timer != NULL) {
            xTimerDelete(s_ping_timer, 0);
            s_ping_timer = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    s_glue = calloc(1, sizeof(*s_glue));
    if (s_glue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_glue->base.post_attach = post_attach;
    s_glue->pan_handle = ESP_PAN_INVALID_HANDLE;
    memcpy(s_glue->mac, mac, 6);

    esp_netif_inherent_config_t base_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_config.if_key = "PANU";
    base_config.if_desc = "Bluetooth PANU";
    base_config.route_prio = 60;

    esp_netif_config_t netif_config = {
        .base = &base_config,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    s_pan_netif = esp_netif_new(&netif_config);
    if (s_pan_netif == NULL) {
        free(s_glue);
        s_glue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_attach(s_pan_netif, s_glue));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               got_ip_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                               lost_ip_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6,
                                               got_ipv6_handler, NULL));
    esp_err_t err = internet_time_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize Internet time: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void pan_netif_on_connected(uint16_t handle)
{
    if (s_glue == NULL || s_pan_netif == NULL) {
        return;
    }

    s_glue->pan_handle = handle;
    s_glue->congested = false;
    s_glue->tx_frames = 0;
    s_glue->tx_errors = 0;
    s_glue->rx_frames = 0;
    s_glue->rx_loopback_frames = 0;
    s_link_up = true;
    s_has_ipv4 = false;
    s_has_ipv6 = false;
    s_network_up = false;
    stop_ping();
    internet_time_on_network_down();
    esp_netif_set_default_netif(s_pan_netif);
    esp_netif_action_start(s_pan_netif, 0, 0, 0);
    esp_netif_action_connected(s_pan_netif, 0, 0, 0);
    esp_err_t ipv6_err = esp_netif_create_ip6_linklocal(s_pan_netif);
    if (ipv6_err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create PAN IPv6 link-local address: %s",
                 esp_err_to_name(ipv6_err));
    }
    xTimerChangePeriod(s_dhcp_timer,
                       pdMS_TO_TICKS(CONFIG_PAN_DHCP_TIMEOUT_SECONDS * 1000), 0);
    ESP_LOGI(TAG,
             "PAN link up; waiting for IPv4 DHCP or IPv6 SLAAC (%d second timeout)",
             CONFIG_PAN_DHCP_TIMEOUT_SECONDS);
}

void pan_netif_on_disconnected(void)
{
    if (s_glue == NULL || s_pan_netif == NULL) {
        return;
    }

    s_glue->pan_handle = ESP_PAN_INVALID_HANDLE;
    s_glue->congested = false;
    s_link_up = false;
    s_has_ipv4 = false;
    s_has_ipv6 = false;
    xTimerStop(s_dhcp_timer, 0);
    network_became_unavailable();
    esp_netif_action_disconnected(s_pan_netif, 0, 0, 0);
    esp_netif_action_stop(s_pan_netif, 0, 0, 0);
    ESP_LOGI(TAG, "PAN link down");
}

void pan_netif_on_congestion(bool congested)
{
    if (s_glue != NULL) {
        s_glue->congested = congested;
    }
}

esp_err_t pan_netif_input(const uint8_t *dst, const uint8_t *src, uint16_t protocol,
                          const uint8_t *payload, uint16_t len)
{
    if (s_pan_netif == NULL || dst == NULL || src == NULL || payload == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t sequence = ++s_glue->rx_frames;
    log_pan_frame("RX", sequence, protocol, payload, len, dst, src);

    if (memcmp(src, s_glue->mac, sizeof(s_glue->mac)) == 0) {
        uint32_t dropped = ++s_glue->rx_loopback_frames;
        if (dropped <= 8) {
            ESP_LOGW(TAG,
                     "dropped reflected local PAN frame #%" PRIu32
                     " to protect IPv6 duplicate-address detection",
                     dropped);
        }
        return ESP_OK;
    }

    size_t frame_len = ETH_HEADER_LEN + len;
    uint8_t *frame = malloc(frame_len);
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(frame, dst, 6);
    memcpy(frame + 6, src, 6);
    frame[12] = protocol >> 8;
    frame[13] = protocol & 0xff;
    memcpy(frame + ETH_HEADER_LEN, payload, len);

    /* Match the ESP-IDF PANU example: esp-netif owns frame after this call. */
    return esp_netif_receive(s_pan_netif, frame, frame_len, NULL);
}

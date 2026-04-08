#include "network.h"
#include "rt4k_config.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

static const char *TAG = "network";

/* Event group bits */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static net_status_t s_status = NET_STATUS_DISCONNECTED;
static bool s_sta_mode = false;
static char s_ip_str[16] = "";

/* Scan results cache */
static wifi_ap_record_t s_scan_results[20];
static uint16_t s_scan_count = 0;
static bool s_scan_done = false;
static SemaphoreHandle_t s_scan_mutex = NULL;

/* Pending connection request from web UI */
static bool s_connect_pending = false;
static char s_pending_ssid[32];
static char s_pending_pass[64];

static int s_retry_count = 0;
static TimerHandle_t s_reconnect_timer = NULL;

#define MAX_RETRIES          15
#define BACKOFF_BASE_MS      1000
#define BACKOFF_CAP_MS       30000

static size_t json_escaped_len(const uint8_t *src)
{
    size_t len = 0;
    size_t src_len = strnlen((const char *)src, sizeof(s_scan_results[0].ssid));

    for (size_t i = 0; i < src_len; i++) {
        unsigned char ch = src[i];
        switch (ch) {
        case '"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            len += 2;
            break;
        default:
            len += (ch < 0x20) ? 6 : 1;
            break;
        }
    }

    return len;
}

static bool append_json_escaped(char *buf, size_t buf_len, int *pos, const uint8_t *src)
{
    size_t src_len = strnlen((const char *)src, sizeof(s_scan_results[0].ssid));

    for (size_t i = 0; i < src_len; i++) {
        unsigned char ch = src[i];
        const char *escaped = NULL;
        char unicode_escape[7];
        char raw[2] = { (char)ch, '\0' };

        switch (ch) {
        case '"':
            escaped = "\\\"";
            break;
        case '\\':
            escaped = "\\\\";
            break;
        case '\b':
            escaped = "\\b";
            break;
        case '\f':
            escaped = "\\f";
            break;
        case '\n':
            escaped = "\\n";
            break;
        case '\r':
            escaped = "\\r";
            break;
        case '\t':
            escaped = "\\t";
            break;
        default:
            if (ch < 0x20) {
                snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", ch);
                escaped = unicode_escape;
            } else {
                escaped = raw;
            }
            break;
        }

        size_t write_len = strlen(escaped);
        if (*pos + (int)write_len >= (int)buf_len) {
            return false;
        }

        memcpy(buf + *pos, escaped, write_len);
        *pos += (int)write_len;
    }

    return true;
}

static uint32_t get_backoff_ms(int retry)
{
    uint32_t ms = BACKOFF_BASE_MS;
    for (int i = 0; i < retry && i < 5; i++) {
        ms *= 2;
    }
    return ms > BACKOFF_CAP_MS ? BACKOFF_CAP_MS : ms;
}

static void ensure_ap_netif(void)
{
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
}

static void reconnect_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (s_sta_mode) {
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            s_status = NET_STATUS_DISCONNECTED;
            s_ip_str[0] = '\0';
            if (s_retry_count < MAX_RETRIES) {
                s_retry_count++;
                uint32_t delay_ms = get_backoff_ms(s_retry_count - 1);
                ESP_LOGI(TAG, "WiFi disconnected, retry %d/%d in %lu ms",
                         s_retry_count, MAX_RETRIES, (unsigned long)delay_ms);
                xTimerChangePeriod(s_reconnect_timer,
                                   pdMS_TO_TICKS(delay_ms), 0);
            } else {
                ESP_LOGW(TAG, "WiFi connection failed after %d retries", MAX_RETRIES);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            break;

        case WIFI_EVENT_SCAN_DONE:
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            s_scan_count = sizeof(s_scan_results) / sizeof(s_scan_results[0]);
            esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_results);
            s_scan_done = true;
            xSemaphoreGive(s_scan_mutex);
            ESP_LOGI(TAG, "Scan complete: %d networks found", s_scan_count);
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Station connected to AP, AID=%d", event->aid);
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Connected - IP: %s", s_ip_str);
        s_status = NET_STATUS_CONNECTED;
        xTimerStop(s_reconnect_timer, 0);
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void start_mdns(void)
{
    mdns_free();

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set("rt4ksd");
    mdns_instance_name_set("RT4K SD WiFi");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS started: rt4ksd.local");
}

esp_err_t network_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_scan_mutex = xSemaphoreCreateMutex();
    s_reconnect_timer = xTimerCreate("wifi_reconn", pdMS_TO_TICKS(1000),
                                      pdFALSE, NULL, reconnect_timer_cb);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    /* AP netif created on demand by ensure_ap_netif() */

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    return ESP_OK;
}

void network_start(void)
{
    rt4k_wifi_config_t cfg;

    if (!rt4k_config_load(&cfg)) {
        ESP_LOGW(TAG, "No valid WiFi config, starting SoftAP");
        network_start_softap();
        return;
    }

    s_status = NET_STATUS_CONNECTING;
    s_sta_mode = true;
    s_retry_count = 0;

    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid, cfg.ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, cfg.password, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s...", cfg.ssid);

    /* Wait for connection or failure */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(NET_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s, IP: %s", cfg.ssid, s_ip_str);
        rt4k_config_save(cfg.ssid, cfg.password);

        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

        start_mdns();
    } else {
        ESP_LOGW(TAG, "Connection failed, starting SoftAP");
        xTimerStop(s_reconnect_timer, 0);
        s_retry_count = 0;
        esp_wifi_stop();
        rt4k_config_clear_wifi();
        network_start_softap();
    }
}

void network_start_softap(void)
{
    s_sta_mode = false;
    s_status = NET_STATUS_DISCONNECTED;

    ensure_ap_netif();

    /* Configure AP netif with static IP */
    esp_netif_ip_info_t ip_info;
    esp_netif_str_to_ip4(NET_AP_IP, &ip_info.ip);
    esp_netif_str_to_ip4(NET_AP_GW, &ip_info.gw);
    esp_netif_str_to_ip4(NET_AP_NETMASK, &ip_info.netmask);

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = NET_AP_SSID,
            .ssid_len = strlen(NET_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: %s (IP: %s)", NET_AP_SSID, NET_AP_IP);

    start_mdns();
}

bool network_connect(const char *ssid, const char *password)
{
    if (!ssid || !password) return false;

    /* If already connected to this SSID, skip */
    if (s_status == NET_STATUS_CONNECTED && s_sta_mode) {
        wifi_config_t current;
        esp_wifi_get_config(WIFI_IF_STA, &current);
        if (strcmp((char *)current.sta.ssid, ssid) == 0) {
            return false; /* Already connected */
        }
    }

    s_status = NET_STATUS_CONNECTING;
    s_retry_count = 0;

    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));

    /* Use AP+STA so the config page remains accessible during connection */
    ensure_ap_netif();
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    /* Configure AP side to keep captive portal accessible */
    esp_netif_ip_info_t ip_info;
    esp_netif_str_to_ip4(NET_AP_IP, &ip_info.ip);
    esp_netif_str_to_ip4(NET_AP_GW, &ip_info.gw);
    esp_netif_str_to_ip4(NET_AP_NETMASK, &ip_info.netmask);
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = NET_AP_SSID,
            .ssid_len = strlen(NET_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();

    /* Wait for result in background — the event handler updates status */
    ESP_LOGI(TAG, "Connecting to %s...", ssid);

    /* Track the requested credentials for status/reporting during connect */
    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    strlcpy(s_pending_pass, password, sizeof(s_pending_pass));
    s_connect_pending = true;
    s_sta_mode = true;

    /* Wait for connection result (blocking) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(NET_CONNECT_TIMEOUT_MS));

    s_connect_pending = false;

    if (bits & WIFI_CONNECTED_BIT) {
        rt4k_config_save(ssid, password);
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

        /* Connection succeeded — drop SoftAP, switch to STA-only */
        esp_wifi_set_mode(WIFI_MODE_STA);

        start_mdns();
        return true;
    }

    return true; /* Connection attempt was started, even if it may fail later */
}

void network_start_scan(void)
{
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    s_scan_done = false;
    s_scan_count = 0;
    xSemaphoreGive(s_scan_mutex);

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    esp_wifi_scan_start(&scan_cfg, false);
    ESP_LOGI(TAG, "WiFi scan started");
}

int network_get_scan_results_json(char *buf, size_t buf_len)
{
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    if (!s_scan_done || s_scan_count == 0) {
        xSemaphoreGive(s_scan_mutex);
        return snprintf(buf, buf_len, "[]");
    }

    int pos = 0;
    pos += snprintf(buf + pos, buf_len - pos, "[");

    for (int i = 0; i < s_scan_count && pos < (int)buf_len - 1; i++) {
        const char *type = (s_scan_results[i].authmode == WIFI_AUTH_OPEN) ? "open" : "close";
        int rssi_len = snprintf(NULL, 0, "%d", s_scan_results[i].rssi);
        size_t entry_needed =
            (i > 0 ? 1U : 0U) +
            strlen("{\"ssid\":\"\",\"rssi\":\"\",\"type\":\"\"}") +
            json_escaped_len(s_scan_results[i].ssid) +
            (size_t)rssi_len + strlen(type);

        if (pos + (int)entry_needed + 1 >= (int)buf_len) {
            break;
        }

        if (i > 0) pos += snprintf(buf + pos, buf_len - pos, ",");
        pos += snprintf(buf + pos, buf_len - pos, "{\"ssid\":\"");
        if (!append_json_escaped(buf, buf_len, &pos, s_scan_results[i].ssid)) {
            break;
        }
        pos += snprintf(buf + pos, buf_len - pos,
            "\",\"rssi\":\"%d\",\"type\":\"%s\"}",
            s_scan_results[i].rssi,
            type);
    }

    pos += snprintf(buf + pos, buf_len - pos, "]");
    xSemaphoreGive(s_scan_mutex);
    return pos;
}

net_status_t network_get_status(void)
{
    return s_status;
}

void network_get_ip_str(char *buf, size_t buf_len)
{
    strlcpy(buf, s_ip_str, buf_len);
}

bool network_is_sta_mode(void)
{
    return s_sta_mode;
}

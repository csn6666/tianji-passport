// main/app_net.c —— 全局唯一的 Wi-Fi 栈(从旧 Chat 页迁出)。
// 初始化、用 NVS 凭据连接、断线自动重连;配网热点由 chat_provision 模块驱动。
#include "demo.h"
#include "chat_provision.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "net";

static esp_netif_t *s_sta_netif;
static uint32_t s_ip_gen;                        // 每拿到一次 IP +1,用于识别"换网成功"
static bool s_started;
static bool s_have_creds;
static EventGroupHandle_t s_eg;
#define GOT_IP BIT0

// 设备无 RTC,日期只能靠授时。排盘的流年/流月/流日全指着它。
static bool s_sntp_started;

static void sntp_kick(void) {
    if (s_sntp_started) return;
    // 不走 DHCP 下发的 NTP(要另开 CONFIG_LWIP_DHCP_GET_NTP_SRV),直接写死两个源
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    cfg.start = true;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
        setenv("TZ", "CST-8", 1);       // 东八区,排盘一律按北京时间
        tzset();
    }
}

// 是否已经拿到可信的墙上时间(年份 >= 2024 即认为同步过)
bool chat_net_time_ok(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_year + 1900 >= 2024;
}

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Wi-Fi 断开, reason=%d", d ? d->reason : -1);
        xEventGroupClearBits(s_eg, GOT_IP);
        if (s_have_creds) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_ip_gen++;
        xEventGroupSetBits(s_eg, GOT_IP);
        sntp_kick();
    }
}

// NVS 初始化(幂等)。生辰在开机首屏就要读,早于 Wi-Fi,所以单独暴露出去。
esp_err_t chat_nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t stack_init(void) {
    if (s_started) return ESP_OK;
    ESP_ERROR_CHECK(chat_nvs_init());
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();          // 配网热点用

    s_eg = xEventGroupCreate();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);               // 语音流对延迟敏感,关省电
    s_started = true;
    return ESP_OK;
}

esp_err_t chat_net_ensure(void) {
    esp_err_t err = stack_init();
    if (err != ESP_OK) return err;
    if (chat_net_ready()) return ESP_OK;
    char ssid[33], pass[65];
    if (chat_prov_get(ssid, sizeof(ssid), pass, sizeof(pass))) {
        if (!s_have_creds) {
            s_have_creds = true;
            wifi_config_t wc = { 0 };
            strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
            strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
            esp_wifi_set_config(WIFI_IF_STA, &wc);
            esp_wifi_connect();
            ESP_LOGI(TAG, "用已存凭据连接 %s", ssid);
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;                    // 无凭据,需配网
}

bool chat_net_ready(void) {
    return s_eg && (xEventGroupGetBits(s_eg) & GOT_IP);
}

// 当前 STA 的点分十进制 IP;未拿到 IP 时返回 false。
bool chat_net_ip(char *buf, size_t len) {
    esp_netif_ip_info_t ip;
    if (!s_sta_netif || !chat_net_ready()) return false;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    return true;
}

// 取得 IP 的次数。配网前后比较即可判断是否真的连上了(新)网络。
uint32_t chat_net_gen(void) { return s_ip_gen; }

// 当前 AP 的信号强度 dBm;未连接返回 0。
int chat_net_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

bool chat_net_has_creds(void) {
    return chat_prov_has_creds();
}

void chat_net_creds_saved(void) {
    s_have_creds = true;                         // 配网写入后,断线回路开始自动重连
}

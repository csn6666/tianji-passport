// main/chat_provision.c —— 热点配网实现。
// 设备开一个开放热点 Tianji-Setup,手机连上后浏览器打开 http://192.168.4.1,
// 网页会列出附近 Wi-Fi,用户选择并填密码 -> 存 NVS -> 切回 STA 连接。
#include "chat_provision.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "prov";

#define NVS_NS   "chatcfg"
#define AP_SSID  "Tianji-Setup"

static httpd_handle_t s_httpd;
static int s_dns_sock = -1;

// ---------------------------------------------------------------- 强制门户 DNS
// 把所有域名都解析到 192.168.4.1。手机连上热点后做联网检测,会被引到配置页,
// 系统就弹出"登录网络"页面 —— 用户无需手输 IP。

static void dns_task(void *arg) {
    uint8_t buf[160];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    for (;;) {
        int sock = s_dns_sock;
        if (sock < 0) break;
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (len < 12) {
            if (s_dns_sock < 0) break;    // stop() 关了 socket
            continue;
        }
        // 找到问题段结尾(跳过 QNAME 各标签 + 终止0 + QTYPE/QCLASS)
        int qend = 12;
        while (qend < len && buf[qend] != 0) qend += buf[qend] + 1;
        qend += 5;
        if (qend > len) continue;
        // 应答头:QR|AA 置位,RCODE=0,QD=1,AN=1,NS/AR=0
        buf[2] |= 0x84; buf[3] = 0x80;
        buf[6] = 0; buf[7] = 1; buf[8] = buf[9] = buf[10] = buf[11] = 0;
        // 追加 A 记录:名字用指针指向问题段,TTL 60s,地址 192.168.4.1
        static const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4,
                                       192, 168, 4, 1 };
        memcpy(buf + qend, ans, sizeof(ans));
        sendto(sock, buf, qend + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
    vTaskDelete(NULL);
}

static void dns_start(void) {
    if (s_dns_sock >= 0) return;
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr));
    xTaskCreate(dns_task, "prov_dns", 2560, NULL, 4, NULL);
}

static void dns_stop(void) {
    int sock = s_dns_sock;
    s_dns_sock = -1;
    if (sock >= 0) close(sock);
}

// ---------------------------------------------------------------- NVS

bool chat_prov_get(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_get_str(h, "ssid", ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(h, "pass", pass, &pass_len);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0';
}

bool chat_prov_has_creds(void) {
    char s[33], p[65];
    return chat_prov_get(s, sizeof(s), p, sizeof(p));
}

void chat_prov_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void save_creds(const char *ssid, const char *pass) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", pass));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

// ---------------------------------------------------------------- HTTP

static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\x22width=dev"
    "ice-width,initial-scale=1\x22><title>\xe5\xa4\xa9\xe6\x9c\xba \xc2\xb7 \xe9\x85\x8d"
    "\xe7\xbd\x91</title><style>body{font-family:sans-serif;max-width:420px;margin:32px auto;"
    "padding:0 16px;color:#17202A}input,button{width:100%;box-sizing:border-box;font-size:18p"
    "x;padding:12px;margin:8px 0;border:2px solid #17202A;border-radius:8px}button{background"
    ":#82BE2D;font-weight:bold}small{color:#666;display:block;line-height:1.5;margin-top:14px"
    "}</style></head><body><h2>\xe5\xa4\xa9\xe6\x9c\xba \xc2\xb7 \xe8\xbf\x9e\xe6\x8e\xa5 Wi-"
    "Fi</h2><form method=POST action=/save><input name=ssid list=aps placeholder=\x22Wi-Fi "
    "\xe5\x90\x8d\xe7\xa7\xb0\x22 required><datalist id=aps></datalist><input name=pass type="
    "password placeholder=\x22Wi-Fi \xe5\xaf\x86\xe7\xa0\x81\x22><button>\xe4\xbf\x9d"
    "\xe5\xad\x98\xe5\xb9\xb6\xe8\xbf\x9e\xe6\x8e\xa5</button></form><p style=color:#666>"
    "\xe6\x8f\x90\xe4\xba\xa4\xe5\x90\x8e\xe8\xae\xbe\xe5\xa4\x87\xe4\xbc\x9a\xe8\x87\xaa"
    "\xe5\x8a\xa8\xe8\xbf\x9e\xe6\x8e\xa5\xef\xbc\x8c\xe7\x9c\x8b\xe8\xae\xbe\xe5\xa4\x87"
    "\xe5\xb1\x8f\xe5\xb9\x95\xe7\xa1\xae\xe8\xae\xa4\xe7\xbb\x93\xe6\x9e\x9c\xe3\x80\x82</p>"
    "<small>\xe5\x90\x8e\xe7\xab\xaf\xe5\x9c\xb0\xe5\x9d\x80\xe4\xb8\x8e AI \xe6\x9c\x8d"
    "\xe5\x8a\xa1\xe6\x98\xaf\xe5\x9c\xa8\xe7\xbc\x96\xe8\xaf\x91\xe5\x9b\xba\xe4\xbb\xb6"
    "\xe6\x97\xb6\xe9\x85\x8d\xe5\xa5\xbd\xe7\x9a\x84\xef\xbc\x88main/chat_config.h"
    "\xef\xbc\x89\xef\xbc\x8c\xe8\xbf\x99\xe9\x87\x8c\xe5\x8f\xaa\xe7\xae\xa1 Wi-Fi"
    "\xe3\x80\x82\xe8\xa6\x81\xe6\x8d\xa2\xe6\x9c\x8d\xe5\x8a\xa1\xe5\xbe\x97\xe6\x94\xb9"
    "\xe9\x82\xa3\xe4\xb8\xaa\xe6\x96\x87\xe4\xbb\xb6\xe5\xb9\xb6\xe9\x87\x8d\xe6\x96\xb0"
    "\xe7\x83\xa7\xe5\xbd\x95\xe3\x80\x82</small><script>fetch('/scan').then(r=>r.json()).the"
    "n(a=>{document.getElementById('aps').innerHTML=a.map(s=>'<option value=\x22'+s+'\x22>')."
    "join('')})</script></body></html>";

static esp_err_t root_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

// 未知路径 302 到配置页:配合 DNS 劫持,让手机的联网检测触发"登录网络"弹窗
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// 扫描附近 AP,返回 JSON 数组(按信号排序、去重、最多 15 个)
static esp_err_t scan_get(httpd_req_t *req) {
    wifi_scan_config_t sc = { 0 };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan 失败: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }
    uint16_t n = 20;
    static wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);

    char json[512] = "[";
    size_t used = 1;
    int count = 0;
    for (int i = 0; i < n && count < 15; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0] || strchr(ssid, '"')) continue;
        bool dup = false;                       // 同名 AP(mesh)只留一个
        for (int j = 0; j < i && !dup; j++) {
            dup = strcmp(ssid, (const char *)recs[j].ssid) == 0;
        }
        if (dup) continue;
        size_t need = strlen(ssid) + 4;
        if (used + need >= sizeof(json) - 2) break;
        used += snprintf(json + used, sizeof(json) - used, "%s\"%s\"",
                         count ? "," : "", ssid);
        count++;
    }
    strcat(json, "]");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// application/x-www-form-urlencoded 解码(%XX 和 +)
static void url_decode(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '+') {
            *o++ = ' '; s++;
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

// 保存后延迟切 STA(先让 HTTP 响应发出去,再断热点)
static void apply_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1500));

    char ssid[33] = { 0 }, pass[65] = { 0 };
    chat_prov_get(ssid, sizeof(ssid), pass, sizeof(pass));
    ESP_LOGI(TAG, "切回 STA,连接 %s", ssid);

    chat_prov_stop();

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_disconnect();                  // 已连着旧网络时,不先断开 connect() 不生效
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    vTaskDelete(NULL);
}

static esp_err_t save_post(httpd_req_t *req) {
    char body[512] = { 0 };
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        total += r;
        if (total >= req->content_len) break;
    }
    if (total <= 0) return httpd_resp_send_500(req);
    body[total] = '\0';

    char ssid[64] = { 0 }, pass[96] = { 0 };
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);
    if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ssid/pass");
    }

    ESP_LOGI(TAG, "收到配网: ssid=%s", ssid);
    save_creds(ssid, pass);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<body style=\"font-family:sans-serif;text-align:center;margin-top:80px\">"
        "<h2>\xE2\x9C\x85 \xE5\xB7\xB2\xE4\xBF\x9D\xE5\xAD\x98</h2>"
        "<p>\xE8\xAE\xBE\xE5\xA4\x87\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5 Wi-Fi\xEF\xBC\x8C"
        "\xE7\x9C\x8B\xE8\xAE\xBE\xE5\xA4\x87\xE5\xB1\x8F\xE5\xB9\x95\xE7\xA1\xAE\xE8\xAE\xA4</p></body>",
        HTTPD_RESP_USE_STRLEN);

    chat_prov_on_saved();
    xTaskCreate(apply_task, "prov_apply", 3072, NULL, 5, NULL);
    return ESP_OK;
}

// ---------------------------------------------------------------- 启停

esp_err_t chat_prov_start(void) {
    if (s_httpd) return ESP_OK;

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 2;
    ap.ap.channel = 1;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;      // 手机联网检测会开一堆连接,自动淘汰旧的
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;

    static const httpd_uri_t u_root = { .uri = "/",     .method = HTTP_GET,  .handler = root_get };
    static const httpd_uri_t u_scan = { .uri = "/scan", .method = HTTP_GET,  .handler = scan_get };
    static const httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_scan);
    httpd_register_uri_handler(s_httpd, &u_save);
    // 其他任何路径(含系统联网检测 /generate_204 等)一律 302 到配置页,触发强制门户弹窗
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);

    dns_start();
    ESP_LOGI(TAG, "配网热点已开启: %s -> http://192.168.4.1 (强制门户)", AP_SSID);
    return ESP_OK;
}

void chat_prov_stop(void) {
    dns_stop();
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        esp_wifi_set_mode(WIFI_MODE_STA);     // 收起热点,回到纯 STA
    }
}

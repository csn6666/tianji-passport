// main/llm_client.c —— 设备直连 LLM。用 esp_http_client + 证书 bundle 做 TLS,
// POST /chat/completions 开 stream,边收边把增量吐给回调。
//
// 这里所有的抠门都是被 ESP32-C3 的内存逼的:没有 PSRAM,UI + Wi-Fi + ws 跑起来
// 之后空闲堆只剩 50KB 上下,一次 TLS 握手就能吃掉一多半。所以:
//   - 请求体在栈上拼,不用 cJSON 建树
//   - 响应按 SSE 逐行处理,只对每行的 JSON 做一次小解析
//   - http_client 的 buffer 压到 1KB/1.5KB
#include "llm_client.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "llm";

#define NVS_NS "llm"
#define SSE_LINE_MAX 1024          // 单条 SSE 行的上限,超了就丢弃该行

static char s_err[96];

const char *llm_last_error(void) { return s_err[0] ? s_err : NULL; }

// ---------------------------------------------------------------- 配置

bool llm_cfg_load(llm_cfg_t *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n;
    bool ok = true;
    n = sizeof(out->base_url); if (nvs_get_str(h, "url", out->base_url, &n) != ESP_OK) ok = false;
    n = sizeof(out->api_key);  if (nvs_get_str(h, "key", out->api_key, &n) != ESP_OK) ok = false;
    n = sizeof(out->model);    if (nvs_get_str(h, "model", out->model, &n) != ESP_OK) ok = false;
    nvs_close(h);
    return ok && out->base_url[0] && out->api_key[0] && out->model[0];
}

void llm_cfg_save(const llm_cfg_t *cfg) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "url", cfg->base_url);
    nvs_set_str(h, "key", cfg->api_key);
    nvs_set_str(h, "model", cfg->model);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "LLM 配置已保存: %s / %s", cfg->base_url, cfg->model);
}

bool llm_cfg_ready(void) {
    llm_cfg_t c;
    return llm_cfg_load(&c);
}

// ---------------------------------------------------------------- SSE 解析

typedef struct {
    llm_delta_cb cb;
    void *user;
    char line[SSE_LINE_MAX];
    size_t len;
    bool overflow;             // 本行超长,丢到下一个换行为止
    bool done;                 // 收到 [DONE] 或回调要求中止
    bool aborted;
    bool saw_done;             // 真的收到了 [DONE](区别于被回调中止)
    size_t got;                // 累计拿到的正文字节数
} sse_ctx_t;

// 处理一整行 SSE。只关心 "data: {...}" 里的 choices[0].delta.content。
static void sse_line(sse_ctx_t *s) {
    if (s->len == 0) return;
    s->line[s->len] = '\0';
    char *p = s->line;
    if (strncmp(p, "data:", 5) != 0) return;      // event:/id:/注释行一律忽略
    p += 5;
    while (*p == ' ') p++;
    if (strcmp(p, "[DONE]") == 0) { s->done = true; s->saw_done = true; return; }

    cJSON *root = cJSON_Parse(p);
    if (!root) return;
    // 出错时对端也走 200 + body,这里顺手把 error.message 捞出来
    const cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        if (m) snprintf(s_err, sizeof(s_err), "%s", m);
        s->done = true;
        cJSON_Delete(root);
        return;
    }
    const cJSON *ch = cJSON_GetObjectItem(root, "choices");
    const cJSON *c0 = ch ? cJSON_GetArrayItem(ch, 0) : NULL;
    const cJSON *delta = c0 ? cJSON_GetObjectItem(c0, "delta") : NULL;
    const char *txt = delta ? cJSON_GetStringValue(cJSON_GetObjectItem(delta, "content")) : NULL;
    if (txt && txt[0]) {
        s->got += strlen(txt);
        if (s->cb && !s->cb(txt, s->user)) { s->done = true; s->aborted = true; }
    }
    cJSON_Delete(root);
}

static void sse_feed(sse_ctx_t *s, const char *data, int len) {
    for (int i = 0; i < len && !s->done; i++) {
        char c = data[i];
        if (c == '\n') {
            if (!s->overflow) sse_line(s);
            s->len = 0;
            s->overflow = false;
        } else if (c != '\r') {
            if (s->len < SSE_LINE_MAX - 1) s->line[s->len++] = c;
            else s->overflow = true;              // 超长行:丢弃,不截断成半个 JSON
        }
    }
}

static esp_err_t http_evt(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        sse_feed((sse_ctx_t *)evt->user_data, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- 请求

// 把字符串按 JSON 规则转义写进 buf。返回写入长度;放不下则返回 -1。
static int json_escape(char *buf, size_t cap, const char *src) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        const char *esc = NULL;
        char tmp[8];
        switch (*p) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (*p < 0x20) { snprintf(tmp, sizeof(tmp), "\\u%04x", *p); esc = tmp; }
            break;
        }
        if (esc) {
            size_t n = strlen(esc);
            if (o + n >= cap) return -1;
            memcpy(buf + o, esc, n);
            o += n;
        } else {
            if (o + 1 >= cap) return -1;
            buf[o++] = (char)*p;
        }
    }
    buf[o] = '\0';
    return (int)o;
}

esp_err_t llm_chat(const char *system, const char *user, int max_tokens,
                   llm_delta_cb cb, void *user_ctx)
{
    s_err[0] = '\0';
    llm_cfg_t cfg;
    if (!llm_cfg_load(&cfg)) {
        snprintf(s_err, sizeof(s_err), "未配置 LLM");
        return ESP_ERR_INVALID_STATE;
    }

    char url[128];
    snprintf(url, sizeof(url), "%s/chat/completions", cfg.base_url);

    // 请求体:整段在堆上拼,不建 cJSON 树(省内存也省栈)
    const size_t BODY_CAP = 2560;
    char *body = malloc(BODY_CAP);
    char *esc_sys = malloc(1024);
    char *esc_usr = malloc(1536);
    if (!body || !esc_sys || !esc_usr) {
        free(body); free(esc_sys); free(esc_usr);
        snprintf(s_err, sizeof(s_err), "内存不足");
        return ESP_ERR_NO_MEM;
    }
    if (json_escape(esc_sys, 1024, system) < 0 || json_escape(esc_usr, 1536, user) < 0) {
        free(body); free(esc_sys); free(esc_usr);
        snprintf(s_err, sizeof(s_err), "提示词过长");
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(body, BODY_CAP,
                     "{\"model\":\"%s\",\"stream\":true,\"max_tokens\":%d,"
                     "\"messages\":[{\"role\":\"system\",\"content\":\"%s\"},"
                     "{\"role\":\"user\",\"content\":\"%s\"}]}",
                     cfg.model, max_tokens, esc_sys, esc_usr);
    free(esc_sys);
    free(esc_usr);
    if (n < 0 || (size_t)n >= BODY_CAP) {
        free(body);
        snprintf(s_err, sizeof(s_err), "请求体过长");
        return ESP_ERR_INVALID_ARG;
    }

    // 一次都没拿到正文的失败重试一轮 —— 这台机器内存紧、Wi-Fi 也不总是稳,
    // 偶发的握手/读取失败很常见,而重来一次比让用户重问一遍便宜得多。
    sse_ctx_t sse;
    esp_err_t err = ESP_FAIL;
    int status = 0;
    for (int attempt = 1; attempt <= 2; attempt++) {
        memset(&sse, 0, sizeof(sse));
        sse.cb = cb;
        sse.user = user_ctx;

        esp_http_client_config_t hc = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .event_handler = http_evt,
            .user_data = &sse,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 30000,
            .buffer_size = 1024,           // 内存紧,够放一条 SSE 行即可
            .buffer_size_tx = 1536,
        };
        esp_http_client_handle_t c = esp_http_client_init(&hc);
        if (!c) {
            free(body);
            snprintf(s_err, sizeof(s_err), "http 初始化失败");
            return ESP_FAIL;
        }

        char auth[8 + sizeof(cfg.api_key)];
        snprintf(auth, sizeof(auth), "Bearer %s", cfg.api_key);
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_header(c, "Authorization", auth);
        esp_http_client_set_header(c, "Accept", "text/event-stream");
        esp_http_client_set_post_field(c, body, n);

        ESP_LOGI(TAG, "请求 %s (%s) 第%d次, 空闲堆=%u", url, cfg.model, attempt,
                 (unsigned)esp_get_free_heap_size());
        err = esp_http_client_perform(c);
        status = esp_http_client_get_status_code(c);
        ESP_LOGI(TAG, "完成 err=%s status=%d 正文=%u字节 空闲堆=%u%s",
                 esp_err_to_name(err), status, (unsigned)sse.got,
                 (unsigned)esp_get_free_heap_size(), sse.aborted ? " (被打断)" : "");

        esp_http_client_cleanup(c);
        memset(auth, 0, sizeof(auth));     // 别把 key 留在栈上


        // 已经吐过字、或者用户主动打断、或者是服务端明确的拒绝,都不该重试
        if (err == ESP_OK || sse.got > 0 || sse.aborted || status >= 400) break;
        if (attempt == 1) {
            ESP_LOGW(TAG, "空手而归(%s),重试一次", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(600));
        }
    }
    free(body);

    // 流已经走完(收到 [DONE])或者正文已经拿到手,就算传输层收尾不干净也当成功。
    // SSE 的语义终点是 [DONE],不是 chunked 的结束块 —— 有的服务端发完就直接断,
    // esp_http_client 会报 ESP_ERR_HTTP_INCOMPLETE_DATA,但回答其实是完整的。
    if (err != ESP_OK && status == 200 && (sse.saw_done || sse.got > 0)) {
        ESP_LOGW(TAG, "传输层收尾异常(%s),但已拿到 %u 字节正文%s,按成功处理",
                 esp_err_to_name(err), (unsigned)sse.got, sse.saw_done ? "且收到 [DONE]" : "");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        if (!s_err[0]) snprintf(s_err, sizeof(s_err), "连接失败 %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "失败: %s (status=%d, 正文 %u 字节)", s_err, status, (unsigned)sse.got);
        return err;
    }
    if (status == 401 || status == 403) {
        snprintf(s_err, sizeof(s_err), "API key 被拒(HTTP %d)", status);
        return ESP_ERR_INVALID_STATE;
    }
    if (status >= 400) {
        if (!s_err[0]) snprintf(s_err, sizeof(s_err), "服务端返回 HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

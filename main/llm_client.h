// main/llm_client.h —— 设备直连 LLM(任何 OpenAI 兼容的 /chat/completions)。
//
// key 不进固件镜像:base_url / api_key / model 由用户在配网页里填,存 NVS。
// 换 key 不用重新烧录,重新配一次即可。
//
// 内存:C3 没有 PSRAM,一次 TLS 会话峰值就是整机最紧的时刻。调用方必须
// 保证同一时间只有一个 llm_chat 在跑,并且先把能让出的缓冲让出来。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char base_url[96];      // 如 https://api.deepseek.com/v1
    char api_key[160];     // 有些家的 key 挺长,留够
    char model[40];         // 如 deepseek-chat
} llm_cfg_t;

// NVS 存取。没配过时 llm_cfg_load 返回 false。
bool llm_cfg_load(llm_cfg_t *out);
void llm_cfg_save(const llm_cfg_t *cfg);
bool llm_cfg_ready(void);   // 三项齐全才算配好

// 流式增量回调。text 是本次新增的片段(非累计),不保证按句切分。
// 返回 false 可请求提前中止(比如用户打断)。
typedef bool (*llm_delta_cb)(const char *text, void *user);

// 一轮对话。阻塞,请在自己的任务里调用,栈至少 6KB。
//   system/user  提示词
//   max_tokens   上限
//   cb/user      增量回调
// 返回 ESP_OK 表示正常收完;其它为网络/TLS/HTTP 错误。
esp_err_t llm_chat(const char *system, const char *user, int max_tokens,
                   llm_delta_cb cb, void *user_ctx);

// 上一次失败的可读原因(给 UI 显示),没有则返回 NULL。
const char *llm_last_error(void);

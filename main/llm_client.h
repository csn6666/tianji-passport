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

// 配置全部来自编译期的 main/chat_config.h(见 README「烧录前要准备什么」)。
// 三项都填好了才算就绪;还是模板里的占位值就返回 false。
bool llm_cfg_ready(void);

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

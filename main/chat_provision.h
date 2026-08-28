// main/chat_provision.h —— 手机配网:设备开热点 + 网页填 Wi-Fi,存 NVS。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// NVS 里是否已存有 Wi-Fi 凭据
bool chat_prov_has_creds(void);

// 读取凭据。没有则返回 false。
bool chat_prov_get(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

// 清除凭据(重新配网用)
void chat_prov_clear(void);

// 进入配网模式:切 APSTA,开热点 Tianji-Setup 和配置网页(192.168.4.1)。
// 要求 Wi-Fi 栈已初始化并 start。用户提交后:凭据写入 NVS -> 回调
// chat_prov_on_saved() -> 切回 STA 并连接 -> 停掉热点和网页。
esp_err_t chat_prov_start(void);

// 停止配网(退出页面时清理;已停则无害)
void chat_prov_stop(void);

// 由应用实现:Wi-Fi 凭据保存成功时被回调(在 httpd 任务上下文,勿阻塞)
void chat_prov_on_saved(void);

// 由应用实现:只更新了 AI 配置、没动 Wi-Fi 时被回调(同样勿阻塞)
void chat_prov_on_llm_saved(void);

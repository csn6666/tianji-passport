// main/demo.h —— 神算子应用的页面与共享设施声明。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bsp_button.h"
#include "esp_err.h"

// 神算子应用(整机唯一应用):首页/命盘/今日/问事/录入向导
void demo_fortune_enter(void);
void demo_fortune_exit(void);
void demo_fortune_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// NVS(app_net.c):生辰/凭据都存这儿,必须在任何 nvs_open 之前调用一次
esp_err_t chat_nvs_init(void);

// 全局 Wi-Fi 栈(app_net.c,全部非阻塞)
esp_err_t chat_net_ensure(void);   // 初始化并用已存凭据发起连接;无凭据返回 ESP_ERR_NOT_FOUND
bool chat_net_ready(void);         // 是否已拿到 IP
bool chat_net_has_creds(void);
bool chat_net_ip(char *buf, size_t len);   // 已联网时填入点分十进制 IP
int chat_net_rssi(void);                   // 当前信号 dBm,未连接为 0
uint32_t chat_net_gen(void);               // 取得 IP 的次数(换网成功检测)
bool chat_net_time_ok(void);               // SNTP 是否已经校到时(排盘要用)
void chat_net_creds_saved(void);   // 配网模块保存凭据后调用,开启自动重连

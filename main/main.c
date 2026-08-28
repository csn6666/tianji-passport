// main/main.c —— 天机:命理专用机。BSP 初始化后直接进入天机应用。
//
// 按键语义(应用内定义):
//   上/下   首页=移动选中;各页面自定义
//   确定 短按 首页=进入选中项;各页面自定义
//   确定 长按 任何子页面=返回首页
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "esp_log.h"

static const char *TAG = "main";

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;
    demo_fortune_key(btn, ev);
    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "天机 启动");

    // NVS 先起:首页要从里面读已录入的生辰,不能等到 Wi-Fi 任务才初始化
    esp_err_t nvs_err = chat_nvs_init();
    if (nvs_err != ESP_OK) ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(nvs_err));

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线"
                      "(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    bool btn_ok = (bsp_button_init(on_key, NULL) == ESP_OK);
    bool audio_ok = (bsp_audio_init() == ESP_OK);
    bsp_battery_init();     // 电量计可选,失败不阻塞

    if (bsp_lvgl_lock(1000)) {
        demo_fortune_enter();
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "就绪: Button=%d Audio=%d", btn_ok, audio_ok);
}

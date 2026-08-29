# 给 AI 编程工具的项目说明

这份文件是 Claude Code / Cursor / Copilot 之类的工具进入本仓库时的第一站。
人类读者建议先看 [README](README.md)。

## 这是什么

`tianji-passport`：一台 ESP32-C3 的命理设备。对它说出生辰，**设备本地**排八字、
看今日运势、以命盘为上下文问事。

分工是这个项目的核心设计，改任何东西之前先记住：

| 谁 | 干什么 | 在哪 |
|---|---|---|
| **设备上的确定性代码** | 全部排盘计算 | `main/bazi_engine.c` + 预烘的 `main/bazi_tables.c` |
| **LLM（设备直连）** | 只做解读，**绝不推算干支** | `main/llm_client.c` |
| **后端** | 只做听写和合成，不参与推理 | `server/voice_server.py` |

---

## 硬性约束（违反了产品就是坏的）

### 1. 排盘绝不能交给 LLM

干支、节气边界、大运顺逆这类计算，LLM 会一本正经地算错。所有排盘结果必须来自
`bazi_engine.c` 的确定性代码。LLM 的提示词里已经写死"严禁自行推算任何干支、日期或大运"，
**不要放宽这条**。

### 2. 改了排盘就必须对拍

```bash
./tools/verify_bazi.sh 2000
```

它在主机上用 gcc 编译 `bazi_engine.c`（所以那个文件是纯 C99、不能依赖 ESP-IDF），
拿几千条随机生辰 + 每个节气交接时刻的 ±1 分钟，跟 `server/bazi/` 的 Python 引擎
**逐字段比对 26 个字段**。必须全绿才能烧录。

加字段时要同步改三处：`bazi_engine.c`、`tools/bazi_ref.py`（Python 侧输出）、
`tools/bazi_probe.c`（C 侧输出），键名必须一致。

### 3. `main/bazi_tables.c` 是生成物，永远不要手改

由 `tools/gen_bazi_tables.py` 从 `lunar_python` 生成。要改内容就改生成器再重新跑。

### 4. 发给设备的文本必须落在 GB2312 内

设备字库是编译进固件的位图，字库外的字显示成空白。后端的 `for_device()`
负责繁转简 + 剔除越界字符，**任何发往设备的文本都要过它**。

### 5. 切句必须落在 UTF-8 字符边界上

把一个汉字劈成两半，WebSocket 文本帧就是非法 UTF-8，协议层会**直接关掉连接**，
整条语音链哑掉且不报错。参考 `llm_on_delta()` 里的做法：整段增量一起追加
（每段本身是合法 UTF-8），只在完整字符边界上切。

### 6. 内存是最主要的约束

ESP32-C3 **没有 PSRAM**，UI + Wi-Fi + WebSocket 起来后空闲堆只有 50KB 上下。
加任何缓冲之前先看 `esp_get_minimum_free_heap_size()`。

历史教训：曾经有一版最低水位只剩 2.5KB，症状是 TLS 偶发读失败、报成
`ESP_ERR_HTTP_INCOMPLETE_DATA`，非常难联想到是内存问题。

已做的妥协别轻易动：`MBEDTLS_DYNAMIC_BUFFER=y`、`SSL_IN_CONTENT_LEN=8192`、
播放环 12KB。

### 7. 动 LVGL 必须持锁

`bsp_lvgl_lock()` / `bsp_lvgl_unlock()`。按键回调进来时外层已持锁（可重入，
再拿一次没问题）。`content_reset()` 会销毁旧控件，**任何跨函数持有的 `lv_obj_t *`
都要在它里面置空**，否则悬垂指针。

### 8. NVS 要在最前面初始化

`chat_nvs_init()` 在 `app_main()` 开头调用。开机首屏就要读生辰，不能等 Wi-Fi
任务起来才初始化。

---

## 命令

### 固件

```bash
# 前置:装好 ESP-IDF v5.5+ 并 source 它的 export.sh
cp main/chat_config.h.example main/chat_config.h   # 没有它构建会直接失败
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

`main/chat_config.h` 是 gitignored 的，里面有后端地址、token、LLM 密钥。
**不要把它提交上去，也不要把编好的 `.bin` 分发出去**（密钥在镜像里）。

### 测试

```bash
./tools/verify_bazi.sh 2000                      # 两侧排盘引擎逐字段对拍
cd server && ./venv/bin/python test_fortune.py   # 解析器 + 运势规则
./tools/check_secrets.sh                         # 提交前扫密钥,--history 连历史
```

### 后端

```bash
cd server
python3 -m venv venv && ./venv/bin/pip install -r requirements.txt
cp .env.example .env    # 至少改 AUTH_TOKEN
./run.sh
```

---

## 想改什么就去哪

| 想做的事 | 改哪儿 |
|---|---|
| 加/改页面、按键、交互 | `main/demo_fortune.c`（所有 UI 都在这一个文件） |
| 排盘算法 | `main/bazi_engine.c`（然后必须跑对拍） |
| 节气/农历/宜忌数据 | `tools/gen_bazi_tables.py`，重新生成 |
| 换 LLM 接口协议 | `main/llm_client.c` 的 `llm_chat()` 和 `sse_line()` |
| 换 ASR/TTS | `server/voice_server.py` 的 `transcribe_any()` / `tts_to_pcm()`，或实现 `custom_*` |
| Wi-Fi、授时 | `main/app_net.c` |
| 配网页 | `main/chat_provision.c`（页面是 C 字符串字面量，用脚本生成转义） |
| 硬件驱动 | `components/bsp/`（来自上游，一般不用动） |

### 加一个页面

1. `fs_state_t` 里加状态
2. 写 `view_xxx()`：`content_reset()` 拿内容区 → 塞控件 → `ui_status()` 写底部提示
3. `demo_fortune_key()` 的 `switch (s_state)` 里加按键处理
4. 要上首页菜单就往 `HOME_ITEMS[]` 加一项并改 `HOME_N`

内容区 `240x244`，底部 `292` 起是状态栏，面板带 5px 投影。

---

## 排查（症状 → 原因）

| 症状 | 大概率原因 |
|---|---|
| `llm: 完成 ... status=401/403` | API Key 不对，或 `CHAT_LLM_BASE_URL` 填错（要到 `/v1` 为止） |
| `llm: err=ESP_ERR_HTTP_INCOMPLETE_DATA` | 堆不够，或对端 TLS 记录超过 8KB |
| 屏幕有字但没声音 | 后端没跑或 ws 断了；看后端日志有没有 `合成:` |
| 说话设备没反应 | 后端日志里没有 `问命 ASR:` → ASR 没通 |
| 语音说到一半哑掉、连接莫名断开 | 切句切碎了 UTF-8（见约束 5） |
| 文字出完才出声 | LLM 没开流式 |
| 汉字显示空白 | 不在 GB2312 字库内（见约束 4） |
| 今日页显示"正在校时" | SNTP 还没成功，设备没有 RTC |
| 构建报"缺少 main/chat_config.h" | 照模板复制一份 |

设备日志里的关键 tag：`fortune:`（应用与排盘）、`llm:`（直连 LLM）、
`net:`（Wi-Fi/SNTP）、`prov:`（配网）。

---

## 代码风格

跟着邻近代码写：四空格缩进、K&R 大括号、`snake_case`、文件内静态变量用 `s_` 前缀、
BSP 接口用 `bsp_` 前缀。**注释和 UI 文本都用中文**（这个项目面向中文用户，
设备字库也只有 GB2312）。解释"为什么这么写"的注释请保留，尤其是记录硬件寄存器值、
内存约束、踩过的坑那些。

## 改完之后

自称完成之前，至少确认：

1. `idf.py build` 无错误、无新增警告
2. 动过排盘 → `./tools/verify_bazi.sh` 全绿
3. 动过后端 → `test_fortune.py` 全过
4. 真机烧录跑一遍受影响的页面（这是嵌入式项目，编译通过不等于能用）
5. `./tools/check_secrets.sh` 通过

上游 BSP 相关的接线、引脚、验收清单见 `docs/HARDWARE_BASELINE.md`。

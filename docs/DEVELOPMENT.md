# 继续开发

## 环境

- ESP-IDF **v5.5** 及以上（`idf.py --version` 确认）
- Python 3.10+（后端）
- `ffmpeg`（后端 TTS 转码用）

```bash
cp main/chat_config.h.example main/chat_config.h    # 填你的后端地址与 token
idf.py set-target esp32c3
idf.py build flash monitor
```

没复制 `chat_config.h` 的话构建会直接报错并告诉你复制哪个文件，不会编到一半才炸。

---

## 代码怎么分层

```
main/
  main.c             启动:NVS -> 显示 -> 按键/音频 -> 进入应用
  demo_fortune.c     ★ 所有页面、状态机、按键、任务编排都在这里
  bazi_engine.c      端侧排盘引擎(纯 C99,不依赖 ESP-IDF)
  bazi_tables.c      预计算的节气/朔日/宜忌表(生成物,别手改)
  llm_client.c       设备直连 LLM:TLS + 流式 SSE
  app_net.c          Wi-Fi 单例 + SNTP 授时
  chat_provision.c   手机配网页(热点 + 强制门户 + AI 配置表单)
  ui_pixel.c         像素风控件:面板、标签、选中态、小道童
components/bsp/      屏幕/音频/按键/电量计驱动(来自上游,一般不用动)
server/
  voice_server.py    语音后端:听写 + 合成
  bazi/              Python 排盘引擎(端侧引擎的对拍基准 + 烘表来源)
```

**改 UI 和交互 → `demo_fortune.c`；改排盘 → `bazi_engine.c`；改接入 → 见
[INTEGRATIONS.md](INTEGRATIONS.md)。**

### 页面状态机

所有页面共用一个 `fs_state_t` 状态机和一块内容区。加一个页面大致是：

1. 在 `fs_state_t` 里加一个状态
2. 写一个 `view_xxx()`：`content_reset()` 拿到内容区 → 往里塞控件 → `ui_status()` 写底部提示
3. 在 `demo_fortune_key()` 的 `switch (s_state)` 里加按键处理
4. 想上首页菜单的话，往 `HOME_ITEMS[]` 里加一项并改 `HOME_N`

内容区是 `240x244`，底部 `292` 起是状态栏。面板带 5px 投影，排版时留出来。

### 几条踩过的规矩

- **动 LVGL 必须持锁**：`bsp_lvgl_lock()` / `bsp_lvgl_unlock()`。按键回调进来时
  外层已经持锁了（锁是可重入的，再拿一次没问题）。
- **`content_reset()` 会销毁旧控件**：任何跨函数持有的 `lv_obj_t *` 都要在
  它里面置空，否则悬垂指针。
- **NVS 要先初始化**：`chat_nvs_init()` 在 `app_main()` 最前面调用。开机首屏
  就要读生辰，不能等 Wi-Fi 任务起来才初始化。

---

## 端侧排盘引擎

设备上**不做天文计算**。1900–2100 年的节气精确时刻、农历朔日、黄历宜忌
都由 `tools/gen_bazi_tables.py` 从 [lunar_python](https://github.com/6tail/lunar-python)
预先算好，烘成 `main/bazi_tables.c`（约 50KB flash）。C 侧只剩查表、二分、
干支取模。

```bash
# 改了年份范围之类才需要重新生成
server/venv/bin/python tools/gen_bazi_tables.py

# 对拍:同一批生辰,C 引擎 vs Python 引擎,26 个字段逐一比对
./tools/verify_bazi.sh 4000
```

**改了 `bazi_engine.c` 或表生成器，一定要跑一遍对拍再烧。** 用例里塞了
最容易出错的场景：每个节气交接时刻的 ±1 分钟、晚子时 23:30、闰日、
真太阳时倒退到上一年的边界、极端经度。

对拍脚本会在主机上用 gcc 编译 `bazi_engine.c`——它是纯 C99、不依赖 ESP-IDF
就是为了这个。加新字段时记得同步 `tools/bazi_ref.py`（Python 侧输出）和
`tools/bazi_probe.c`（C 侧输出），两边键名要一致。

已知边界：**只支持 1900–2100 年**，超出范围 `bz_compute()` 返回 false。

---

## 内存：这是最主要的约束

ESP32-C3 **没有 PSRAM**，片上 400KB SRAM 就是全部。UI + Wi-Fi + WebSocket
跑起来之后，空闲堆只有 50KB 上下。

```
一次设备直连 LLM 的实测水位
  请求前 49.8KB → 收流中 36.9KB → 全程最低 18.4KB
```

已经做过的妥协，动它们之前先想清楚：

| 在哪 | 做了什么 | 为什么 |
|---|---|---|
| `sdkconfig.defaults` | `MBEDTLS_DYNAMIC_BUFFER=y` | 握手完把大缓冲还回堆 |
| `sdkconfig.defaults` | `SSL_IN_CONTENT_LEN` 16384→8192 | 常驻收缓冲减半 |
| `demo_fortune.c` | 播放环 32KB→12KB | 只做抖动缓冲，靠逐句流控匀速 |
| `llm_client.c` | 请求体在堆上拼，不建 cJSON 树 | 省堆也省栈 |

**加功能前先看一眼 `esp_get_minimum_free_heap_size()`。** 掉到 10KB 以下就危险了
——之前有一版最低只剩 2.5KB，表现是 TLS 偶发读失败、报成
`ESP_ERR_HTTP_INCOMPLETE_DATA`，很难一眼看出是内存问题。

---

## 排查问题

设备日志（`idf.py monitor`）里几个关键 tag：

```
fortune: 端侧排盘: 己丑 丙子 壬子 甲辰 日主壬 大运乙亥   开机就排,不等网络
fortune: ws 已连接                                    后端连上了
llm:     请求 https://.../chat/completions 第1次, 空闲堆=49836
llm:     完成 err=ESP_OK status=200 正文=455字节        看 status 分辨是谁的问题
fortune: 送合成(待播2): 你今年这个丙午流年...            每句送去合成时打一行
```

| 现象 | 大概率原因 |
|---|---|
| `llm: 完成 ... status=401/403` | API Key 不对，或接口地址填错了 |
| `llm: ... err=ESP_ERR_HTTP_INCOMPLETE_DATA` | 堆不够，或对端 TLS 记录超过 8KB |
| 屏幕有字但没声音 | 后端没跑，或 ws 断了；看后端日志有没有 `合成:` |
| 文字出完才出声 | 你的 LLM 没开流式，或不支持 `stream: true` |
| 今日页显示"正在校时" | SNTP 还没成功，设备没有 RTC |
| 汉字显示成空白 | 那个字不在设备字库里，见 INTEGRATIONS.md 的字库一节 |
| 说了话设备没反应 | 后端日志里没有 `问命 ASR:` 那行 → ASR 没通 |
| `NotImplementedError: ASR_PROVIDER=custom ...` | 设了 `custom` 但没实现那两个函数 |

后端日志在启动时会打出用的是哪套 ASR/TTS：

```
ASR: 本机 faster-whisper(small),加载中 ...
TTS: edge (zh-CN-XiaoxiaoNeural)
```

---

## 提交之前

```bash
./tools/verify_bazi.sh 2000       # 改过排盘就跑
cd server && ./venv/bin/python test_fortune.py
./tools/check_secrets.sh          # 别把密钥推上去
```

`check_secrets.sh` 会检查敏感文件是否真的被忽略、将要提交的内容里有没有
密钥特征或硬编码 IP。加 `--history` 连历史提交一起扫。

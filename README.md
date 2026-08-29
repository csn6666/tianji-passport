# 天机 · 掌心命理机

一台巴掌大的 ESP32-C3 设备：对它说出生辰，它排八字、看今日运势、回答关于命盘的问题。

**排盘完全在设备上算**——四柱、大运、流年流月、真太阳时校正，全部是确定性代码 +
预烘的节气/朔日表，断网也能用，而且逐字段对得上专业排盘库。

**LLM 由设备直接调用**，用你自己的 API key。后端只剩语音转换（听写 + 合成），
**不需要任何 LLM 的 key**。

三样外部服务（LLM、语音识别、语音合成）都用你自己的账号，
**烧录前先配好**——见下面的[烧录前要准备什么](#烧录前要准备什么)。

> 基于 [FoloToy AI Passport](https://github.com/folotoy/ai-passport) 硬件与 BSP 改造。
>
> **用 AI 编程工具（Claude Code / Cursor 等）读这个仓库的话**，先看
> [AGENTS.md](AGENTS.md) —— 那里列了硬性约束、命令、"想改什么就去哪"的对照表，
> 以及一份症状→原因的排查表。

---

## 为什么排盘不交给大模型

因为大模型算不对。干支、节气边界、大运顺逆这类东西，LLM 会一本正经地算反——
尤其是大运顺逆和交节时刻。所以这个项目里分工很清楚：

| 谁来做 | 做什么 |
|---|---|
| **设备上的确定性代码** | 排盘：四柱、大运起运、流年流月流日、农历、真太阳时、五行强弱、幸运色/数字/方位、黄历宜忌 |
| **LLM（设备直连）** | 只做解读：一句话解盘文案、回答"我今年适合换工作吗"这类问题 |
| **后端** | 只做语音转换：听写、合成。不参与任何推理 |

设备侧引擎 (`main/bazi_engine.c`) 和服务器侧的 Python 引擎 (`server/bazi/`) 用同一套口径，
`tools/verify_bazi.sh` 会拿上万条随机生辰 + 节气交接点逐字段对拍，两边必须一字不差。

---

## 哪些跑在设备上，哪些要联网

这是这个项目最该先看懂的一张表。**排盘全在设备上**，云端只碰"说话"和"解读"。

| 能力 | 跑在哪 | 断网还能用吗 | 谁的账号 |
|---|---|---|---|
| 四柱 / 大运起运 / 流年流月流日 | **设备** | ✅ | — |
| 农历、节气、真太阳时校正 | **设备** | ✅ | — |
| 五行强弱、喜用神 | **设备** | ✅ | — |
| 幸运色 / 数字 / 方位 / 运势指数 | **设备** | ✅ | — |
| 黄历宜忌 | **设备** | ✅ | — |
| 命盘页、今日页的全部显示 | **设备** | ✅ | — |
| 校时（设备没有 RTC） | 云 · SNTP | ❌ | 公共 NTP |
| 一句话解盘文案 | 云 · **LLM** | ❌ 退回确定性兜底文案 | **你的 LLM** |
| 问事的回答 | 云 · **LLM** | ❌ | **你的 LLM** |
| 语音听写（说生辰、提问） | 云 · **后端 ASR** | ❌ | 你选的 ASR |
| 语音播报 | 云 · **后端 TTS** | ❌ | 你选的 TTS |
| 出生城市 → 经度 | 云 · 后端（OpenStreetMap） | ❌ 可跳过，按东八区 | 免费 |

一句话总结：**断网时设备照样能排盘、看命盘和今日运势，只是不会说话、也没有 LLM 的解读。**

### 数据实际怎么走

```
                    ┌──────────────────────────────┐
   说话 ──────────► │  设备 (ESP32-C3)             │
                    │                              │
                    │  排盘引擎:全部本地计算        │  ← 不联网
                    │  节气/朔日表:烧在 flash 里    │
                    └──┬────────────────────┬──────┘
                       │ WebSocket          │ HTTPS(直连)
                       │ (音频/文本)         │
                       ▼                    ▼
              ┌─────────────────┐   ┌────────────────┐
              │  你的后端        │   │  你的 LLM       │
              │  听写 + 合成     │   │  只做解读       │
              │  没有 LLM 密钥   │   │  密钥编在固件里 │
              └─────────────────┘   └────────────────┘
```

两处密钥**互不相见**：LLM 的密钥编在固件里，后端看不到；后端的 ASR/TTS 密钥只在后端的
`.env` 里，设备看不到。后端**不需要任何 LLM 密钥**。

> 因为密钥编在固件里，**别把自己编好的 `.bin` 直接发给别人**——那等于把密钥一起发出去。
> 每个人编译自己的固件，这也是为什么配置集中在 `main/chat_config.h` 一个文件里。

---

## 功能

- **语音录入生辰**：说"1998年3月15日早上7点半"，也能说农历；可以补出生城市做真太阳时校正
- **命盘页**：四柱按五行套色、日主、起运信息、大运长河
- **今日页**：运势指数、幸运色/数字/方位、黄历宜忌、一句话解盘
- **问事**：以你的完整命盘为上下文的语音问答
- **设置**：电量、Wi-Fi 状态、换网重新配网
- **手机配网**：设备开热点，手机连上自动弹配置页

---

## 硬件

FoloToy AI Passport（ESP32-C3 + 240×320 SPI 屏 + ES8311 codec + CW2017 电量计 + 三个按键）。
其它 ESP32 板子理论上换掉 `components/bsp` 就能跑，但没验证过。

接线、引脚定义、踩过的硬件坑、验收清单——上游那份硬件基线文档原样保留在
`docs/HARDWARE_BASELINE.md`([简体中文](docs/HARDWARE_BASELINE.zh_CN.md))。

---

## 烧录前要准备什么

设备本身不需要账号。但**说话**和**解读**这两件事要用外部服务，一共三样，
都是你自己的账号。下面这张表先看个全貌：

| | 干什么 | 在哪配 | 最省事的选择 |
|---|---|---|---|
| **LLM** | 写解盘文案、回答问事 | 固件 `main/chat_config.h` | 任何 OpenAI 兼容接口 |
| **ASR** | 把你说的话转成字 | 后端 `server/.env` | 默认本机 whisper，免费 |
| **TTS** | 把字念出来 | 后端 `server/.env` | 默认 edge-tts，免费免密钥 |

**只有 LLM 必须自己去开通账号**，ASR/TTS 用默认配置就能跑（都是免费的）。

### 1. LLM（必须，要花钱）

去任何一家开通，拿到 base_url、API Key、模型名。要求两条：**OpenAI 兼容的
`/chat/completions`**，并且**支持流式**（`stream: true`，否则语音会等整段说完才出声）。

常见的有 DeepSeek、OpenAI、月之暗面、智谱、硅基流动；本地跑 Ollama / vLLM 也行
（填局域网地址即可，那样连钱都不用花）。

一轮问事大概几百 token，日常用一个月也就几块钱。

### 2. ASR / TTS（有免费方案，可以先不管）

默认配置**不需要任何账号**：ASR 用本机的 faster-whisper（首次启动自动下模型），
TTS 用微软的 edge-tts（免费免密钥）。想跑得更快或者中文效果更好，有四条路：

| 路径 | 适合谁 | 要写代码吗 |
|---|---|---|
| **A 默认** faster-whisper + edge-tts | 先跑起来试试 | 不用，免费 |
| **B OpenAI 兼容** Groq / 硅基流动 等 | 后端机器弱（树莓派、NAS） | 不用，填配置 |
| **C 国内云厂商** 腾讯云 / 阿里云 / 百度 | 要好的中文效果和低延迟 | 二三十行 |
| **D 自建** GPU whisper / whisper.cpp / Piper | 语音不想出自己的服务器 | 看情况 |

每条路具体怎么配、腾讯云用哪个产品、自建选什么引擎，见
**[docs/INTEGRATIONS.md](docs/INTEGRATIONS.md)**。

---

## 跑起来

### 1. 后端（听写 + 合成）

跑在任何一台设备能访问到的机器上——云服务器、家里的小主机、树莓派、NAS 都行。

```bash
cd server
python3 -m venv venv && ./venv/bin/pip install -r requirements.txt
sudo apt install ffmpeg          # 音频转码要用

cp .env.example .env
$EDITOR .env                     # 至少改 AUTH_TOKEN;ASR/TTS 想用默认就不用动
./run.sh
```

监听 8765。**建议用 nginx 反代到 80/443**——很多家庭网络会拦冷门端口：

```nginx
location /tianji-voice {
    proxy_pass http://127.0.0.1:8765;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_read_timeout 3600s;
}
```

### 2. 固件

需要 ESP-IDF v5.5 以上。

```bash
cp main/chat_config.h.example main/chat_config.h
$EDITOR main/chat_config.h
```

这个文件是烧录前要填的**全部**配置，一共两块：

```c
// 你的后端在哪
#define CHAT_SERVER_HOST "your-server.example.com"
#define CHAT_SERVER_PORT 80
#define CHAT_SERVER_PATH "/tianji-voice"
#define CHAT_AUTH_TOKEN  "……"          // 与 server/.env 里的一模一样

// 你的 LLM
#define CHAT_LLM_BASE_URL "https://api.deepseek.com/v1"   // 到 /v1 为止
#define CHAT_LLM_API_KEY  "sk-……"
#define CHAT_LLM_MODEL    "deepseek-chat"
```

然后：

```bash
idf.py set-target esp32c3
idf.py build flash monitor
```

忘了复制 `chat_config.h` 的话，构建会直接停下并告诉你复制哪个文件，不会编到一半才炸。

`chat_config.h` 和 `server/.env` 都在 `.gitignore` 里，不会被误提交。

### 3. 首次开机：连 Wi-Fi

设备开一个叫 `Tianji-Setup` 的热点，手机连上后自动弹出配网页
（没弹就浏览器打开 `http://192.168.4.1`），填 Wi-Fi 名称和密码即可。

**配网页只管 Wi-Fi。** 后端地址和 LLM 是编译时定好的，要换得改
`main/chat_config.h` 重新烧录。之后要换网络，在「设置」页按 OK 重新配网。

---

## 端侧排盘引擎

设备上不做天文计算。`tools/gen_bazi_tables.py` 从
[lunar_python](https://github.com/6tail/lunar-python) 把 1900–2100 年的节气精确时刻、
农历朔日、黄历宜忌预先烘成 `main/bazi_tables.c`（约 50KB flash），
C 侧只剩查表 + 二分 + 干支取模。

```bash
# 重新生成表(改了年份范围之类才需要)
server/venv/bin/python tools/gen_bazi_tables.py

# 对拍:同一批生辰,C 引擎 vs Python 引擎,逐字段比
./tools/verify_bazi.sh 4000
```

对拍用例里专门塞了容易翻车的场景：每个节气交接时刻的 ±1 分钟、晚子时 23:30、闰日、
真太阳时倒退到上一年的边界、极端经度。**改了引擎就要跑一遍再烧。**

设备没有 RTC，日期靠 SNTP。还没校到时的时候，命盘页照常显示四柱和大运（它们只跟生辰有关），
只有"当前大运"和今日页要等授时。

---

## 接入你自己的服务

| | 在哪里调 | 怎么换 |
|---|---|---|
| **LLM** | 设备直接调 | 手机上填表，不用改代码、不用重烧 |
| **ASR / TTS** | 后端调 | 改两个 Python 函数 |

详见 **[docs/INTEGRATIONS.md](docs/INTEGRATIONS.md)** —— 包含 OpenAI 兼容接口怎么填、
非标准协议怎么改、设备字库这条硬约束、以及为什么后端还没法完全去掉。

## 想改代码

**[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)** —— 代码分层、怎么加一个页面、
排盘引擎的表怎么重新生成和对拍、内存约束（这是最主要的限制）、常见问题对照表。

一句话版本：改 UI 看 `main/demo_fortune.c`，改排盘看 `main/bazi_engine.c`，
**动过排盘就必须跑 `./tools/verify_bazi.sh` 再烧**。

## 目录

```
main/
  bazi_engine.c      端侧排盘引擎(纯 C99,不依赖 ESP-IDF,能在主机上编译对拍)
  bazi_tables.c      预烘的节气/朔日/宜忌表(生成物,勿手改)
  llm_client.c       设备直连 LLM(TLS + 流式 SSE),key 存 NVS
  demo_fortune.c     全部页面与交互
  chat_provision.c   手机配网 + AI 接口配置(热点 + 强制门户)
  app_net.c          Wi-Fi 单例 + SNTP
components/bsp/      屏幕/音频/按键/电量计驱动
server/
  voice_server.py    WebSocket 语音后端:听写 + 合成,不碰 LLM
  bazi/              Python 排盘引擎(端侧引擎的对拍基准 + 烘表来源)
tools/
  gen_bazi_tables.py 烘表
  verify_bazi.sh     两侧引擎对拍
  check_secrets.sh   推之前扫密钥
docs/
  DEVELOPMENT.md     继续开发
  INTEGRATIONS.md    接自己的 LLM / ASR / TTS
```

---

## 免责声明

命理内容是**娱乐向**的。不要拿它做医疗、健康、重大财务或人生决策。
提示词里也写了同样的红线，但模型不是每次都听话——请自行判断。

---

## 致谢 / Credits

这个项目站在别人的工作上面。下面这些是**原样留用**的，版权归原作者，
文件头都标了 SPDX 与来源：

**板级支持包 —— [folotoy/ai-passport](https://github.com/folotoy/ai-passport)（MIT）**

整套硬件驱动是他们的，我一行没改：

| | |
|---|---|
| `components/bsp/` | 屏幕(ST7789)、音频(ES8311)、按键、电量计(CW2017)、共享 I2C 驱动 |
| `main/ui_pixel_math.*` + `tests/` | 像素排版数学与单测 |
| `docs/AI_HARDWARE_DEVELOPMENT_GUIDE.md` | 硬件开发指南 |
| `docs/HARDWARE_BASELINE*.md` | 接线、引脚、踩坑记录、验收清单(上游 README 原文) |

没有这层驱动，这个项目起不来。另外照他们文件里的自述，BSP 本身又移植自
`trae_card`，所以这份 credit 要往上再传一层。

**字体（SIL Open Font License 1.1）**

`main/font_cn_16.c` / `main/font_mystic_28.c` 是用 `lv_font_conv` 把下面两款字体转成的
LVGL 位图，属于 OFL 意义上的衍生作品，随本项目一同以 OFL 分发，许可原文见 `licenses/`：

- [Noto Sans SC](https://fonts.google.com/noto/specimen/Noto+Sans+SC) —— 正文
- [Ma Shan Zheng](https://fonts.google.com/specimen/Ma+Shan+Zheng) —— 书法标题

**历法算法 —— [lunar-python](https://github.com/6tail/lunar-python)（MIT）**

`main/bazi_tables.c` 里的节气时刻、农历朔日、黄历宜忌全部由它算出后预烘；
`server/bazi/` 的排盘引擎也依赖它。端侧引擎能对得上专业口径，靠的是这个库。

---

## 许可

MIT，见 [`LICENSE`](LICENSE)。上游 BSP 的版权声明按 MIT 要求一并保留在其中。

内嵌字体另按 SIL OFL 1.1，见 [`licenses/`](licenses/)。

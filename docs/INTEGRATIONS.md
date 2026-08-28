# 接入你自己的 LLM / ASR / TTS

这个项目把三种外部服务分成了两处，因为它们的约束完全不同：

| | 在哪里调 | 谁的密钥 | 怎么换 |
|---|---|---|---|
| **LLM** | 设备直接调 | 存在设备 NVS 里 | 手机上填表，不用改代码、不用重烧 |
| **ASR / TTS** | 后端调 | 存在后端 `.env` 里 | 改两个 Python 函数 |

之所以这么分：LLM 是纯文本、一次往返，ESP32-C3 扛得住 TLS；音频是流式的、
数据量大、各家协议五花八门，放在后端灵活得多。

---

## 一、换 LLM（不用改代码）

设备直接请求任何 **OpenAI 兼容**的 `/chat/completions`。

1. 长按 OK 回首页 → 进「设置」→ 按 OK 进配置页（设备会开一个热点）
2. 手机连上 `Tianji-Setup`，自动弹出页面（没弹就浏览器打开 `http://192.168.4.1`）
3. 填「AI 服务」三栏，保存

| 栏位 | 填什么 | 例子 |
|---|---|---|
| 接口地址 | 到 `/v1` 为止，**不要**带 `/chat/completions` | `https://api.deepseek.com/v1` |
| API Key | 你自己的密钥 | `sk-...` |
| 模型名 | 服务商的模型标识 | `deepseek-chat` |

只想改 AI 配置、不想动 Wi-Fi 的话，把 Wi-Fi 两栏留空即可。

**密钥只写进设备的 NVS**：不进固件镜像、不经过后端、不上传任何地方。
换密钥重新填一次就行，不用重新烧录。

### 用非 OpenAI 协议的服务（如某些云厂商的原生接口）

两个办法：

- **省事**：很多云厂商都提供 OpenAI 兼容端点，优先找那个。
- **改代码**：`main/llm_client.c` 里 `llm_chat()` 一个函数就是全部请求逻辑——
  拼 JSON 请求体、发 POST、按行解析 SSE。换协议主要改两处：请求体的字段名，
  以及 `sse_line()` 里取增量文本的路径（现在取 `choices[0].delta.content`）。

### 会卡住的地方

- **TLS 记录大小**：`sdkconfig.defaults` 里把 `MBEDTLS_SSL_IN_CONTENT_LEN`
  从 16384 砍到了 8192（内存不够）。如果你的服务端发送超过 8KB 的 TLS 记录，
  握手会失败。调回 16384 内存未必够，届时得先省出别的地方。
- **必须支持流式**（`stream: true`）。设备靠增量逐句送去合成，非流式会退化成
  "等全部说完才出声"。
- **密钥长度**上限 160 字节，超了配置页会直接报错而不是悄悄截断。

---

## 二、换 ASR / TTS（改两个函数）

后端只有两个函数是"语音"，`server/voice_server.py` 里：

```python
async def transcribe_any(pcm: bytes) -> str
    # 进:PCM s16le / 16kHz / 单声道 的原始字节
    # 出:识别出来的文本

async def tts_to_pcm(text: str) -> bytes
    # 进:一句话
    # 出:PCM s16le / 16kHz / 单声道 的原始字节
```

**协议、分片、ADPCM 压缩、与设备的握手全都不用动**——它们在这两个函数外面。

内置了三种现成实现，用 `.env` 里的 `ASR_PROVIDER` / `TTS_PROVIDER` 切换：

| 值 | 说明 |
|---|---|
| `ASR_PROVIDER=local` | 本机跑 faster-whisper。免费，但吃 CPU，首次启动下约 500MB 模型 |
| `ASR_PROVIDER=openai` | 任何 OpenAI 兼容的 `/audio/transcriptions` |
| `TTS_PROVIDER=edge` | 微软 edge-tts，免费免密钥（默认） |
| `TTS_PROVIDER=openai` | 任何 OpenAI 兼容的 `/audio/speech` |

### 接一个云厂商的原生接口

照着下面的形状加一个分支就行。以 TTS 为例：

```python
async def tts_to_pcm(text: str) -> bytes:
    if TTS_PROVIDER == "mycloud":
        # 1. 调你的服务,拿到 mp3 / wav / 任意格式的字节
        audio = await my_cloud_tts(text, voice=TTS_VOICE)
        # 2. 交给下面已有的 ffmpeg 管道转成 PCM 16k 单声道
        return await _to_pcm16k(audio)
    ...
```

现成的 mp3→PCM 转换（ffmpeg 子进程）就在同一个函数里，直接复用。
ASR 那边同理：把音频交给你的服务，返回文本即可，`_pcm_to_wav()` 已经帮你
封好了 WAV 头。

### 一条硬约束：设备字库

**发给设备的文本必须落在设备字库内**（完整 GB2312）。设备的字库是编译进
固件的位图，没有的字会显示成空白。

`for_device()` 已经处理了：繁体转简体，字库外的字符按映射表替换或丢弃。
你新接的 ASR 返回的文本会自动过这一层——但如果你**绕过**了 `transcribe_any`
自己发文本给设备，记得手动调用它。

字库定义在 `server/device_charset.txt`，与固件里 `font_cn_16.c` 的生成参数同源。
要支持更多字，得同时改这两处并重新生成字体。

---

## 三、彻底不用后端行不行

不行，至少现在不行。ESP32-C3 上跑不了语音识别，而语音合成的数据量也不适合
让设备直接扛（内存见下）。

但后端已经很薄了：**它不需要任何 LLM 密钥**，只做两次格式转换。
你可以把它跑在树莓派、NAS、家里任何一台常开的机器上，
不一定要公网服务器（设备只要能访问到它就行）。

如果你想让设备也直连 ASR/TTS，`main/llm_client.c` 是现成的参考——
TLS + 流式解析都在那儿了。要解决的是内存：

```
一次 TLS 会话期间的空闲堆(实测)
  请求前 49.8KB → 收流中 36.9KB → 全程最低水位 18.4KB
```

再叠一路音频流上去，余量就不够了。真要做，得先把 12KB 的播放环、
2 个 LVGL 缓冲、ws 客户端这些地方省出来。

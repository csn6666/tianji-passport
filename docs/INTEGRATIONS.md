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

## 二、换 ASR / TTS

**ASR** = 语音识别，把人说的话转成文字。**TTS** = 语音合成，把文字念出来。
设备自己做不了这两件事（ESP32-C3 跑不动），所以交给后端。

### 先看你属于哪种情况

| 你的情况 | 建议走哪条 | 花钱吗 |
|---|---|---|
| 只想赶紧跑起来试试 | **A. 默认配置**，什么都不用改 | 不花 |
| 后端机器很弱（树莓派、老 NAS） | **B. OpenAI 兼容的云服务** | 按量，很便宜 |
| 在国内，想要好的中文效果和低延迟 | **C. 国内云厂商** | 按量，一般有免费额度 |
| 有自己的服务器，不想把语音发给第三方 | **D. 自建** | 不花，费电 |

---

### A. 默认配置（开箱即用，不花钱）

`.env` 里什么都不改就是这套：

```bash
ASR_PROVIDER=local      # 本机跑 faster-whisper
TTS_PROVIDER=edge       # 微软 edge-tts
```

- **ASR**：[faster-whisper](https://github.com/SYSTRAN/faster-whisper) 在你的后端机器上跑，
  完全离线，不需要任何账号。首次启动会自动下模型（`small` 约 500MB）。
  代价是吃 CPU——一句话大概要一两秒，机器弱的话更久。
- **TTS**：edge-tts 调微软 Edge 浏览器的朗读接口，**免费、不需要密钥**，中文音色也不错。
  它不是正式商用 API，稳定性看微软脸色，但个人玩足够了。

嫌慢可以把模型调小：`ASR_LOCAL_MODEL=base` 或 `tiny`（准确率会降）。

---

### B. OpenAI 兼容的云服务（最省事的付费方案）

很多服务商都提供跟 OpenAI 一样的接口，填几行配置就能用，**不用写代码**：

```bash
ASR_PROVIDER=openai
ASR_BASE_URL=https://api.groq.com/openai/v1     # 换成你的服务商
ASR_API_KEY=你的密钥
ASR_MODEL=whisper-large-v3

TTS_PROVIDER=openai
TTS_BASE_URL=https://api.openai.com/v1
TTS_API_KEY=你的密钥
TTS_MODEL=tts-1
TTS_VOICE=alloy
```

常见的有 OpenAI 自家、Groq（whisper 很快）、硅基流动、以及不少国内厂商的兼容端点。
**优先找服务商有没有"OpenAI 兼容"的说明**，有的话这条路最省事。

---

### C. 国内云厂商（腾讯云 / 阿里云 / 百度）

这些厂商的原生接口**不是 OpenAI 格式**，需要写一小段适配代码——大概二三十行。

以腾讯云为例，它有现成的
[语音识别 ASR](https://cloud.tencent.com/product/asr)（一句话识别 / 实时识别 / 录音文件识别）
和 [语音合成 TTS](https://cloud.tencent.com/product/tts)，
官方 Python SDK 在 [tencentcloud-speech-sdk-python](https://github.com/TencentCloud/tencentcloud-speech-sdk-python)。
我们这个场景用「一句话识别」就够（每次录音都是几秒的短句）。

阿里云叫「智能语音交互」，百度叫「短语音识别 / 语音合成」，形状都差不多。

**怎么接**：`server/voice_server.py` 里已经留好了两个空函数，实现它们就行：

```python
async def custom_transcribe(pcm: bytes) -> str:
    """进:PCM s16le 16kHz 单声道的原始字节。出:识别文本。"""
    wav = _pcm_to_wav(pcm)          # 大多数服务要 WAV,这个帮你加好文件头
    # ... 调你的 SDK,返回文本 ...
    return text


async def custom_tts(text: str) -> bytes:
    """进:一句话。出:音频字节,mp3/wav 都行 —— 外层会用 ffmpeg
    统一转成设备要的格式,采样率声道你都不用管。"""
    # ... 调你的 SDK,返回音频字节 ...
    return audio
```

然后在 `.env` 里：

```bash
ASR_PROVIDER=custom
TTS_PROVIDER=custom
```

密钥怎么放随你（一般是 `SecretId` / `SecretKey` 之类），从环境变量读就行，
`.env` 已经在 `.gitignore` 里了。

**注意**：这两个函数是 `async` 的。厂商 SDK 多半是同步阻塞的，别直接调，
否则会卡住整个后端。用线程池包一层：

```python
loop = asyncio.get_event_loop()
text = await loop.run_in_executor(None, sdk_recognize_sync, wav)
```

（`transcribe_any` 里现成的 `local` 分支就是这么写的，照抄即可。）

---

### D. 自建（有自己的服务器）

后端本来就是你自己的，"自建"就是把 ASR/TTS 也放在同一台机器上，
语音数据完全不出你的服务器。

- **ASR**：默认的 `local` 就是自建。有 GPU 的话把 faster-whisper 换成 GPU 模式
  （改 `WhisperModel(..., device="cuda")`），速度快一个数量级。
  也可以另跑一个 [whisper.cpp](https://github.com/ggerganov/whisper.cpp) 服务，
  它自带 OpenAI 兼容的 HTTP 接口——那样直接走上面的 **B**，连代码都不用改。
- **TTS**：可以跑本地合成引擎，把它包进 `custom_tts()` 即可。
  中文可选的有 [Piper](https://github.com/rhasspy/piper)（轻量、CPU 就能跑）、
  以及各种更重的模型。挑的时候注意两点：**能出 16kHz 单声道**（其实随便什么格式都行，
  ffmpeg 会转）、以及**单句延迟**——设备是逐句合成的，每句慢一秒，整段就慢很多。

后端不一定要公网服务器。树莓派、NAS、家里常开的旧电脑都行，**设备能访问到就够了**。

---

### 无论走哪条，都要过这一关：设备字库

**发给设备的文本必须落在设备字库内**（完整 GB2312）。字库是编译进固件的位图，
没有的字会显示成空白。

`for_device()` 已经处理了：繁体转简体、字库外的字符按映射表替换或丢弃。
`transcribe_any()` 的返回值会自动过这一层——但如果你**绕过它**自己发文本给设备，
记得手动调用 `for_device()`。

字库定义在 `server/device_charset.txt`，与固件里 `font_cn_16.c` 的生成参数同源。
要支持更多字得同时改这两处并重新生成字体。

---

### 接完怎么验

后端启动时会打出用的是哪套：

```
ASR: 本机 faster-whisper(small),加载中 ...
TTS: edge (zh-CN-XiaoxiaoNeural)
```

然后对着设备说话，看后端日志：

```
问命 ASR: '我今年适合换工作吗'      ← ASR 通了
合成: '从你的命盘来看...'           ← TTS 通了
```

设备上有字没声，多半是 TTS 那边报错了；说了话设备没反应，看 ASR 那行有没有出来。

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

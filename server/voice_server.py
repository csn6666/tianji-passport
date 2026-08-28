#!/usr/bin/env python3
"""天机 · 语音后端。只做两件事:把声音转成字,把字转成声音。

推理不在这儿 —— 排盘由设备本地算,LLM 由设备直接调用它自己配置的接口,
所以这个后端**不需要任何 LLM 的 key**。

想换成腾讯云/自建的 ASR/TTS,只要改两个函数:
    transcribe_any(pcm) -> str     PCM s16le 16k mono 进,文本出
    tts_to_pcm(text)    -> bytes   文本进,PCM s16le 16k mono 出
协议、分片、ADPCM 压缩都不用动。

设备通过 WebSocket 连上来,协议(单连接、逐轮):
  设备 -> 服务器:
    {"type":"start","mode":"ask|bazi|geo"}  开始录音
    <二进制 PCM s16le 16k mono>             音频分片(若干)
    {"type":"end"}                          录音结束,触发识别
    {"type":"say","text":"一句话"}           把这句合成语音发回来
    {"type":"abort"}                        丢弃当前轮
  服务器 -> 设备:
    {"type":"asr","text":...}               识别结果
    {"type":"bazi_parsed"|"geo_parsed",...} 生辰/出生地解析结果
    <二进制 ADPCM>                          TTS 音频分片
    {"type":"tts_end"}                      本轮音频发完
    {"type":"error","message":...}

配置见 .env.example。设备鉴权:连接时 ?token=... 必须匹配 AUTH_TOKEN。
"""
import asyncio
import audioop  # py3.12 中已弃用但可用;3.13 移除时改用 pip 包 audioop-lts
import io
import json
import logging
import os
import re
import wave

import edge_tts
import websockets
from openai import AsyncOpenAI

from bazi.parse import parse_birth, birth_to_engine_str, BirthParseError
from bazi.fortune import today_fortune, build_caption_prompt, char_color
from bazi.engine import _format_dayun_text, _format_chart_extras_text

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("voice")

SAMPLE_RATE = 16000
TTS_VOICE = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
AUTH_TOKEN = os.environ.get("AUTH_TOKEN", "")
PORT = int(os.environ.get("PORT", "8765"))

# ── ASR:本机 whisper 或任何 OpenAI 兼容的 /audio/transcriptions ──
# local  = faster-whisper 跑在本机,不花钱但吃 CPU,首次启动要下模型
# openai = 自己配一个云端 ASR(OpenAI / Groq / SiliconFlow / 通义 …),快且省资源
ASR_PROVIDER = os.environ.get("ASR_PROVIDER", "local").lower()
ASR_MODEL = os.environ.get("ASR_MODEL", "whisper-1")
ASR_LOCAL_MODEL = os.environ.get("ASR_LOCAL_MODEL", "small")

_whisper = None
_asr_client = None

if ASR_PROVIDER == "openai":
    _asr_client = AsyncOpenAI(base_url=os.environ["ASR_BASE_URL"],
                              api_key=os.environ["ASR_API_KEY"])
    log.info("ASR: 云端 %s", ASR_MODEL)
else:
    # 惰性加载:用云端 ASR 的人不必装 faster-whisper,也不用下模型
    from faster_whisper import WhisperModel
    log.info("ASR: 本机 faster-whisper(%s),加载中 ...", ASR_LOCAL_MODEL)
    _whisper = WhisperModel(ASR_LOCAL_MODEL, device="cpu", compute_type="int8")
    log.info("ASR 就绪")

# ── TTS:edge-tts(免费、免 key)或任何 OpenAI 兼容的 /audio/speech ──
TTS_PROVIDER = os.environ.get("TTS_PROVIDER", "edge").lower()
TTS_MODEL = os.environ.get("TTS_MODEL", "tts-1")
_tts_client = None
if TTS_PROVIDER == "openai":
    _tts_client = AsyncOpenAI(base_url=os.environ["TTS_BASE_URL"],
                              api_key=os.environ["TTS_API_KEY"])
log.info("TTS: %s (%s)", TTS_PROVIDER, TTS_VOICE)


# ---- 设备字符契约:发往设备的任何文本必须落在设备字库内 ----
# 设备字库 = 完整 GB2312(含符号区) + ASCII,与 font_cn_16 生成参数同源
# (server/device_charset.txt)。Whisper 会转写出繁体(如"時候/錢"),
# DeepSeek 也可能输出繁体/emoji——先繁转简,仍在字库外的字符直接丢弃。
from zhconv import convert as _zh_convert

with open(os.path.join(os.path.dirname(__file__), "device_charset.txt"),
          encoding="utf-8") as _f:
    _DEVICE_CHARS = set(_f.read()) | {chr(c) for c in range(0x20, 0x7F)} | {"\n"}


# 字库外但有同义字库内写法的,映射而不是丢弃(丢弃会改变语义,如年份少一位)
_CHAR_MAP = {"〇": "零",      # U+3007 农历纪年零,GBK 区,GB2312 没有
             "－": "-", "—": "—", "﹣": "-", "〜": "~"}


def for_device(text: str) -> str:
    """繁->简,映射同义字,再过滤设备字库外字符。发往设备的文本都要经过这里。"""
    text = _zh_convert(text, "zh-cn")
    text = "".join(_CHAR_MAP.get(ch, ch) for ch in text)
    return "".join(ch for ch in text if ch in _DEVICE_CHARS)


def _sanitize(o):
    """递归净化 JSON 对象里的全部字符串字段。"""
    if isinstance(o, str):
        return for_device(o)
    if isinstance(o, dict):
        return {k: _sanitize(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [_sanitize(v) for v in o]
    return o


def J(obj, **_kw) -> str:
    """发设备专用的 json.dumps:先净化所有字符串。文本出口唯一通道。"""
    return json.dumps(_sanitize(obj), ensure_ascii=False)


def _pcm_to_wav(pcm: bytes) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)
    return buf.getvalue()


def transcribe(pcm: bytes) -> str:
    """PCM s16le 16k mono -> 文本(本机 whisper 分支,阻塞,放线程里跑)。"""
    buf = io.BytesIO(_pcm_to_wav(pcm))
    segments, _info = _whisper.transcribe(
        buf, language="zh", vad_filter=True,
        initial_prompt="以下是普通话的句子,用简体中文书写。")
    return for_device("".join(s.text for s in segments).strip())


async def transcribe_any(pcm: bytes) -> str:
    """按 ASR_PROVIDER 走本机或云端。返回的文本一律过设备字库净化。"""
    if ASR_PROVIDER == "openai":
        f = ("speech.wav", _pcm_to_wav(pcm), "audio/wav")
        r = await _asr_client.audio.transcriptions.create(
            model=ASR_MODEL, file=f, language="zh")
        return for_device((r.text or "").strip())
    return await asyncio.get_event_loop().run_in_executor(None, transcribe, pcm)


async def tts_to_pcm(text: str) -> bytes:
    """TTS 输出 mp3，用 ffmpeg 转成设备要的 PCM s16le 16k mono。"""
    mp3 = bytearray()
    if TTS_PROVIDER == "openai":
        r = await _tts_client.audio.speech.create(
            model=TTS_MODEL, voice=TTS_VOICE, input=text, response_format="mp3")
        mp3.extend(r.read() if hasattr(r, "read") else r.content)
    else:
        async for chunk in edge_tts.Communicate(text, TTS_VOICE).stream():
            if chunk["type"] == "audio":
                mp3.extend(chunk["data"])
    proc = await asyncio.create_subprocess_exec(
        "ffmpeg", "-loglevel", "error", "-i", "pipe:0",
        "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", "-ar", str(SAMPLE_RATE),
        "pipe:1",
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
    )
    pcm, _ = await proc.communicate(bytes(mp3))
    return pcm


SENT_SPLIT = re.compile(r"(?<=[。！？；!?;\n])")


# ---------------------------------------------------------------- 玄历(八字运势)

# 问命/解盘人设。红线:
# 只用系统排盘数据、非宿命论、不做医疗/寿命/重大决策断言。TTS 场景故要求简短。
FORTUNE_PERSONA = (
    "你是一位温和笃定的八字命理师,在一个语音小设备上回答命主的问题。规则:"
    "1) 只依据下方系统排好的命盘数据作答,严禁自行推算任何干支、日期或大运;"
    "2) 回答口语化,总共不超过四句话:先给结论,再给一两句命理依据,"
    "提到干支术语时顺带用一句白话解释;"
    "3) 非宿命论:讲趋势和可行建议,不做绝对断言;绝不预测寿命、重病、生死;"
    "不给医疗、法律、投资的决策结论,涉及时提醒仅供参考;"
    "4) 不用markdown、列表和表情符号。")


async def handle_ask_turn(ws, pcm: bytes, ctx: dict) -> None:
    """问命轮:只把话转成字发回去。推理由设备自己拿命盘去问它配置的 LLM,
    再把回答按句发 {"type":"say"} 回来合成语音。"""
    text = await transcribe_any(pcm)
    log.info("问命 ASR: %r", text)
    if not text:
        await ws.send(J({"type": "error", "message": "没听清，请再说一次"}))
        await ws.send(J({"type": "tts_end"}))
        return
    await ws.send(J({"type": "asr", "text": text}))


async def speak_sentence(ws, sent: str, adp: list) -> None:
    pcm = await tts_to_pcm(sent)
    if len(pcm) % 4:                       # 补齐到 4 字节(2 采样 = 1 压缩字节)
        pcm += b"\x00" * (4 - len(pcm) % 4)
    data, adp[0] = audioop.lin2adpcm(pcm, 2, adp[0])
    # 压缩后按 1KB 分片 ≈ 256ms 音频
    for i in range(0, len(data), 1024):
        await ws.send(data[i:i + 1024])


# ---------------------------------------------------------------- 出生地(真太阳时)

import urllib.parse
import urllib.request

_geo_cache: dict = {}


def _geocode(city: str):
    """城市名 -> 经纬度(OpenStreetMap Nominatim)。"""
    if city in _geo_cache:
        return _geo_cache[city]
    url = ("https://nominatim.openstreetmap.org/search?" +
           urllib.parse.urlencode({"q": city, "format": "json", "limit": 1,
                                   "accept-language": "zh"}))
    req = urllib.request.Request(url, headers={"User-Agent": "tianji-passport/1.0"})
    with urllib.request.urlopen(req, timeout=10) as r:
        data = json.loads(r.read().decode())
    if not data:
        return None
    item = data[0]
    res = {"city": for_device(item.get("name") or city),
           "longitude": round(float(item["lon"]), 2)}
    _geo_cache[city] = res
    return res


_GEO_STRIP = ("我出生在", "出生在", "出生地是", "出生地", "我在", "是", "在")


async def handle_geo_input(ws, pcm: bytes) -> None:
    """出生地录入轮:ASR -> 地理编码 -> 回显城市与经度。"""
    text = await transcribe_any(pcm)
    log.info("出生地 ASR: %r", text)
    if not text:
        await ws.send(J({"type": "geo_error", "message": "没听清，请再说一次"}))
        return
    t = text.strip().strip("。．.!！,，?？")
    for p in _GEO_STRIP:
        if t.startswith(p):
            t = t[len(p):]
            break
    t = t.strip() or text
    try:
        res = await asyncio.get_event_loop().run_in_executor(None, _geocode, t)
    except Exception:
        log.exception("geocode 失败")
        res = None
    if not res:
        await ws.send(J(
            {"type": "geo_error", "message": f"没找到「{t}」，请说城市名，如：北京"},
            ensure_ascii=False))
        return
    await ws.send(J({"type": "geo_parsed", **res, "asr": text},
                             ensure_ascii=False))
    log.info("出生地: %s 东经%.2f", res["city"], res["longitude"])


async def handle_bazi_input(ws, pcm: bytes) -> None:
    """生日录入轮:ASR -> 确定性解析(绝不用 LLM 推算) -> 回显给设备确认。"""
    text = await transcribe_any(pcm)
    log.info("生日 ASR: %r", text)
    if not text:
        await ws.send(J({"type": "bazi_error", "message": "没听清，请再说一次"}))
        return
    try:
        birth = parse_birth(text)
    except BirthParseError as e:
        await ws.send(J(
            {"type": "bazi_error", "message": str(e), "asr": text}, ensure_ascii=False))
        return
    await ws.send(J(
        {"type": "bazi_parsed", "birth": birth, "asr": text}, ensure_ascii=False))
    log.info("生日解析: %s", birth["echo"].replace("\n", " / "))


async def handle_say(ws, text: str, adp: list) -> None:
    """{"type":"say","text":"一句话"} -> TTS 音频帧。设备算完 LLM 才发这个,
    所以合成期间设备那边的 TLS 已经关了,内存不打架。"""
    text = for_device(text).strip()
    if not text:
        return
    log.info("合成: %r", text)
    await speak_sentence(ws, text, adp)


async def handler(ws):
    from urllib.parse import parse_qs, urlparse
    q = parse_qs(urlparse(ws.request.path).query)
    if AUTH_TOKEN and q.get("token", [""])[0] != AUTH_TOKEN:
        log.warning("拒绝未授权连接 %s", ws.remote_address)
        await ws.close(4001, "unauthorized")
        return
    log.info("设备已连接 %s", ws.remote_address)
    ctx: dict = {}
    audio = bytearray()
    recording = False
    rec_mode = "ask"        # ask | bazi | geo
    adp = [None]            # ADPCM 编码器状态(audioop 要 None 或元组),跨句连续
    turn_task: asyncio.Task | None = None   # 正在生成/播报的回答任务

    async def cancel_turn():
        """设备打断/发起新一轮时,立刻掐掉旧回答(LLM流+TTS一并终止)。"""
        nonlocal turn_task
        if turn_task and not turn_task.done():
            turn_task.cancel()
            try:
                await turn_task
            except asyncio.CancelledError:
                pass
            log.info("旧回答已被打断")
        turn_task = None

    async def run_turn(coro):
        try:
            await coro
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("回答生成失败")
            try:
                await ws.send(J(
                    {"type": "error", "message": "服务出错了，请稍后再试"}))
                await ws.send(J({"type": "tts_end"}))
            except Exception:
                pass

    try:
        async for msg in ws:
            if isinstance(msg, bytes):
                if recording:
                    audio.extend(msg)
                continue
            ev = json.loads(msg)
            if ev["type"] == "start":
                await cancel_turn()             # 新提问优先,旧回答让路
                audio.clear()
                recording = True
                rec_mode = ev.get("mode", "ask")
                adp[0] = None       # 设备那边也会 adpcm_reset(),两边必须同步
            elif ev["type"] == "end":
                recording = False
                if not audio:
                    continue
                pcm = bytes(audio)
                if rec_mode == "bazi":
                    turn_task = asyncio.create_task(run_turn(handle_bazi_input(ws, pcm)))
                elif rec_mode == "geo":
                    turn_task = asyncio.create_task(run_turn(handle_geo_input(ws, pcm)))
                else:
                    turn_task = asyncio.create_task(run_turn(handle_ask_turn(ws, pcm, ctx)))
            elif ev["type"] == "say":
                # 设备已经拿到 LLM 的回答,这里只负责把字变成声音
                await handle_say(ws, ev.get("text", ""), adp)
                await ws.send(J({"type": "tts_end"}))
            elif ev["type"] == "abort":
                await cancel_turn()
                recording = False
                audio.clear()
            elif ev["type"] == "ping":
                await ws.send(J({"type": "pong"}))
    except websockets.ConnectionClosed:
        pass
    finally:
        if turn_task and not turn_task.done():
            turn_task.cancel()
        log.info("设备断开 %s", ws.remote_address)


async def main():
    async with websockets.serve(handler, "0.0.0.0", PORT, max_size=2 ** 22):
        log.info("语音服务已启动 ws://0.0.0.0:%d", PORT)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())

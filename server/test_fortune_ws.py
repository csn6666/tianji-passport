#!/usr/bin/env python3
"""玄历 ws 协议端到端测试:模拟设备语音报生日 -> bazi_parsed -> fortune -> fortune_data"""
import asyncio
import json
import subprocess
import sys

import edge_tts
import websockets

URL = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8765/?token=change-me"


async def tts_pcm(text):
    mp3 = bytearray()
    async for c in edge_tts.Communicate(text, "zh-CN-YunxiNeural").stream():
        if c["type"] == "audio":
            mp3.extend(c["data"])
    return subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-i", "pipe:0",
         "-f", "s16le", "-ac", "1", "-ar", "16000", "pipe:1"],
        input=bytes(mp3), capture_output=True).stdout


async def main():
    pcm = await tts_pcm("1998年3月15日早上7点半")
    async with websockets.connect(URL, max_size=2**22) as ws:
        # 1. 生日录入轮
        await ws.send(json.dumps({"type": "start", "mode": "bazi"}))
        for i in range(0, len(pcm), 2048):
            await ws.send(pcm[i:i+2048])
        await ws.send(json.dumps({"type": "end"}))
        ev = json.loads(await asyncio.wait_for(ws.recv(), 60))
        print("[1] bazi 轮:", ev["type"])
        assert ev["type"] == "bazi_parsed", ev
        print("    ASR:", ev["asr"])
        print("    回显:", ev["birth"]["echo"].replace("\n", " / "))
        assert ev["birth"]["year"] == 1998 and ev["birth"]["hour"] == 7

        # 2. 排盘 + 今日运势
        await ws.send(json.dumps(
            {"type": "fortune", "birth": ev["birth"], "gender": "男"}))
        ev2 = json.loads(await asyncio.wait_for(ws.recv(), 60))
        print("[2] fortune:", ev2["type"])
        assert ev2["type"] == "fortune_data", ev2
        c, t = ev2["chart"], ev2["today"]
        print(f"    四柱: {c['eight_char']}  当前大运: {c['current_dayun']}")
        print(f"    今日: {t['liuri']}日 喜{t['favorable']} 色={t['color_name']}"
              f"(#{t['color_rgb']:06X}) 数={t['number']} 方位={t['direction']} 指数={t['score']}")
        print(f"    宜: {t['yi']}  忌: {t['ji']}")
        print(f"    文案: 「{t['caption']}」")
        raw = json.dumps(ev2, ensure_ascii=False).encode()
        print(f"    JSON 大小: {len(raw)} bytes (设备缓冲需 >= 此值)")
        assert c["eight_char"] == "戊寅 乙卯 辛酉 壬辰"

        # 3. 已排盘后语音问答应带八字上下文
        pcm2 = await tts_pcm("今天适合穿什么颜色的衣服")
        await ws.send(json.dumps({"type": "start"}))
        for i in range(0, len(pcm2), 2048):
            await ws.send(pcm2[i:i+2048])
        await ws.send(json.dumps({"type": "end"}))
        texts = []
        while True:
            m = await asyncio.wait_for(ws.recv(), 90)
            if isinstance(m, bytes):
                continue
            e = json.loads(m)
            if e["type"] == "reply":
                texts.append(e["text"])
            if e["type"] == "tts_end":
                break
        print("[3] 带八字上下文的语音问答:", "".join(texts))
    print("\nWS-E2E PASS")


asyncio.run(main())

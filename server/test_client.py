#!/usr/bin/env python3
"""模拟设备的测试客户端:合成一句话当作"麦克风输入"发给语音服务,打印收到的一切。"""
import asyncio
import json
import subprocess
import sys

import edge_tts
import websockets

URL = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8765/?token=test-token"
QUESTION = sys.argv[2] if len(sys.argv) > 2 else "你好，请用一句话介绍你自己"


async def main():
    # 用 TTS 合成"用户说的话",转成设备同款 PCM
    mp3 = bytearray()
    async for c in edge_tts.Communicate(QUESTION, "zh-CN-YunxiNeural").stream():
        if c["type"] == "audio":
            mp3.extend(c["data"])
    pcm = subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-i", "pipe:0",
         "-f", "s16le", "-ac", "1", "-ar", "16000", "pipe:1"],
        input=bytes(mp3), capture_output=True).stdout
    print(f"[client] 模拟语音 {len(pcm)} bytes ({len(pcm)/32000:.1f}s): {QUESTION!r}")

    async with websockets.connect(URL, max_size=2**22) as ws:
        await ws.send(json.dumps({"type": "start"}))
        for i in range(0, len(pcm), 2048):        # 模拟设备 64ms 分片
            await ws.send(pcm[i:i+2048])
        await ws.send(json.dumps({"type": "end"}))
        print("[client] 音频已发送,等待回复 ...")

        audio_bytes = 0
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=60)
            if isinstance(msg, bytes):
                audio_bytes += len(msg)
                continue
            ev = json.loads(msg)
            print(f"[client] <- {ev}")
            if ev["type"] == "tts_end":
                break
        print(f"[client] 共收到 TTS 音频 {audio_bytes} bytes ({audio_bytes/32000:.1f}s)")


asyncio.run(main())

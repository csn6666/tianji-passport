#!/bin/bash
# 启动语音后端。首次使用前把 .env.example 复制成 .env 并填入 key。
cd "$(dirname "$0")"
set -a
source .env
set +a
exec ./venv/bin/python voice_server.py

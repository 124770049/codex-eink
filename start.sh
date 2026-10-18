#!/bin/bash
# Codex 额度墨水屏 · 后台启动
DIR="$(cd "$(dirname "$0")" && pwd)"
PID_FILE="$DIR/codex-eink.pid"
PORT=8090
PYTHON="/Users/jue/.pyenv/versions/3.13.3/bin/python3"

if lsof -ti:$PORT >/dev/null 2>&1; then
    echo "端口 $PORT 已被占用，服务可能已在运行 (PID: $(lsof -ti:$PORT | tr '\n' ' '))"
    exit 1
fi

nohup "$PYTHON" "$DIR/codex_eink.py" --serve --port $PORT >> "$DIR/server.log" 2>&1 &
echo $! > "$PID_FILE"
echo "已启动 (PID $(cat "$PID_FILE")) · http://0.0.0.0:$PORT"
echo "日志: $DIR/server.log"

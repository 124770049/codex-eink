#!/bin/bash
# Codex 额度墨水屏 · 停止
DIR="$(cd "$(dirname "$0")" && pwd)"
PID_FILE="$DIR/codex-eink.pid"
PORT=8090

PIDS="$(lsof -ti:$PORT 2>/dev/null)"
if [ -z "$PIDS" ]; then
    echo "服务未在运行"
    rm -f "$PID_FILE"
    exit 0
fi

kill $PIDS
rm -f "$PID_FILE"
echo "已停止 (PID: $(echo $PIDS | tr '\n' ' '))"
echo "提示: 若已注册 launchd 开机自启，进程会被自动拉起；彻底停用请运行:"
echo "  launchctl bootout gui/\$(id -u)/com.jue.codex-eink"

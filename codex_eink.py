#!/usr/bin/env python3
"""
额度墨水屏看板 · Mac 端 (多页)
- 页 0: Codex 额度 (ChatGPT 桌面版 ~/.codex/auth.json -> wham/usage, 走代理)
- 页 1: TRAE 积分 (Trae CN 加密存储解密 token -> ide_user_ent_usage, 直连)
- 页 2: 硬件监控 (CPU / 内存 / 磁盘 / 网络, 10 秒刷新)
- 各页独立抓取、独立渲染、失败不影响其他页

用法:
  python3 codex_eink.py --once            # 抓取一次并生成 preview.png 后退出
  python3 codex_eink.py --serve           # 常驻: 定时刷新 + HTTP 供图
端点:
  /bitmap?page=N       1-bit 打包位图 (400x300, MSB first, 1=黑, 15000 字节)
  /preview.png?page=N  PNG 预览 (浏览器调试用)
  /json?page=N         最近一次接口原始返回 + 抓取时间
  /                    状态页(两页预览)
"""

import argparse
import base64
import hashlib
import io
import json
import sqlite3
import subprocess
import threading
import time
import urllib.request
import urllib.error
import urllib.parse
import shutil
import tempfile
import os
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# ---------- 配置 ----------
WIDTH, HEIGHT = 400, 300
USAGE_URL = "https://chatgpt.com/backend-api/wham/usage"
AUTH_PATH = Path.home() / ".codex" / "auth.json"

TRAE_STORAGE = Path.home() / "Library/Application Support/Trae CN/User/globalStorage/storage.json"
TRAE_AUTH_KEY = "iCubeAuthInfo://icube.cloudide"
TRAE_USAGE_URL = "https://api.trae.cn/trae/api/v2/pay/ide_user_ent_usage"

TRAE_AGENT_DIR = Path.home() / "Library/Application Support/Trae CN/ModularData/ai-agent"
CODEX_DIR = Path.home() / ".codex"

FONT_CANDIDATES = [
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
]
FONT_PATH = next((p for p in FONT_CANDIDATES if Path(p).exists()), None)
USER_AGENT = "codex-cli/0.162.0"
PROXY = "http://127.0.0.1:7890"  # launchd 服务不继承终端代理环境变量, 必须显式指定
REFRESH_SECONDS = 300            # 默认 5 分钟刷一次
HTTP_PORT = 8090
WARN_THRESHOLD = 90              # 用量 >= 90% 显示告急角标
PAGE_COUNT = 4
HW_REFRESH_SECONDS = 10  # 硬件页刷新间隔(墨水屏建议 ≥10s)

PROJECT_DIR = Path(__file__).resolve().parent
PREVIEW_PATH = PROJECT_DIR / "preview.png"
LATEST_JSON_PATH = PROJECT_DIR / "latest.json"


def http_json(url, headers, data=None, proxy=True, timeout=20):
    """通用 JSON 请求。"""
    body = json.dumps(data).encode() if data is not None else None
    req = urllib.request.Request(url, data=body, headers=headers)
    if proxy and PROXY:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler(
            {"http": PROXY, "https": PROXY}))
    else:
        opener = urllib.request.build_opener()
    with opener.open(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


# ---------- 页 0: Codex ----------
def fetch_codex(retry=True):
    """wham/usage。401 时重新读 auth.json 再试一次(App 会自动刷新 token)。"""
    with open(AUTH_PATH) as f:
        auth = json.load(f)
    tokens = auth.get("tokens") or {}
    access_token, account_id = tokens.get("access_token"), tokens.get("account_id")
    if not access_token:
        raise RuntimeError("auth.json 中无 access_token，请先登录 ChatGPT 桌面版")
    headers = {
        "Authorization": f"Bearer {access_token}",
        "ChatGPT-Account-Id": account_id or "",
        "User-Agent": USER_AGENT,
    }
    try:
        return http_json(USAGE_URL, headers, proxy=True)
    except urllib.error.HTTPError as e:
        if e.code == 401 and retry:
            time.sleep(2)
            return fetch_codex(retry=False)
        raise RuntimeError(f"HTTP {e.code}: {e.read().decode()[:200]}")


def window_name(seconds):
    for target, name in [(18000, "5 小时窗口"), (86400, "24 小时窗口"),
                         (604800, "每周窗口"), (2592000, "30 天窗口")]:
        if abs(seconds - target) < 600:
            return name
    if seconds % 86400 == 0:
        return f"{seconds // 86400} 天窗口"
    if seconds % 3600 == 0:
        return f"{seconds // 3600} 小时窗口"
    return f"{seconds // 60} 分钟窗口"


def fmt_countdown(seconds):
    if seconds <= 0:
        return "即将重置"
    if seconds < 3600:
        return f"{(seconds + 59) // 60} 分钟后"
    if seconds < 86400:
        return f"{(seconds + 1799) // 3600} 小时后"
    return f"{(seconds + 86399) // 86400} 天后"


def draw_bar(draw, x, y, w, h, percent):
    draw.rectangle([x, y, x + w, y + h], outline=0, width=2)
    inner = int((w - 6) * min(max(percent, 0), 100) / 100)
    if inner > 0:
        draw.rectangle([x + 3, y + 3, x + 3 + inner, y + h - 3], fill=0)


def draw_header(draw, fonts, title, badge):
    draw.text((16, 10), title, font=fonts.title, fill=0)
    if badge:
        pw = draw.textbbox((0, 0), badge, font=fonts.small)[2] + 12
        draw.rectangle([384 - pw, 12, 384, 32], outline=0, width=1)
        draw.text((384 - pw + 6, 15), badge, font=fonts.small, fill=0)
    draw.line([(16, 42), (384, 42)], fill=0, width=1)


def draw_footer(draw, fonts, fetched_at, error):
    draw.line([(16, 268), (384, 268)], fill=0, width=1)
    if error:
        status = f"更新失败 {datetime.now():%H:%M} · 显示 {fetched_at:%H:%M} 旧数据" if fetched_at \
            else f"更新失败: {str(error)[:34]}"
    else:
        status = f"更新于 {fetched_at:%H:%M} · 每 {REFRESH_SECONDS // 60} 分钟刷新 · KEY 切页"
    draw.text((16, 276), status, font=fonts.small, fill=0)


def _detect_wal_activity(wal_paths, samples=5, interval=1.5, recent_seconds=60):
    """通用 WAL 活动检测。
    判定 running=True 的条件 (满足任一即可):
      1. 采样窗口内任意 WAL 被写入 (mtime 变化) → 正在活跃执行
      2. 任意 WAL 在最近 recent_seconds 秒内被修改过 → 近期有活动
    多文件 + 较长窗口可捕捉写入间隔较长的活动 (如 Codex ~20s 一次)。"""
    paths = [p for p in wal_paths if p.exists()]
    if not paths:
        return False, None
    now = time.time()  # 在采样开始时记录, 避免采样耗时导致时间差偏大
    history = {p: [] for p in paths}
    for _ in range(samples):
        for p in paths:
            try:
                history[p].append(p.stat().st_mtime)
            except OSError:
                history[p].append(0)
        time.sleep(interval)
    # 条件 1: 采样窗口内有 mtime 变化
    changed = any(
        any(history[p][i] != history[p][i - 1] for i in range(1, len(history[p])))
        for p in paths
    )
    # 条件 2: 最近 recent_seconds 内有修改
    all_ts = [ts for p in paths for ts in history[p] if ts > 0]
    last_ts = max(all_ts) if all_ts else 0
    recent = (now - last_ts) < recent_seconds if last_ts else False
    running = changed or recent
    last_activity = datetime.fromtimestamp(last_ts) if last_ts else None
    return running, last_activity


def get_trae_status():
    """检测 Trae Agent 是否正在执行任务 (database.db-wal 写入活动)。"""
    return _detect_wal_activity([TRAE_AGENT_DIR / "database.db-wal"])


def get_codex_status():
    """检测 Codex 是否正在执行任务 (logs_2 / state_5 / thread_history WAL 写入活动)。"""
    return _detect_wal_activity([
        CODEX_DIR / "logs_2.sqlite-wal",
        CODEX_DIR / "state_5.sqlite-wal",
        CODEX_DIR / "thread_history_1.sqlite-wal",
    ])


def draw_agent_status(draw, fonts, running, last_activity, label="AGENT"):
    """右下角绘制运行状态。区域: y≈246-264, 右对齐 x=384。
    label: 状态前缀名 (如 CODEX / TRAE)。"""
    y = 248
    if running:
        dot = "●"
        text = f"{label} 运行中"
    else:
        dot = "○"
        text = f"{label} 空闲"
        if last_activity:
            text += f" {last_activity:%H:%M}"
    label_str = f"{dot} {text}"
    pw = draw.textbbox((0, 0), label_str, font=fonts.small)[2]
    draw.text((384 - pw, y), label_str, font=fonts.small, fill=0)


def render_codex(payload, fetched_at, error=None):
    img = Image.new("1", (WIDTH, HEIGHT), 1)
    draw = ImageDraw.Draw(img)
    fonts = Fonts()
    draw_header(draw, fonts, "CODEX 额度", (payload or {}).get("plan_type", "?").upper())

    y = 52
    rate = (payload or {}).get("rate_limit") or {}
    primary = rate.get("primary_window")
    secondary = rate.get("secondary_window")

    def section(win):
        nonlocal y
        used = win.get("used_percent", 0)
        seconds = win.get("limit_window_seconds", 0)
        name = window_name(seconds)
        reset_after = win.get("reset_after_seconds") or 0
        reset_at = win.get("reset_at")

        draw.text((16, y), name, font=fonts.sec, fill=0)
        if used >= WARN_THRESHOLD:
            badge = "告急"
            bw = draw.textbbox((0, 0), badge, font=fonts.small)[2] + 12
            draw.rectangle([16 + 110, y + 1, 16 + 110 + bw, y + 20], fill=0)
            draw.text((16 + 116, y + 3), badge, font=fonts.small, fill=1)

        pct = f"{used}%"
        pw = draw.textbbox((0, 0), pct, font=fonts.big, stroke_width=1)[2]
        draw.text((384 - pw, y - 6), pct, font=fonts.big, fill=0,
                  stroke_width=1, stroke_fill=0)

        draw_bar(draw, 16, y + 54, 368, 18, used)

        reset_txt = f"{fmt_countdown(reset_after)}重置"
        if reset_at:
            reset_txt += " · " + datetime.fromtimestamp(reset_at).strftime("%m/%d %H:%M")
        draw.text((16, y + 80), reset_txt, font=fonts.small, fill=0)
        y += 100

    if primary:
        section(primary)
    if secondary:
        y += 8
        section(secondary)
    elif primary:
        credits = (payload or {}).get("credits") or {}
        if credits.get("has_credits") and credits.get("balance") is not None:
            txt = f"Credits 余额: {credits['balance']}"
        else:
            txt = "当前套餐为单窗口额度"
        draw.text((16, y + 10), txt, font=fonts.small, fill=0)

    if not primary and not secondary:
        draw.text((16, y + 20), "无额度数据", font=fonts.sec, fill=0)

    running, last = get_codex_status()
    draw_agent_status(draw, fonts, running, last, label="CODEX")
    draw_footer(draw, fonts, fetched_at, error)
    return img


# ---------- 页 1: TRAE ----------
# Trae CN byteCrypto 解密 (AES-128-CBC, 自包含本地算法, 无需 Keychain)
_TRAE_GLE = bytes([82,9,106,213,48,54,165,56,191,64,163,158,129,243,215,251,124,227,57,130,
                   155,47,255,135,52,142,67,68,196,222,233,203,84,123,148,50,166,194,35,61,
                   238,76,149,11,66,250,195,78,8,46,161,102,40,217,36,178,118,91,162,73,109,139,209,37])
_TRAE_ZLE = bytes([31,221,168,51,136,7,199,49,177,18,16,89,39,128,236,95,96,81,127,169,25,181,74,
                   13,45,229,122,159,147,201,156,239,160,224,59,77,174,42,245,176,200,235,187,60,
                   131,83,153,97,23,43,4,126,186,119,214,38,225,105,20,99,85,33,12,125])


def _aes_cbc_decrypt(key16, iv16, data):
    p = subprocess.run(["openssl", "enc", "-d", "-aes-128-cbc", "-nopad",
                        "-K", key16.hex(), "-iv", iv16.hex()],
                       input=data, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError("openssl decrypt fail")
    return p.stdout


def trae_load_token():
    """从 Trae CN 加密存储解密出当前 token (IDE 运行时会自动续期写回)。"""
    storage = json.loads(TRAE_STORAGE.read_text())
    enc = storage.get(TRAE_AUTH_KEY)
    if not enc:
        raise RuntimeError("Trae CN 未登录 (storage 中无认证信息)")
    data = base64.b64decode(enc)
    key = data[6:38]
    ct = data[38:]
    s = bytearray(128)
    s[0:64] = hashlib.sha512(key).digest()
    s[64:128] = bytes(g ^ z for g, z in zip(_TRAE_GLE, _TRAE_ZLE))
    s[0:64] = hashlib.sha512(bytes(s)).digest()
    plain = _aes_cbc_decrypt(bytes(s[0:16]), bytes(s[16:32]), ct)
    pad = plain[-1]
    if 1 <= pad <= 16:
        plain = plain[:-pad]
    digest, payload = plain[:64], plain[64:]
    if hashlib.sha512(payload).digest() != digest:
        raise RuntimeError("认证数据校验失败")
    info = json.loads(payload.decode())
    exp = info.get("expiredAt")
    return info["token"], exp


def fetch_trae():
    """TRAE 积分用量。token 每次实时解密(IDE 开着会自动续期)。"""
    token, _ = trae_load_token()
    try:
        return http_json(
            TRAE_USAGE_URL,
            headers={"Content-Type": "application/json",
                     "Authorization": f"Cloud-IDE-JWT {token}"},
            data={"require_usage": True, "req_source": 1},
            proxy=False,  # api.trae.cn 国内直连
        )
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"HTTP {e.code}: {e.read().decode()[:150]}")


def render_trae(payload, fetched_at, error=None):
    img = Image.new("1", (WIDTH, HEIGHT), 1)
    draw = ImageDraw.Draw(img)
    fonts = Fonts()
    summary = (payload or {}).get("usage_summary") or {}
    packs = (payload or {}).get("user_entitlement_pack_list") or []

    plan = "?"
    if packs:
        desc = packs[0].get("display_desc") or ""
        for word in ("Pro", "Max", "Lite", "Enterprise"):
            if word.lower() in desc.lower():
                plan = word
                break
        else:
            plan = desc[:6] or "?"
    draw_header(draw, fonts, "TRAE 积分", plan)

    used = summary.get("consumed_amount")
    total = summary.get("total_amount")
    ratio = summary.get("consumption_ratio")

    if used is None or total is None or not total:
        draw.text((16, 72), "无积分数据", font=fonts.sec, fill=0)
        draw_footer(draw, fonts, fetched_at, error)
        return img

    pct = int(round((ratio or (used / total)) * 100))
    pw = draw.textbbox((0, 0), f"{pct}%", font=fonts.big, stroke_width=1)[2]
    draw.text((384 - pw, 46), f"{pct}%", font=fonts.big, fill=0,
              stroke_width=1, stroke_fill=0)
    draw.text((16, 52), "积分用量", font=fonts.sec, fill=0)
    if pct >= WARN_THRESHOLD:
        badge = "告急"
        bw = draw.textbbox((0, 0), badge, font=fonts.small)[2] + 12
        draw.rectangle([16 + 110, 53, 16 + 110 + bw, 72], fill=0)
        draw.text((16 + 116, 55), badge, font=fonts.small, fill=1)

    draw_bar(draw, 16, 106, 368, 18, pct)
    draw.text((16, 132), f"已用 {used:g} / 共 {total:g} 积分", font=fonts.small, fill=0)

    if packs:
        base = packs[0].get("entitlement_base_info") or {}
        name = (packs[0].get("display_desc") or "").replace("会员 ", "")
        expire = base.get("expire_time")
        line = name or "套餐"
        if expire:
            line += f" · {datetime.fromtimestamp(expire):%m/%d} 到期"
        draw.text((16, 156), line, font=fonts.small, fill=0)
        extra = len(packs) - 1
        if extra > 0:
            draw.text((16, 174), f"另有 {extra} 个权益包", font=fonts.small, fill=0)

    if error:
        msg = str(error)
        draw.text((16, 220), f"提示: {msg[:44]}", font=fonts.tiny, fill=0)
    elif pct >= WARN_THRESHOLD:
        draw.text((16, 220), "积分即将耗尽, 可在 Trae 内购买加量包", font=fonts.tiny, fill=0)

    running, last = get_trae_status()
    draw_agent_status(draw, fonts, running, last, label="TRAE")
    draw_footer(draw, fonts, fetched_at, error)
    return img


# ---------- 硬件监控 (页 2) ----------
_NET_CACHE = {"ts": 0, "ibytes": 0, "obytes": 0}


def _run(cmd):
    try:
        return subprocess.check_output(cmd, shell=True, text=True, timeout=8)
    except Exception:
        return ""


def fetch_hardware():
    """采集 Mac 硬件使用情况: CPU / 内存 / 磁盘 / 网络。"""
    # --- CPU ---
    cpu_out = _run("top -l 1 -n 0 | grep 'CPU usage'")
    cpu_pct = 0.0
    if cpu_out:
        # "CPU usage: 11.69% user, 16.91% sys, 71.39% idle"
        parts = cpu_out.replace("%", "").split()
        try:
            user = float(parts[2])
            sys = float(parts[4])
            cpu_pct = round(user + sys, 1)
        except (IndexError, ValueError):
            pass
    ncpu = int(_run("sysctl -n hw.ncpu").strip() or "0")
    load_raw = _run("sysctl -n vm.loadavg").strip()
    # "{ 6.93 5.64 5.34 }"
    load = load_raw.strip("{} ").split() if load_raw else []
    load1 = float(load[0]) if len(load) >= 1 else 0.0

    # --- 内存 ---
    mem_total = int(_run("sysctl -n hw.memsize").strip() or "0")
    vm = _run("vm_stat")
    page_size = 16384
    pages = {}
    for line in vm.splitlines():
        line = line.strip()
        if line.startswith("page size of"):
            try:
                page_size = int(line.split()[3])
            except (IndexError, ValueError):
                pass
            continue
        if ":" in line:
            key, val = line.split(":", 1)
            key = key.strip().lower().replace(" ", "_")
            val = val.strip().rstrip(".")
            try:
                pages[key] = int(val)
            except ValueError:
                pass
    mem_used = (pages.get("pages_active", 0) + pages.get("pages_inactive", 0)
                + pages.get("pages_wired_down", 0) + pages.get("pages_occupied_by_compressor", 0)) * page_size
    mem_pct = round(mem_used / mem_total * 100, 1) if mem_total else 0.0

    # --- 磁盘 ---
    df = _run("df -h / | tail -1")
    disk_pct = 0.0
    disk_used = ""
    disk_total = ""
    if df:
        fields = df.split()
        # Filesystem Size Used Avail Use% ...
        if len(fields) >= 5:
            disk_total = fields[1]
            disk_used = fields[2]
            try:
                disk_pct = float(fields[4].rstrip("%"))
            except ValueError:
                pass

    # --- 网络 ---
    # 选流量最大的非 lo 接口 (避免被 Thunderbolt 等小流量接口抢占)
    net_out = _run("netstat -ib | awk 'NR>1 && $1!~/^lo/ {print $1, $7, $10}'")
    iface = ""
    ibytes = obytes = 0
    best = 0
    seen = set()
    for line in net_out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            name = parts[0]
            if name in seen:
                continue
            seen.add(name)
            try:
                ib, ob = int(parts[1]), int(parts[2])
                total = ib + ob
                if total > best:
                    best = total
                    iface = name
                    ibytes, obytes = ib, ob
            except ValueError:
                continue
    now = time.time()
    down_rate = up_rate = 0.0
    if _NET_CACHE["ts"] > 0 and iface:
        dt = now - _NET_CACHE["ts"]
        if dt > 0:
            down_rate = max(0, (ibytes - _NET_CACHE["ibytes"])) / dt
            up_rate = max(0, (obytes - _NET_CACHE["obytes"])) / dt
    if iface:
        _NET_CACHE.update(ts=now, ibytes=ibytes, obytes=obytes)

    # --- 运行时间 ---
    uptime_out = _run("uptime")
    uptime_str = ""
    if "up " in uptime_out:
        seg = uptime_out.split("up ", 1)[1].split(",")[0].strip()
        uptime_str = seg

    hostname = (_run("scutil --get LocalHostName").strip()
                or _run("scutil --get ComputerName").strip()
                or _run("hostname -s").strip() or "Mac")

    return {
        "hostname": hostname,
        "uptime": uptime_str,
        "cpu": {"pct": cpu_pct, "ncpu": ncpu, "load1": load1},
        "memory": {"pct": mem_pct, "used_gb": round(mem_used / 1024**3, 1),
                   "total_gb": round(mem_total / 1024**3, 1)},
        "disk": {"pct": disk_pct, "used": disk_used, "total": disk_total},
        "network": {"iface": iface, "down_kb": round(down_rate / 1024, 1),
                    "up_kb": round(up_rate / 1024, 1)},
    }


def _fmt_speed(kb):
    if kb >= 1024:
        return f"{kb/1024:.1f} MB/s"
    return f"{kb:.0f} KB/s"


def _draw_bar(draw, x, y, w, h, pct, fill=0):
    """画一个进度条。"""
    draw.rectangle([x, y, x + w, y + h], outline=0, width=1)
    fill_w = int((w - 2) * min(100, max(0, pct)) / 100)
    if fill_w > 0:
        draw.rectangle([x + 1, y + 1, x + 1 + fill_w, y + h - 1], fill=fill)


def render_hardware(payload, fetched_at, error=None):
    img = Image.new("1", (WIDTH, HEIGHT), 1)
    draw = ImageDraw.Draw(img)
    fonts = Fonts()
    d = payload or {}

    # 标题
    draw.text((16, 14), "硬件监控", font=fonts.title, fill=0)
    host = d.get("hostname", "Mac")
    up = d.get("uptime", "")
    sub = f"{host}" + (f" · 运行 {up}" if up else "")
    draw.text((16, 40), sub, font=fonts.small, fill=0)

    bar_x, bar_w, bar_h = 16, 368, 14
    label_x = 16
    val_x = 300

    # CPU
    cpu = d.get("cpu", {})
    cpct = cpu.get("pct", 0)
    y = 68
    draw.text((label_x, y), "CPU", font=fonts.sec, fill=0)
    cpu_val = f"{cpct:.1f}%"
    cw = draw.textbbox((0, 0), cpu_val, font=fonts.sec)[2]
    draw.text((val_x, y), cpu_val, font=fonts.sec, fill=0)
    _draw_bar(draw, bar_x, y + 22, bar_w, bar_h, cpct)
    # 负载
    load1 = cpu.get("load1", 0)
    ncpu = cpu.get("ncpu", 0)
    draw.text((label_x, y + 42), f"负载 {load1:.1f} · {ncpu} 核", font=fonts.small, fill=0)

    # 内存
    mem = d.get("memory", {})
    mpct = mem.get("pct", 0)
    y = 130
    draw.text((label_x, y), "内存", font=fonts.sec, fill=0)
    mem_val = f"{mpct:.1f}%"
    mw = draw.textbbox((0, 0), mem_val, font=fonts.sec)[2]
    draw.text((val_x, y), mem_val, font=fonts.sec, fill=0)
    _draw_bar(draw, bar_x, y + 22, bar_w, bar_h, mpct)
    draw.text((label_x, y + 42), f"{mem.get('used_gb', 0)} / {mem.get('total_gb', 0)} GB",
              font=fonts.small, fill=0)

    # 磁盘
    disk = d.get("disk", {})
    dpct = disk.get("pct", 0)
    y = 192
    draw.text((label_x, y), "磁盘", font=fonts.sec, fill=0)
    disk_val = f"{dpct:.0f}%"
    draw.text((val_x, y), disk_val, font=fonts.sec, fill=0)
    _draw_bar(draw, bar_x, y + 22, bar_w, bar_h, dpct)
    draw.text((label_x, y + 42), f"{disk.get('used', '')} / {disk.get('total', '')}",
              font=fonts.small, fill=0)

    # 网络
    net = d.get("network", {})
    y = 254
    draw.text((label_x, y), "网络", font=fonts.sec, fill=0)
    iface = net.get("iface", "")
    if iface:
        down = _fmt_speed(net.get("down_kb", 0))
        up = _fmt_speed(net.get("up_kb", 0))
        draw.text((110, y), f"{iface}  ↓{down}  ↑{up}", font=fonts.small, fill=0)
    else:
        draw.text((110, y), "无活跃接口", font=fonts.small, fill=0)

    return img


def fetch_smart_home_test():
    """占位测试页：先验证显示布局，不连接或控制真实设备。"""
    return {
        "air_conditioner": {
            "name": "美的风管机",
            "model": "KFR-51T2/BDN8-CF(3)Ⅲ",
            "status": "待接入",
        },
        "feeder": {
            "name": "霍曼喂食器",
            "model": "PF20",
            "status": "待接入",
        },
        "demo_only": True,
    }


def render_smart_home_test(payload, fetched_at, error=None):
    """智能家居测试页的静态预览；接入前不发送任何设备命令。"""
    img = Image.new("1", (WIDTH, HEIGHT), 1)
    draw = ImageDraw.Draw(img)
    fonts = Fonts()
    draw_header(draw, fonts, "智能家居测试", "预览")

    data = payload or {}
    ac = data.get("air_conditioner", {})
    feeder = data.get("feeder", {})

    draw.rounded_rectangle([16, 52, 384, 135], radius=8, outline=0, width=2)
    draw.text((28, 62), ac.get("name", "美的空调"), font=fonts.sec, fill=0)
    draw.text((28, 87), ac.get("model", "型号待确认"), font=fonts.small, fill=0)
    draw.text((28, 111), f"状态：{ac.get('status', '待接入')}", font=fonts.small, fill=0)

    draw.rounded_rectangle([16, 145, 384, 228], radius=8, outline=0, width=2)
    draw.text((28, 155), feeder.get("name", "霍曼喂食器"), font=fonts.sec, fill=0)
    draw.text((28, 180), feeder.get("model", "PF20"), font=fonts.small, fill=0)
    draw.text((28, 204), f"状态：{feeder.get('status', '待接入')}", font=fonts.small, fill=0)

    notice = "布局预览 · 当前不会控制设备" if data.get("demo_only") else "设备状态"
    draw.text((16, 242), notice, font=fonts.small, fill=0)
    draw_footer(draw, fonts, fetched_at, error)
    return img


# ---------- 渲染基础 ----------
class Fonts:
    def __init__(self):
        self.title = ImageFont.truetype(FONT_PATH, 20)
        self.sec = ImageFont.truetype(FONT_PATH, 16)
        self.big = ImageFont.truetype(FONT_PATH, 44)
        self.small = ImageFont.truetype(FONT_PATH, 12)
        self.tiny = ImageFont.truetype(FONT_PATH, 10)


def pack_1bit(img):
    """PIL mode '1' 是 1=白; 反转为 1=黑，供固件逐位绘制 (RAM 1=黑)。"""
    return bytes(b ^ 0xFF for b in img.tobytes())


# ---------- 页面状态 ----------
class Page:
    def __init__(self, idx, name, fetch_fn, render_fn, interval=None):
        self.idx = idx
        self.name = name
        self.fetch_fn = fetch_fn
        self.render_fn = render_fn
        self.interval = interval  # None 表示使用全局 REFRESH_SECONDS
        self.payload = None
        self.fetched_at = None
        self.error = None
        self.bitmap = b""
        self.png = b""
        self.preview_path = PROJECT_DIR / (f"preview.png" if idx == 0 else f"preview{idx}.png")
        self.json_path = PROJECT_DIR / (f"latest.json" if idx == 0 else f"latest{idx}.json")

    def refresh_once(self):
        try:
            payload = self.fetch_fn()
            self._update(payload, datetime.now(), None)
            return payload
        except Exception as e:
            print(f"[{datetime.now():%H:%M:%S}] [{self.name}] 抓取失败: {e}", flush=True)
            with STATE.lock:
                old_payload, old_at = self.payload, self.fetched_at
            self._update(old_payload, old_at, e)
            return None

    def _update(self, payload, fetched_at, error):
        img = self.render_fn(payload, fetched_at, error)
        buf = io.BytesIO()
        img.save(buf, "PNG")
        img.save(self.preview_path)
        if payload is not None:
            with open(self.json_path, "w") as f:
                json.dump({"fetched_at": fetched_at.isoformat() if fetched_at else None,
                           "response": payload}, f, ensure_ascii=False, indent=2)
        with STATE.lock:
            self.payload = payload
            self.fetched_at = fetched_at
            self.error = error
            self.bitmap = pack_1bit(img)
            self.png = buf.getvalue()

    def refresher(self, default_interval):
        interval = self.interval if self.interval is not None else default_interval
        while True:
            self.refresh_once()
            time.sleep(interval)


STATE_LOCK = threading.Lock()


class State:
    lock = STATE_LOCK

    def __init__(self):
        self.pages = [
            Page(0, "CODEX", fetch_codex, render_codex),
            Page(1, "TRAE", fetch_trae, render_trae),
            Page(2, "HW", fetch_hardware, render_hardware, interval=HW_REFRESH_SECONDS),
            Page(3, "HOME_TEST", fetch_smart_home_test, render_smart_home_test),
        ]

    def get(self, idx):
        return self.pages[idx] if 0 <= idx < len(self.pages) else None


STATE = State()


# ---------- HTTP 服务 ----------
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)
        try:
            page_idx = int((qs.get("page") or ["0"])[0])
        except ValueError:
            page_idx = 0
        page = STATE.get(page_idx)
        if not page:
            self._send(404, b"no such page", "text/plain")
            return
        with STATE.lock:
            bitmap, png, payload, fetched_at, error = (
                page.bitmap, page.png, page.payload, page.fetched_at, page.error)
        if path == "/bitmap":
            self._send(200, bitmap, "application/octet-stream",
                       {"X-Width": str(WIDTH), "X-Height": str(HEIGHT),
                        "X-Page": page.name})
        elif path == "/preview.png":
            self._send(200, png, "image/png")
        elif path == "/json":
            body = json.dumps({"page": page.name, "fetched_at": fetched_at.isoformat() if fetched_at else None,
                               "error": str(error) if error else None,
                               "response": payload}, ensure_ascii=False).encode()
            self._send(200, body, "application/json; charset=utf-8")
        elif path == "/":
            imgs = "".join(
                f"<h4>{p.name} <a href='/json?page={p.idx}'>json</a></h4>"
                f"<img src='/preview.png?page={p.idx}' style='image-rendering:pixelated;border:1px solid #999'>"
                for p in STATE.pages)
            body = ("<html><head><meta charset='utf-8'><title>Quota E-Ink</title>"
                    "<meta http-equiv='refresh' content='60'></head><body>"
                    f"<h3>额度墨水屏 · {PAGE_COUNT} 页 (KEY 切换)</h3>{imgs}"
                    "</body></html>").encode()
            self._send(200, body, "text/html; charset=utf-8")
        else:
            self._send(404, b"not found", "text/plain")

    def _send(self, code, body, ctype, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    global REFRESH_SECONDS
    ap = argparse.ArgumentParser(description="额度墨水屏 Mac 端 (多页)")
    ap.add_argument("--once", action="store_true", help="抓取一次并生成 preview 后退出")
    ap.add_argument("--page", type=int, default=None, help="--once 时只抓指定页")
    ap.add_argument("--serve", action="store_true", help="常驻服务模式")
    ap.add_argument("--port", type=int, default=HTTP_PORT)
    ap.add_argument("--refresh", type=int, default=REFRESH_SECONDS, help="刷新间隔(秒)")
    ap.add_argument("--host", default="0.0.0.0")
    args = ap.parse_args()
    REFRESH_SECONDS = args.refresh

    if args.once:
        targets = [STATE.pages[args.page]] if args.page is not None else STATE.pages
        for p in targets:
            payload = p.refresh_once()
            if payload:
                print(f"[{p.name}] OK (预览: {p.preview_path})")
            else:
                print(f"[{p.name}] 抓取失败: {p.error}")
        return

    for p in STATE.pages:
        t = threading.Thread(target=p.refresher, args=(args.refresh,), daemon=True)
        t.start()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[{datetime.now():%H:%M:%S}] HTTP 服务: http://{args.host}:{args.port} "
          f"(页: {[p.name for p in STATE.pages]}, 刷新间隔 {args.refresh}s)", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()

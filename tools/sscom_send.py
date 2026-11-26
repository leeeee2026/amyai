#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把一帧数据注入 sscom，由 sscom 从它占着的 COM 口发出去。

为什么需要它：串口是独占资源，sscom 打开了 COM3 后，任何别的程序都写不进去
（打开会 Access denied）。这个脚本不去抢串口，而是用 Win32 消息直接操作
sscom 的界面：勾上「HEX发送」→ 把字节填进发送区 → 点「发 送」。
sscom 全程保持打开，数据也确实是它从 COM3 发出去的。

用法：
    python sscom_send.py --cmd-json "{\"cmd\":\"stop\"}"
    python sscom_send.py --hex "A5 5A 01 01 01 20 00 ..."
    python sscom_send.py --cmd-json "{\"cmd\":\"set_zone\",\"zone\":\"neck\"}" --seq 7
    python sscom_send.py --frames          # 跑内置的 5 条演示指令

依赖：只用 ctypes，不需要 pyserial，也不需要抢串口。
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from send_uart import build_frame, TYPE_CMD  # noqa: E402

user32 = ctypes.WinDLL("user32", use_last_error=True)

BM_GETCHECK = 0x00F0
BM_CLICK = 0x00F5
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D

EnumWindows = user32.EnumWindows
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
EnumChildWindows = user32.EnumChildWindows
GetClassNameW = user32.GetClassNameW
GetWindowTextW = user32.GetWindowTextW
GetWindowRect = user32.GetWindowRect
SendMessageW = user32.SendMessageW
IsWindowVisible = user32.IsWindowVisible
GetWindowThreadProcessId = user32.GetWindowThreadProcessId


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


def _class_name(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    GetClassNameW(hwnd, buf, 256)
    return buf.value


def _window_text(hwnd):
    buf = ctypes.create_unicode_buffer(1024)
    GetWindowTextW(hwnd, buf, 1024)
    return buf.value


def _rect(hwnd):
    r = RECT()
    GetWindowRect(hwnd, ctypes.byref(r))
    return r


def _children(hwnd):
    out = []

    @EnumWindowsProc
    def cb(h, _):
        out.append(h)
        return True

    EnumChildWindows(hwnd, cb, 0)
    return out


def find_sscom_window(title_hint="SSCOM"):
    """按窗口类 TForm1 + 标题找 sscom 主窗口"""
    found = []

    @EnumWindowsProc
    def cb(h, _):
        if not IsWindowVisible(h):
            return True
        if _class_name(h) != "TForm1":
            return True
        if title_hint.upper() in _window_text(h).upper():
            found.append(h)
        return True

    EnumWindows(cb, 0)
    return found[0] if found else None


def locate_controls(win):
    """在 sscom 窗口里定位：发送按钮 / HEX发送复选框 / 发送区 / 串口开关按钮"""
    send_btn = hex_chk = send_memo = port_btn = None
    memos = []

    for h in _children(win):
        cls = _class_name(h)
        txt = _window_text(h)
        compact = txt.replace(" ", "")
        if cls == "TBitBtn" and compact == "发送":
            send_btn = h
        elif cls == "TBitBtn" and txt in ("打开串口", "关闭串口"):
            port_btn = h
        elif cls == "TCheckBox" and "HEX发送" in txt:
            hex_chk = h
        elif cls == "TMemo":
            memos.append(h)

    if send_btn is None:
        return None, None, None, None

    b = _rect(send_btn)
    # 发送区是紧挨着发送按钮、位于其右下方的那个 TMemo
    best = None
    for h in memos:
        r = _rect(h)
        if r.top >= b.top - 40 and r.left > b.left and (r.right - r.left) > 100:
            area = (r.right - r.left) * (r.bottom - r.top)
            if best is None or area < best[0]:
                best = (area, h)
    if best:
        send_memo = best[1]

    return send_btn, hex_chk, send_memo, port_btn


def ensure_port_open(port_btn):
    """如果 sscom 的串口是关着的，先点开；点开后按钮文字会变成「关闭串口」"""
    if port_btn is None:
        return True
    if _window_text(port_btn) == "打开串口":
        click(port_btn)
        time.sleep(0.8)
        if _window_text(port_btn) != "关闭串口":
            print("点「打开串口」后状态没变，可能串口被占用或未选 COM 口")
            return False
    return True


def get_check(hwnd):
    return int(SendMessageW(hwnd, BM_GETCHECK, 0, 0))


def click(hwnd):
    SendMessageW(hwnd, BM_CLICK, 0, 0)


def set_text(hwnd, text):
    SendMessageW(hwnd, WM_SETTEXT, 0, ctypes.c_wchar_p(text))


def get_text(hwnd, capacity=200000):
    buf = ctypes.create_unicode_buffer(capacity)
    SendMessageW(hwnd, WM_GETTEXT, capacity - 10, ctypes.byref(buf))
    return buf.value


def send_hex(hex_text, verbose=True):
    win = find_sscom_window()
    if win is None:
        print("没找到 sscom 窗口（类名 TForm1 / 标题含 SSCOM）。请先打开 sscom。")
        return False

    send_btn, hex_chk, send_memo, port_btn = locate_controls(win)
    if not (send_btn and hex_chk and send_memo):
        print("找不到控件：发送按钮/HEX发送/发送区。sscom 版本可能不同。")
        return False

    if not ensure_port_open(port_btn):
        return False

    if get_check(hex_chk) != 1:
        click(hex_chk)
        time.sleep(0.15)
        if get_check(hex_chk) != 1:
            print("勾不上 HEX发送 复选框")
            return False

    set_text(send_memo, hex_text)
    time.sleep(0.15)
    if get_text(send_memo)[:8].upper() != hex_text[:8].upper():
        print("内容没写进发送区")
        return False

    click(send_btn)
    time.sleep(0.25)
    if verbose:
        print(f"已注入并点击发送：{len(hex_text.split())} 字节")
    return True


def to_hex_spaced(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


DEMO = [
    ("我脖子好酸", '{"cmd":"set_zone","zone":"neck"}'),
    ("力气太大了", '{"cmd":"set_level","level":2}'),
    ("来个半小时的", '{"cmd":"set_time","minutes":30}'),
    ("停一下", '{"cmd":"pause"}'),
    ("帮我订张机票", '{"cmd":"unknown"}'),
]


def main():
    ap = argparse.ArgumentParser(description="把帧注入 sscom 并由它从串口发出")
    ap.add_argument("--cmd-json", help="要发的 JSON 指令")
    ap.add_argument("--text", help="用户说的话，先问大模型再把结果发出去")
    ap.add_argument("--text-file", help="从 UTF-8 文件读用户说的话")
    ap.add_argument("--key", help="智谱 API Key，或设环境变量 LLM_API_KEY")
    ap.add_argument("--hex", help="直接给十六进制字节，空格分隔")
    ap.add_argument("--seq", type=int, default=1)
    ap.add_argument("--frames", action="store_true", help="跑内置 5 条演示指令")
    args = ap.parse_args()

    if args.frames:
        for i, (text, js) in enumerate(DEMO, start=1):
            frame = build_frame(js.encode("utf-8"), typ=TYPE_CMD, seq=i)
            print(f"\n[{i}] {text} -> {js}")
            print("    " + to_hex_spaced(frame))
            if not send_hex(to_hex_spaced(frame)):
                return 1
            time.sleep(0.4)
        return 0

    if args.text or args.text_file:
        from send_uart import ask_llm, clean_json
        api_key = (args.key or os.environ.get("LLM_API_KEY", "")).strip()
        if not api_key:
            sys.exit("要问大模型请提供 --key 或设置 LLM_API_KEY")
        if args.text_file:
            with open(args.text_file, "r", encoding="utf-8") as f:
                user_text = f.read().strip()
        else:
            user_text = args.text
        print(f"用户: {user_text}")
        reply = clean_json(ask_llm(user_text, api_key))
        print(f"大模型: {reply}")
        frame = build_frame(reply.encode("utf-8"), typ=TYPE_CMD, seq=args.seq)
        print("帧: " + to_hex_spaced(frame))
        return 0 if send_hex(to_hex_spaced(frame)) else 1

    if args.hex:
        return 0 if send_hex(args.hex) else 1

    if args.cmd_json:
        frame = build_frame(args.cmd_json.encode("utf-8"), typ=TYPE_CMD, seq=args.seq)
        print(to_hex_spaced(frame))
        return 0 if send_hex(to_hex_spaced(frame)) else 1

    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())

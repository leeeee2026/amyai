#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
关掉所有 sscom 实例占用的串口。

sscom 会一直抓着 COM 口不放，导致 esptool 无法烧录。
这里直接找到每个 sscom 窗口里的「关闭串口」按钮并点一下。

用法：
    python sscom_close.py           # 关掉所有 sscom 的串口
    python sscom_close.py --list    # 只看有哪些 sscom 窗口
"""

import ctypes
import sys
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)

BM_CLICK = 0x00F5
WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E


def get_text(hwnd):
    n = user32.SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0)
    if n <= 0:
        return ""
    buf = ctypes.create_unicode_buffer(n + 1)
    user32.SendMessageW(hwnd, WM_GETTEXT, n + 1, buf)
    return buf.value


def iter_children(hwnd):
    children = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(child, _):
        children.append(child)
        return True

    user32.EnumChildWindows(hwnd, cb, 0)
    return children


def main():
    show_only = "--list" in sys.argv

    tops = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def top_cb(hwnd, _):
        title = get_text(hwnd)
        if "sscom" in title.lower() or title.strip().startswith("SSCOM"):
            tops.append((hwnd, title))
        return True

    user32.EnumWindows(top_cb, 0)

    if not tops:
        print("没找到 sscom 窗口")
        return

    for hwnd, title in tops:
        print(f"窗口: {title!r}")
        closed = False
        for child in iter_children(hwnd):
            if get_text(child).strip() == "关闭串口":
                if not show_only:
                    user32.SendMessageW(child, BM_CLICK, 0, 0)
                print("  → 已点击「关闭串口」" if not show_only else "  → 当前是打开状态")
                closed = True
                break
        if not closed:
            print("  → 串口已经是关闭状态")


if __name__ == "__main__":
    main()

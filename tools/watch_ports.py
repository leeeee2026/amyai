#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
同时监听多个串口，任何口收到字节就打印出来（十六进制 + 帧解析）。
用来判断「主控线到底接在哪个口上」，接对了立刻能看到语音板的心跳帧。

用法：
    python watch_ports.py COM9 COM10
"""

import sys
import time

import serial

sys.path.insert(0, r"F:\work\amyai-main\tools")
from send_uart import parse_frames, TYPE_NAMES  # noqa: E402

ports = sys.argv[1:] or ["COM9", "COM10"]

handles = {}
for p in ports:
    try:
        handles[p] = serial.Serial(p, 115200, timeout=0.05)
        handles[p].reset_input_buffer()
        print(f"[{time.strftime('%H:%M:%S')}] 已打开 {p}", flush=True)
    except Exception as e:
        print(f"[{time.strftime('%H:%M:%S')}] {p} 打不开: {e}", flush=True)

buffers = {p: b"" for p in handles}

while handles:
    for p, sp in list(handles.items()):
        try:
            chunk = sp.read(256)
        except Exception as e:
            print(f"[{time.strftime('%H:%M:%S')}] {p} 读失败（设备可能被拔掉）: {e}", flush=True)
            try:
                sp.close()
            except Exception:
                pass
            handles.pop(p, None)
            continue

        if not chunk:
            continue

        buffers[p] += chunk
        frames = parse_frames(buffers[p])
        print(f"[{time.strftime('%H:%M:%S')}] {p} 收到 {len(chunk)} 字节: "
              + " ".join(f"{b:02X}" for b in chunk[:24]), flush=True)

        if frames:
            buffers[p] = b""
            for typ, seq, payload, crc_ok in frames:
                name = TYPE_NAMES.get(typ, f"0x{typ:02X}")
                print(f"    -> 帧 type={name}(0x{typ:02X}) seq={seq} "
                      f"crc={'OK' if crc_ok else 'BAD'} payload={payload.decode('utf-8', 'ignore')}",
                      flush=True)

    time.sleep(0.02)

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
串口桥：把语音板从 COM6（USB 调试口）发出的协议帧，转发到 COM9 / COM10。

用途：板子的 UART1 排针不方便接线时，让协议帧走调试口出来（固件开
CONFIG_AMYAI_UART_ON_CONSOLE），再由 PC 转发到真正的"主控"串口上。

流向：
    COM6（板子，日志+帧混合）
        ├─ 解析出 A5 5A 帧 ── 原样写入 COM9 / COM10（主控侧，sscom 能看到）
        └─ 同时自动回 ACK 给板子（可关）

    COM9 / COM10 上如果有回复（ACK 帧），也会转发回 COM6。

用法：
    python uart_bridge.py --src COM6 --dst COM9 COM10
    python uart_bridge.py --src COM6 --dst COM9 --no-ack
"""

import argparse
import sys
import threading
import time

import serial

sys.path.insert(0, r"F:\work\amyai-main\tools")
from send_uart import (  # noqa: E402
    HEAD0, HEAD1, TYPE_NAMES, TYPE_CMD, TYPE_ACK, TYPE_ESTOP, TYPE_HEARTBEAT,
    build_frame,
)
from uart_lab import ack_payload, validate_command  # noqa: E402

HEADER = bytes([HEAD0, HEAD1])
_print_lock = threading.Lock()


def log(tag: str, text: str) -> None:
    with _print_lock:
        print(f"[{time.strftime('%H:%M:%S')}] {tag} {text}", flush=True)


def hexs(b: bytes, n: int = 32) -> str:
    return " ".join(f"{x:02X}" for x in b[:n])


def main() -> int:
    ap = argparse.ArgumentParser(description="COM6 帧转发到 COM9/COM10")
    ap.add_argument("--src", default="COM6", help="板子所在的口")
    ap.add_argument("--dst", nargs="+", default=["COM9"], help="要转发到的口（可多个）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--no-ack", action="store_true", help="不自动回 ACK（交给对端回）")
    args = ap.parse_args()

    src = serial.Serial(args.src, args.baud, timeout=0.05)
    src.reset_input_buffer()

    dsts = []
    for d in args.dst:
        try:
            sp = serial.Serial(d, args.baud, timeout=0.05)
            sp.reset_input_buffer()
            dsts.append((d, sp))
            log("**", f"已打开目标口 {d}")
        except Exception as e:
            log("!!", f"{d} 打不开: {e}")

    if not dsts:
        log("!!", "没有可用的目标口")
        return 1

    log("**", f"桥接启动：{args.src} -> {', '.join(d for d, _ in dsts)}"
              + ("（只转发，不回 ACK）" if args.no_ack else "（自动回 ACK）"))

    # 目标口 -> 源口 的反向转发（主控的应答往板子送）
    def reverse_loop():
        import select
        while True:
            for name, sp in dsts:
                try:
                    data = sp.read(256)
                except Exception:
                    continue
                if data:
                    src.write(data)
                    src.flush()
                    log(f"{name}->{args.src}", f"{len(data)} 字节: {hexs(data)}")
            time.sleep(0.02)

    threading.Thread(target=reverse_loop, daemon=True).start()

    buf = b""
    line = b""
    fwd = 0
    hb = 0
    last_note = time.time()

    def flush_text(chunk: bytes) -> None:
        """把日志文本按行打印出来（协议帧之外的字节）"""
        nonlocal line
        line += chunk
        while b"\n" in line:
            l, line = line.split(b"\n", 1)
            s = l.decode("utf-8", "replace").rstrip("\r")
            if s.strip():
                with _print_lock:
                    print("   " + s, flush=True)

    while True:
        chunk = src.read(512)
        if chunk:
            buf += chunk

        while True:
            idx = buf.find(HEADER)
            if idx < 0:
                flush_text(buf)
                buf = b""
                break
            if idx > 0:
                flush_text(buf[:idx])
                buf = buf[idx:]
            if len(buf) < 7:
                break
            ln = buf[5] | (buf[6] << 8)
            total = 7 + ln + 2
            if len(buf) < total:
                break

            frame = buf[:total]
            buf = buf[total:]

            typ = frame[3]
            seq = frame[4]
            payload = frame[7:7 + ln]
            name = TYPE_NAMES.get(typ, f"0x{typ:02X}")

            # 转发到所有目标口
            for dname, sp in dsts:
                try:
                    sp.write(frame)
                    sp.flush()
                except Exception as e:
                    log("!!", f"{dname} 写失败: {e}")
            fwd += 1

            if typ == TYPE_HEARTBEAT:
                hb += 1
                if time.time() - last_note >= 10:
                    log("**", f"已转发 {fwd} 帧（其中心跳 {hb} 个）")
                    last_note = time.time()
                    hb = 0
                continue

            log("<--", f"转发 {name} seq={seq} payload={payload.decode('utf-8', 'ignore')}")
            log("   ", "字节: " + hexs(frame))

            if not args.no_ack and typ == TYPE_CMD:
                ok, detail = validate_command(payload)
                ack = build_frame(ack_payload(seq, ok, 0 if ok else 1), typ=TYPE_ACK, seq=seq)
                src.write(ack)
                src.flush()
                log("-->", f"已回 ACK seq={seq} ack={ok}（{detail}）")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n退出")

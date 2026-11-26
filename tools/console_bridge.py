#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
调试串口桥：在同一个 COM 口上既当"日志监视器"，又当"主控板"。

背景：手边没有空闲串口/接线不方便时，可以让语音板的协议帧走调试串口 UART0
（固件里打开 CONFIG_AMYAI_UART_ON_CONSOLE），这样日志和协议帧共用一个口。
本工具负责：
    - 把日志原样打印出来（可读）
    - 识别 A5 5A 开头的协议帧，解析并打印
    - 收到指令帧（0x01）/ 急停帧（0x04）时，自动回 ACK 帧
    - 心跳帧（0x03）只统计，不刷屏

用法：
    python console_bridge.py --port COM6
"""

import argparse
import sys
import time

import serial

sys.path.insert(0, r"F:\work\amyai-main\tools")
from send_uart import (  # noqa: E402
    HEAD0, HEAD1, TYPE_NAMES, TYPE_CMD, TYPE_ACK, TYPE_ESTOP, TYPE_HEARTBEAT,
    build_frame,
)
from uart_lab import ack_payload, validate_command  # noqa: E402

HEADER = bytes([HEAD0, HEAD1])


def log(tag: str, text: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {tag} {text}", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="调试串口桥（日志 + 主控模拟）")
    ap.add_argument("--port", default="COM6")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--no-ack", action="store_true", help="只监听，不自动回 ACK")
    args = ap.parse_args()

    sp = serial.Serial(args.port, args.baud, timeout=0.05)
    sp.reset_input_buffer()
    log("**", f"桥接已启动：{args.port} @ {args.baud}"
              + ("（只监听）" if args.no_ack else "（自动回 ACK）"))

    buf = b""
    line = b""
    hb_count = 0
    last_hb_note = time.time()

    while True:
        chunk = sp.read(512)
        if chunk:
            buf += chunk

        # 先把日志文本和协议帧分开处理
        while True:
            idx = buf.find(HEADER)
            if idx < 0:
                # 没有帧头，全是日志文本
                line += buf
                buf = b""
                while b"\n" in line:
                    l, line = line.split(b"\n", 1)
                    s = l.decode("utf-8", "replace").rstrip("\r")
                    if s.strip():
                        print("   " + s, flush=True)
                break

            # 帧头之前的都是日志
            if idx > 0:
                line += buf[:idx]
                buf = buf[idx:]
                while b"\n" in line:
                    l, line = line.split(b"\n", 1)
                    s = l.decode("utf-8", "replace").rstrip("\r")
                    if s.strip():
                        print("   " + s, flush=True)

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
            crc_rx = frame[7 + ln] | (frame[8 + ln] << 8)
            from send_uart import crc16_ccitt
            crc_ok = crc16_ccitt(frame[2:7 + ln]) == crc_rx

            name = TYPE_NAMES.get(typ, f"0x{typ:02X}")
            text = payload.decode("utf-8", "ignore")

            if typ == TYPE_HEARTBEAT:
                hb_count += 1
                if time.time() - last_hb_note >= 10:
                    log("**", f"心跳正常（10 秒内 {hb_count} 帧），链路是通的")
                    hb_count = 0
                    last_hb_note = time.time()
                continue

            log("<--", f"帧 type={name} seq={seq} crc={'OK' if crc_ok else 'BAD'} payload={text}")

            if not crc_ok:
                log("!!", "CRC 错误，丢弃并回 NACK")
                if not args.no_ack:
                    sp.write(build_frame(ack_payload(seq, False, 1), typ=TYPE_ACK, seq=seq))
                    sp.flush()
                continue

            if typ == TYPE_ESTOP:
                log("!!", "急停帧！立即停机")
                if not args.no_ack:
                    sp.write(build_frame(ack_payload(seq, True), typ=TYPE_ACK, seq=seq))
                    sp.flush()
                    log("-->", f"回 ACK seq={seq}")
                continue

            if typ == TYPE_CMD:
                ok, detail = validate_command(payload)
                log("**", f"白名单校验{'通过' if ok else '拒绝'}（{detail}）")
                if not args.no_ack:
                    sp.write(build_frame(ack_payload(seq, ok, 0 if ok else 1), typ=TYPE_ACK, seq=seq))
                    sp.flush()
                    log("-->", f"回 ACK seq={seq} ack={ok}")

        time.sleep(0.005)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n退出")

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把语音指令按《双芯片通信协议》打包，从 PC 串口发给主控板。

帧格式（docs/语音交互模块设计方案.md 第 6 节）：

    A5 5A | ver(1) | type(1) | seq(1) | len(2, 小端) | payload(JSON) | crc16(2)

    type: 0x01 指令  0x02 应答  0x03 心跳  0x04 急停  0x05 状态上报
    crc 覆盖范围：从 ver 到 payload 末尾（不含 A5 5A 和 crc 自身）

用法：

    # 只调整了帧内容，直接发固定指令
    python send_uart.py --port COM3 --cmd "{\"cmd\":\"set_zone\",\"zone\":\"neck\"}"

    # 先问智谱大模型，再把模型返回的 JSON 发出去
    set LLM_API_KEY=xxxxx.yyyyy
    python send_uart.py --port COM3 --text "我脖子好酸"

    # 板子只认纯文本时（不加帧头帧尾，只补 CRLF）
    python send_uart.py --port COM3 --raw --cmd "{\"cmd\":\"set_zone\",\"zone\":\"neck\"}"

    # 干跑：只打印算出来的字节，不占串口
    python send_uart.py --dry-run --cmd "{\"cmd\":\"stop\"}"
"""

import argparse
import json
import os
import struct
import sys
import time

# ---------------------------------------------------------------- 帧格式常量
HEAD0 = 0xA5
HEAD1 = 0x5A
VER = 0x01
TYPE_CMD = 0x01
TYPE_ACK = 0x02
TYPE_HEARTBEAT = 0x03
TYPE_ESTOP = 0x04
TYPE_STATUS = 0x05

TYPE_NAMES = {
    TYPE_CMD: "指令",
    TYPE_ACK: "应答",
    TYPE_HEARTBEAT: "心跳",
    TYPE_ESTOP: "急停",
    TYPE_STATUS: "状态上报",
}

LLM_URL = "https://open.bigmodel.cn/api/paas/v4/chat/completions"
LLM_MODEL = "glm-4-flash"

# 提示词不要另写一份：直接复用 test_llm.py 里的那份（带 few-shot 示例，
# 没有示例时模型会把"我脖子好酸"判成 unknown）。
try:
    from test_llm import SYSTEM_PROMPT  # noqa: F401  同目录，脚本方式运行时可用
except Exception:  # 独立拷走时兜底
    SYSTEM_PROMPT = """你是按摩椅的指令解析器。只输出 JSON，不要任何解释文字、不要 Markdown 代码块。

允许的指令：
{"cmd":"start"}                          开始按摩
{"cmd":"stop"}                           停止
{"cmd":"pause"}                          暂停
{"cmd":"set_zone","zone":"..."}          部位：neck/back/waist/leg/foot/full
{"cmd":"set_level","level":1-5}          力度
{"cmd":"set_time","minutes":5-60}        时长（分钟）
{"cmd":"query_status"}                   查询当前状态
{"cmd":"unknown"}                        听不懂或与按摩无关

示例：
用户："脖子好酸"          -> {"cmd":"set_zone","zone":"neck"}
用户："把腰也按按"        -> {"cmd":"set_zone","zone":"waist"}
用户："力气太大了"        -> {"cmd":"set_level","level":2}
用户："轻一点"            -> {"cmd":"set_level","level":2}
用户："来个半小时的"      -> {"cmd":"set_time","minutes":30}
用户："停一下"            -> {"cmd":"stop"}
用户："现在什么模式"      -> {"cmd":"query_status"}
用户："帮我订张机票"      -> {"cmd":"unknown"}"""


# ---------------------------------------------------------------- CRC
def crc16_ccitt(data: bytes, init: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE：poly 0x1021，init 0xFFFF，不反转，不异或"""
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc16_modbus(data: bytes) -> int:
    """CRC-16/MODBUS：poly 0xA001（反向），init 0xFFFF"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 0x0001 else crc >> 1
    return crc


CRC_ALGOS = {"ccitt": crc16_ccitt, "modbus": crc16_modbus}


# ---------------------------------------------------------------- 组帧
def build_frame(payload: bytes, ver: int = VER, typ: int = TYPE_CMD,
                seq: int = 0, crc_name: str = "ccitt") -> bytes:
    body = bytes([ver & 0xFF, typ & 0xFF, seq & 0xFF]) + struct.pack("<H", len(payload)) + payload
    crc = CRC_ALGOS[crc_name](body)
    return bytes([HEAD0, HEAD1]) + body + struct.pack("<H", crc)


def parse_frames(buf: bytes):
    """从收到的字节流里切出完整帧，返回 [(type, seq, payload, crc_ok), ...]"""
    out = []
    i = 0
    while i + 7 <= len(buf):
        if buf[i] != HEAD0 or buf[i + 1] != HEAD1:
            i += 1
            continue
        ver, typ, seq = buf[i + 2], buf[i + 3], buf[i + 4]
        (length,) = struct.unpack("<H", buf[i + 5:i + 7])
        end = i + 7 + length + 2
        if end > len(buf):
            break
        payload = buf[i + 7:i + 7 + length]
        (crc_rx,) = struct.unpack("<H", buf[i + 7 + length:end])
        crc_calc = CRC_ALGOS["ccitt"](buf[i + 2:i + 7 + length])
        out.append((typ, seq, payload, crc_rx == crc_calc))
        i = end
    return out


# ---------------------------------------------------------------- 大模型
def ask_llm(text: str, api_key: str) -> str:
    import urllib.error
    import urllib.request

    body = {
        "model": LLM_MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": text},
        ],
        "temperature": 0,
        "max_tokens": 80,
    }
    req = urllib.request.Request(
        LLM_URL,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        sys.exit(f"大模型返回错误 {e.code}: {e.read().decode('utf-8', 'ignore')[:300]}")
    except Exception as e:
        sys.exit(f"调用大模型失败: {e}")

    try:
        return data["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError):
        sys.exit(f"大模型返回格式异常: {json.dumps(data, ensure_ascii=False)[:300]}")


def clean_json(text: str) -> str:
    """模型有时会包 ```json 围栏，剥掉"""
    t = text.strip()
    if t.startswith("```"):
        t = t.split("\n", 1)[1] if "\n" in t else t
        if t.endswith("```"):
            t = t[:-3]
    t = t.strip()
    start, end = t.find("{"), t.rfind("}")
    if start >= 0 and end > start:
        t = t[start:end + 1]
    return t


# ---------------------------------------------------------------- 主流程
def main() -> int:
    ap = argparse.ArgumentParser(description="按双芯片协议给主控板发指令")
    ap.add_argument("--port", default="COM3", help="串口，默认 COM3")
    ap.add_argument("--baud", type=int, default=115200, help="波特率，默认 115200")
    ap.add_argument("--cmd", help="直接指定的 JSON 指令")
    ap.add_argument("--cmd-file", help="从文件读 JSON 指令（避免命令行引号被 shell 吃掉）")
    ap.add_argument("--text", help="用户说的话，先送大模型再发结果")
    ap.add_argument("--text-file", help="从 UTF-8 文件读用户说的话（避免命令行中文乱码）")
    ap.add_argument("--key", help="智谱 API Key（也可放环境变量 LLM_API_KEY）")
    ap.add_argument("--type", default="cmd",
                    choices=["cmd", "estop", "heartbeat", "status"],
                    help="帧类型，默认 cmd(0x01)")
    ap.add_argument("--seq", type=int, default=0, help="序号 0-255")
    ap.add_argument("--ver", type=int, default=VER, help="协议版本，默认 1")
    ap.add_argument("--crc", default="ccitt", choices=list(CRC_ALGOS), help="CRC 算法")
    ap.add_argument("--raw", action="store_true", help="不加帧头，纯文本 + CRLF")
    ap.add_argument("--dry-run", action="store_true", help="只打印字节，不开串口")
    ap.add_argument("--wait-ack", type=float, default=1.5, help="等待应答秒数，0 表示不等")
    args = ap.parse_args()

    # 1. 拿到要发的 JSON
    if args.text or args.text_file:
        api_key = (args.key or os.environ.get("LLM_API_KEY", "")).strip()
        if not api_key:
            sys.exit("要问大模型请提供 --key 或设置环境变量 LLM_API_KEY")
        if args.text_file:
            with open(args.text_file, "r", encoding="utf-8") as f:
                user_text = f.read().strip()
        else:
            user_text = args.text
        print(f"用户: {user_text}")
        reply = clean_json(ask_llm(user_text, api_key))
        print(f"大模型: {reply}")
    elif args.cmd_file:
        with open(args.cmd_file, "r", encoding="utf-8") as f:
            reply = clean_json(f.read())
    elif args.cmd:
        reply = clean_json(args.cmd)
    else:
        sys.exit("必须提供 --cmd 或 --text 之一")

    payload = reply.encode("utf-8")

    # 2. 组帧
    if args.raw:
        frame = payload + b"\r\n"
        print(f"模式: 纯文本 + CRLF")
    else:
        typ = {"cmd": TYPE_CMD, "estop": TYPE_ESTOP,
               "heartbeat": TYPE_HEARTBEAT, "status": TYPE_STATUS}[args.type]
        frame = build_frame(payload, ver=args.ver, typ=typ, seq=args.seq, crc_name=args.crc)
        print(f"模式: 帧 type=0x{typ:02X}({TYPE_NAMES[typ]}) seq={args.seq} crc={args.crc}")

    print("发送字节:")
    print("  " + " ".join(f"{b:02X}" for b in frame))

    if args.dry_run:
        print("(--dry-run，未打开串口)")
        return 0

    # 3. 发送
    try:
        import serial
    except ImportError:
        sys.exit("需要 pyserial：pip install pyserial")

    try:
        with serial.Serial(args.port, args.baud, timeout=0.2) as sp:
            sp.reset_input_buffer()
            sp.write(frame)
            sp.flush()
            print(f"已发送 {len(frame)} 字节到 {args.port} @ {args.baud}")

            if args.wait_ack > 0:
                deadline = time.time() + args.wait_ack
                buf = b""
                while time.time() < deadline:
                    chunk = sp.read(256)
                    if chunk:
                        buf += chunk
                if not buf:
                    print(f"{args.wait_ack:.1f}s 内没有收到任何回复")
                else:
                    print("收到: " + " ".join(f"{b:02X}" for b in buf[:64]))
                    for typ, seq, pl, ok in parse_frames(buf):
                        print(f"  帧 type=0x{typ:02X}({TYPE_NAMES.get(typ, '?')}) "
                              f"seq={seq} crc={'OK' if ok else 'BAD'} "
                              f"payload={pl.decode('utf-8', 'ignore')}")
    except Exception as e:
        sys.exit(f"串口操作失败: {e}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

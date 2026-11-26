#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
单串口双角色实验台：在一台 PC、一个 COM 口上把《双芯片通信协议》跑通。

为什么需要它：一个串口只能被一个程序打开，所以 sscom 开着的时候脚本发不了。
这个工具把这件两件事合到一个进程里，并且真的会回 ACK 帧——
调固件的 ACK / 重传 / 超时逻辑时，比只能干看的串口助手有用。

用法：

    # 角色 A（语音模块）：说一句话 -> 大模型 -> 组帧发出 -> 等 ACK
    python uart_lab.py --port COM3 --role a --text "我脖子好酸"

    # 角色 A：直接发固定指令
    python uart_lab.py --port COM3 --role a --cmd "{\"cmd\":\"stop\"}"

    # 角色 B（主控板模拟）：一直收帧、解包、校验白名单、回 ACK
    python uart_lab.py --port COM3 --role b

    # 角色 B 顺便每秒发心跳（type=0x03），验证 A 侧的看门狗
    python uart_lab.py --port COM3 --role b --heartbeat 1

    # 不用串口，喂一段字节自测解析逻辑
    python uart_lab.py --selftest
"""

import argparse
import json
import os
import struct
import sys
import time

# 复用 send_uart.py 的帧实现，避免两处 CRC 各写一遍
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from send_uart import (  # noqa: E402
    HEAD0, HEAD1, VER, TYPE_CMD, TYPE_ACK, TYPE_HEARTBEAT, TYPE_ESTOP, TYPE_STATUS,
    TYPE_NAMES, build_frame, parse_frames, crc16_ccitt,
)

ZONES = {"neck", "back", "waist", "leg", "foot", "full"}
LEVEL_RANGE = (1, 5)
MINUTES_RANGE = (1, 60)


def log(direction: str, text: str) -> None:
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {direction} {text}", flush=True)


# ---------------------------------------------------------------- 指令校验
ZONES_V2 = {"neck", "shoulder", "back", "waist", "hip", "leg", "foot", "arm", "full"}
MODES_V2 = {"knead", "tap", "press", "knead_tap", "shiatsu", "stretch"}
PROGRAMS_V2 = {"sleep", "relax", "neck", "back", "waist", "leg", "foot", "full",
               "office", "sport", "stretch", "heat", "zero_g", "knead", "tap", "shiatsu",
               "morning", "night", "elder", "female", "male", "quick", "deep", "gentle"}


def _check_one(obj):
    """校验单条指令，返回 (ok, 描述)"""
    cmd = obj.get("cmd")
    if not isinstance(cmd, str):
        return False, "缺少 cmd"

    if cmd in ("start", "stop", "pause", "resume", "query_status", "unknown"):
        return True, cmd

    if cmd in ("set_zone",):
        zs = obj.get("zones")
        if not isinstance(zs, list) or not zs or any(z not in ZONES_V2 for z in zs):
            return False, f"zones={zs}"
        return True, f"zones={zs}"

    if cmd == "set_intensity":
        v = obj.get("level")
        return (isinstance(v, int) and 1 <= v <= 5), f"level={v}"

    if cmd == "adjust_intensity":
        v = obj.get("delta")
        if not (isinstance(v, int) and -2 <= v <= 2):
            return False, f"delta={v}"
        zs = obj.get("zones")
        if zs is not None and (not isinstance(zs, list) or any(z not in ZONES_V2 for z in zs)):
            return False, f"zones={zs}"
        return True, f"delta={v} zones={zs}"

    if cmd == "set_speed":
        v = obj.get("level")
        return (isinstance(v, int) and 1 <= v <= 5), f"level={v}"

    if cmd == "adjust_speed":
        v = obj.get("delta")
        return (isinstance(v, int) and -2 <= v <= 2), f"delta={v}"

    if cmd == "set_mode":
        return (obj.get("mode") in MODES_V2), f"mode={obj.get('mode')}"

    if cmd in ("set_heat", "set_zero_gravity", "set_foot_roller"):
        return isinstance(obj.get("on"), bool), f"on={obj.get('on')}"

    if cmd == "set_airbag":
        v = obj.get("level")
        return (isinstance(v, int) and 0 <= v <= 5), f"level={v}"

    if cmd == "set_recline":
        v = obj.get("angle")
        return (isinstance(v, int) and 100 <= v <= 170), f"angle={v}"

    if cmd == "adjust_recline":
        v = obj.get("delta")
        return (isinstance(v, int) and -30 <= v <= 30), f"delta={v}"

    if cmd == "set_leg_rest":
        v = obj.get("angle")
        return (isinstance(v, int) and 0 <= v <= 90), f"angle={v}"

    if cmd == "adjust_leg_rest":
        v = obj.get("delta")
        return (isinstance(v, int) and -30 <= v <= 30), f"delta={v}"

    if cmd == "set_time":
        v = obj.get("minutes")
        return (isinstance(v, int) and MINUTES_RANGE[0] <= v <= MINUTES_RANGE[1]), f"minutes={v}"

    if cmd == "set_program":
        return (obj.get("program") in PROGRAMS_V2), f"program={obj.get('program')}"

    return False, f"未知 cmd={cmd}"


def validate_command(payload: bytes) -> tuple[bool, str]:
    """模拟主控板的白名单校验：LLM 的输出永远当不可信输入"""
    try:
        obj = json.loads(payload.decode("utf-8"))
    except Exception:
        return False, "不是合法 JSON"

    # v2：一句话多个动作
    actions = obj.get("actions")
    if isinstance(actions, list):
        if not actions:
            return False, "actions 为空"
        descs = []
        for item in actions:
            ok, desc = _check_one(item)
            if not ok:
                return False, desc
            descs.append(desc)
        return True, " | ".join(descs)

    return _check_one(obj)


def ack_payload(seq: int, ok: bool, err: int = 0) -> bytes:
    return json.dumps({"id": seq, "ack": ok, "err": err},
                      separators=(",", ":")).encode("utf-8")


# ---------------------------------------------------------------- 角色 A：发
def run_role_a(port: str, baud: int, frame: bytes, seq: int,
               ack_timeout: float, retries: int) -> int:
    import serial

    with serial.Serial(port, baud, timeout=0.1) as sp:
        sp.reset_input_buffer()
        for attempt in range(1, retries + 1):
            log("-->", f"发送 {len(frame)} 字节 (第 {attempt} 次): "
                       + " ".join(f"{b:02X}" for b in frame))
            sp.write(frame)
            sp.flush()

            deadline = time.time() + ack_timeout
            buf = b""
            while time.time() < deadline:
                chunk = sp.read(256)
                if chunk:
                    buf += chunk
                    if parse_frames(buf):
                        break

            frames = parse_frames(buf)
            if not frames:
                log("<--", f"{ack_timeout:.1f}s 内没有收到应答")
                continue

            for typ, rseq, payload, crc_ok in frames:
                name = TYPE_NAMES.get(typ, f"0x{typ:02X}")
                log("<--", f"type={name} seq={rseq} crc={'OK' if crc_ok else 'BAD'} "
                           f"payload={payload.decode('utf-8', 'ignore')}")
                if typ == TYPE_ACK and rseq == seq and crc_ok:
                    log("**", "收到匹配的 ACK，指令送达")
                    return 0

            log("**", "收到的帧对不上 seq / 类型，继续重试")
        log("!!", f"{retries} 次都没有等到有效 ACK")
        return 1
    return 0


# ---------------------------------------------------------------- 角色 B：收
def run_role_b(port: str, baud: int, heartbeat: float) -> int:
    import serial

    # USB 串口在插拔/重新枚举时会抛 SerialException，这里自动重连，
    # 免得接一次线模拟器就退出
    while True:
        try:
            with serial.Serial(port, baud, timeout=0.1) as sp:
                sp.reset_input_buffer()
                log("**", f"主控板模拟器已启动，监听 {port} @ {baud}")
                _role_b_loop(sp, heartbeat)
        except KeyboardInterrupt:
            log("**", "手动退出")
            return 0
        except Exception as e:
            log("!!", f"串口异常 {type(e).__name__}: {e} —— 2 秒后自动重连")
            time.sleep(2)


def _role_b_loop(sp, heartbeat: float) -> None:
    buf = b""
    last_hb = time.time()
    last_note = time.time()
    got_frames = 0
    total_bytes = 0
    last_raw_log = 0.0

    while True:
        chunk = sp.read(256)
        if chunk:
            total_bytes += len(chunk)
            buf += chunk
            frames = parse_frames(buf)
            if frames:
                # 简化处理：解析成功后清空，靠 A5 5A 重同步
                buf = b""
                got_frames += len(frames)

                for typ, seq, payload, crc_ok in frames:
                    name = TYPE_NAMES.get(typ, f"0x{typ:02X}")
                    text = payload.decode("utf-8", "ignore")
                    if not crc_ok:
                        log("<--", f"type={name} seq={seq} CRC 错误，丢弃")
                        continue

                    if typ == TYPE_ESTOP:
                        log("<--", f"!!! 急停帧 seq={seq} —— 立即停止所有动作")
                        sp.write(build_frame(ack_payload(seq, True), typ=TYPE_ACK, seq=seq))
                        sp.flush()
                        continue

                    ok, detail = validate_command(payload) if typ == TYPE_CMD else (True, "")
                    log("<--", f"type={name} seq={seq} payload={text} "
                               f"-> 校验{'通过' if ok else '拒绝'} ({detail})")

                    if typ == TYPE_CMD:
                        reply = build_frame(ack_payload(seq, ok, 0 if ok else 1),
                                            typ=TYPE_ACK, seq=seq)
                        sp.write(reply)
                        sp.flush()
                        log("-->", f"回 ACK seq={seq} ack={ok}")
            else:
                # 收到字节但解不出帧：把原始字节打出来，判断是不是电平/波特率接错
                if time.time() - last_raw_log >= 1.0:
                    last_raw_log = time.time()
                    head = " ".join(f"{b:02X}" for b in buf[:32])
                    log("??", f"收到 {len(chunk)} 字节（累计 {total_bytes}），但还解不出帧: {head}")
        else:
            # 每 10 秒报一次"还活着 + 累计帧数"
            if time.time() - last_note >= 10:
                log("..", f"等待数据中（累计收到 {got_frames} 帧 / {total_bytes} 字节）")
                last_note = time.time()

        # 心跳
        if heartbeat > 0 and time.time() - last_hb >= heartbeat:
            hb = build_frame(b'{"stat":"idle"}', typ=TYPE_HEARTBEAT, seq=0)
            sp.write(hb)
            sp.flush()
            last_hb = time.time()
            log("-->", "心跳")


def run_loopback(port: str, baud: int) -> int:
    """回环自测：把 USB 转串口的 RXD 和 TXD 短接，能收到自己发的字节就说明转接头和工具都正常"""
    import serial

    with serial.Serial(port, baud, timeout=0.2) as sp:
        log("**", f"回环测试已启动（{port} @ {baud}）")
        log("**", "请用一根杜邦线把转接头的 RXD 和 TXD 短接")
        seq = 0
        while True:
            frame = build_frame(b'{"t":"loop"}', typ=TYPE_CMD, seq=seq & 0xFF)
            sp.write(frame)
            sp.flush()
            time.sleep(0.15)
            data = sp.read(512)
            if data:
                log("<-", f"回环收到 {len(data)} 字节: " + " ".join(f"{b:02X}" for b in data[:24]))
            else:
                log("..", "没收到回环数据（RXD/TXD 还没短接，或转接头有问题）")
            seq += 1
            time.sleep(1.2)


# ---------------------------------------------------------------- 自测
def selftest() -> int:
    payload = b'{"cmd":"set_zone","zone":"neck"}'
    f = build_frame(payload, typ=TYPE_CMD, seq=7)
    print("构造帧:", " ".join(f"{b:02X}" for b in f))

    frames = parse_frames(f)
    assert len(frames) == 1, frames
    typ, seq, pl, ok = frames[0]
    assert (typ, seq, pl, ok) == (TYPE_CMD, 7, payload, True), frames[0]
    print("解析: type=%d seq=%d crc=%s payload=%s" % (typ, seq, ok, pl.decode()))

    # 故意改坏一个字节，CRC 必须报错
    bad = bytearray(f)
    bad[8] ^= 0xFF
    frames = parse_frames(bytes(bad))
    assert frames and frames[0][3] is False, frames
    print("坏帧检测: CRC 正确报错")

    # 粘包 / 半包
    merged = f + build_frame(b'{"cmd":"stop"}', typ=TYPE_CMD, seq=8)
    assert len(parse_frames(merged)) == 2
    assert parse_frames(f[:-3]) == []  # 不完整的帧不返回
    print("粘包/半包处理: OK")

    # 白名单
    assert validate_command(b'{"cmd":"set_zone","zone":"neck"}')[0] is True
    assert validate_command(b'{"cmd":"set_zone","zone":"hand"}')[0] is False
    assert validate_command(b'{"cmd":"set_level","level":9}')[0] is False
    assert validate_command(b'{"cmd":"set_time","minutes":30}')[0] is True
    assert validate_command(b'not json')[0] is False
    print("白名单校验: OK")

    print("\n全部自测通过")
    return 0


# ---------------------------------------------------------------- 入口
def main() -> int:
    ap = argparse.ArgumentParser(description="单串口双角色协议实验台")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--role", choices=["a", "b"], help="a=语音模块(发) b=主控板(收)")
    ap.add_argument("--cmd", help="角色 A：直接发这条 JSON")
    ap.add_argument("--cmd-file", help="角色 A：从 UTF-8 文件读 JSON")
    ap.add_argument("--text", help="角色 A：先问大模型再发")
    ap.add_argument("--text-file", help="角色 A：从 UTF-8 文件读用户的话")
    ap.add_argument("--key", help="智谱 API Key，或设 LLM_API_KEY")
    ap.add_argument("--seq", type=int, default=1)
    ap.add_argument("--crc", default="ccitt", choices=["ccitt", "modbus"])
    ap.add_argument("--ack-timeout", type=float, default=0.3, help="等 ACK 秒数，默认 0.3")
    ap.add_argument("--retries", type=int, default=3, help="重传次数，默认 3")
    ap.add_argument("--heartbeat", type=float, default=0, help="角色 B：心跳间隔秒，0=不发")
    ap.add_argument("--selftest", action="store_true", help="不用串口，自测帧逻辑")
    ap.add_argument("--loopback", action="store_true",
                    help="回环自测：短接转接头 RXD/TXD，验证转接头和工具")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if args.loopback:
        return run_loopback(args.port, args.baud)

    if args.role == "b":
        return run_role_b(args.port, args.baud, args.heartbeat)

    if args.role != "a":
        sys.exit("请用 --role a 或 --role b，或 --selftest")

    # 准备 payload
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
        log("**", f"用户: {user_text}")
        reply = clean_json(ask_llm(user_text, api_key))
        log("**", f"大模型: {reply}")
    elif args.cmd_file:
        with open(args.cmd_file, "r", encoding="utf-8") as f:
            reply = f.read().strip()
    elif args.cmd:
        reply = args.cmd
    else:
        sys.exit("角色 A 需要 --cmd / --cmd-file / --text / --text-file 之一")

    frame = build_frame(reply.encode("utf-8"), typ=TYPE_CMD, seq=args.seq, crc_name=args.crc)
    return run_role_a(args.port, args.baud, frame, args.seq, args.ack_timeout, args.retries)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PC 端完整语音链路测试（不需要开发板）：

    麦克风录音 -> 智谱 GLM-ASR 识别文字 -> GLM-4-Flash 解析成指令
    -> 按双芯片协议组帧 -> 注入 sscom 由它从 COM3 发出去

用途：在固件还没烧进板子之前，先验证「语音 -> 大模型 -> 协议帧 -> 串口」
整条链路，同时把延迟拆开测出来。

用法：
    set LLM_API_KEY=xxxxx.yyyyy
    python pc_voice_test.py                 # 录音 15 秒，自动挑出有声音的片段
    python pc_voice_test.py --seconds 8     # 录 8 秒
    python pc_voice_test.py --wav a.wav     # 用现成的 wav，不录音
    python pc_voice_test.py --device 1      # 指定录音设备（sounddevice 编号）
"""

import argparse
import io
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from send_uart import ask_llm, clean_json, build_frame, TYPE_CMD  # noqa: E402
from sscom_send import send_hex, to_hex_spaced  # noqa: E402

ASR_URL = "https://open.bigmodel.cn/api/paas/v4/audio/transcriptions"
ASR_MODEL = "glm-asr-2512"
ASR_BOUNDARY = "----amyaiBoundary7d3f9a"

RATE = 16000


def log(tag: str, text: str) -> None:
    print(f"{tag} {text}", flush=True)


# ---------------------------------------------------------------- 录音
def record(seconds: int, device=None):
    import numpy as np
    import sounddevice as sd

    log("**", f"开始录音 {seconds} 秒，请对着麦克风说话…")
    t0 = time.time()
    audio = sd.rec(int(seconds * RATE), samplerate=RATE, channels=1,
                   dtype="int16", device=device)
    sd.wait()
    log("**", f"录音结束（{time.time() - t0:.1f}s，{len(audio)} 样本）")
    return np.squeeze(audio)


def pick_speech(pcm, min_sec=0.4, pad_sec=0.2):    """从长录音里挑出有声片段，容忍用户开口晚"""
    import numpy as np

    x = pcm.astype(np.float32) / 32768.0
    frame = int(RATE * 0.02)
    if len(x) < frame:
        return pcm, 0.0, 0.0

    n = len(x) // frame
    rms = np.sqrt(np.mean(x[: n * frame].reshape(n, frame) ** 2, axis=1))
    peak_rms = float(np.max(rms))
    if peak_rms < 1e-3:
        return None, 0.0, 0.0

    thresh = max(0.01, peak_rms * 0.15)
    loud = np.flatnonzero(rms > thresh)
    if len(loud) == 0:
        return None, 0.0, 0.0

    start = max(0, int((loud[0] * 0.02 - pad_sec) * RATE))
    end = min(len(pcm), int((loud[-1] * 0.02 + pad_sec) * RATE))
    seg = pcm[start:end]
    dur = len(seg) / RATE
    log("**", f"检测到语音：{loud[0]*0.02:.2f}s 开始，时长 {dur:.2f}s，峰值 RMS {peak_rms:.3f}")
    if dur < min_sec:
        return None, start / RATE, dur
    return seg, start / RATE, dur


# ---------------------------------------------------------------- WAV
def to_wav_bytes(pcm) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())
    return buf.getvalue()


def peak_norm(pcm):
    import numpy as np
    x = pcm.astype(np.float32)
    peak = float(np.max(np.abs(x))) or 1.0
    if peak < 3000:                      # 太小就放大，ASR 对音量敏感
        pcm = np.clip(x * (3000.0 / peak), -32768, 32767).astype(np.int16)
    return pcm


# ---------------------------------------------------------------- ASR
def asr_transcribe(wav_bytes: bytes, api_key: str) -> str:
    parts = []
    parts.append(f"--{ASR_BOUNDARY}\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n{ASR_MODEL}\r\n".encode())
    parts.append(f"--{ASR_BOUNDARY}\r\nContent-Disposition: form-data; name=\"file\"; "
                 f"filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n".encode())
    parts.append(wav_bytes)
    parts.append(f"\r\n--{ASR_BOUNDARY}--\r\n".encode())
    body = b"".join(parts)

    req = urllib.request.Request(
        ASR_URL, data=body,
        headers={
            "Content-Type": f"multipart/form-data; boundary={ASR_BOUNDARY}",
            "Authorization": f"Bearer {api_key}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "ignore")[:300]
        log("!!", f"ASR 失败 HTTP {e.code}: {detail}")
        return ""
    except Exception as e:
        log("!!", f"ASR 请求失败: {e}")
        return ""

    text = data.get("text", "").strip()
    return text


# ---------------------------------------------------------------- 主流程
def main() -> int:
    ap = argparse.ArgumentParser(description="语音 -> 大模型 -> 协议帧 -> sscom 全链路测试")
    ap.add_argument("--seconds", type=int, default=15, help="录音秒数，默认 15")
    ap.add_argument("--wav", help="用现成 wav，不录音")
    ap.add_argument("--device", type=int, help="录音设备编号")
    ap.add_argument("--key", help="智谱 Key，或设 LLM_API_KEY")
    ap.add_argument("--seq", type=int, default=1)
    ap.add_argument("--no-send", action="store_true", help="只解析，不发 sscom")
    ap.add_argument("--wake", action="store_true",
                    help="先模拟唤醒并播放「在的」应答音，再录音（和真机时序一致）")
    ap.add_argument("--prompt-wav",
                    default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompt_preview.wav"),
                    help="唤醒后播放的应答音文件")
    args = ap.parse_args()

    api_key = (args.key or os.environ.get("LLM_API_KEY", "")).strip()
    if not api_key:
        sys.exit("请提供 --key 或设置 LLM_API_KEY")

    t_start = time.time()

    # 1. 取音频
    if args.wav:
        with wave.open(args.wav, "rb") as w:
            if w.getframerate() != RATE or w.getnchannels() != 1:
                sys.exit("wav 需要 16kHz 单声道")
            wav_bytes = w.readframes(w.getnframes())
        log("**", f"使用现成文件 {args.wav}")
    else:
        if args.wake:
            # 真机流程：唤醒词命中 -> 立刻播「在的」-> 开始录音
            log("🔔", "模拟唤醒：等待 3 秒（真机上这里是 WakeNet 本地识别，<300ms）")
            time.sleep(3.0)
            log("🔔", "唤醒命中，播放应答音「在的」")
            play_prompt(args.prompt_wav)
            time.sleep(0.15)

        pcm = record(args.seconds, args.device)
        seg, offset, dur = pick_speech(pcm)
        if seg is None:
            log("!!", "没有检测到有效语音。请确认麦克风没静音，然后重试。")
            return 2
        seg = peak_norm(seg)
        wav_bytes = to_wav_bytes(seg)
        log("**", f"音频 {len(wav_bytes)/1024:.0f} KB / {dur:.2f}s")

    # 2. ASR
    t0 = time.time()
    text = asr_transcribe(wav_bytes, api_key)
    t_asr = time.time() - t0
    if not text:
        log("!!", "语音识别没返回文字")
        return 3
    log("🎤", f"识别结果: \"{text}\"  ({t_asr*1000:.0f} ms)")

    # 3. 大模型
    t0 = time.time()
    reply = clean_json(ask_llm(text, api_key))
    t_llm = time.time() - t0
    log("🤖", f"模型输出: {reply}  ({t_llm*1000:.0f} ms)")

    # 4. 组帧
    frame = build_frame(reply.encode("utf-8"), typ=TYPE_CMD, seq=args.seq)
    log("📦", f"帧 {len(frame)} 字节: {to_hex_spaced(frame)}")

    # 5. 发给 sscom
    if args.no_send:
        log("--", "按 --no-send 要求没有发送")
    else:
        t0 = time.time()
        ok = send_hex(to_hex_spaced(frame))
        log("📤", f"注入 sscom {'成功' if ok else '失败'} ({(time.time()-t0)*1000:.0f} ms)")
        if not ok:
            return 4

    log("⏱ ", f"端到端总计 {(time.time()-t_start)*1000:.0f} ms "
              f"(ASR {t_asr*1000:.0f} + LLM {t_llm*1000:.0f} + 串口 ~250)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

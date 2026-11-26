#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
用微软 Edge 在线 TTS（edge-tts）生成唤醒应答语音，并转成固件用的 C 数组。

为什么换掉原来的：原来的 SAPI(Huihui) 合成偏机械、且没有做去噪，
在 ES8311 + 小喇叭上听起来发闷、字头不清。XiaoxiaoNeural 是标准的
播音女声，吐字清楚，短句「在的」辨识度明显更好。

流程：文字 -> mp3 -> 解码 -> 重采样到 22050Hz -> 去首尾静音 -> 加淡入淡出
      -> 归一化 -> 写成 main/prompt_zai_de.h

用法：
    set HTTPS_PROXY=http://127.0.0.1:7897   # 需要能访问微软语音服务
    python gen_prompt_edge.py
"""

import asyncio
import os
import sys

import numpy as np
import soundfile as sf

try:
    import edge_tts
except ImportError:
    sys.exit("请先安装：pip install edge-tts soundfile numpy")

# ---------------------------------------------------------------- 可调参数
VOICE = "zh-CN-XiaoxiaoNeural"   # 标准女声；备选 zh-CN-XiaoyiNeural
TEXT = "在的"
RATE = "+0%"                     # 语速，想更干脆可以 "+10%"
OUT_RATE = 22050                 # 固件播放代码按 22050 处理（再 2x 升到 44100）
PEAK = 0.85                      # 归一化峰值
FADE_MS = 8                      # 首尾淡入淡出，防止爆音
TRIM_DB = -32                    # 低于这个电平视为静音

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(os.path.dirname(HERE), "main", "prompt_zai_de.h")


async def synth(text: str, voice: str, rate: str, mp3_path: str) -> None:
    comm = edge_tts.Communicate(text, voice, rate=rate)
    await comm.save(mp3_path)


def resample_linear(x: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    if sr_in == sr_out:
        return x
    n_out = int(round(len(x) * sr_out / sr_in))
    idx = np.linspace(0.0, len(x) - 1, n_out)
    return np.interp(idx, np.arange(len(x)), x).astype(np.float32)


def trim_silence(x: np.ndarray, sr: int, threshold_db: float) -> np.ndarray:
    thresh = 10.0 ** (threshold_db / 20.0) * np.max(np.abs(x))
    loud = np.flatnonzero(np.abs(x) > thresh)
    if len(loud) == 0:
        return x
    # 前后各留 25ms，避免把字头削掉
    pad = int(sr * 0.025)
    start = max(0, loud[0] - pad)
    end = min(len(x), loud[-1] + pad)
    return x[start:end]


def fade(x: np.ndarray, sr: int, ms: int) -> np.ndarray:
    n = int(sr * ms / 1000)
    if n * 2 >= len(x):
        return x
    ramp = np.linspace(0.0, 1.0, n, dtype=np.float32)
    y = x.copy()
    y[:n] *= ramp
    y[-n:] *= ramp[::-1]
    return y


def write_header(pcm: np.ndarray, sr: int, text: str, voice: str) -> None:
    peak = int(np.max(np.abs(pcm)))
    lines = [
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        f"// 由 edge-tts（{voice}）合成的「{text}」，已裁剪静音并归一化",
        f"// 采样率 {sr}Hz，单声道，16bit；播放时需升采样到 44100（2x）",
        f"constexpr int kPromptZaiDeRate = {sr};",
        f"constexpr size_t kPromptZaiDeLen = {len(pcm)};",
        "static const int16_t kPromptZaiDe[] = {",
    ]
    for i in range(0, len(pcm), 16):
        chunk = pcm[i:i + 16]
        lines.append("    " + ",".join(str(int(v)) for v in chunk) + ",")
    lines.append("};")
    lines.append("")

    with open(HEADER, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print(f"已写入 {HEADER}")
    print(f"  样本数 {len(pcm)}，时长 {len(pcm) / sr:.2f}s，峰值 {peak}")


def main() -> None:
    mp3 = os.path.join(HERE, "prompt_raw.mp3")
    if not os.path.exists(mp3):
        print(f"合成中：{TEXT} / {VOICE} ...")
        asyncio.run(synth(TEXT, VOICE, RATE, mp3))

    data, sr = sf.read(mp3, dtype="float32", always_2d=True)
    x = data.mean(axis=1)                      # 多声道取平均
    print(f"原始：{len(x)} 样本 @ {sr}Hz")

    x = resample_linear(x, sr, OUT_RATE)
    x = trim_silence(x, OUT_RATE, TRIM_DB)
    x = fade(x, OUT_RATE, FADE_MS)

    peak = np.max(np.abs(x))
    if peak > 0:
        x = x / peak * PEAK

    pcm = np.clip(np.round(x * 32767.0), -32768, 32767).astype(np.int16)

    wav = os.path.join(HERE, "prompt_preview.wav")
    sf.write(wav, pcm, OUT_RATE, subtype="PCM_16")
    print(f"试听文件：{wav}")

    write_header(pcm, OUT_RATE, TEXT, VOICE)


if __name__ == "__main__":
    main()

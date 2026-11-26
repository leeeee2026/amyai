#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把一段 WAV 转成固件用的 C 数组（main/prompt_zai_de.h）。

用途：音频在 edge-tts 生成后，往往还要调电平/裁剪，最后统一用这个脚本落成头文件，
避免手改 700 行数组。

用法：
    python wav_to_header.py 输入.wav --peak 0.95
    python wav_to_header.py 输入.wav --peak 0.95 --trim
"""

import argparse
import os
import wave

import numpy as np


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getnchannels() == 1, "需要单声道"
        assert w.getsampwidth() == 2, "需要 16bit"
        rate = w.getframerate()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return data, rate


def trim_silence(x, rate, thresh_db=-40.0, pad_ms=15):
    thresh = 10 ** (thresh_db / 20.0) * np.max(np.abs(x.astype(np.float32)))
    loud = np.flatnonzero(np.abs(x.astype(np.float32)) > thresh)
    if len(loud) == 0:
        return x
    pad = int(rate * pad_ms / 1000)
    return x[max(0, loud[0] - pad): min(len(x), loud[-1] + pad)]


def write_header(pcm, rate, out_path, comment):
    lines = [
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        comment,
        f"// 采样率 {rate}Hz，单声道，16bit；播放时需升采样到 44100（2x）",
        f"constexpr int kPromptZaiDeRate = {rate};",
        f"constexpr size_t kPromptZaiDeLen = {len(pcm)};",
        "static const int16_t kPromptZaiDe[] = {",
    ]
    for i in range(0, len(pcm), 16):
        lines.append("    " + ",".join(str(int(v)) for v in pcm[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"已写入 {out_path}")
    print(f"  样本 {len(pcm)}，时长 {len(pcm)/rate:.2f}s，峰值 {int(np.max(np.abs(pcm)))}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  "..", "main", "prompt_zai_de.h"))
    ap.add_argument("--peak", type=float, default=0.95, help="归一化峰值 0~1")
    ap.add_argument("--trim", action="store_true", help="先裁剪首尾静音")
    ap.add_argument("--comment", default="// 由 edge-tts 合成，已裁剪静音并归一化")
    args = ap.parse_args()

    pcm, rate = read_wav(args.wav)
    if args.trim:
        pcm = trim_silence(pcm, rate)

    x = pcm.astype(np.float32)
    peak = float(np.max(np.abs(x))) or 1.0
    x = x / peak * args.peak
    pcm = np.clip(np.round(x * 32767.0), -32768, 32767).astype(np.int16)

    write_header(pcm, rate, os.path.abspath(args.out), args.comment)


if __name__ == "__main__":
    main()

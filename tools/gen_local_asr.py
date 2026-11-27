#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 docs/离线命令词表.md 生成固件用的离线命令词表。

用法：
    python tools/gen_local_asr.py            # 生成/更新
    python tools/gen_local_asr.py --check    # 只校验，不写文件

做的事情：
1. 解析文档里的 Markdown 表格（指令ID / 分类 / 中文说法 / 拼音 / 下发指令）
2. 「拼音」列为空的，用 pypinyin 自动补（不带声调、空格分隔）
3. 把补好的拼音写回文档，保持表格对齐
4. 生成 main/local_asr_cmds.h
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "离线命令词表.md"
HEADER = ROOT / "main" / "local_asr_cmds.h"

MAX_PHRASES = 200
MAX_ID = 200

ROW_RE = re.compile(r"^\|\s*(\d+)\s*\|\s*([^|]*?)\s*\|\s*([^|]*?)\s*\|\s*([^|]*?)\s*\|\s*(.*?)\s*\|\s*$")
PINYIN_RE = re.compile(r"^[a-z]+( [a-z]+)*$")


def load_pinyin():
    try:
        from pypinyin import lazy_pinyin
    except ImportError:
        sys.exit("缺少依赖：pip install pypinyin")
    return lazy_pinyin


def parse(doc_text):
    rows = []
    for line in doc_text.splitlines():
        m = ROW_RE.match(line)
        if not m:
            continue
        cid, cat, cn, py, js = m.groups()
        # 跳过表头分隔行（---）以及其他非数据行
        if not cn or cn == "中文说法":
            continue
        rows.append({
            "id": int(cid),
            "cat": cat.strip(),
            "cn": cn.strip(),
            "pinyin": py.strip(),
            "json": js.strip(),
        })
    return rows


def rewrite_doc(doc_text, rows):
    """把补好的拼音写回表格，其余内容原样保留。"""
    it = iter(rows)
    out = []
    for line in doc_text.splitlines():
        m = ROW_RE.match(line)
        if not m:
            out.append(line)
            continue
        cn = m.group(3).strip()
        if not cn or cn == "中文说法":
            out.append(line)
            continue
        r = next(it)
        out.append("| {id} | {cat} | {cn} | {py} | {js} |".format(
            id=r["id"], cat=r["cat"], cn=r["cn"], py=r["pinyin"], js=r["json"]))
    return "\n".join(out) + "\n"


def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def gen_header(rows):
    max_id = max(r["id"] for r in rows)
    lines = [
        "// 由 tools/gen_local_asr.py 自动生成，请勿手改！",
        "// 修改词表请编辑 docs/离线命令词表.md，然后重新运行：",
        "//     python tools/gen_local_asr.py",
        "#pragma once",
        "",
        "#include <cstddef>",
        "",
        "struct LocalCmdEntry {",
        "    int id;             // 指令 ID（同 ID 的多条短语 = 同义词）",
        "    const char* pinyin; // 交给 MultiNet 的拼音（不带声调、空格分隔）",
        "    const char* display;// 中文说法（日志用）",
        "    const char* json;   // 下发给主控的指令 JSON",
        "};",
        "",
        "static const LocalCmdEntry kLocalCmds[] = {",
    ]
    for r in rows:
        lines.append('    {%d, "%s", "%s", R"(%s)"},' % (
            r["id"], c_escape(r["pinyin"]), c_escape(r["cn"]), r["json"]))
    lines += [
        "};",
        "",
        "static constexpr int kLocalCmdCount = static_cast<int>(sizeof(kLocalCmds) / sizeof(kLocalCmds[0]));",
        "static constexpr int kLocalCmdMaxId = %d;" % max_id,
        "",
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只校验，不写文件")
    args = ap.parse_args()

    if not DOC.exists():
        sys.exit("找不到词表文档：%s" % DOC)
    text = DOC.read_text(encoding="utf-8")
    rows = parse(text)
    if not rows:
        sys.exit("文档里没解析到任何命令词，检查表格格式")

    lazy_pinyin = load_pinyin()

    # 1) 补拼音
    filled = 0
    for r in rows:
        if not r["pinyin"]:
            r["pinyin"] = " ".join(lazy_pinyin(r["cn"]))
            filled += 1

    # 2) 校验
    errors = []
    warnings = []
    if len(rows) > MAX_PHRASES:
        errors.append("短语总数 %d 超过上限 %d" % (len(rows), MAX_PHRASES))

    seen_pinyin = {}
    for i, r in enumerate(rows, 1):
        if not (1 <= r["id"] <= MAX_ID):
            errors.append("第 %d 行：指令ID %d 超出 1~%d" % (i, r["id"], MAX_ID))
        if not PINYIN_RE.match(r["pinyin"]):
            errors.append("第 %d 行「%s」拼音非法：%r（只能是小写字母和空格）"
                          % (i, r["cn"], r["pinyin"]))
        if len(r["pinyin"].split()) > 8:
            warnings.append("第 %d 行「%s」拼音较长（%d 个音节），识别率可能下降"
                            % (i, r["cn"], len(r["pinyin"].split())))
        if r["pinyin"] in seen_pinyin:
            errors.append("拼音重复：「%s」和「%s」都是 %r"
                          % (seen_pinyin[r["pinyin"]], r["cn"], r["pinyin"]))
        seen_pinyin[r["pinyin"]] = r["cn"]
        try:
            obj = json.loads(r["json"])
        except json.JSONDecodeError as e:
            errors.append("第 %d 行「%s」JSON 非法：%s" % (i, r["cn"], e))
            continue
        if "cmd" not in obj:
            errors.append("第 %d 行「%s」JSON 缺少 cmd 字段" % (i, r["cn"]))

    # 同一 ID 的 JSON 必须一致（否则同义词会下发不同指令）
    by_id = {}
    for r in rows:
        by_id.setdefault(r["id"], set()).add(r["json"])
    for cid, js_set in sorted(by_id.items()):
        if len(js_set) > 1:
            errors.append("指令ID %d 有多条不同的下发指令：%s" % (cid, list(js_set)))

    uniq_ids = len(by_id)
    print("解析到 %d 条短语，%d 个指令 ID，自动补拼音 %d 条" % (len(rows), uniq_ids, filled))
    for w in warnings:
        print("  ⚠️  " + w)
    if errors:
        for e in errors:
            print("  ❌ " + e)
        sys.exit("校验失败，未写入文件")

    if args.check:
        print("✅ 校验通过")
        return

    if filled:
        DOC.write_text(rewrite_doc(text, rows), encoding="utf-8")
        print("已把拼音写回 %s" % DOC)

    HEADER.parent.mkdir(parents=True, exist_ok=True)
    HEADER.write_text(gen_header(rows), encoding="utf-8")
    print("已生成 %s" % HEADER)

    # 分类统计
    cats = {}
    for r in rows:
        cats[r["cat"]] = cats.get(r["cat"], 0) + 1
    print("分类分布：" + "，".join("%s %d" % (k, v) for k, v in cats.items()))


if __name__ == "__main__":
    main()

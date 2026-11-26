#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
按摩椅意图解析 —— 大模型联调脚本（纯标准库，无需 pip install）

用途：在电脑上先验证「语音文本 -> 结构化指令」这一段，
      确认 API Key、prompt、输出格式都对了，再搬到 ESP32 上去。

用法：
    setx LLM_API_KEY "你的key"        # 只需一次，重新开终端生效
    python test_llm.py "我脖子好酸"
    python test_llm.py --provider bailian "来个半小时的"
    python test_llm.py --raw "力气太大了"    # 同时打印原始报文

支持：
    zhipu   (默认) 智谱 GLM-4-Flash      —— 永久免费
    bailian        阿里云百炼 qwen-flash  —— 有免费额度
    volcano        火山引擎 doubao-lite   —— 有免费额度
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

SYSTEM_PROMPT = """你是按摩椅的语音指令解析器。只输出一行 JSON，不要解释文字、不要 Markdown 代码块。

【输出格式】
单意图：{"cmd":"...", ...}
一句话里两个动作：{"actions":[{"cmd":...},{"cmd":...}]}

【指令表】
start / stop / pause / resume
set_zone          zones: ["neck","shoulder","back","waist","hip","leg","foot","arm","full"]，可多选
set_intensity     level: 1-5                力度绝对值
adjust_intensity  delta: -2..2，可带 zones   力度相对调整
set_speed         level: 1-5                节奏速度绝对值
adjust_speed      delta: -2..2              速度相对调整
set_mode          mode: knead|tap|press|knead_tap|shiatsu|stretch   手法
set_heat          on: true|false            热敷
set_zero_gravity  on: true|false            零重力
set_foot_roller   on: true|false            足底滚轮
set_airbag        level: 0-5                气囊（0=关）
set_recline       angle: 100-170            靠背角度（度，越大越躺）
adjust_recline    delta: -30..30            靠背相对调整
set_leg_rest      angle: 0-90               腿托角度（度）
adjust_leg_rest   delta: -30..30            腿托相对调整
set_time          minutes: 5-60             定时
set_program       program: sleep|relax|neck|back|waist|leg|foot|full|office|sport|stretch|heat|zero_g|knead|tap|shiatsu|morning|night|elder|female|male|quick|deep|gentle
query_status / unknown

【相对 vs 绝对】
"一点 / 稍微 / 再 / 太…了"        -> adjust_*（给 delta）
"调到 N 档 / 最高 / 最低 / 最大"  -> set_*（给具体值）

【规则】
1. 没提部位就不要输出 zones（表示作用于当前部位/全身）
2. 用户表达"疼 / 不舒服 / 太重 / 太紧" -> adjust_intensity delta:-1（严重的用 -2）
3. 一句话里"部位 + 其他维度"要拆成 actions 数组
4. 与按摩无关、或听不出意图 -> {"cmd":"unknown"}
5. 字段名和枚举值必须是英文字面量，不要多余字段，不要中文

【示例】
用户："脖子好酸"              -> {"cmd":"set_zone","zones":["neck"]}
用户："肩膀和脖子都按按"      -> {"cmd":"set_zone","zones":["neck","shoulder"]}
用户："重一点"                -> {"cmd":"adjust_intensity","delta":1}
用户："重点按肩膀"            -> {"actions":[{"cmd":"set_zone","zones":["shoulder"]},{"cmd":"adjust_intensity","delta":1}]}
用户："太疼了"                -> {"cmd":"adjust_intensity","delta":-2}
用户："调到三档"              -> {"cmd":"set_intensity","level":3}
用户："节奏慢一点"            -> {"cmd":"adjust_speed","delta":-1}
用户："腿抬高一点"            -> {"cmd":"adjust_leg_rest","delta":10}
用户："腿放低"                -> {"cmd":"adjust_leg_rest","delta":-20}
用户："躺下来一点"            -> {"cmd":"adjust_recline","delta":20}
用户："坐起来"                -> {"cmd":"set_recline","angle":110}
用户："来个零重力"            -> {"cmd":"set_zero_gravity","on":true}
用户："开热敷"                -> {"cmd":"set_heat","on":true}
用户："用揉捏的"              -> {"cmd":"set_mode","mode":"knead"}
用户："足底滚轮关掉"          -> {"cmd":"set_foot_roller","on":false}
用户："来个半小时的"          -> {"cmd":"set_time","minutes":30}
用户："来个解乏的"            -> {"cmd":"set_program","program":"relax"}
用户："现在什么状态"          -> {"cmd":"query_status"}
用户："帮我订张机票"          -> {"cmd":"unknown"}
"""

# 各家 OpenAI 兼容接口
PROVIDERS = {
    'zhipu': {
        'url': 'https://open.bigmodel.cn/api/paas/v4/chat/completions',
        'model': 'glm-4-flash',
        'note': '智谱 GLM-4-Flash（永久免费）',
    },
    'bailian': {
        'url': 'https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions',
        'model': 'qwen-flash',
        'note': '阿里云百炼 qwen-flash',
    },
    'volcano': {
        'url': 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
        'model': 'doubao-lite-32k',
        'note': '火山引擎 doubao-lite-32k',
    },
}

ALLOWED_CMDS = {'start', 'stop', 'pause', 'set_zone', 'set_level',
                'set_time', 'query_status', 'unknown'}


def call_llm(provider: str, api_key: str, user_text: str, raw: bool) -> int:
    conf = PROVIDERS[provider]

    payload = {
        'model': conf['model'],
        'messages': [
            {'role': 'system', 'content': SYSTEM_PROMPT},
            {'role': 'user', 'content': user_text},
        ],
        'temperature': 0.0,
        'max_tokens': 80,
    }

    req = urllib.request.Request(
        conf['url'],
        data=json.dumps(payload).encode('utf-8'),
        headers={
            'Content-Type': 'application/json',
            'Authorization': f'Bearer {api_key}',
        },
        method='POST',
    )

    print(f'--- 提供方: {conf["note"]}  模型: {conf["model"]} ---')
    print(f'用户: {user_text}')

    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            body = resp.read().decode('utf-8')
    except urllib.error.HTTPError as e:
        detail = e.read().decode('utf-8', errors='replace')
        print(f'\n[HTTP {e.code}] 请求失败')
        print(detail[:1000])
        return 1
    except urllib.error.URLError as e:
        print(f'\n[网络错误] {e.reason}')
        print('提示：检查网络/代理；公司网络可能拦截了该域名')
        return 1

    if raw:
        print(f'\n原文: {body[:800]}')

    try:
        data = json.loads(body)
        text = data['choices'][0]['message']['content'].strip()
    except (KeyError, IndexError, json.JSONDecodeError) as e:
        print(f'\n[解析失败] {e}')
        print(body[:800])
        return 1

    print(f'\n模型返回: {text}')

    # 模拟固件侧的严格校验
    try:
        cmd = json.loads(text)
    except json.JSONDecodeError:
        print('[校验] [REJECT] 不是合法 JSON —— 固件应当拒绝执行')
        return 1

    if 'cmd' not in cmd:
        print('[校验] [REJECT] 缺少 cmd 字段 —— 固件应当拒绝执行')
        return 1

    if cmd['cmd'] not in ALLOWED_CMDS:
        print(f'[校验] [REJECT] cmd="{cmd["cmd"]}" 不在白名单 —— 固件应当拒绝执行')
        return 1

    print(f'[校验] [OK] 合法指令: {json.dumps(cmd, ensure_ascii=False)}')
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description='按摩椅意图解析联调脚本')
    ap.add_argument('text', help='用户说的话，例如 "我脖子好酸"')
    ap.add_argument('--provider', default='zhipu', choices=list(PROVIDERS),
                    help='默认 zhipu（免费）')
    ap.add_argument('--raw', action='store_true', help='打印原始响应报文')
    args = ap.parse_args()

    key = os.environ.get('LLM_API_KEY', '').strip()
    if not key:
        print('错误：没有找到环境变量 LLM_API_KEY')
        print('请先执行（然后重新打开一个终端）：')
        print('    setx LLM_API_KEY "你的key"')
        return 2

    return call_llm(args.provider, key, args.text, args.raw)


if __name__ == '__main__':
    sys.exit(main())

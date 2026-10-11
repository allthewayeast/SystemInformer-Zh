# -*- coding: utf-8 -*-
"""
扫描 System Informer 源码里的 .rc 资源，建立「英文串 -> 资源类别」映射。

为什么要走资源而不是猜：
  用户要的分类（对话框 / 字符串表 / 消息表 / 版本）**本来就是 Windows 资源类型**，
  它们在 .rc 里是有明确边界的块。只要把块切出来，类别就是证据而不是猜测。

做法：
  1) 用「块头关键字 + BEGIN/END 配对」切块，块头决定类别；
  2) 块内所有 "..." 字面量登记为该类别；
  3) 输出 类别 -> 字面量集合，供 organize.py 给词典条目归类。

输出：rc_literals.json  { "DIALOG": [...], "MENU": [...], ... }
      rc_survey.txt    人读报告
"""

import os
import re
import json
import collections

import paths                   # 所有路径集中在 tools\paths.py
SRC = paths.SI_SRC
OUT_JSON = paths.RC_LITERALS
OUT_TXT = os.path.join(paths.BUILD, "rc_survey.txt")

# 块头关键字 -> 归类名。顺序即优先级：靠前的先匹配。
BLOCK_KINDS = [
    ("DIALOGEX",     "DIALOG"),
    ("DIALOG",       "DIALOG"),
    ("STRINGTABLE",  "STRINGTABLE"),
    ("MESSAGETABLE", "MESSAGETABLE"),
    ("MESSAGE",      "MESSAGETABLE"),
    ("VERSIONINFO",  "VERSIONINFO"),
    ("MENUEX",       "MENU"),
    ("MENU",         "MENU"),
    ("ACCELERATORS", "ACCELERATORS"),
    ("RCDATA",       "RCDATA"),
]
KIND_RE = "|".join(k for k, _ in BLOCK_KINDS)

# 块头：<id> <KIND> [DISCARDABLE] [MOVEABLE]...  允许 id 是标识符或数字
HEAD_RE = re.compile(
    r"^[ \t]*([A-Za-z_][A-Za-z0-9_]*|\d+)[ \t]+(" + KIND_RE + r")\b[^\n]*$",
    re.MULTILINE)

STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
TOKEN_BEGIN = re.compile(r"\bBEGIN\b")
TOKEN_END = re.compile(r"\bEND\b")

# tools\ 下的 rc 属于 peview/安装向导/simcp，不在 SystemInformer.exe 里出现
SKIP_DIRS = (".git", ".vs", "build")


def collect_rc_files():
    out = []
    for root, dirs, files in os.walk(SRC):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in files:
            if fn.lower().endswith(".rc"):
                out.append(os.path.join(root, fn))
    return out


def block_end(txt, start):
    """从 start 起找与第一个 BEGIN 配对的 END 结束位置。找不到返回 len(txt)。"""
    mb = TOKEN_BEGIN.search(txt, start)
    if not mb:
        return len(txt), None, None
    depth = 0
    pos = mb.start()
    for m in re.finditer(r"\b(BEGIN|END)\b", txt[pos:]):
        if m.group(1) == "BEGIN":
            depth += 1
        else:
            depth -= 1
            if depth == 0:
                return pos + m.end(), mb.end(), pos + m.start()
    return len(txt), mb.end(), pos


def main():
    files = collect_rc_files()
    by_kind = collections.defaultdict(set)
    per_file = []
    kind_count = collections.Counter()

    for p in files:
        rel = os.path.relpath(p, SRC)
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                txt = f.read()
        except Exception as e:
            per_file.append((rel, "读取失败: %s" % e))
            continue

        n_blocks = 0
        for m in HEAD_RE.finditer(txt):
            kind = dict(BLOCK_KINDS)[m.group(2)]
            end, bstart, btok = block_end(txt, m.end())
            if bstart is None:
                continue
            body = txt[bstart:end]
            for s in STR_RE.findall(body):
                s = s.replace('\\"', '"').replace("\\\\", "\\")
                if s.strip():
                    by_kind[kind].add(s)
            kind_count[kind] += 1
            n_blocks += 1

        per_file.append((rel, "%d 个块" % n_blocks))

    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump({k: sorted(v) for k, v in by_kind.items()}, f,
                  ensure_ascii=False, indent=1)

    lines = []
    lines.append("rc 文件数: %d" % len(files))
    lines.append("")
    lines.append("=== 各类别：块数 / 唯一字面量数 ===")
    for k in sorted(by_kind, key=lambda x: -len(by_kind[x])):
        lines.append("%-14s 块 %4d   字面量 %5d" % (k, kind_count[k], len(by_kind[k])))
    lines.append("")
    lines.append("=== 逐文件块数 ===")
    for rel, info in per_file:
        lines.append("%-60s %s" % (rel, info))

    txt = "\n".join(lines)
    with open(OUT_TXT, "w", encoding="utf-8") as f:
        f.write(txt)
    print(txt)
    print()
    print("RC_JSON = %s" % OUT_JSON)


if __name__ == "__main__":
    main()

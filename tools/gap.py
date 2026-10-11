# -*- coding: utf-8 -*-
r"""
量一下「还剩多少没覆盖」——上界估计，不是精确值。

第 3 段只收了源码里以 & 开头的 L"..." 字面量（因为只有那批能确定是菜单串）。
但源码里还有大量**不带 &** 的 UI 串：菜单里共用首字母的项（L"Terminate tree"）、
选项页条目（L"Allow only one instance"）、插件显示名。它们不在 Passolo 导出里
（Passolo 只抽资源），也没被我捞到 —— 这就是截图里仍显示英文的那一批。

本脚本把它们捞出来做上界统计，并列出最集中的文件。
"""
import os, re, collections

import paths                   # 所有路径集中在 tools\paths.py
CUR     = paths.MERGED
SRCROOT = paths.SI_SRC
LIT     = re.compile(r'L"((?:[^"\\\n]|\\.){1,90})"')

def c_unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            out.append(c); i += 1; continue
        if i + 1 >= len(s): return None
        n = s[i + 1]
        if   n == "b":  out.append("\b"); i += 2
        elif n == "t":  out.append("\t"); i += 2
        elif n == "n":  return None
        elif n == "\\": out.append("\\"); i += 2
        elif n == '"':  out.append('"');  i += 2
        else:           return None
    return "".join(out)

keys = set()
with open(CUR, "r", encoding="utf-8") as f:
    for l in f:
        l = l.rstrip("\r\n")
        if l.startswith("#") or "\t" not in l: continue
        keys.add(l.split("\t", 1)[0])

BAD_EXT = (".dll", ".exe", ".sys", ".json", ".txt", ".ini", ".log", ".sig", ".cat",
           ".mui", ".db", ".xml", ".csv", ".md", ".bat", ".cmd", ".ps1")
found = {}                      # literal -> (rel, line)
for root, dirs, files in os.walk(SRCROOT):
    dirs[:] = [d for d in dirs if d not in (".git", ".vs", "build")]
    for fn in files:
        if not fn.endswith((".c", ".cpp", ".h")):
            continue
        p = os.path.join(root, fn)
        rel = os.path.relpath(p, SRCROOT)
        if rel.startswith("tools\\") or "\\tools\\" in rel:
            continue
        try:
            with open(p, "r", encoding="utf-8", errors="ignore", newline="") as f:
                lines = f.read().split("\n")
        except Exception:
            continue
        for ln, text in enumerate(lines, 1):
            for m in LIT.finditer(text):
                s = c_unescape(m.group(1))
                if not s or s.startswith("&") or s in keys or s in found:
                    continue
                if "%" in s or "\\" in s or "/" in s or "://" in s:
                    continue
                if s.lower().endswith(BAD_EXT):
                    continue
                if not re.search(r"[A-Za-z]", s):
                    continue
                if re.match(r"^[A-Za-z0-9_\-\. ]*$", s) is None:
                    continue
                if re.match(r"^[A-Z0-9_]+$", s):        # 全大写常量
                    continue
                if "_" in s and " " not in s:           # setting 名之类
                    continue
                # 至少要像个 UI 标签：含空格的多词，或首字母大写的 5+ 词
                if " " in s.replace(" ", " ", 1) or (len(s) >= 5 and s[0].isupper()):
                    found[s] = (rel, ln)

byfile = collections.Counter(v[0] for v in found.values())
print("候选未覆盖串（上界，含非 UI 噪声）: %d" % len(found))
print("\n最集中的文件：")
for f, c in byfile.most_common(15):
    print("  %-58s %d" % (f, c))
print("\n样本（按文件分组，前 60）：")
shown = 0
for s, (rel, ln) in sorted(found.items(), key=lambda kv: kv[1][0]):
    print("  %-40s %s:%d" % (s[:40], rel.split("\\")[-1], ln))
    shown += 1
    if shown >= 60: break

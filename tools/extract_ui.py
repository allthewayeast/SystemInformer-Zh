# -*- coding: utf-8 -*-
r"""
抽出「选项页 + 插件页」那一批 UI 串（第 4 段要补的条目）。

为什么单独抽这一批：
  第 2 段来自 Passolo（只覆盖 .rc 资源），第 3 段只收了源码里 L"&Xxx" 形式的菜单串。
  选项页的条目是**代码内联的普通字符串**（PhAddListViewItem(..., L"Allow only one instance", ...)），
  既不在资源里、也不带 & —— 前两段天然碰不到，所以界面上仍显示英文。

抽取方式刻意只认「明确会把字符串画到界面上的调用」，不泛抽所有 L"..."：
  - PhOptionsCreateSection(L"…")                  -> 页签名（左侧树）
  - PhAddListViewItem(h, idx, L"…", …)            -> 列表/勾选项文本
  - PhSetWindowText(h, L"…")                      -> 静态文本
  - PhAddComboBoxItem / PhAddTreeNewColumn 等      -> 同类
这样精度高，不需要大堆启发式过滤。

输出 _work\dict\ui_candidates.tsv：group \t literal \t file:line
"""
import os, re, collections

import paths                   # 所有路径集中在 tools\paths.py
SRC = paths.SI_SRC
OUT = os.path.join(paths.BUILD, "ui_candidates.tsv")

LIT = re.compile(r'L"((?:[^"\\\n]|\\.){1,300})"')

def c_unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            out.append(c); i += 1; continue
        if i + 1 >= len(s):
            return None
        n = s[i + 1]
        if   n == "b":  out.append("\b"); i += 2
        elif n == "t":  out.append("\t"); i += 2
        elif n == "\\": out.append("\\"); i += 2
        elif n == '"':  out.append('"');  i += 2
        else:           return None
    return "".join(out)

# 只匹配这些调用；每条正则取「最后一个宽字符串字面量」作为 UI 文本
PATTERNS = [
    ("page",   re.compile(r'PhOptionsCreateSection\s*\(\s*(L"(?:[^"\\]|\\.)*")')),
    ("entry",  re.compile(r'PhAddListViewItem\s*\([^,]+,[^,]+,\s*(L"(?:[^"\\]|\\.)*")')),
    ("static", re.compile(r'PhSetWindowText\s*\([^,]+,\s*(L"(?:[^"\\]|\\.)*")')),
    ("combo",  re.compile(r'PhAddComboBoxItem\s*\([^,]+,\s*(L"(?:[^"\\]|\\.)*")')),
    ("column", re.compile(r'PhAddTreeNewColumn\w*\s*\([^,]+,\s*[^,]+,\s*[^,]+,\s*(L"(?:[^"\\]|\\.)*")')),
    ("button", re.compile(r'PhSetDialogItemText\s*\([^,]+,\s*[^,]+,\s*(L"(?:[^"\\]|\\.)*")')),
]

found = collections.defaultdict(dict)   # literal -> {group: [loc,...]}
for root, dirs, files in os.walk(SRC):
    dirs[:] = [d for d in dirs if d not in (".git", ".vs", "build", "tools")]
    for fn in files:
        if not fn.endswith((".c", ".cpp")):
            continue
        p = os.path.join(root, fn)
        rel = os.path.relpath(p, SRC)
        if rel.startswith("tools\\"):
            continue
        try:
            with open(p, "r", encoding="utf-8", errors="ignore", newline="") as f:
                lines = f.read().split("\n")
        except Exception:
            continue
        for ln, text in enumerate(lines, 1):
            for group, rx in PATTERNS:
                for m in rx.finditer(text):
                    raw = m.group(1)
                    mm = LIT.match(raw)
                    if not mm:
                        continue
                    s = c_unescape(mm.group(1))
                    if not s or len(s) < 3:
                        continue
                    if s in ("", " ", "\\"):
                        continue
                    found[s].setdefault(group, []).append("%s:%d" % (rel, ln))

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("group\tliteral\tloc\n")
    for s in sorted(found, key=str.lower):
        groups = ",".join(sorted(found[s]))
        loc = found[s][sorted(found[s])[0]][0]
        f.write("%s\t%s\t%s\n" % (groups, s, loc))

byg = collections.Counter()
for s in found:
    for g in found[s]:
        byg[g] += 1
print("唯一 UI 串: %d" % len(found))
for g, c in byg.most_common():
    print("  %-8s %d" % (g, c))
print("OUT = %s" % OUT)

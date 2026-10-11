# -*- coding: utf-8 -*-
"""把源码里所有 L"&Xxx" 菜单串连同上下文打出来，用于准确翻译（避免望文生义）。"""
import os, re

import paths                   # 所有路径集中在 tools\paths.py
SRCROOT = paths.SI_SRC
LIT = re.compile(r'L"((?:[^"\\\n]|\\.){1,90})"')

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
        elif n == "n":  return None
        elif n == "\\": out.append("\\"); i += 2
        elif n == '"':  out.append('"');  i += 2
        else:           return None
    return "".join(out)

rows = []
for root, dirs, files in os.walk(SRCROOT):
    dirs[:] = [d for d in dirs if d not in (".git", ".vs", "build")]
    for fn in files:
        if not fn.endswith((".c", ".cpp", ".h", ".rc")):
            continue
        p = os.path.join(root, fn)
        try:
            with open(p, "r", encoding="utf-8", errors="ignore", newline="") as f:
                lines = f.read().split("\n")
        except Exception:
            continue
        rel = os.path.relpath(p, SRCROOT)
        for ln, text in enumerate(lines, 1):
            for m in LIT.finditer(text):
                lit = c_unescape(m.group(1))
                if not lit or not re.match(r"^&[A-Za-z0-9]", lit):
                    continue
                ctx = text.strip()
                ctx = re.sub(r"\s+", " ", ctx)
                if len(ctx) > 104:
                    i = ctx.find(lit)
                    lo = max(0, i - 40) if i >= 0 else 0
                    ctx = ctx[lo:lo + 104]
                rows.append((lit, rel, ln, ctx))

seen = {}
for lit, rel, ln, ctx in rows:
    seen.setdefault(lit, (rel, ln, ctx))

print("distinct = %d" % len(seen))
for lit in sorted(seen, key=str.lower):
    rel, ln, ctx = seen[lit]
    print("%-34s %-42s %s" % (lit, "%s:%d" % (rel, ln), ctx))

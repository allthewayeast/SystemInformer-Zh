# -*- coding: utf-8 -*-
r"""
核对第 4 段：译文表 / 源码缺口清单 / 最终词典 三者对账，输出 UTF-8 文件（控制台会乱码）。

注意 ui_gap.py 是**拿当前词典做减法**生成的 —— 已经译好的串不会再出现在缺口清单里，
所以不能再用「UI_ZH 里的键是否在缺口清单里」当判据，那会反过来全是「不在」。
正确的判据是：**每个 UI_ZH 键最终都要落在合并后的词典里**。
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ui_zh import UI_ZH, SKIP
import paths                   # 所有路径集中在 tools\paths.py

def entries(p):
    out = []
    for l in open(p, encoding="utf-8"):
        l = l.rstrip("\r\n")
        if l.startswith("#") or "\t" not in l: continue
        out.append(l.split("\t", 1)[0])
    return out

def gap_of(p):
    out = {}
    for l in open(p, encoding="utf-8").read().split("\n")[1:]:
        l = l.rstrip("\r")
        if "\t" in l:
            g, lit, loc = l.split("\t", 2)
            out.setdefault(lit, (g, loc))
    return out

merged = entries(paths.MERGED)
base   = entries(paths.BASE_DICT)
gap    = gap_of(paths.UI_GAP_TSV)
mset   = set(merged)

L = []
L.append("最终词典条目        : %d" % len(merged))
L.append("  第 1 段 baseline  : %d" % len(base))
L.append("  重复键            : %s" % (sorted(k for k in set(merged) if merged.count(k) > 1) or "无"))
L.append("")
L.append("译文表 UI_ZH        : %d" % len(UI_ZH))
missing = sorted(k for k in UI_ZH if k not in mset)
L.append("  ！未进最终词典     : %d %s" % (len(missing), missing))
L.append("刻意不译 SKIP       : %d" % len(SKIP))
L.append("  ！SKIP 却进了词典  : %s" % (sorted(k for k in SKIP if k in mset) or "无"))
L.append("  ！UI_ZH 与 SKIP 重叠: %s" % (sorted(set(UI_ZH) & set(SKIP)) or "无"))
L.append("")
rest = sorted(k for k in gap if k not in mset and k not in UI_ZH and k not in SKIP)
L.append("源码里仍未被覆盖的 UI 串（下一批）: %d" % len(rest))
import collections
byfile = collections.Counter(gap[k][1].split(":")[0] for k in rest)
L.append("  最集中的文件：")
for f, n in byfile.most_common(15):
    L.append("    %-56s %d" % (f, n))
byg = collections.Counter(gap[k][0] for k in rest)
L.append("  按抽取来源：%s" % ", ".join("%s=%d" % (a, b) for a, b in byg.most_common()))

open(os.path.join(paths.BUILD, "reconcile.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(L))
print("written")

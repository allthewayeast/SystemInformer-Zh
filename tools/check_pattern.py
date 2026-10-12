# -*- coding: utf-8 -*-
r"""
校验 dict\zh_dict.pattern.txt —— 通配符模式表。

查的是「写错了不会报错、只会在界面上显示成乱码或悄悄漏数据」的那类问题。
判据与 src\hook\zh_hook.cpp 里的 PatAnalyze() / PatExpand() 一一对应 ——
改那边就要同时改这里。

  * 一行里有第二个 TAB        -> 错误。译文取第一个 TAB 之后的**全部**内容，
                                  所以行尾写注释会变成译文的一部分（真踩过）。
  * 原文没有通配符            -> 警告。这种该放进 zh_dict.txt 走精确匹配。
  * 译文占位符少于原文通配符  -> 警告。没被引用的捕获会追加到译文末尾。
  * \N 的 N 超过通配符个数    -> 错误。永远取不到东西。
  * 原文重复                  -> 错误。
  * 字面部分太短              -> 警告。判据是「通配符之外还剩几个字」而不是
                                  「前缀有多长」：'*Notes: *' 前缀为 0 但字面有
                                  7 个字，它是**故意**写成首尾带 *（子串替换）的，
                                  不该报警；真正危险的是 '*is *' 这种字面只剩 3 个字
                                  的写法。
  * 靠前的模式覆盖靠后的      -> 提示。模式按顺序应用，后者可能永远轮不到。

用法：
    python tools\check_pattern.py
    python tools\check_pattern.py --strict      # 警告也算失败（构建时用）

退出码：0 = 通过；1 = 有错误（--strict 时警告同样算失败）。
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths  # noqa: E402

PATTERN_FILE = os.path.join(paths.DICT, "zh_dict.pattern.txt")

# 与 zh_hook.cpp 里的 #define PAT_MAX 对应
PAT_MAX = 256
# 通配符之外至少要有这么多字，才算「够独特」。见文件头说明。
LITERAL_MIN = 6


def split_pattern(p):
    r"""把原文模式拆成 [(kind, text)]，kind ∈ {'lit','star','q'}。

    与 zh_hook.cpp 的 PatAnalyze() 同一套规则：\* \? \\ 是转义；
    其它位置的 \ 按普通字符处理（C 侧也是这样，不做报错）。
    """
    out = []
    i = 0
    while i < len(p):
        c = p[i]
        if c == "\\" and i + 1 < len(p) and p[i + 1] in "*?\\":
            out.append(("lit", p[i + 1]))
            i += 2
            continue
        if c == "*":
            out.append(("star", "*"))
        elif c == "?":
            out.append(("q", "?"))
        else:
            out.append(("lit", c))
        i += 1
    return out


def count_holes(zh):
    r"""译文里占位符的个数：* 与 \1..\9 各算一个，\\ 不算。"""
    n = 0
    i = 0
    while i < len(zh):
        c = zh[i]
        if c == "\\" and i + 1 < len(zh):
            if zh[i + 1] == "\\":
                i += 2
                continue
            if zh[i + 1] in "123456789":
                n += 1
                i += 2
                continue
        if c == "*":
            n += 1
        i += 1
    return n


def max_ref(zh):
    r"""译文里 \N 引用到的最大 N（没有则 0）。"""
    m = 0
    for x in re.findall(r"\\([1-9])", zh):
        m = max(m, int(x))
    return m


def literal_prefix(parts):
    r"""第一个通配符之前的字面前缀（C 侧用它做预筛）。"""
    s = []
    for kind, text in parts:
        if kind != "lit":
            break
        s.append(text)
    return "".join(s)


def main(argv):
    strict = "--strict" in argv

    if not os.path.exists(PATTERN_FILE):
        print("没有模式表（可选，不算错误）: %s" % PATTERN_FILE)
        return 0

    with open(PATTERN_FILE, encoding="utf-8-sig") as f:
        lines = f.read().splitlines()

    errors, warns, infos = [], [], []
    seen = {}
    entries = []

    for no, line in enumerate(lines, 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if "\t" not in line:
            errors.append("第 %d 行：没有 TAB，分不出原文与译文" % no)
            continue
        if line.count("\t") > 1:
            errors.append(
                "第 %d 行：有 %d 个 TAB。译文取第一个 TAB 之后的全部内容，"
                "行尾注释会变成译文的一部分 -> %r"
                % (no, line.count("\t"), line.split("\t", 1)[1])
            )
            continue

        pat, zh = line.split("\t", 1)
        if not pat.strip():
            errors.append("第 %d 行：原文为空" % no)
            continue
        if not zh.strip():
            errors.append("第 %d 行：译文为空（模式表里的空译文没有意义，删掉即可）" % no)
            continue

        parts = split_pattern(pat)
        caps = sum(1 for k, _ in parts if k == "star")
        lit = sum(len(t) for k, t in parts if k == "lit")
        holes = count_holes(zh)
        ref = max_ref(zh)
        pfx = literal_prefix(parts)

        if pat in seen:
            errors.append("第 %d 行：原文与第 %d 行重复 -> %r" % (no, seen[pat], pat))
        else:
            seen[pat] = no

        if caps == 0:
            warns.append("第 %d 行：原文没有任何通配符，应当放进 zh_dict.txt 走精确匹配 -> %r" % (no, pat))
        if ref > caps:
            errors.append(
                "第 %d 行：译文引用了 \\%d，但原文只有 %d 个通配符 —— 永远取不到内容 -> %r"
                % (no, ref, caps, pat)
            )
        if caps > 0 and holes < caps:
            warns.append(
                "第 %d 行：译文占位符 %d 个 < 原文通配符 %d 个；没被引用的捕获会追加到译文末尾 -> %r"
                % (no, holes, caps, pat)
            )
        if caps > 0 and lit < LITERAL_MIN:
            warns.append(
                "第 %d 行：通配符之外只剩 %d 个字（%r），模式太不独特，"
                "容易误伤别的串 -> %r" % (no, lit, "".join(t for k, t in parts if k == "lit"), pat)
            )

        entries.append((no, pat, zh, caps, holes, lit, pfx))

    if len(entries) > PAT_MAX:
        errors.append("模式共 %d 条，超过 C 侧上限 PAT_MAX=%d，多出来的会被忽略" % (len(entries), PAT_MAX))

    # 靠前的模式是否把靠后的「吃掉」：前缀相同或更短且后面还跟着通配符
    for a in range(len(entries)):
        na, pa, _, capsa, _, _, pfa = entries[a]
        if not pfa or capsa == 0:
            continue
        for b in range(a + 1, len(entries)):
            nb, pb, _, _, _, _, pfb = entries[b]
            if not pfb:
                continue
            if pfb.startswith(pfa):
                infos.append(
                    "第 %d 行（%r）的字面前缀 %r 覆盖了第 %d 行（%r）：靠前的先匹配，"
                    "后面那条可能永远轮不到" % (na, pa, pfa, nb, pb)
                )

    # ---------------------------------------------------------------- 报告
    print("模式表 : %s" % PATTERN_FILE)
    print("条目   : %d 条" % len(entries))
    print()
    if entries:
        print("%-5s %-46s %4s %4s %4s" % ("行", "原文（截断）", "通配", "占位", "字面"))
        print("-" * 72)
        for no, pat, _zh, caps, holes, lit, _pfx in entries:
            show = pat if len(pat) <= 44 else pat[:41] + "..."
            print("%-5d %-46s %4d %4d %4d" % (no, show, caps, holes, lit))
        print()

    for label, items in (("错误", errors), ("警告", warns), ("提示", infos)):
        if items:
            print("=== %s %d 条 ===" % (label, len(items)))
            for it in items:
                print("  - %s" % it)
            print()

    if errors or (strict and warns):
        print("结果：不通过（错误 %d / 警告 %d）" % (len(errors), len(warns)))
        return 1

    print("结果：通过（错误 0 / 警告 %d / 提示 %d）" % (len(warns), len(infos)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

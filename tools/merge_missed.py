# -*- coding: utf-8 -*-
"""
运行时未命中收集 · 查看与合并。

角色分工（三件事分开，各管一段）：

  zh_hook.dll          运行期记录「界面上出现过、但词典里没有」的串
                       -> _work\\hijack\\zh_dict.missed.txt   （程序自己维护的专有文件）

  merge_missed.py      本脚本：读上面那个文件，出报告、出翻译工作表，
                       然后调用 organize.py 把它并进词典的 [12] 专有区域

  organize.py          把扁平词典按类别重组成最终 zh_dict.txt（含 [12] 区）

为什么程序不直接写 zh_dict.txt：
  那个文件是启动时被加载的词典，运行期往里追加，进程被强杀就会留下半行，
  下次加载直接读到脏数据；而且它是生成物，重跑构建就被覆盖。
  所以程序只写自己的收集文件，合并这一步显式做（可回滚、可去重、可分类）。

用法：
  python merge_missed.py                 # 报告 + 工作表 + 合并进词典
  python merge_missed.py --no-organize   # 只报告，不动词典
"""

import os
import sys
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import organize

import paths                   # 所有路径集中在 tools\paths.py
WORK = paths.MISS_WORK


def main():
    misses, n_skipped = organize.load_missed()
    print("=" * 68)
    print("运行时未命中收集")
    found = [p for p in organize.MISS_FILES if os.path.isfile(p)]
    for p in organize.MISS_FILES:
        print("  %s %s" % ("[有]" if os.path.isfile(p) else "[无]", p))
    if not found:
        print("  一个都没有 —— 说明还没跑过带收集功能的 zh_hook.dll。")
        return
    print("  合并后共 %d 条唯一原文" % len(misses))
    print()

    # 已收录的（用户已经翻译过的）单列出来，避免误以为漏了
    entries = organize.load_merged()
    known = set(k for k, _, _ in entries)
    fresh = [m for m in misses if m[0] not in known]
    print("  其中已在词典里的   : %d 条" % (len(misses) - len(fresh)))
    print("  其中尚未收录(待译) : %d 条" % len(fresh))
    print()

    if not fresh:
        print("没有新的未命中项。")
        return

    by_api = collections.Counter(m[2] for m in fresh)
    src_kind = {}
    for m in fresh:
        lbl, kd = organize.miss_source(m[3], m[4], m[5])
        src_kind[lbl] = kd
    by_src = collections.Counter(organize.miss_source(m[3], m[4], m[5])[0] for m in fresh)
    by_kind = collections.Counter(organize.miss_source(m[3], m[4], m[5])[1] for m in fresh)

    print("=== 按载体 API ===")
    for k, v in by_api.most_common(12):
        print("  %-22s %4d" % (k, v))
    print()
    print("=== 按来源（控件链 + 列表列头 + 所在列）===")
    for k, v in by_src.most_common():
        print("  %-30s %-8s %4d" % (k, organize.KIND_NAME[src_kind[k]], v))
    print()
    print("=== 按类型 ===")
    for k, v in by_kind.most_common():
        print("  %-10s %4d" % (organize.KIND_NAME[k], v))
    print()
    print("=== 出现次数最多的 20 条（优先翻译这些） ===")
    for t, n, api, chain, cols, col in sorted(fresh, key=lambda m: (-m[1], m[0].lower()))[:20]:
        print("  %5d x  %-38s %-16s %s" % (n, t[:38], api, chain))
    print()

    # ---------------- 翻译工作表 ----------------
    with open(WORK, "w", encoding="utf-8", newline="\n") as f:
        f.write("原文\t出现次数\tAPI\t控件链\t列表列头\t所在列\t来源\t类型\t译文\n")
        for t, n, api, chain, cols, col in sorted(fresh, key=lambda m: (-m[1], m[0].lower())):
            lbl, kd = organize.miss_source(chain, cols, col)
            f.write("%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t\n"
                    % (t, n, api, chain or "-", cols or "-", col or "-", lbl,
                       organize.KIND_NAME[kd]))
    print("翻译工作表 -> %s" % WORK)
    print("（填好「译文」列后，把内容并进 _work\\dict\\ui_zh.py 即可长期保留；")
    print("  直接改 zh_dict.txt 也可以，但下次重跑 organize.py 会被覆盖。）")
    print("=" * 68)


if __name__ == "__main__":
    main()
    if "--no-organize" not in sys.argv:
        print()
        print(">>> 调用 organize.py 把未命中项并入词典 [12] 区")
        organize.main()

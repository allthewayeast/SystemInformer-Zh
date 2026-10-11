# -*- coding: utf-8 -*-
r"""
第 4 段的「缺口清单」：从源码里抽 UI 串，减去当前词典已有的，剩下的就是要译的。

抽取来源（只认「明确会把字符串画到界面上」的调用，精度优先）：
  menu    PhCreateEMenuItem(..., L"...", ...)   / PhPluginCreateEMenuItem(...,L"...",...)
  entry   PhAddListViewItem(..., L"...", ...)
  page    PhOptionsCreateSection(L"...", ...)
  static  PhSetWindowText(..., L"...")
  dialog  PhSetDialogItemText(..., ..., L"...")
  column  PhAddTreeNewColumn*(..., ..., ..., L"...", ...)

菜单文本里可能带 \b（加速键文本分隔符，如 L"Terminate tree\bShift+Del"）——
实测菜单画出来的是分隔符之前那半截，所以键取 \b 之前。

输出 _work\dict\ui_gap.tsv：group \t literal \t loc
"""
import os, re, collections

import paths                   # 所有路径集中在 tools\paths.py
SRC     = paths.SI_SRC
CUR     = paths.MERGED
OUT     = paths.UI_GAP_TSV

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

PATTERNS = [
    # 注意：`[^,]+` 这种写法会被「第一个参数里带逗号」的调用骗过去，例如
    #   PhSetWindowText(GetDlgItem(WindowHandle, IDC_DEFSTATE), L"...")
    # —— GetDlgItem 里的逗号会让简单正则在第一处就断掉，整条 UI 串被漏掉
    #   （"System Informer is not the default Task Manager:" 就是这么漏的）。
    # 所以第一参数一律用非贪婪 .*? 让它自己回溯到正确的那一处逗号。
    ("menu",   re.compile(r'Ph(?:Plugin)?CreateEMenuItem\s*\(.*?,\s*(?:[^,()]+,\s*)?(L"(?:[^"\\]|\\.)*")')),
    ("entry",  re.compile(r'PhAddListView(?:Item|Group\w*)\s*\(.*?,\s*(?:[^,()]+,\s*)?(L"(?:[^"\\]|\\.)*")')),
    # 列头用的是 PhAddListViewColumn(..., 宽度, L"文字")：文字是最后一个参数
    ("column2", re.compile(r'PhAddListViewColumn\w*\s*\(.*,\s*(L"(?:[^"\\]|\\.)*")\s*\)')),
    ("static", re.compile(r'PhSetWindowText\s*\(.*?,\s*(L"(?:[^"\\]|\\.)*")')),
    ("dialog", re.compile(r'PhSetDialogItemText\s*\(.*?,\s*(?:[^,()]+,\s*)?(L"(?:[^"\\]|\\.)*")')),
    ("column", re.compile(r'PhAddTreeNewColumn\w*\s*\(.*?,\s*(?:FALSE|TRUE)\s*,\s*(L"(?:[^"\\]|\\.)*")')),
]

# 选项页页签名是**跨行**的：
#   optionsEntry->CreateSection(
#       L"Disk Devices",
# 逐行扫描看不见，必须对整个文件正文匹配。
PAGE_RX = re.compile(r'(?:PhOptionsCreateSection|(?:\w+\s*->\s*)?CreateSection)\s*\([\s(]*(L"(?:[^"\\]|\\.)*")')

# 已有词典键
keys = set()
with open(CUR, "r", encoding="utf-8") as f:
    for l in f:
        l = l.rstrip("\r\n")
        if l.startswith("#") or "\t" not in l:
            continue
        keys.add(l.split("\t", 1)[0])

hit = collections.defaultdict(dict)
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
                raw = f.read()
        except Exception:
            continue
        lines = raw.split("\n")
        for ln, text in enumerate(lines, 1):
            if text.lstrip().startswith("//"):
                continue
            for group, rx in PATTERNS:
                for m in rx.finditer(text):
                    mm = LIT.match(m.group(1))
                    if not mm:
                        continue
                    s = c_unescape(mm.group(1))
                    if not s or len(s) < 2:
                        continue
                    if "\b" in s or "\t" in s:
                        s = re.split(r"[\b\t]", s)[0]
                    s = s.strip()
                    if len(s) < 2 or s in keys:
                        continue
                    if "%" in s:            # 格式化串，精确匹配拿不到
                        continue
                    hit[s].setdefault(group, set()).add("%s:%d" % (rel, ln))

        # 跨行的选项页页签名
        for m in PAGE_RX.finditer(raw):
            mm = LIT.match(m.group(1))
            if not mm:
                continue
            s = c_unescape(mm.group(1))
            if not s or len(s) < 2 or "%" in s or s in keys:
                continue
            ln = raw.count("\n", 0, m.start()) + 1
            hit[s].setdefault("page", set()).add("%s:%d" % (rel, ln))

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("group\tliteral\tloc\n")
    for s in sorted(hit, key=str.lower):
        f.write("%s\t%s\t%s\n" % (",".join(sorted(hit[s])), s,
                                  sorted(sorted(v)[0] for v in hit[s].values())[0]))

byg = collections.Counter()
for s in hit:
    for g in hit[s]:
        byg[g] += 1
print("缺口唯一串: %d" % len(hit))
for g, c in byg.most_common():
    print("  %-8s %d" % (g, c))
byfile = collections.Counter()
for s in hit:
    for g, locs in hit[s].items():
        for l in locs:
            byfile[l.split(":")[0]] += 1
print("最集中的文件:")
for f_, c in byfile.most_common(12):
    print("  %-56s %d" % (f_, c))
print("OUT = %s" % OUT)

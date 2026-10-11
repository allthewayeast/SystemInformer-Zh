# -*- coding: utf-8 -*-
"""
把 Passolo 导出的「英中对照」合并进 zh_hook.dll 用的显示层词典。

为什么不能直接拼（本脚本处理的坑）：
  1. **加速键后缀**：Passolo 的译文大量写成 "刷新(&R)"。SI 的自绘菜单用
     DT_HIDEPREFIX（plugins/ToolStatus/menubar.c:955），"(&R)" 会被原样画成
     "(R)"。必须剥掉 -> "刷新"。
  2. **同一原文多条译法**（683 条里 24 个 "Close"、23 个 "Cancel"…），剥掉加速键
     后仍有 8 处真冲突。人工整理过的条目优先，其次取出现次数多的。
  3. **Passolo 只抽资源串**，代码里内联的菜单串（mainwnd.c:3896 的 L"&View"）
     不在其中 —— 要从源码 L"&Xxx" 字面量里捞，再用裸词条反查。
  4. **C 转义**：菜单串里带加速键文本分隔符 \\b（如 L"&Copy\\bCtrl+C"）。
     实测菜单栏画出来的是分隔符之前那半截（"&View"），所以键取 \\b 之前。
  5. **上下文冲突**：同一个英文串在不同控件里意思不同，例如
     "Free" 在内存列表里是「可用」，在内存右键菜单里是「释放」。裸词表无法区分，
     用 SECTION3_OVERRIDE 按代码串纠正。
  6. Passolo 工程本身有错译（"Parent console:" -> "PEB 地址："），用 OVERRIDE 修。
  7. **假阳性**：源码里的 L"&amp;" / L"&asdf" 不是菜单串（前者是 HTML 转义、
     后者是单元测试串），用 NOT_MENU 排除。tools\\ 下的串属于 peview/安装向导，
     不在 SystemInformer.exe 里出现，也排除。

输出：LF 换行、UTF-8 无 BOM（与现有词典一致；加载器会剥尾部 \\r，CRLF 也能吃）。
"""

import os
import re
import sys
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ui_zh import UI_ZH, SKIP          # 第 4 段：代码内联 UI 串（选项页条目 / 无 & 的菜单项）

import paths                   # 所有路径集中在 tools\paths.py
PASSOLO = paths.PASSOLO
# 第 1 段 = 人工整理的基线（zh_dict.base.txt，121 条，勿动）。
# 注意：不要把它指向生成物（zh_dict.merged.txt），否则重建时会把上一轮的产物
# 当成基线，段落结构会被自我吞噬 —— 上一轮就是这么被覆盖掉的。
CUR     = paths.BASE_DICT
OUT     = paths.MERGED
SRCROOT = paths.SI_SRC

# ---------------------------------------------------------------- A) Passolo 错译修正
OVERRIDE = {
    "Parent console:":              "父控制台：",     # 原译 "PEB 地址：" —— 含义译丢
    "Plugin":                       "插件",           # 原译 "插入" —— 插件属性对话框字段名
    "Credits.":                     "鸣谢。",         # 原译 "名单。"
    "I/O deltas":                   "I/O 增量",       # 原译 "I/O 详情" —— deltas 是增量
    "Terminated due to job limits": "因作业限制而终止",  # 原译 "终止进程" —— 主语丢
    "With full memory":             "完整内存",       # 原译 "内存满"
    "With data segments":           "包含数据段",     # 原译 "使用数据细分"
    "Save":                         "保存",           # 原译 "确定(&O)" —— Save 不该变确定
    "&Start":                       "启动",           # 原译 "开始" —— srvctl.c 里是服务的「启动」按钮
    "System Informer is the default Task Manager:":
                                    "System Informer 是默认任务管理器：",  # 原译 "作为…" —— 语气译偏
    "Abandoned:":                   "已放弃：",       # 原译 "抛弃："
    "Image file name (Win32):":     "映像文件名称 (Win32)：",   # 原译掉了 (Win32)
    "<a href=\"https://systeminformer.sourceforge.io\">System Informer on SourceForge.net</a>":
        "<a href=\"https://systeminformer.sourceforge.io\">SourceForge.net 上的 System Informer</a>",
    # 原译写成 <a href="http：//processhacker.sourceforge.net">Process Hacker 在 …</a>
    # —— 域名里是全角冒号、品牌还是旧名，链接会坏。
}

# 剥掉加速键后仍不一致的真冲突，按 Windows 中文习惯裁定
CONFLICT_PICK = {
    "General":              "常规",   # 一般 / 常规
    "Add":                  "添加",
    "Name:":                "名称：",  # 姓名 / 名称
    "Description:":         "描述：",
    "Filter":               "过滤",
    "Exception dispatches": "异常分派",
    "Modified no-write":    "修改不写",
    "Changes may require a restart to take effect.": "更改需要重新启动后才能生效。",
}

# ---------------------------------------------------------------- B) 代码内菜单串
# 源码里 L"&Xxx" 捞出来、Passolo 覆盖不到的那些。译文全部对照源码上下文
# （_work\dict\ctx.py 打印的出处）确定，不是望文生义：
#   &Real time/&High/&Above normal/&Normal/&Below normal/&Idle  mainwnd.c:5023-5028
#                                                            = 进程优先级(6 档)
#   &Highest/&Lowest/&Critical                                 prpgthrd.c:540/544 等 = 线程/杂项
#   &Very low/&Low/&Medium/&High                               I/O 与内存页优先级
#   &Fast(&Normal(&Below normal(&Slow(&Very slow                mainwnd.c:3755-3762
#                                                            = 视图▸刷新速度，单位秒
MANUAL_MENU = {
    # --- 进程优先级 / I/O / 内存页 / 线程优先级 ---
    "&Real time":           "实时",
    "&High":                "高",
    "&Above normal":        "高于正常",
    "&Below normal":        "低于正常",
    "&Idle":                "空闲",
    "&Low":                 "低",
    "&Very low":            "很低",
    "&Medium":              "中等",
    "&Highest":             "最高",
    "&Lowest":              "最低",
    "&Critical":            "关键",
    # --- 视图▸刷新速度 ---
    "&Fast (0.5s)":         "快 (0.5 秒)",
    "&Normal (1s)":         "正常 (1 秒)",
    "&Below normal (2s)":   "低于正常 (2 秒)",
    "&Slow (5s)":           "慢 (5 秒)",
    "&Very slow (10s)":     "很慢 (10 秒)",
    # --- 进程/线程/模块/内存右键菜单 ---
    "&Expand all":          "全部展开",
    "&Collapse all":        "全部折叠",
    "&Freeze":              "冻结",
    "&Thaw":                "解冻",
    "&Suspend":             "挂起",
    "&Inherit":             "继承",
    "&Protected":           "受保护",
    "&Security":            "安全性",
    "&Reset sort":          "重置排序",
    "&WS watch":            "工作集监视",
    "&Unloaded modules":    "已卸载的模块",
    "&Zombie processes":    "僵尸进程",
    "&Read/Write memory":   "读写内存",
    "&Read/Write memory...": "读写内存...",
    "&Empty working set...": "清空工作集...",
    "&Combine memory pages": "合并内存页",
    "&Decommit":            "取消提交",
    "&Free":                "释放",
    "&Registers...":        "寄存器...",
    "&Search thread stacks": "搜索线程堆栈",
    "&Miscellaneous":       "杂项",
    "&Run as...":           "运行方式...",
    "&Run...":              "运行...",
    "&Restore":             "还原",
    "&Window":              "窗口",
    "&Priority class":      "优先级类别",
    "&Detach from debugger": "分离调试器",
    # --- 转储 ---
    "&Full...":             "完整...",
    "&Limited...":          "受限...",
    "&Minimal...":          "最小...",
    "&Custom...":           "自定义...",
    "&Create live dump...": "创建实时转储...",
    # --- 服务 / 用户 / 设备 ---
    "&Connect":             "连接",
    "&Disconnect":          "断开",
    "&Logoff":              "注销",
    "&Start":               "启动",
    "&Continue":            "继续",
    "&Create service...":   "创建服务...",
    "&Hide driver services": "隐藏驱动程序服务",
    "&Modified services":   "已修改的服务",
    "&Deleted services":    "已删除的服务",
    "&Arrived devices":     "已到达的设备",
    "&Removed devices":     "已移除的设备",
    "&Hide waiting connections": "隐藏等待中的连接",
    "&Hide processes from other users": "对其他用户隐藏进程",
    "&Enable all":          "启用所有",
    "&Disable all":         "禁用所有",
    # --- 工具 / 查看 / 窗口 ---
    "&Environment variables": "环境变量",
    "&Pagefiles":           "页面文件",
    "&Process monitor":     "进程监视器",
    "&Find handles or DLLs...": "查找句柄或 DLL...",
    "&Options...":          "选项...",
    "&Save...":             "保存...",
    "&Open file location":  "打开文件位置",
    "&Go to process":       "转到进程",
    "&Go to process...":    "转到进程...",
    "&Go to processes":     "转到进程",
    "&Go to service":       "转到服务",
    "&Go to thread...":     "转到线程...",
    "&Go to device driver": "转到设备驱动程序",
    "&Go to lower device driver": "转到下层设备驱动程序",
    "&Tray icons":          "托盘图标",
    "&Show/Hide System Informer": "显示/隐藏 System Informer",
    "&Always on top":       "总在最前",
    # --- 插件菜单 ---
    "&System":              "系统",
    "&Processes":           "进程",
    "&Handles":             "句柄",
    "&Object Manager":      "对象管理器",
    "&Trusted Platform Module": "受信任的平台模块",
    "&Named Pipes":         "命名管道",
    "&Network Tools":       "网络工具",
    "&Online Checks":       "在线检查",
    "&User Notes":          "用户备注",
    "&Ping":                "Ping",
    "&Ping address...":     "Ping 地址...",
    "&Traceroute":          "路由追踪",
    "&Traceroute address...": "路由追踪地址...",
    "&Whois":               "Whois",
    "&Whois address...":    "Whois 地址...",
    "&GeoLite database update...": "GeoLite 数据库更新...",
    "&Windows":             "窗口",
    "&Opacity":             "不透明度",
    "&Opaque":              "不透明",
    "&Save...2":            "",   # 占位，勿删（保持表有序，不会命中）
    # --- 通知 / 托盘菜单 ---
    "&I/O history":         "I/O 历史",
    "&Network history":     "网络历史",
    "&Disk history":        "磁盘历史",
    "&GPU history":         "GPU 历史",
    "&NPU history":         "NPU 历史",
    "&GPU memory history":  "GPU 显存历史",
    "&GPU temperature history": "GPU 温度历史",
    "&Commit charge history": "提交用量历史",
    "&Physical memory history": "物理内存历史",
    "&Disk usage (text)":   "磁盘使用 (文本)",
    "&Network usage (text)": "网络使用 (文本)",
    "&GPU usage (text)":    "GPU 使用 (文本)",
    "&GPU memory usage (text)": "GPU 显存使用 (文本)",
    "&GPU temperature (text)": "GPU 温度 (文本)",
    "&Expand all2":         "",   # 占位
    # --- 补齐：Passolo 未覆盖的最后 11 条（出处见 ctx.py 输出） ---
    "&ACPI Tables":         "ACPI 表",          # ExtendedTools\main.c:313
    "&Boost":               "提升",             # UserNotes\main.c:2203
    "&Disable":             "禁用",             # tokprp.c:2721 权限
    "&Enable":              "启用",             # tokprp.c:2720 权限
    "&Efficiency":          "效率模式",         # UserNotes\main.c:2236
    "&Hibernate":           "休眠",             # ToolStatus\main.c:1194
    "&Lock":                "锁定",             # ToolStatus\main.c:1190
    "&Sleep":               "睡眠",             # ToolStatus\main.c:1193
    "&Unload":              "卸载",             # prpgmod.c:129
    "&Normal...":           "正常...",          # mwpgproc.c:989 进程转储
    "&Open link":           "打开链接",         # objmgr.c:3348
}
MANUAL_MENU = {k: v for k, v in MANUAL_MENU.items() if v}

# 上下文冲突：Passolo 的裸词条在这里意思不对，按代码串改写
SECTION3_OVERRIDE = {
    "&Free": "释放",      # Passolo "Free" 是内存列表的「可用」，右键菜单里是动作
}

# 不是菜单串的假阳性（HTML 转义、单元测试串、站点品牌名）
NOT_MENU = {
    "&amp;", "&apos;", "&gt;", "&lt;", "&quot;", "&asdf",
    "&filescan.io", "&virustotal.com", "&hybrid-analysis.com",
    "&Donate",           # mainwnd.c:3854 已被注释掉，程序里不会出现
}

ACCEL = re.compile(r"\(&.\)")
LIT   = re.compile(r'L"((?:[^"\\\n]|\\.){1,90})"')

def strip_accel(s):
    return ACCEL.sub("", s).replace("  ", " ").strip()

def read_text(p):
    with open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()

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

# ---------------------------------------------------------------- 1) 现有词典
cur_lines = read_text(CUR).split("\n")
cur, dup_cur = {}, []
for l in cur_lines:
    l = l.rstrip("\r")
    if l.startswith("#") or "\t" not in l:
        continue
    k, v = l.split("\t", 1)
    if k in cur and cur[k] != v:
        dup_cur.append((k, cur[k], v))
    cur.setdefault(k, v)

# ---------------------------------------------------------------- 2) Passolo
counts, n_pas = collections.defaultdict(collections.Counter), 0
for l in read_text(PASSOLO).split("\n"):
    l = l.rstrip("\r")
    if not l.strip() or "\t" not in l:
        continue
    k, v = l.split("\t", 1)
    v = strip_accel(v)
    if not k or not v:
        continue
    n_pas += 1
    counts[k][v] += 1

pas = {}
for k, cnt in counts.items():
    if k in OVERRIDE:            pas[k] = OVERRIDE[k]
    elif len(cnt) == 1:          pas[k] = cnt.most_common(1)[0][0]
    elif k in CONFLICT_PICK:     pas[k] = CONFLICT_PICK[k]
    else:                        pas[k] = cnt.most_common(1)[0][0]

# ---------------------------------------------------------------- 3) 源码 & 菜单串
harvested, unmapped = set(), {}
if os.path.isdir(SRCROOT):
    for root, dirs, files in os.walk(SRCROOT):
        dirs[:] = [d for d in dirs if d not in (".git", ".vs", "build")]
        for fn in files:
            if not fn.endswith((".c", ".cpp", ".h", ".rc")):
                continue
            p = os.path.join(root, fn)
            rel = os.path.relpath(p, SRCROOT)
            if rel.startswith("tools\\") or "\\tools\\" in rel:
                continue          # peview / 安装向导，不在 SystemInformer.exe 里出现
            try:
                txt = read_text(p)
            except Exception:
                continue
            for m in LIT.finditer(txt):
                lit = c_unescape(m.group(1))
                if not lit or not re.match(r"^&[A-Za-z0-9]", lit):
                    continue
                for sep in ("\b", "\t"):
                    if sep in lit:
                        lit = lit.split(sep)[0]
                        break
                lit = lit.strip()
                if len(lit) < 3 or lit in harvested or lit in NOT_MENU:
                    continue
                if "%" in lit or "&#" in lit:
                    continue
                harvested.add(lit)
                unmapped[lit] = (MANUAL_MENU.get(lit) or pas.get(lit[1:]) or cur.get(lit[1:]))

# ---------------------------------------------------------------- 4) 组装
banner = """# System Informer 中文词典
# 格式: 原文<TAB>译文      以 # 开头为注释
#
# 编码必须是 UTF-8（无 BOM），加载器用 MultiByteToWideChar(CP_UTF8)。
# 只在「整串精确相等」时替换，所以短词误伤面有限。
#
# 本文件由 _work\\dict\\build_zh_dict.py 生成：
#   第 1 段 = 人工整理、已实测生效的条目（原样保留，优先级最高）
#   第 2 段 = Passolo 导出的资源串（已剥掉 (&X) 加速键后缀）
#   第 3 段 = 源码 L"&Xxx" 里捞出的代码内菜单串（译文对照源码上下文确定）
# 冲突裁定：人工条目 > OVERRIDE 修正表 > 出现次数多者；语义冲突见 CONFLICT_PICK。
"""

sec2 = ["%s\t%s" % (k, pas[k]) for k in sorted(pas, key=str.lower) if k not in cur]
sec3 = ["%s\t%s" % (k, v) for k, v in sorted(unmapped.items()) if v and k not in cur and k not in pas]

# ---- 第 4 段：代码内联 UI 串（选项页条目 / 无 & 的菜单项 / 插件页字段） ----
# 键由 ui_gap.py 机器抽取，这里只负责接上人工译文，并做双向核对。
GAP_TSV = paths.UI_GAP_TSV
gap_keys = set()
if os.path.isfile(GAP_TSV):
    for l in read_text(GAP_TSV).split("\n")[1:]:
        l = l.rstrip("\r")
        if "\t" in l:
            gap_keys.add(l.split("\t", 2)[1])      # 列序：group \t literal \t loc

emitted_src = set(cur)                                        # 第 1 段
emitted_src |= set(k.split("\t", 1)[0] for k in sec2)         # 第 2 段
emitted_src |= set(k.split("\t", 1)[0] for k in sec3)         # 第 3 段
sec4 = ["%s\t%s" % (k, UI_ZH[k]) for k in sorted(UI_ZH, key=str.lower)
        if UI_ZH[k] and k not in emitted_src]
sec4_keys = set(k.split("\t", 1)[0] for k in sec4)

body = [banner]
body.append("# ============================================================")
body.append("# 第 1 段：人工整理（已实测生效，勿动）")
body.append("# ============================================================")
seen1 = set()
for l in cur_lines:
    l = l.rstrip("\r")
    if l.startswith("#") or "\t" not in l:
        body.append(l); continue
    k = l.split("\t", 1)[0]
    if k in seen1:
        continue
    seen1.add(k); body.append(l)
body.append("")
body.append("# ============================================================")
body.append("# 第 2 段：Passolo 资源串（去重后 %d 条，新增 %d）" % (len(pas), len(sec2)))
body.append("# ============================================================")
body.extend(sec2)
body.append("")
body.append("# ============================================================")
body.append("# 第 3 段：源码内联菜单串（& 形式，新增 %d）" % len(sec3))
body.append("# ============================================================")
body.extend(sec3)
body.append("")
body.append("# ============================================================")
body.append("# 第 4 段：代码内联 UI 串（选项页条目 / 无 & 的菜单项 / 插件页字段，新增 %d）" % len(sec4))
body.append("# 键由 _work\\dict\\ui_gap.py 从源码抽取，译文见 _work\\dict\\ui_zh.py")
body.append("# ============================================================")
body.extend(sec4)
body.append("")

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(body))

# ---------------------------------------------------------------- 5) 报告
keys = [l.split("\t", 1)[0] for l in body if not l.startswith("#") and "\t" in l]
dupes = [k for k, c in collections.Counter(keys).items() if c > 1]
print("Passolo 原始条目        : %d" % n_pas)
print("Passolo 唯一原文        : %d" % len(pas))
print("  被人工条目压制        : %d" % len([k for k in pas if k in cur]))
print("  计入第 2 段           : %d" % len(sec2))
print("OVERRIDE 生效           : %d" % len([k for k in OVERRIDE if k in pas]))
print("源码 L\"&Xxx\" 去重后     : %d" % len(harvested))
print("  映射成功              : %d" % len([1 for v in unmapped.values() if v]))
print("  计入第 3 段           : %d" % len(sec3))
print("")
print("第 4 段（代码内联 UI 串）:")
print("  源码缺口清单          : %d" % len(gap_keys))
print("  译文表 UI_ZH          : %d" % len(UI_ZH))
print("  计入第 4 段           : %d" % len(sec4))
print("  因前 3 段已覆盖而跳过  : %d %s" %
      (len([k for k in UI_ZH if k in emitted_src and k not in cur]),
       sorted(k for k in UI_ZH if k in emitted_src and k not in cur)[:5]))
bad = sorted(k for k in UI_ZH if k not in gap_keys)
print("  ！译文表有、源码没有的键: %d %s" % (len(bad), bad[:12]))
ov = sorted(set(UI_ZH) & set(SKIP))
print("  ！UI_ZH 与 SKIP 重叠    : %d %s" % (len(ov), ov))
left = sorted(k for k in gap_keys if k not in emitted_src and k not in UI_ZH)
print("  仍未覆盖的缺口        : %d %s" % (len(left), left[:12]))
print("  刻意不译（SKIP）       : %d" % len(SKIP))
print("")
print("最终总条目              : %d" % len(keys))
print("重复键                  : %d %s" % (len(dupes), dupes[:5]))
print("原词典内冲突重复        : %s" % (dup_cur or "无"))
print("")
rest = sorted(k for k, v in unmapped.items() if not v)
print("仍未译的代码菜单串 %d 条：%s" % (len(rest), "  ".join(rest) if rest else "无"))
print("MANUAL_MENU 里源码不存在的：%s" %
      ("  ".join(sorted(k for k in MANUAL_MENU if k not in harvested)) or "无"))
print("OUT = %s" % OUT)

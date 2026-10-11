# -*- coding: utf-8 -*-
"""
把扁平的 zh_dict.merged.txt 重组成「按类别分区」的 zh_dict.txt。

分类不是猜的，三个证据源合起来用，优先级从高到低：

  E1  .rc 资源块（rc_literals.json）
      对话框 / 版本 这两类是**真资源**，有明确块边界，命中即归类。
      实测：System Informer 只有 DIALOG(274 块) 与 VERSIONINFO(18 块)，
      **没有 STRINGTABLE、没有 MESSAGE TABLE、没有 MENU 资源** ——
      它的菜单是 menubar.c 里代码搭的，字符串是 C 字面量。
      所以「字符串表 / 消息表」在 SI 上不存在对应资源，只能用代码侧等价类别代替。

  E2  ui_gap.tsv 的 group/loc（源码抽取时记下的控件上下文）
      column / entry / page / menu / dialog / static

  E3  源码全文扫描：这个串在哪些 .c/.cpp/.h/.rc 里出现过
      用于 E1/E2 都没覆盖的条目。文件路径按规则表映射成类别。

输出：
  zh_dict.categorized.txt   给人看 / 部署用（分区 + 统计）
  zh_dict.index.tsv         原文 \t 类别 \t 证据 \t 出处（审计用，不进部署）
  _work\\hijack\\zh_dict.txt   部署副本（build_launcher.ps1 就是从这里拷的）

注意：条目行**不能**加行尾注释。加载器按「第一个 TAB」切分，
第二个 TAB 会被当成译文的一部分（译文里混进一个制表符和注释）。
所以出处信息只进 index.tsv，不进 txt。
"""

import os
import re
import json
import collections

import paths                   # 所有路径集中在 tools\paths.py

MERGED  = paths.MERGED
RCJSON  = paths.RC_LITERALS
GAPTSV  = paths.UI_GAP_TSV
OUT_TXT = paths.CATEGORIZED
OUT_TSV = paths.INDEX_TSV
DEPLOY  = paths.ZH_DICT
SRCROOT = paths.SI_SRC

CATS = [
    ("01", "对话框 (DIALOG)",              "源码 .rc 的 DIALOGEX 块 + 控件文本 API"),
    ("02", "版本信息 (VERSIONINFO)",        "源码 version.rc 的 VERSIONINFO 块"),
    ("03", "菜单 (MENU)",                   "menubar.c/mainwnd.c 的菜单串（含 & 加速键）"),
    ("04", "工具栏与按钮 (TOOLBAR)",        "plugins\\ToolStatus\\toolbar.c 等"),
    ("05", "列表与树 · 列头 (COLUMN)",      "PhAddListViewColumn / PhAddTreeNewColumn 的列标题"),
    ("06", "选项页 (SETTINGS)",             "options.c 的设置项与页签"),
    ("07", "属性页字段 (PROPERTY)",         "*prp.c / 进程·线程·内存属性页"),
    ("08", "插件 (PLUGIN)",                 "plugins\\*（未被上面更具体的 API 判据收走的）"),
    ("09", "提示与消息 (MESSAGE)",          "PhShowMessage / PhShowError / PhShowWarning"),
    ("10", "其他代码内字符串 (CODE)",       "有源码出处，但不在上述任何控件上下文里"),
    ("11", "外部数据与系统名称 (EXTERNAL)",  "源码里搜不到：Windows 进程名/描述、外部数据、人工补的条目"),
    ("12", "运行时未命中（自动收集·待翻译）", "zh_hook.dll 运行期收集，见 zh_dict.missed.txt"),
    ("13", "数据行内容（按来源·勿译）",      "运行期指纹判定为列表里的对象名，保留取证用"),
]

# 未命中收集文件（由 zh_hook.dll 维护，见 _work\\hijack\\zh_hook.cpp）。
# 它落在「词典所在目录」，而词典在构建期和部署期不是同一个目录，所以把这些
# 位置都扫一遍并按原文合并计数 —— 比硬编码一个路径可靠。
MISS_FILES = paths.miss_files()
MISSED = MISS_FILES[0]      # 头部注释里展示用


# 运行期收集到的串里，已知属于「数据」而不是界面文字的那些。
#
# 为什么需要它：miss_is_ui_text 只能滤掉「长得像数值」的串，而进程描述
# （"TortoiseGit status cache"）、单位符号（"EB"）不含数字、字母也够多，
# 判据上无法与真正的界面文字区分 —— 只能靠指名。
# 列在这里就不会进 [12] 待翻译区；原文仍留在 zh_dict.missed.txt 里，证据不丢。
MISS_IGNORE = {
    # 进程列表里的「描述」列内容，是各程序的版本信息，不是本程序的界面文字
    "ShellHost",
    "Device Association Framework Provider Host",
    "Internet Download Manager (IDM)",
    "Notepad4 Text Editor",
    "TortoiseGit status cache",
    "EB",                                    # 磁盘容量单位（EB = 艾字节）
    # 运行时拼出来的串（形如 Copy "%s"）。精确匹配的词典机制拿不到，
    # 要前缀匹配 —— 属钩子侧改造，先不让它占着待翻译区。
    'Copy "Name"',
    # 本机服务显示名，但它的 DisplayName 是资源间接引用、LoadString 没取到
    # （Windows 的 AppMgmt 服务）。这类靠「查本机」漏掉的，只能指名。
    "Application Management",
}

# 运行期「拼接出来的」串，按模式丢弃 —— 判据与理由见 miss_is_runtime_concat()。
# 用 search 而不是 match：这些片段经常缀在别的串后面（tooltip 就是一路拼下来的）。
MISS_DROP_PATTERNS = [
    re.compile(r"Console application: "),   # tooltip：L"Console application: %s (%lu)"
    re.compile(r"Signer: "),                # tooltip：L"Signer: %s"
    re.compile(r"Notes: "),                 # tooltip：L"Notes: %s"
    re.compile(r"Flags: "),                 # tooltip：L"Flags: %s"
    re.compile(r"Process is "),             # tooltip：L"Process is %s"
    re.compile(r'^Copy "'),                 # 列表列菜单：L"Copy \"%s\""
    re.compile(r"^Ser&vice \("),            # ExtendedServices：L"Ser&vice (%s)"
]


_RES_DLL = {}


def _resolve_resource_string(s):
    r"""'@%SystemRoot%\system32\Audiosrv.dll,-200' -> 'Windows Audio'（取不到返回 ""）.

    LOAD_LIBRARY_AS_DATAFILE(0x2) 只把文件当资源映射进来，**不执行任何代码**，
    所以对系统目录里那些 DLL 做这一步是安全的（实测本机 611 个资源型显示名，
    成功解析 365 个）。
    """
    if not s.startswith("@") or "," not in s:
        return ""
    path, _, sid = s[1:].rpartition(",")
    path = os.path.expandvars(path.strip())
    if not os.path.isabs(path):
        path = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), path)
    try:
        rid = abs(int(sid.strip()))
    except ValueError:
        return ""

    global _RES_DLL
    import ctypes
    if not _RES_DLL:
        k32 = ctypes.WinDLL("kernel32")
        u32 = ctypes.WinDLL("user32")
        k32.LoadLibraryExW.restype = ctypes.c_void_p
        u32.LoadStringW.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                    ctypes.c_wchar_p, ctypes.c_int]
        _RES_DLL = {"k32": k32, "u32": u32}

    k32, u32 = _RES_DLL["k32"], _RES_DLL["u32"]
    try:
        h = k32.LoadLibraryExW(path, None, 0x2)
        if not h:
            return ""
        buf = ctypes.create_unicode_buffer(1024)
        n = u32.LoadStringW(ctypes.c_void_p(h), rid & 0xFFFF, buf, 1024)
        k32.FreeLibrary(ctypes.c_void_p(h))
        return buf.value.strip() if n else ""
    except Exception:
        return ""


def _add_display_name(names, d):
    """把服务的 DisplayName 收进「系统数据名」集合。

    DisplayName 有三种形态（实测本机 876 个服务）：
      1. 直接字面量            "Windows Audio"
      2. INF 本地化引用        "@acpi.inf,%ACPI.SvcDesc%;Microsoft ACPI Driver"
         —— **最后一段分号之后**才是真正显示的文本，前半截是 INF 里的占位；
      3. 资源间接引用          "@%SystemRoot%\\system32\\Audiosrv.dll,-200"
         —— 要 LoadString 才能取到，这里不解析（进程外加载 DLL 代价与风险都不划算），
            这类名字极少出现在收集文件里（实测 0 条），留作已知缺口。
    """
    if not isinstance(d, str):
        return
    s = d.strip()
    if not s:
        return
    names.add(s.lower())
    if s.startswith("@"):
        if ";" in s:
            tail = s.rsplit(";", 1)[1].strip()
            if tail:
                names.add(tail.lower())
        else:
            r = _resolve_resource_string(s)
            if r:
                names.add(r.lower())


def system_data_names():
    """本机真实存在的服务 / 驱动 / 进程名 —— 这些是**数据**，不是界面文字。

    为什么用「查本机」而不是猜词形：
      「Auto start (trigger)」和「AppvStrm」在词形上无法区分（都是首字母大写的词），
      但后者确实是本机注册表里的服务项，前者是界面的启动类型枚举值。
      查注册表 + 进程表是确定性的，不靠猜。
    查不到的（设备页那种「1394 OHCI Compliant Host Controller」）仍会留在 [12]，
    那类属于「机器描述数据」，只能靠 MISS_IGNORE 指名。
    """
    names = set()
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Services")
        i = 0
        while True:
            try:
                sub = winreg.EnumKey(k, i)
            except OSError:
                break
            i += 1
            names.add(sub.strip().lower())
            # 服务显示名也要收：服务列表「显示名称/描述」列显示的就是它，
            # 由 Windows 提供、不是本程序的界面文字。
            try:
                sk = winreg.OpenKey(k, sub)
                try:
                    d, _ = winreg.QueryValueEx(sk, "DisplayName")
                    _add_display_name(names, d)
                except OSError:
                    pass
                winreg.CloseKey(sk)
            except OSError:
                pass
        winreg.CloseKey(k)
    except Exception:
        pass

    # 注册表只覆盖服务/驱动，进程列表「描述」列的内容得从进程表拿
    try:
        import subprocess
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True, errors="replace").stdout
        for line in out.splitlines():
            if not line.startswith('"'):
                continue
            exe = line.split('","')[0].strip('"').strip().lower()
            if exe:
                names.add(exe)
                if exe.endswith(".exe"):
                    names.add(exe[:-4])
    except Exception:
        pass
    return names


def load_missed():
    """读所有已知位置的未命中收集文件。

    返回 (rows, skipped)，rows = [(原文, 次数, API, 控件链, 列表列头, 所在列)]（按原文合并）。
    字段由 zh_hook.dll 写：原文 / 次数 / API / 控件链 / 列表列头 / 所在列。

    容错：
      * 旧版本只写 4 列，而且 API 恒为 "-"、第 4 列恒为 "?"（读回时没解析的 bug）——
        这种行指纹缺失，归到「来源未知」。
      * 早期版本没净化控制字符，多行文字会把一行拆成几行；这些行字段不全，
        计入 skipped，不静默丢弃。
    """
    merged = {}
    skipped = 0
    for path in MISS_FILES:
        if not os.path.isfile(path):
            continue
        for l in read_text(path).split("\n"):
            l = l.rstrip("\r")
            if not l or l.startswith("#"):
                continue
            p = l.split("\t")
            if len(p) < 3 or not p[0]:
                skipped += 1
                continue
            text = p[0]
            api = p[2] if p[2] != "-" else ""
            chain = p[3] if len(p) > 3 and p[3] not in ("", "-", "?") else ""
            cols = p[4] if len(p) > 4 and p[4] not in ("", "-") else ""
            col = p[5] if len(p) > 5 and p[5] != "-" else ""
            try:
                n = int(p[1])
            except ValueError:
                n = 1
            if text in merged:
                old = merged[text]
                merged[text] = (text, old[1] + n, old[2] or api,
                                old[3] or chain, old[4] or cols, old[5] or col)
            else:
                merged[text] = (text, n, api, chain, cols, col)
    return list(merged.values()), skipped


FILE_EXT = {"exe", "dll", "sys", "ocx", "cpl", "msc", "bat", "cmd",
            "com", "scr", "drv", "mui", "log", "tmp", "ini", "dat"}
DECIMAL_RE = re.compile(r"\d\.\d")


def miss_is_ui_text(s):
    """与 zh_hook.dll 里 MissLooksTranslatable 同构的**合并侧**过滤。

    运行期已经滤过一遍，这里是第二道 —— 早期版本收集的噪声（"95.17"、"smss.exe"、
    "NT AUTHORITY\\SYSTEM"）还留在文件里，而且用户也可能手工往文件里塞东西。
    判据保持一致，改的时候两边都要改。
    """
    if len(s) < 2:
        return False
    letters = sum(1 for c in s if c.isascii() and c.isalpha())
    digits = sum(1 for c in s if c.isdigit())
    cjk = sum(1 for c in s if ord(c) >= 0x2E80)
    punct = sum(1 for c in s if (not c.isalnum()) and c != " " and ord(c) < 0x2E80)

    if cjk or not letters or "\\" in s:
        return False
    if " " not in s and "." in s and s.rsplit(".", 1)[-1].lower() in FILE_EXT:
        return False                                  # smss.exe
    if DECIMAL_RE.search(s):
        return False                                  # 30.29 GB
    if not digits:
        return True
    runs = re.findall(r"[A-Za-z]+", s)
    if runs and max(len(r) for r in runs) <= 3 and len(runs) <= 2:
        return False
    if (digits + punct) * 100 > len(s) * 45:
        return False
    return True


KIND_NAME = {"ui": "界面文字", "data": "数据行", "mixed": "混合", "unknown": "来源未知"}
KIND_ORDER = {"ui": 0, "mixed": 1, "data": 2, "unknown": 3}   # 报告里的排序：该翻译的排前面


def _colname(co, ci):
    """列头签名里第 ci 列的列名。"""
    parts = [p for p in co.split("|")]
    if 0 <= ci < len(parts):
        return parts[ci]
    return "(列未知)" if ci < 0 else ("第 %d 列" % ci)


def _colidx(col):
    try:
        return int(col)
    except (TypeError, ValueError):
        return -1


# ---------------------------------------------------------------------------
# 来源判定：靠运行期采集的**指纹**（控件链 + 列表列头 + 所在列）归类。
#
# 为什么列头是关键：进程列表 / 服务列表 / 设备列表的控件类全是 PhTreeNew、
# 控件 ID 全是 0、父链也一模一样 —— 唯一的区别就是列头。实测（probe_cols.py）：
#   * 进程列表   PID|CPU|I/O 总速率|私有字节|用户名|描述
#   * 服务列表   名称|PID|显示名称|类型|状态|启动类型
#   * 设备列表   制造商|Service|类别|Enumerator|Installed
#   * 网络连接   名称|PID|本地地址|本地端口|远程地址|远程端口|协议|状态|所有者
#   * 防火墙     名称|PID|Action|Direction|Rule|描述|筛选源|本地地址|…
#   * 磁盘       名称|PID|文件|Read rate average|…|读取字节
#
# 「所在列」决定同一个列表里的这一格是数据还是界面文字：
#   服务列表的名称/显示名称列 = 服务名（数据）
#   网络连接的状态列 = SI 自己的枚举文本（SYN sent / 侦听…，要翻译）
#
# kind：ui = 该翻译；data = 列表里的对象名，通常不翻；unknown = 判不出。
# ---------------------------------------------------------------------------
def miss_source(chain, cols, col):
    """指纹 -> (来源标签, kind)。判不出就给机械描述，不硬塞进某个来源。"""
    head = chain.split("<")[0] if chain else ""
    cls = head.split("#")[0] if head else ""
    co = cols or ""
    ci = _colidx(col)
    cn = _colname(co, ci)

    # 画在列头控件上的就是列标题本身，永远是界面文字
    if cls == "SysHeader32":
        return "列标题（表头控件）", "ui"

    if not co:
        if not chain:
            # 旧版钩子只记了「窗口类」，而且读回时没解析 —— 这批记录没有任何指纹。
            # 新钩子会在**再次看到同一条**时自动把指纹补上（MissFillProbe 按
            # 「更长者胜」合并），所以这个分组会随着使用逐步缩小、直到消失。
            return "旧数据 · 无指纹（待重采）", "unknown"
        return "%s（列头读不到）" % cls, "unknown"

    # ---- 进程列表：PID 列 + 用户名/描述/私有字节 ----
    if "PID" in co and any(k in co for k in
                           ("User name", "用户名", "Private bytes", "私有字节",
                            "Description", "描述")):
        return "进程列表 · %s" % cn, "data"

    # ---- 服务列表 ----
    if any(k in co for k in ("Display name", "显示名称", "Start type", "启动类型")):
        return "服务列表 · %s" % cn, "data"

    # ---- 设备列表 ----
    if any(k in co for k in ("Enumerator", "Installed", "制造商", "Manufacturer")):
        return "设备列表 · %s" % cn, "data"

    # ---- 网络连接列表：只有状态列是界面枚举文本 ----
    if any(k in co for k in ("Local port", "本地端口", "Remote address", "远程地址")):
        if cn in ("状态", "State"):
            return "网络连接列表 · 状态列", "ui"
        return "网络连接列表 · %s" % cn, "data"

    # ---- 防火墙列表 ----
    if any(k in co for k in ("Action", "Direction", "Rule", "筛选源")):
        return "防火墙列表 · %s" % cn, "data"

    # ---- 磁盘列表 ----
    if any(k in co for k in ("Read rate", "Write rate", "Total rate", "读取字节")):
        return "磁盘列表 · %s" % cn, "data"

    return "列表（列头: %s）" % co[:36], "unknown"


def miss_is_runtime_concat(s):
    r"""判断一个未命中串是不是「运行期拼出来的产物」。

    为什么必须滤掉：这类串在源码里是**片段**，由 PhaFormatString / wcscat 拼成整句
    再交给绘制函数。收集器只看得见拼完的结果，里面夹着 PID、进程名、签名者这些
    运行时数据，所以每次运行的内容都不一样 —— 词典的精确匹配永远命中不了，
    留在 [12] 里只会把「真正待翻译的界面文字」淹没。

    实测样本（进程列表的 tooltip）：
        Console application: svchost.exe (2180) Flags:     Process is UIAccess.
        Microsoft Corporation Notes:  Signer: Microsoft Windows  Console application: ...
    对应源码里的片段是 L"Console application: %s (%lu)"、L"Signer: %s" 等。

    同理还有两处带 %s 的菜单项：
        L"Copy \"%s\""        —— 列名由运行时给出
        L"Ser&vice (%s)"      —— ExtendedServices 插件，%s 是进程名
    """
    return any(p.search(s) for p in MISS_DROP_PATTERNS)


def guess_cat(api, cls):
    """按「窗口类 + 载体 API」给未命中项猜类别。猜错也无妨 —— 它在 [12] 区，
    译好后应该手工挪到对应分区（organize.py 下次重跑时按原文归位）。"""
    c = (cls or "").lower()
    a = api or ""
    if c == "#32768":                       # 系统菜单窗口类
        return "03"
    if "toolbar" in c or a.upper().startswith("TB_"):
        return "04"
    if ("listview" in c or "treeview" in c or "treenew" in c or "list" in c
            or a.startswith("CM_LVM") or a.startswith("CM_HDM")):
        return "05"
    if c == "#32770":                       # 标准对话框
        return "01"
    if any(k in c for k in ("button", "static", "edit", "combobox", "syslink")):
        return "01"
    if a in ("WM_SETTEXT", "CM_SB_SETTEXTW"):
        return "01"
    return "10"

# API 调用上下文 -> 类别。用「括号配平的调用体」而不是行正则：
# 列标题这类串常常跨行、且是第 7 个参数，逐行正则会漏。
# 顺序即优先级（列头放在最前，因为它最容易被别的规则抢走）。
API_RULES = [
    ("05", r"PhAddListViewColumn|PhAddTreeNewColumn|PhAddListViewGroup|PhSetListViewSubItem"
           r"|PhAddTreeNewColumnEx"),
    ("09", r"PhShowMessage|PhShowMessage2|PhShowError|PhShowWarning|PhShowInformation"
           r"|PhShowConfirm|PhShowStatus"),
    ("01", r"PhSetWindowText|PhSetControlText|SetDlgItemTextW|Static_SetText|Button_SetText"
           r"|PhCreateDialog|PhpCreateDialog|PhCreateDialogParam"),
    ("06", r"PhTabPage|CreateSection|PhAddSetting|PhpAddSetting"),
]
CAT_NAME = {c: n for c, n, _ in CATS}
CAT_WHY  = {c: w for c, _, w in CATS}

# 文件路径 -> 类别。自上而下第一个命中者胜。plugins 放最后：
# 插件目录里的串很多与主程序重名（"Name"/"Value"），主程序侧的证据更具体。
FILE_RULES = [
    (r"menubar\.c$|mainwnd\.c$",                      "03"),
    (r"toolbar\.c$|toolstatus",                       "04"),
    (r"columndlg\.c$|columns\.c$|columndlg",          "05"),
    (r"options\.c$|phsettings|setting",               "06"),
    (r"prp\.c$|_prp\.c$|properties\.c$",              "07"),
    (r"status\.c$|statusbar",                         "09"),
    (r"plugins\\",                                    "08"),
]

LIT = re.compile(r'L?"((?:[^"\\]|\\.)*)"')


def read_text(p):
    # 必须用 utf-8-sig：收集文件带 BOM（记事本友好），用 utf-8 读会把 BOM 留在
    # 首行，于是 "# System Informer..." 不以 # 开头、被当成「字段不全的数据行」。
    # 之前报告里那个「字段不全 2 行」就是这么来的假警报 —— 文件本身没有问题。
    with open(p, "r", encoding="utf-8-sig", errors="replace", newline="") as f:
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


def load_merged():
    """返回 [(key, val, seg)]，seg 取 1..4。"""
    out, seg, seen = [], 1, set()
    for l in read_text(MERGED).split("\n"):
        l = l.rstrip("\r")
        if l.startswith("#"):
            m = re.search(r"第\s*([1-4])\s*段", l)
            if m and "====" not in l:
                seg = int(m.group(1))
            continue
        if "\t" not in l:
            continue
        k, v = l.split("\t", 1)
        if k in seen:
            continue
        seen.add(k)
        out.append((k, v, seg))
    return out


def load_rc():
    if not os.path.isfile(RCJSON):
        return {}
    rc = json.load(open(RCJSON, encoding="utf-8"))
    return {k: set(v) for k, v in rc.items()}


def load_gap():
    g = {}
    if not os.path.isfile(GAPTSV):
        return g
    for l in read_text(GAPTSV).split("\n")[1:]:
        p = l.rstrip("\r").split("\t")
        if len(p) >= 3:
            g.setdefault(p[1], (p[0], p[2]))
    return g


def call_body(txt, open_paren, cap=6000):
    """从 '(' 起取到括号配平的 ')'（跳过字符串与字符字面量）。"""
    depth, j, n = 0, open_paren, len(txt)
    end = min(n, open_paren + cap)
    while j < end:
        ch = txt[j]
        if ch == '"' or ch == "'":
            q = ch
            j += 1
            while j < end and txt[j] != q:
                if txt[j] == "\\":
                    j += 1
                j += 1
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return txt[open_paren:j + 1]
        j += 1
    return txt[open_paren:end]


def harvest_api(txt):
    """返回 类别 -> 该类别 API 调用体里出现的字面量集合。"""
    found = {}
    for cat, pat in API_RULES:
        for m in re.finditer(r"\b(?:" + pat + r")\s*\(", txt):
            body = call_body(txt, m.end() - 1)
            for lm in LIT.finditer(body):
                s = c_unescape(lm.group(1))
                if s and s not in found:
                    found[s] = cat
    return found


def scan_sources(keys):
    """一趟扫源码，返回 (key小写 -> [出处...], key小写 -> 类别, 文件数)。

    键统一用小写比对：源码里 "Company Name" / "Company name" 这种大小写差异
    在界面上是同一个列头，按原文精确比对会漏掉一半证据。
    """
    klow = set(k.lower() for k in keys)
    index = collections.defaultdict(list)
    api = {}
    nfile = 0
    for root, dirs, files in os.walk(SRCROOT):
        dirs[:] = [d for d in dirs if d not in (".git", ".vs", "build")]
        for fn in files:
            if not fn.lower().endswith((".c", ".cpp", ".h", ".rc")):
                continue
            p = os.path.join(root, fn)
            rel = os.path.relpath(p, SRCROOT)
            try:
                txt = read_text(p)
            except Exception:
                continue
            nfile += 1
            if '"' not in txt:
                continue
            for m in LIT.finditer(txt):
                lit = c_unescape(m.group(1))
                if not lit:
                    continue
                low = lit.lower()
                if low not in klow:
                    continue
                lst = index[low]
                if rel not in lst and len(lst) < 4:
                    lst.append(rel)
            for lit, cat in harvest_api(txt).items():
                low = lit.lower()
                if low not in api:
                    api[low] = cat
    return index, api, nfile


def classify(key, seg, rc, fidx, apix):
    """返回 (类别号, 证据类型, 出处)"""
    low = key.lower()
    if key in rc.get("DIALOG", ()):
        return "01", "rc", "DIALOGEX"
    if key in rc.get("VERSIONINFO", ()):
        return "02", "rc", "VERSIONINFO"

    if low in apix:
        return apix[low], "api", API_RULES[[c for c, _ in API_RULES].index(apix[low])][1][:40]

    if key.startswith("&"):
        return "03", "accelerator", "L\"&...\""

    files = fidx.get(low) or []
    for pat, cat in FILE_RULES:
        for rel in files:
            if re.search(pat, rel, re.I):
                return cat, "file", rel

    if files:
        return "10", "file", files[0]
    return "11", "none", ""


def main():
    entries = load_merged()
    rc = load_rc()
    keys = set(k for k, _, _ in entries)
    print("词典条目      : %d" % len(entries))
    fidx, apix, nfile = scan_sources(keys)
    print("扫描源文件    : %d 个" % nfile)
    print("  文件级出处  : %d 个键" % len(fidx))
    print("  API 上下文  : %d 个字面量" % len(apix))
    print()

    rows = []
    for k, v, seg in entries:
        cat, ev, loc = classify(k, seg, rc, fidx, apix)
        rows.append((k, v, cat, ev, loc, seg))

    by = collections.defaultdict(list)
    for r in rows:
        by[r[2]].append(r)

    # ---------------- 运行时未命中（自动收集区，见 [12]） ----------------
    misses, n_skipped = load_missed()
    sysnames = system_data_names()

    kept_fmt = [m for m in misses if miss_is_ui_text(m[0])]
    n_fmt = len(misses) - len(kept_fmt)

    # 再滤掉「运行期拼接出来的」串：它们在源码里是若干片段拼成的，
    # 收集器看到的是拼完的结果（含 PID、进程名等运行时数据），于是每次都不同、
    # 永远不可能被词典命中。放进 [12] 只会把待翻译区淹没。详见 MISS_DROP_PATTERNS。
    kept_cat = [m for m in kept_fmt if not miss_is_runtime_concat(m[0])]
    n_cat = len(kept_fmt) - len(kept_cat)

    # 先按**结构化指纹**分流：指纹判定为「列表里的对象名」的，直接进 [13] 数据行区。
    # 这比按名字猜可靠得多 —— 进程名 / 服务名 / 设备名在词形上和界面枚举值无法区分
    # （"Auto start (trigger)" vs "AppvStrm"），但「它画在服务列表的名称列里」是硬证据。
    src_of = {m[0]: miss_source(m[3], m[4], m[5]) for m in kept_cat}
    data_rows = [m for m in kept_cat if src_of[m[0]][1] == "data"]
    rest_rows = [m for m in kept_cat if src_of[m[0]][1] != "data"]

    # 剩下的（界面文字 / 指纹判不出的）再用「指名 + 本机服务名」兜底 ——
    # 没有指纹的旧数据只有这一条路。
    kept_ig = [m for m in rest_rows if m[0] not in MISS_IGNORE]
    n_ig = len(rest_rows) - len(kept_ig)
    kept = [m for m in kept_ig if m[0].strip().lower() not in sysnames]
    n_sys = len(kept_ig) - len(kept)

    fresh = [m for m in kept if m[0] not in keys]
    miss_by = collections.defaultdict(list)      # 来源标签 -> [记录]
    miss_kind = {}                               # 来源标签 -> kind
    for t, n, api, chain, cols, col in fresh:
        label, kind = miss_source(chain, cols, col)
        miss_by[label].append((t, n, api, chain, cols, col))
        miss_kind[label] = kind

    fresh_data = [m for m in data_rows if m[0] not in keys]
    data_by = collections.defaultdict(list)
    for t, n, api, chain, cols, col in fresh_data:
        label, kind = miss_source(chain, cols, col)
        data_by[label].append((t, n, api, chain, cols, col))

    n_data = len(fresh_data)
    print("未命中收集文件: 读到 %d 条 -> 数值类 %d / 运行期拼接串 %d / 指纹判定为数据行 %d "
          "/ 指名的数据 %d / 本机服务或进程名 %d / 字段不全 %d 行"
          % (len(misses), n_fmt, n_cat, n_data, n_ig, n_sys, n_skipped))
    print("  -> [12] 待翻译 %d 条   [13] 数据行（勿译）%d 条" % (len(fresh), len(fresh_data)))
    print("按来源分布（来源 = 控件链 + 列表列头 + 所在列，运行期采集）:")
    for label, v, km in sorted(
            [(l, miss_by[l], miss_kind[l]) for l in miss_by] +
            [(l, data_by[l], "data") for l in data_by],
            key=lambda x: (KIND_ORDER.get(x[2], 9), -len(x[1]))):
        print("   -> %-26s %-8s %4d 条   %s"
              % (label, KIND_NAME[km], len(v),
                 ("列: " + v[0][4]) if v[0][4] else ""))
    print("   [12] 是待翻译（界面文字/未知）；[13] 是列表里的对象名，通常不需要翻译。")
    print()

    # 分布统计放在最后打印：早期版本把它放在加载未命中之前，
    # 于是 [12] 恒显示 0、合计也不含收集来的条目 —— 报告自己跟自己矛盾。
    print("=== 分类分布 ===")
    for c, name, why in CATS:
        if c in ("12", "13"):
            continue
        print("  [%s] %-28s %4d" % (c, name, len(by[c])))
    print("  [12] %-28s %4d  <- 运行时自动收集，译文留空待翻译"
          % (CAT_NAME["12"], len(fresh)))
    print("  [13] %-28s %4d  <- 指纹判定为列表里的对象名（勿译，保留取证）"
          % (CAT_NAME["13"], len(fresh_data)))
    print("  %-33s %4d" % ("合计", len(rows) + len(fresh) + len(fresh_data)))
    print()
    noev = [r for r in rows if r[3] == "none"]
    print("完全没有出处证据: %d" % len(noev))
    if noev:
        print("  样例:", [r[0] for r in noev[:15]])
    print()

    # ---------------- 输出 ----------------
    head = []
    head.append("# System Informer 中文词典")
    head.append("# 格式: 原文<TAB>译文      # 开头为注释")
    head.append("# 编码: UTF-8 无 BOM（加载器用 MultiByteToWideChar(CP_UTF8)）")
    head.append("# 替换时机: 整串精确相等（另有「去掉 & 后再查一次」的兜底）")
    head.append("#")
    head.append("# 本文件由 _work\\dict\\build_zh_dict.py 生成扁平表，")
    head.append("# 再由 _work\\dict\\organize.py 按类别重组。改词直接改这里即可，")
    head.append("# 但下次重跑 organize.py 会覆盖 —— 要长期保留请写进 ui_zh.py / 第 1 段基线。")
    head.append("#")
    head.append("# 出处不写在条目行里：加载器按第一个 TAB 切分，行尾加注释会把注释吃进译文。")
    head.append("# 逐条出处见 _work\\dict\\zh_dict.index.tsv（原文/译文/类别/证据/出处）。")
    head.append("#")
    head.append("# ------------------------------------------------------------")
    head.append("# 关于「字符串表 / 消息表 / 菜单」这几个资源类型：")
    head.append("#   把源码 34 个 .rc 全扫了一遍，System Informer **只有**：")
    head.append("#     DIALOG     274 个块 / 732 个唯一字面量   -> [01]")
    head.append("#     VERSIONINFO 18 个块 /  48 个唯一字面量   -> [02]")
    head.append("#     ACCELERATORS 3 个块（无一可译文本）")
    head.append("#   STRINGTABLE = 0 个，MESSAGE TABLE = 0 个，MENU = 0 个。")
    head.append("#   它的菜单是 menubar.c 里用代码搭的，字符串是 C 字面量 ——")
    head.append("#   所以「字符串表 / 消息表」在这个程序上不存在对应资源，")
    head.append("#   只能用代码侧等价分区代替（见下面的判据）。")
    head.append("#")
    head.append("# [09] 提示与消息 现在是 0 条，这不是漏了：")
    head.append("#   全树消息类 API 只有 45 处调用（PhShowMessage2 28 / PhShowError 6 /")
    head.append("#   PhShowMessage 5 / MessageBoxW 3 / MessageBox 2 / PhShowWarning 1），")
    head.append("#   这些调用点的字面量目前都还没进词典。")
    head.append("#   运行时的未命中收集（zh_dict.missed.txt + merge_missed.py）会把它们补进来。")
    head.append("# ------------------------------------------------------------")
    head.append("#")
    head.append("# 分区索引：")
    for c, name, why in CATS:
        if c in ("12", "13"):
            continue
        head.append("#   [%s] %-28s %4d 条   <- %s" % (c, name, len(by[c]), why))
    head.append("#   [12] %-28s %4d 条   <- 运行时自动收集，译文留空待翻译"
                % (CAT_NAME["12"], len(fresh)))
    head.append("#   [13] %-28s %4d 条   <- 指纹判定为列表里的对象名，勿译"
                % (CAT_NAME["13"], len(fresh_data)))
    head.append("#   %-33s %4d 条" % ("合计", len(rows) + len(fresh) + len(fresh_data)))
    head.append("#")

    body = list(head)
    for c, name, why in CATS:
        if c in ("12", "13"):
            continue
        body.append("")
        body.append("# ============================================================")
        body.append("# [%s] %s    %d 条" % (c, name, len(by[c])))
        body.append("#   判据: %s" % why)
        body.append("# ============================================================")
        for k, v, cat, ev, loc, seg in sorted(by[c], key=lambda r: r[0].lower()):
            body.append("%s\t%s" % (k, v))

    # ---- [12] 专有区域：运行时未命中（译文留空 = 待翻译） ----
    body.append("")
    body.append("# ============================================================")
    body.append("# [12] %s    %d 条" % (CAT_NAME["12"], len(fresh)))
    body.append("#   来源: %s" % MISSED)
    body.append("#   这些是程序运行时「界面上出现过、但词典里没有」的串，由 zh_hook.dll")
    body.append("#   自动收集（每条只记一次，跨次运行不重复）。译文留空 —— 加载器对空译文")
    body.append("#   的条目直接跳过，所以留着不影响运行。")
    body.append("#   数值类（内存占用、CPU 百分比、PID、DOMAIN\\user）在收集端和合并端")
    body.append("#   各滤了一遍，所以这里只有看着像界面文字的；再改判据要两边同时改。")
    body.append("#")
    body.append("#   ★ 下面按【来源】分组。来源是运行期采集的指纹：控件链 + 列表列头 +")
    body.append("#     所在列。进程列表 / 服务列表 / 设备列表的控件类都是 PhTreeNew、")
    body.append("#     控件 ID 都是 0、父链也一样，**只有列头能区分** —— 所以能分出")
    body.append("#     「这是服务列表的名称列」还是「这是某个对话框上的文字」。")
    body.append("#")
    body.append("#   补法：把译文填到 TAB 右边即可；填好之后这一条就生效了。")
    body.append("#   想让它归类归位，可把该行挪进上面对应分区（organize.py 重跑会按原文归位）。")
    body.append("# ============================================================")

    def write_groups(groups, kinds, named):
        for label in sorted(groups, key=lambda x: (KIND_ORDER.get(kinds.get(x, named), 9),
                                                   -len(groups[x]))):
            rs = groups[label]
            body.append("#")
            body.append("# ---- 来源: %s   [%s]   %d 条 ----"
                        % (label, KIND_NAME[kinds.get(label, named)], len(rs)))
            if rs[0][3]:
                body.append("#      控件链: %s" % rs[0][3])
            if rs[0][4]:
                body.append("#      列头:   %s" % rs[0][4])
            if rs[0][5] != "":
                body.append("#      所在列: %s" % rs[0][5])
            for t, n, api, chain, cols, col in sorted(rs, key=lambda m: (-m[1], m[0].lower())):
                body.append("%s\t" % t)

    write_groups(miss_by, miss_kind, "ui")

    # ---- [13] 专有区域：指纹判定为「列表里的对象名」的内容 ----
    body.append("")
    body.append("# ============================================================")
    body.append("# [13] %s    %d 条" % (CAT_NAME["13"], len(fresh_data)))
    body.append("#   这些**不要翻译**，留在这里是为了留证据、也为了让你能一眼看清")
    body.append("#   「哪些东西是列表里的数据」。它们和 [12] 的差别不是猜的：")
    body.append("#   运行期采集的指纹证明它们画在列表的数据列上（例如「服务列表的")
    body.append("#   名称列」「设备列表的制造商列」），而不是某个对话框上的界面文字。")
    body.append("#")
    body.append("#   注：译文一律留空。加载器对空译文直接跳过，所以这些条目对运行")
    body.append("#   没有任何影响；就算某天真要翻其中一个，把译文填上即可生效。")
    body.append("#   想彻底不要，可以删掉整个 [13] 区 —— 下次运行还会重新采集到。")
    body.append("# ============================================================")
    write_groups(data_by, {}, "data")

    txt = "\n".join(body) + "\n"
    with open(OUT_TXT, "w", encoding="utf-8", newline="\n") as f:
        f.write(txt)
    with open(DEPLOY, "w", encoding="utf-8", newline="\n") as f:
        f.write(txt)

    with open(OUT_TSV, "w", encoding="utf-8", newline="\n") as f:
        f.write("原文\t译文\t类别\t类别名\t证据\t出处\n")
        for k, v, cat, ev, loc, seg in sorted(rows, key=lambda r: (r[2], r[0].lower())):
            f.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (k, v, cat, CAT_NAME[cat], ev, loc))

    # 校验：TAB 数必须等于条目数（每行恰好一个 TAB）
    ntab = txt.count("\t")
    nentry = len([1 for l in txt.split("\n") if l and not l.startswith("#") and "\t" in l])
    print("部署副本      : %s" % DEPLOY)
    print("分类文本      : %s" % OUT_TXT)
    print("出处索引      : %s" % OUT_TSV)
    print("条目数 %d / TAB 数 %d / 行均 TAB %.3f" % (nentry, ntab, ntab / max(nentry, 1)))
    assert ntab == nentry, "存在多余 TAB —— 行尾注释会把注释吃进译文，必须修掉"
    print("断言通过：每个条目行恰好一个 TAB")


if __name__ == "__main__":
    main()

# -*- coding: utf-8 -*-
r"""诊断：运行期到底有哪些 PhTreeNew、它们的直接子窗口是什么、列头有几列。

用来解释「为什么有些行内容读不到列头」——不猜，直接枚举。
"""
import ctypes, json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_dict as V

import paths                   # 所有路径集中在 tools\paths.py
DIST = paths.DIST
TARGET = paths.SI_TEST_DIR
APP = os.path.join(TARGET, "SystemInformer.exe")
SETT = APP + ".settings.json"
LAUNCHER = os.path.join(DIST, "si_zh_launcher.exe")

u32 = V.u32
u32.GetWindow.argtypes = [ctypes.c_void_p, ctypes.c_uint]
u32.GetWindow.restype = ctypes.c_void_p
u32.GetParent.argtypes = [ctypes.c_void_p]
u32.GetParent.restype = ctypes.c_void_p
u32.GetDlgCtrlID.argtypes = [ctypes.c_void_p]
u32.GetDlgCtrlID.restype = ctypes.c_int
u32.IsWindowVisible.argtypes = [ctypes.c_void_p]
u32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_void_p]
u32.SendMessageW.restype = ctypes.c_ssize_t
u32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]

GW_CHILD, GW_HWNDNEXT = 5, 2
LVM_GETHEADER = 0x1000 + 31
HDM_GETITEMCOUNT = 0x1200 + 0
HDM_GETITEMW = 0x1200 + 11


class HDITEMW(ctypes.Structure):
    _fields_ = [("mask", ctypes.c_uint), ("cxy", ctypes.c_int),
                ("pszText", ctypes.c_wchar_p), ("cchTextMax", ctypes.c_int),
                ("iImage", ctypes.c_int), ("iOrder", ctypes.c_int),
                ("type", ctypes.c_uint), ("pvFilter", ctypes.c_void_p),
                ("cchFilter", ctypes.c_uint), ("fmt", ctypes.c_int),
                ("lx", ctypes.c_int), ("piState", ctypes.c_void_p)]


def cls(h):
    b = ctypes.create_unicode_buffer(128)
    u32.GetClassNameW(h, b, 128)
    return b.value


def children(h):
    out, c = [], u32.GetWindow(h, GW_CHILD)
    while c:
        out.append(c)
        c = u32.GetWindow(c, GW_HWNDNEXT)
    return out


def columns(hh):
    n = u32.SendMessageW(hh, HDM_GETITEMCOUNT, 0, 0)
    res = []
    for i in range(min(n, 16)):
        buf = ctypes.create_unicode_buffer(128)
        it = HDITEMW()
        it.mask = 0x0002          # HDI_TEXT
        it.pszText = ctypes.cast(buf, ctypes.c_wchar_p)
        it.cchTextMax = 128
        u32.SendMessageW(hh, HDM_GETITEMW, i, ctypes.byref(it))
        res.append(buf.value)
    return n, res


def main():
    cfg = json.load(open(SETT, encoding="utf-8-sig"))
    keep = {k: cfg.get(k) for k in ("StartHidden", "AllowOnlyOneInstance", "EnableStartAsAdmin")}
    cfg["StartHidden"] = 0
    cfg["AllowOnlyOneInstance"] = 0
    cfg["EnableStartAsAdmin"] = 0
    json.dump(cfg, open(SETT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    try:
        subprocess.Popen([LAUNCHER, "--app", APP], cwd=DIST)
        main_h = None
        for _ in range(90):
            for h in V.enum_windows():
                if V.win_class(h) == "MainWindowClassName" and u32.IsWindowVisible(h):
                    try:
                        if V.proc_path(V.win_pid(h)).lower() == APP.lower():
                            main_h = h
                            break
                    except Exception:
                        pass
            if main_h:
                break
            time.sleep(1)
        if not main_h:
            print("没等到主窗口")
            return
        print("主窗口 %#x" % main_h)
        time.sleep(5)

        def walk(h, depth=0, limit=6):
            if depth > limit:
                return
            for c in children(h):
                cc = cls(c)
                extra = ""
                if cc == "SysHeader32":
                    n, names = columns(c)
                    extra = "  列数=%d  %s" % (n, " | ".join(names))
                if cc == "PhTreeNew" or cc == "SysHeader32" or depth < 2:
                    print("%s%s#%d hwnd=%#x%s" % ("  " * depth, cc, u32.GetDlgCtrlID(c), c, extra))
                if cc in ("PhTreeNew", "SysHeader32"):
                    continue
                walk(c, depth + 1, limit)

        print("\n=== 控件树（只看 PhTreeNew / SysHeader32）===")
        walk(main_h)

        print("\n=== 单独看每个 PhTreeNew 的直接子窗口 ===")
        def find(h, acc):
            for c in children(h):
                if cls(c) == "PhTreeNew":
                    acc.append(c)
                find(c, acc)
        acc = []
        find(main_h, acc)
        for t in acc:
            vis = bool(u32.IsWindowVisible(t))
            print("-" * 70)
            print("PhTreeNew %#x 可见=%s 父=%s" % (t, vis, cls(u32.GetParent(t)) if u32.GetParent(t) else "-"))
            for c in children(t):
                cc = cls(c)
                extra = ""
                if cc == "SysHeader32":
                    n, names = columns(c)
                    extra = "  列数=%d  %s" % (n, " | ".join(names))
                print("    子: %s#%d%s" % (cc, u32.GetDlgCtrlID(c), extra))
        V.u32.PostMessageW(main_h, 0x0010, 0, 0)
        time.sleep(2)
    finally:
        # 收尾：先关掉测试实例（脚本中途抛异常时也不能把它留下，
        # 否则 dist\zh_hook.dll 被占用，下次编译直接 LNK1104）
        for h in V.enum_windows():
            try:
                if V.proc_path(V.win_pid(h)).lower() == APP.lower():
                    V.u32.PostMessageW(h, 0x0010, 0, 0)
            except Exception:
                pass
        time.sleep(2)
        for k, v in keep.items():
            if v is not None:
                cfg[k] = v
        json.dump(cfg, open(SETT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        print("settings 已复原 %s" % keep)


if __name__ == "__main__":
    main()

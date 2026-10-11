# -*- coding: utf-8 -*-
r"""
验证「运行时未命中收集」—— 三段式，把三种落盘路径**分别隔离开**测。

  第 1 段  周期性刷新关掉（ZHHK_MISSFLUSH=0）+ 强杀
           -> 文件里必须已经有数据，且**没有**「最后一次刷新」行。
              这证明数据只可能来自「首次见到就立即追加」那条路。

  第 2 段  周期性刷新关掉 + 正常退出（ID_HACKER_EXIT）
           -> 必须出现「最后一次刷新」行。
              这证明退出钩子（ntdll!NtTerminateProcess / RtlExitUserProcess）真的被调用了
              —— 而 DLL_PROCESS_DETACH 在 System Informer 上根本不会发生。

  第 3 段  周期性刷新开启（ZHHK_MISSFLUSH=3）+ 强杀
           -> 必须出现「最后一次刷新」行。
              这证明「外部 taskkill /F」这种进程内钩子够不着的情况也有保险。

全程用独立实例（test_app），不碰用户正在跑的进程。
"""

import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes

import paths                   # 所有路径集中在 tools\paths.py
DIST = paths.DIST
APP = paths.app_exe()
SETT = APP + ".settings.json"
MISS = os.path.join(DIST, "zh_dict.missed.txt")
LOGP = os.path.join(DIST, "zh_hook.log")
LAUNCHLOG = os.path.join(DIST, "si_zh_launcher.log")

ID_HACKER_OPTIONS = 10083
ID_HACKER_EXIT = 10001
REFRESH_MARK = "最后一次刷新"

u32 = ctypes.WinDLL("user32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
CHILDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
u32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
u32.EnumChildWindows.argtypes = [wintypes.HWND, CHILDENUMPROC, wintypes.LPARAM]
u32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
u32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
u32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
u32.SetForegroundWindow.argtypes = [wintypes.HWND]
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                           ctypes.POINTER(wintypes.DWORD)]


def win_class(h):
    b = ctypes.create_unicode_buffer(256); u32.GetClassNameW(h, b, 256); return b.value


def win_pid(h):
    p = wintypes.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(p)); return p.value


def proc_path(pid):
    h = k32.OpenProcess(0x1000, False, pid)
    if not h: return ""
    b = ctypes.create_unicode_buffer(1024); n = wintypes.DWORD(1024)
    ok = k32.QueryFullProcessImageNameW(h, 0, b, ctypes.byref(n))
    k32.CloseHandle(h)
    return b.value if ok else ""


def enum_windows():
    out = []
    u32.EnumWindows(WNDENUMPROC(lambda h, l: (out.append(h), True)[1]), 0)
    return out


def enum_children(p):
    out = []
    u32.EnumChildWindows(p, CHILDENUMPROC(lambda h, l: (out.append(h), True)[1]), 0)
    return out


def wait_for(pred, timeout=60, step=0.4):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = pred()
        if r: return r
        time.sleep(step)
    return None


def drive_ui(pid, main_h):
    kids = enum_children(main_h)
    tree = None
    for name in ("PhTreeNew", "SysListView32"):
        for h in kids:
            if win_class(h) == name:
                tree = h; break
        if tree: break

    if tree:
        cr = wintypes.RECT(); u32.GetClientRect(tree, ctypes.byref(cr))
        cx, cy = 60, max(60, int((cr.bottom - cr.top) * 0.25))
        lp = (cy << 16) | (cx & 0xFFFF)
        u32.SetForegroundWindow(main_h)
        time.sleep(0.8)
        u32.PostMessageW(tree, 0x0200, 0, lp)
        time.sleep(0.3)
        u32.PostMessageW(tree, 0x0204, 2, lp)
        time.sleep(0.3)
        u32.PostMessageW(tree, 0x0205, 0, lp)
        time.sleep(2.5)
        u32.PostMessageW(main_h, 0x0100, 0x1B, 0)
        time.sleep(1.0)

    u32.PostMessageW(main_h, 0x0111, ID_HACKER_OPTIONS, 0)
    time.sleep(3.5)
    dlg = None; best = 0
    for h in enum_windows():
        if win_pid(h) == pid and u32.IsWindowVisible(h) and h != main_h:
            r = wintypes.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
            a = (r.right - r.left) * (r.bottom - r.top)
            if a > best and win_class(h) not in ("#32768", "tooltips_class32"):
                best, dlg = a, h
    if dlg:
        u32.PostMessageW(dlg, 0x0010, 0, 0)
        time.sleep(1.0)


def launch_and_drive(flush_sec):
    for p in (MISS, LOGP, LAUNCHLOG):
        if os.path.exists(p):
            os.remove(p)
    env = dict(os.environ)
    env["ZHHK_MISSFLUSH"] = str(flush_sec)
    proc = subprocess.Popen([os.path.join(DIST, "si_zh_launcher.exe"), "--log", "--app", APP],
                            cwd=DIST, close_fds=True, env=env)

    def find_main():
        for h in enum_windows():
            if win_class(h) == "MainWindowClassName" and u32.IsWindowVisible(h):
                if proc_path(win_pid(h)).lower() == APP.lower():
                    return h
        return None

    main_h = wait_for(find_main, 60)
    if not main_h:
        print("!! 找不到主窗口")
        return None
    pid = win_pid(main_h)
    print("  主窗口 hwnd=%#x pid=%d" % (main_h, pid))
    time.sleep(2.5)
    drive_ui(pid, main_h)
    return proc, pid, main_h


def report(tag, expect_refresh):
    if not os.path.exists(MISS):
        print("  [%s] !! 文件不存在" % tag)
        return False
    lines = open(MISS, "rb").read().decode("utf-8-sig", errors="replace").splitlines()
    data = [l for l in lines if l and not l.startswith("#")]
    has = any(REFRESH_MARK in l for l in lines)
    mark = "OK " if has == expect_refresh else "!! "
    print("  %s[%s] 数据行=%d  刷新标记=%s（期望 %s）"
          % (mark, tag, len(data), "有" if has else "无", "有" if expect_refresh else "无"))
    return (has == expect_refresh) and len(data) > 0


def main():
    with open(SETT, "r", encoding="utf-8") as f:
        backup = json.load(f)
    cfg = dict(backup)
    cfg["AllowOnlyOneInstance"] = "0"
    cfg["EnableStartAsAdmin"] = "0"
    with open(SETT, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False)

    ok = []
    try:
        # ---- 第 1 段：纯追加 + 强杀 ----
        print("=" * 70)
        print("第 1 段  周期刷新关 + 强杀   -> 期望：有数据 / 无刷新标记")
        print("=" * 70)
        r = launch_and_drive(0)
        if not r: return 1
        proc, pid, main_h = r
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
        time.sleep(1.5); proc.terminate()
        ok.append(report("纯追加+强杀", False))

        # ---- 第 2 段：纯追加 + 正常退出 ----
        print()
        print("=" * 70)
        print("第 2 段  周期刷新关 + 正常退出 -> 期望：有数据 / 有刷新标记")
        print("         （这一段的刷新标记只能来自退出钩子，因为 SI 不走 DETACH）")
        print("=" * 70)
        r = launch_and_drive(0)
        if not r: return 1
        proc, pid, main_h = r
        u32.SetForegroundWindow(main_h); time.sleep(0.5)
        u32.PostMessageW(main_h, 0x0111, ID_HACKER_EXIT, 0)
        gone = wait_for(lambda: not any(win_pid(h) == pid for h in enum_windows()), 30, 0.5)
        print("  进程已退出: %s" % bool(gone))
        time.sleep(1.5); proc.terminate()
        ok.append(report("退出钩子", True))

        # ---- 第 3 段：周期刷新 + 强杀 ----
        print()
        print("=" * 70)
        print("第 3 段  周期刷新开(3s) + 强杀 -> 期望：有数据 / 有刷新标记")
        print("=" * 70)
        r = launch_and_drive(3)
        if not r: return 1
        proc, pid, main_h = r
        time.sleep(4)                       # 等一次检查点
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
        time.sleep(1.5); proc.terminate()
        ok.append(report("周期检查点", True))

    finally:
        with open(SETT, "w", encoding="utf-8") as f:
            json.dump(backup, f, ensure_ascii=False)
        print()
        print("已复原 settings。")

    print()
    print("结论：%d / %d 段通过" % (sum(1 for x in ok if x), len(ok)))
    return 0 if all(ok) and ok else 1


if __name__ == "__main__":
    sys.exit(main())

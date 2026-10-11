# -*- coding: utf-8 -*-
r"""
验证合并后的词典在真实 SystemInformer 里的效果 —— 用独立实例，不动用户正在跑的进程。

做法：
  1. 备份并临时改 test_app 的 settings：AllowOnlyOneInstance=0（否则单实例策略会让
     新进程直接退出）、EnableStartAsAdmin=0（否则非提权会卡在 UAC）。
  2. 用 dist\si_zh_launcher.exe --log --app <test_app exe> 启动。
  3. 等 MainWindowClassName 出现（按 exe 路径确认是我们的实例，不是用户那个）。
  4. 截图主窗口。
  5. 找子窗口里的进程列表（EnumChildWindows；上一版只枚举顶层所以漏了），
     PostMessage(WM_CONTEXTMENU, lParam=-1) → 弹右键菜单 → 从屏幕 BitBlt 抓图。
     这一批菜单项在词典第 3 段（& 形式），是本轮新增能力的关键验证。
  6. PostMessage(WM_COMMAND, 10083=ID_HACKER_OPTIONS) → 选项对话框 → 截图。
  7. 分析 zh_hook.log：命中/未命中，并标出哪些命中来自「本轮新增条目」。
  8. 复原 settings，结束测试进程。
"""
import ctypes, json, os, struct, subprocess, sys, time
from ctypes import wintypes

import paths                   # 所有路径集中在 tools\paths.py
DIST = paths.DIST
APP  = paths.app_exe()
SETT = APP + ".settings.json"
OUTDIR = paths.SHOTS
LOGP = os.path.join(DIST, "zh_hook.log")

u32 = ctypes.WinDLL("user32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

WNDENUMPROC   = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
CHILDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
u32.EnumWindows.argtypes      = [WNDENUMPROC, wintypes.LPARAM]
u32.EnumChildWindows.argtypes = [wintypes.HWND, CHILDENUMPROC, wintypes.LPARAM]
u32.GetClassNameW.argtypes    = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes   = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u32.IsWindowVisible.argtypes  = [wintypes.HWND]
u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
u32.PostMessageW.argtypes     = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u32.PrintWindow.argtypes      = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
u32.GetWindowRect.argtypes    = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
u32.GetDC.argtypes            = [wintypes.HWND]
u32.SetForegroundWindow.argtypes = [wintypes.HWND]
g32.BitBlt.argtypes           = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int, wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                           ctypes.POINTER(wintypes.DWORD)]
SRCCOPY = 0x00CC0020
WM_CONTEXTMENU, WM_COMMAND, WM_KEYDOWN = 0x007B, 0x0111, 0x0100
ID_HACKER_OPTIONS = 10083          # SystemInformer\resource.h:787

def win_text(h):
    b = ctypes.create_unicode_buffer(512); u32.GetWindowTextW(h, b, 512); return b.value

def win_class(h):
    b = ctypes.create_unicode_buffer(256); u32.GetClassNameW(h, b, 256); return b.value

def win_pid(h):
    p = wintypes.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(p)); return p.value

def win_rect(h):
    r = wintypes.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right - r.left, r.bottom - r.top

def proc_path(pid):
    h = k32.OpenProcess(0x1000, False, pid)
    if not h: return ""
    buf = ctypes.create_unicode_buffer(1024); n = wintypes.DWORD(1024)
    ok = k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
    k32.CloseHandle(h)
    return buf.value if ok else ""

def enum_windows():
    out = []
    u32.EnumWindows(WNDENUMPROC(lambda h, l: (out.append(h), True)[1]), 0)
    return out

def enum_children(parent):
    out = []
    u32.EnumChildWindows(parent, CHILDENUMPROC(lambda h, l: (out.append(h), True)[1]), 0)
    return out

class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]

class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]

def _new_dib(hdc, w, h):
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h            # 负数 = 自顶向下，省得翻转
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0
    bits = ctypes.c_void_p()
    hbmp = g32.CreateDIBSection(hdc, ctypes.byref(bi), 0, ctypes.byref(bits), None, 0)
    return hbmp, bits

def _write_bmp(path, raw, w, h):
    row = ((w * 3 + 3) // 4) * 4
    px = bytearray()
    for y in range(h):
        line = raw[y * w * 4:(y + 1) * w * 4]
        px += bytes(b for x in range(w) for b in (line[x*4+0], line[x*4+1], line[x*4+2]))
        px += b"\x00" * (row - w * 3)
    with open(path, "wb") as f:
        f.write(struct.pack("<2sIHHI", b"BM", 14 + 40 + len(px), 0, 0, 14 + 40))
        # biHeight 必须为负（自顶向下）：我们的数据是按 top-down 顺序写的。
        # 上一版写成正数，BMP 规范就按 bottom-up 解释 -> 整张图上下翻转。
        f.write(struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 24, 0, len(px), 2835, 2835, 0, 0))
        f.write(px)

def capture(hwnd, out_bmp):
    x, y, w, h = win_rect(hwnd)
    if w <= 0 or h <= 0: return None, "rect 为空"
    hdc = u32.GetDC(0); mem = g32.CreateCompatibleDC(hdc)
    hbmp, bits = _new_dib(hdc, w, h)
    old = g32.SelectObject(mem, hbmp)
    ok = u32.PrintWindow(hwnd, mem, 2)    # PW_RENDERFULLCONTENT
    g32.GdiFlush()
    raw = ctypes.string_at(bits, w * h * 4)
    g32.SelectObject(mem, old); g32.DeleteObject(hbmp); g32.DeleteDC(mem); u32.ReleaseDC(0, hdc)
    uniq = len(set(raw[i:i+4] for i in range(0, min(len(raw), 400000), 4)))
    _write_bmp(out_bmp, raw, w, h)
    return (w, h), "PrintWindow=%d 唯一色数=%d" % (ok, uniq)

def capture_screen(x, y, w, h, out_bmp):
    hdc = u32.GetDC(0); mem = g32.CreateCompatibleDC(hdc)
    hbmp, bits = _new_dib(hdc, w, h)
    old = g32.SelectObject(mem, hbmp)
    ok = g32.BitBlt(mem, 0, 0, w, h, hdc, x, y, SRCCOPY)
    g32.GdiFlush()
    raw = ctypes.string_at(bits, w * h * 4)
    g32.SelectObject(mem, old); g32.DeleteObject(hbmp); g32.DeleteDC(mem); u32.ReleaseDC(0, hdc)
    _write_bmp(out_bmp, raw, w, h)
    return (w, h), "BitBlt=%d" % ok

def wait_for(pred, timeout=60, step=0.4):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = pred()
        if r: return r
        time.sleep(step)
    return None

def read_keys(path):
    ks = set()
    with open(path, "r", encoding="utf-8") as f:
        for l in f:
            l = l.rstrip("\r\n")
            if l.startswith("#") or "\t" not in l: continue
            ks.add(l.split("\t", 1)[0])
    return ks

def main():
    os.makedirs(OUTDIR, exist_ok=True)
    merged = paths.MERGED
    oldkeys = read_keys(paths.BASE_DICT)
    newkeys = read_keys(merged) - oldkeys
    print("本轮新增键 = %d" % len(newkeys))

    with open(SETT, "r", encoding="utf-8") as f:
        backup = json.load(f)
    cfg = dict(backup)
    cfg["AllowOnlyOneInstance"] = "0"
    cfg["EnableStartAsAdmin"] = "0"
    with open(SETT, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False)

    for p in (LOGP, os.path.join(DIST, "si_zh_launcher.log")):
        if os.path.exists(p): os.remove(p)

    print("启动: si_zh_launcher.exe --log --app %s" % APP)
    proc = subprocess.Popen([os.path.join(DIST, "si_zh_launcher.exe"), "--log", "--app", APP],
                            cwd=DIST, close_fds=True)

    def find_main():
        for h in enum_windows():
            if win_class(h) == "MainWindowClassName" and u32.IsWindowVisible(h):
                if proc_path(win_pid(h)).lower() == APP.lower():
                    return h
        return None
    main_h = wait_for(find_main, 60)
    if not main_h:
        print("!! 找不到主窗口。启动器日志尾部：")
        lp = os.path.join(DIST, "si_zh_launcher.log")
        if os.path.exists(lp):
            print("\n".join(open(lp, encoding="utf-16", errors="ignore").read().splitlines()[-25:]))
        return 1
    pid = win_pid(main_h)
    print("主窗口 hwnd=%#x pid=%d title=%s" % (main_h, pid, win_text(main_h)))
    time.sleep(3)
    print("主窗口截图 %s" % (capture(main_h, os.path.join(OUTDIR, "01_main.bmp")),))

    # --- 进程列表（子窗口）---
    kids = enum_children(main_h)
    classes = {}
    for h in kids:
        classes.setdefault(win_class(h), []).append(h)
    print("子窗口类：%s" % ", ".join("%s×%d" % (k, len(v)) for k, v in classes.items()))

    tree = None
    for name in ("PhTreeNew", "SysListView32"):
        if name in classes:
            tree = classes[name][0]; break
    if tree:
        print("进程列表 hwnd=%#x class=%s" % (tree, win_class(tree)))
        u32.SetForegroundWindow(main_h)
        time.sleep(1.0)
        # WM_CONTEXTMENU(lParam=-1) 无效（PhTreeNew 不处理键盘菜单键），
        # 改成真右击：PhTreeNew 走 WM_RBUTTONDOWN 命中测试 -> TrackPopupMenu。
        cr = wintypes.RECT()
        u32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        u32.GetClientRect(tree, ctypes.byref(cr))
        cx, cy = 60, max(60, int((cr.bottom - cr.top) * 0.25))
        lp = (cy << 16) | (cx & 0xFFFF)
        print("模拟右击 client=(%d,%d)  客户区=%dx%d" % (cx, cy, cr.right, cr.bottom))
        u32.PostMessageW(tree, 0x0200, 0, lp)          # WM_MOUSEMOVE
        time.sleep(0.3)
        u32.PostMessageW(tree, 0x0204, 2, lp)          # WM_RBUTTONDOWN (MK_RBUTTON)
        time.sleep(0.3)
        u32.PostMessageW(tree, 0x0205, 0, lp)          # WM_RBUTTONUP
        time.sleep(2.5)
        popup = None
        cands = []
        for h in enum_windows():
            if win_pid(h) == pid and u32.IsWindowVisible(h) and h != main_h:
                px, py, pw, ph = win_rect(h)
                if win_class(h) == "#32768":
                    popup = h
                    break
                if 40 < pw < 900 and 40 < ph < 1200:
                    cands.append((pw * ph, h, win_class(h)))
        if not popup and cands:
            cands.sort()
            popup = cands[0][1]
            print("未找到 #32768，但发现候选弹出窗 class=%s" % cands[0][2])
        mx, my, mw, mh = win_rect(main_h)
        if popup:
            px, py, pw, ph = win_rect(popup)
            x0, y0 = min(mx, px), min(my, py)
            x1, y1 = max(mx + mw, px + pw), max(my + mh, py + ph)
            print("弹出菜单 hwnd=%#x class=%s rect=%s" % (popup, win_class(popup), (px, py, pw, ph)))
        else:
            x0, y0, x1, y1 = mx, my, mx + mw + 500, my + mh
            print("!! 仍未发现弹出窗")
        print("含菜单截图 %s" % (capture_screen(x0, y0, min(x1 - x0, 1900), min(y1 - y0, 1200),
                                              os.path.join(OUTDIR, "02_menu.bmp")),))
        u32.PostMessageW(main_h, WM_KEYDOWN, 0x1B, 0)   # ESC 关菜单
        time.sleep(1)
    else:
        print("!! 子窗口里没有可识别的列表控件")

    # --- 选项对话框 ---
    print("投递 WM_COMMAND %d (ID_HACKER_OPTIONS)" % ID_HACKER_OPTIONS)
    u32.PostMessageW(main_h, WM_COMMAND, ID_HACKER_OPTIONS, 0)
    time.sleep(3)
    dlg, best = None, 0
    for h in enum_windows():
        if win_pid(h) == pid and u32.IsWindowVisible(h) and h != main_h:
            _, _, w, hh = win_rect(h)
            if w * hh > best and win_class(h) not in ("#32768", "tooltips_class32"):
                best, dlg = w * hh, h
    if dlg:
        print("对话框 hwnd=%#x class=%s title=%s" % (dlg, win_class(dlg), win_text(dlg)))
        print("选项对话框截图 %s" % (capture(dlg, os.path.join(OUTDIR, "03_options.bmp")),))
    else:
        print("!! 未出现新顶层窗口")

    # --- 日志分析 ---
    print("\n=========== zh_hook.log 分析 ===========")
    if os.path.exists(LOGP):
        txt = open(LOGP, encoding="utf-16", errors="ignore").read().splitlines()
        hit = [l for l in txt if l.startswith("[HIT ]")]
        miss = [l for l in txt if l.startswith("[miss]")]
        print("行数=%d  HIT=%d  miss=%d" % (len(txt), len(hit), len(miss)))
        for l in txt[:6]: print("  " + l)
        newsrc, menuhit = [], []
        for l in hit:
            try: src = l.split('"', 2)[1]
            except Exception: continue
            if src in newkeys: newsrc.append(l)
            if src.startswith("&"): menuhit.append(l)
        print("\n--- 命中「本轮新增条目」的 %d 条（节选 45，去重）---" % len(newsrc))
        seen = set()
        for l in newsrc:
            s = l.split('"', 2)[1] if '"' in l else l
            if s in seen: continue
            seen.add(s); print("  " + l)
            if len(seen) >= 45: break
        print("\n--- 命中 & 菜单串的 %d 条（去重）---" % len(menuhit))
        seen = set()
        for l in menuhit:
            s = l.split('"', 2)[1] if '"' in l else l
            if s in seen: continue
            seen.add(s); print("  " + l)
        print("\n--- 仍未命中的 miss 里像 UI 文本的（前 30）---")
        cnt = 0
        for l in miss:
            if '"' not in l: continue
            s = l.split('"')[1]
            if len(s) >= 4 and sum(c.isalpha() for c in s) >= 4 and not any(c.isdigit() for c in s) \
               and "\\" not in s and ":" not in s:
                print("  " + l); cnt += 1
                if cnt >= 30: break
    else:
        print("!! 没有 hook 日志")

    # --- 清理 ---
    subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
    time.sleep(1)
    proc.terminate()
    with open(SETT, "w", encoding="utf-8") as f:
        json.dump(backup, f, ensure_ascii=False)
    print("\n已结束测试进程并复原 settings")
    return 0

if __name__ == "__main__":
    sys.exit(main())

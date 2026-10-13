# -*- coding: utf-8 -*-
r"""
验证字体替换（dict\zh_font.txt）在真实 SystemInformer 里的效果。

为什么要跑两遍：只有一趟截图说明不了任何事 —— 换个字体看起来总是「变了」。
唯一变量是 ZHHK_FONT，两次运行的其他条件完全相同：

  before : ZHHK_FONT=0 + ZHHK_FONTLOG=1
           → 不替换，只把 SI 造过的每个字体原样记下来（替换前的基线）
  after  : ZHHK_FONTLOG=1 + ZHHK_FONTFACE=1
           → 真替换，并且用 GetTextFace 核对 **GDI 实际解析到**的字体名

两遍都截主窗口 + 选项对话框，最后把两份日志里的
[font]（替换了什么）/ [fontall]（造过哪些字体）/ [fontface]（GDI 真给了什么）摆在一起。

用 test_app 里的独立副本跑，不动用户正在使用的那个进程；跑完复原 settings。

用法：
    set SI_TEST_DIR=<测试副本目录>
    python tools\verify\verify_font.py
"""
import ctypes, json, os, struct, subprocess, sys, time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import paths

DIST = paths.DIST
APP = paths.app_exe()
SETT = APP + ".settings.json"
OUTDIR = paths.SHOTS
LOGP = os.path.join(DIST, "zh_hook.log")
LAUNCH_LOG = os.path.join(DIST, "si_zh_launcher.log")

u32 = ctypes.WinDLL("user32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
CHILDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
u32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
u32.EnumChildWindows.argtypes = [wintypes.HWND, CHILDENUMPROC, wintypes.LPARAM]
u32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u32.IsWindowVisible.argtypes = [wintypes.HWND]
u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
u32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
u32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
u32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
u32.GetDC.argtypes = [wintypes.HWND]
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                           ctypes.POINTER(wintypes.DWORD)]
g32.BitBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                       wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
SRCCOPY = 0x00CC0020
WM_COMMAND = 0x0111
ID_HACKER_OPTIONS = 10083          # SystemInformer\resource.h


def win_text(h):
    b = ctypes.create_unicode_buffer(512)
    u32.GetWindowTextW(h, b, 512)
    return b.value


def win_class(h):
    b = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, b, 256)
    return b.value


def win_pid(h):
    p = wintypes.DWORD()
    u32.GetWindowThreadProcessId(h, ctypes.byref(p))
    return p.value


def win_rect(h):
    r = wintypes.RECT()
    u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right - r.left, r.bottom - r.top


def proc_path(pid):
    h = k32.OpenProcess(0x1000, False, pid)
    if not h:
        return ""
    buf = ctypes.create_unicode_buffer(1024)
    n = wintypes.DWORD(1024)
    ok = k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
    k32.CloseHandle(h)
    return buf.value if ok else ""


def enum_windows():
    out = []
    u32.EnumWindows(WNDENUMPROC(lambda h, l: (out.append(h), True)[1]), 0)
    return out


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD),
                ("biCompression", wintypes.DWORD), ("biSizeImage", wintypes.DWORD),
                ("biXPelsPerMeter", wintypes.LONG), ("biYPelsPerMeter", wintypes.LONG),
                ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


def capture(hwnd, out_bmp):
    """把窗口内容画进内存位图（PrintWindow + PW_RENDERFULLCONTENT）。"""
    x, y, w, h = win_rect(hwnd)
    if w <= 0 or h <= 0:
        return None, "rect 为空"
    hdc = u32.GetDC(0)
    mem = g32.CreateCompatibleDC(hdc)
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h          # 负数 = 自顶向下
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bits = ctypes.c_void_p()
    hbmp = g32.CreateDIBSection(hdc, ctypes.byref(bi), 0, ctypes.byref(bits), None, 0)
    old = g32.SelectObject(mem, hbmp)
    ok = u32.PrintWindow(hwnd, mem, 2)
    g32.GdiFlush()
    raw = ctypes.string_at(bits, w * h * 4)
    g32.SelectObject(mem, old)
    g32.DeleteObject(hbmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(0, hdc)

    row = ((w * 3 + 3) // 4) * 4
    px = bytearray()
    for yy in range(h):
        line = raw[yy * w * 4:(yy + 1) * w * 4]
        px += bytes(b for xx in range(w) for b in (line[xx * 4 + 0], line[xx * 4 + 1], line[xx * 4 + 2]))
        px += b"\x00" * (row - w * 3)
    with open(out_bmp, "wb") as f:
        f.write(struct.pack("<2sIHHI", b"BM", 14 + 40 + len(px), 0, 0, 14 + 40))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 24, 0, len(px), 2835, 2835, 0, 0))
        f.write(px)
    return (w, h), "PrintWindow=%d" % ok


def wait_for(pred, timeout=60, step=0.4):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = pred()
        if r:
            return r
        time.sleep(step)
    return None


def read_log():
    if not os.path.exists(LOGP):
        return []
    with open(LOGP, "r", encoding="utf-16", errors="ignore") as f:
        return f.read().splitlines()


def run_once(tag, env_extra):
    """跑一遍并截图。返回 (日志行, 截图路径列表)。"""
    for p in (LOGP, LAUNCH_LOG):
        if os.path.exists(p):
            os.remove(p)

    env = dict(os.environ)
    env.update(env_extra)
    print("  启动: si_zh_launcher.exe --log --app %s   env=%s" % (APP, env_extra))
    proc = subprocess.Popen([os.path.join(DIST, "si_zh_launcher.exe"), "--log", "--app", APP],
                            cwd=DIST, env=env, close_fds=True)

    def find_main():
        for h in enum_windows():
            if win_class(h) == "MainWindowClassName" and u32.IsWindowVisible(h):
                if proc_path(win_pid(h)).lower() == APP.lower():
                    return h
        return None

    main_h = wait_for(find_main, 60)
    if not main_h:
        print("  !! 找不到主窗口。启动器日志尾部：")
        if os.path.exists(LAUNCH_LOG):
            for l in open(LAUNCH_LOG, encoding="utf-16", errors="ignore").read().splitlines()[-20:]:
                print("     " + l)
        proc.terminate()
        return [], []

    pid = win_pid(main_h)
    print("  主窗口 hwnd=%#x pid=%d 标题=%s" % (main_h, pid, win_text(main_h)))
    time.sleep(3.5)                     # 等首屏画完再截

    shots = []
    bmp = os.path.join(OUTDIR, "font_%s_main.bmp" % tag)
    print("  主窗口截图 %s" % (capture(main_h, bmp),))
    shots.append(bmp)

    # 选项对话框：字体设置那一页正好在这里，顺便验证标签有没有跟着换字体
    u32.PostMessageW(main_h, WM_COMMAND, ID_HACKER_OPTIONS, 0)
    time.sleep(3.0)
    dlg, best = None, 0
    for h in enum_windows():
        if win_pid(h) == pid and u32.IsWindowVisible(h) and h != main_h:
            _, _, w, hh = win_rect(h)
            if w * hh > best and win_class(h) not in ("#32768", "tooltips_class32"):
                best, dlg = w * hh, h
    if dlg:
        bmp = os.path.join(OUTDIR, "font_%s_options.bmp" % tag)
        print("  选项对话框 %s 标题=%s 截图 %s" % (win_class(dlg), win_text(dlg), capture(dlg, bmp)))
        shots.append(bmp)
    else:
        print("  !! 选项对话框没出现")

    lines = read_log()
    # 日志归档：这一趟的 [font]/[fontall]/[fontface] 是唯一的原始证据，跑完就删掉
    # 会把「为什么这么改」一起删掉。每趟存一份。
    with open(os.path.join(OUTDIR, "font_%s.log" % tag), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
    time.sleep(1.5)
    proc.terminate()
    return lines, shots


def section(lines, prefix):
    return [l for l in lines if l.startswith(prefix)]


def subs(lines):
    """只取编号的替换记录（'[font] #3 ...'），别把 '[font] path=' 这种头也算进去。"""
    return [l for l in lines if l.startswith("[font] #")]


def inventory(lines):
    """[fontall] 去重后的「字体名|高度|字重」计数，用来当替换前的基线。"""
    import collections
    c = collections.Counter()
    for l in section(lines, "[fontall]"):
        try:
            body = l.split("[fontall]", 1)[1].strip()
            api, rest = body.split(" ", 1)
            face = rest.split('"')[1]
            hw = rest.split('"')[2].strip()
            c["%s %s" % (face, hw.replace(" ", ""))] += 1
        except Exception:
            continue
    return c


def main():
    paths.ensure(OUTDIR)

    if not os.path.exists(SETT):
        print("!! 找不到测试副本的 settings：%s" % SETT)
        return 1

    with open(SETT, "r", encoding="utf-8") as f:
        backup = json.load(f)
    cfg = dict(backup)
    cfg["AllowOnlyOneInstance"] = "0"      # 否则单实例策略会让新进程直接退出
    cfg["EnableStartAsAdmin"] = "0"        # 否则非提权会卡在 UAC
    with open(SETT, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False)

    result = {}
    try:
        print("=== before：不替换字体，只记录 SI 造了哪些字体 ===")
        result["before"] = run_once("before", {"ZHHK_FONT": "0", "ZHHK_FONTLOG": "1"})

        print("\n=== after：替换字体 + 核对 GDI 实际给的字体 ===")
        result["after"] = run_once("after", {"ZHHK_FONTLOG": "1", "ZHHK_FONTFACE": "1"})
    finally:
        with open(SETT, "w", encoding="utf-8") as f:
            json.dump(backup, f, ensure_ascii=False)
        print("\n已复原 settings")

    print("\n================ 对照 ================")
    for tag in ("before", "after"):
        lines = result[tag][0]
        print("\n--- %s --- 日志 %d 行" % (tag, len(lines)))
        print("  [font] #    替换记录 %d 条" % len(subs(lines)))
        print("  [fontall]   造过的字体 %d 条" % len(section(lines, "[fontall]")))
        print("  [fontface]  实际字体核对 %d 条" % len(section(lines, "[fontface]")))
        for l in subs(lines):
            print("   " + l)
        for l in section(lines, "[fontface]"):
            print("   " + l)

    print("\n=========== 字体清单（[fontall] 去重，占位字体也列出来）===========")
    print("%-38s %5s | %5s" % ("字体 | 高度 | 字重", "before", "after"))
    ib, ia = inventory(result["before"][0]), inventory(result["after"][0])
    for k in sorted(set(ib) | set(ia), key=lambda x: -(ib[x] + ia[x])):
        print("%-38s %5d | %5d" % (k, ib.get(k, 0), ia.get(k, 0)))

    print("\n截图：")
    for tag in ("before", "after"):
        for s in result[tag][1]:
            print("  " + s)
    return 0


if __name__ == "__main__":
    sys.exit(main())

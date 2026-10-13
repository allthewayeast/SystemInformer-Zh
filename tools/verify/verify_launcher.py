# -*- coding: utf-8 -*-
r"""
verify_launcher.py —— 端到端验收 si_zh_launcher.exe

验的是四件事，每件都有可判读的落点：
  1. 启动器把 SystemInformer 起起来了（主窗口出现，进程存活）
  2. zh_hook.dll 确实被注入了（目标进程的模块表里有它）
  3. **时序**：钩子启用发生在目标代码执行之前 —— 读启动器日志里的
     「Done ... 此刻有可见主窗口=0」+ 钩子日志里首条 HIT 之前没有绘制
  4. 界面真的成了中文（截图 + 直接读控件文本，后者不受 DWM/截图影响）

不依赖 PIL：自带纯 Python PNG 编码器（zlib + crc32）。
"""
import ctypes, os, struct, subprocess, sys, time, zlib

PV = ctypes.c_void_p
DWORD = ctypes.c_ulong
BOOL = ctypes.c_int
SIZE_T = ctypes.c_size_t

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
user32 = ctypes.WinDLL('user32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
gdi32 = ctypes.WinDLL('gdi32', use_last_error=True)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # tools\paths.py
import paths

DIST = paths.DIST
LAUNCHER = os.path.join(DIST, 'si_zh_launcher.exe')
APP = paths.SI_TEST_DIR
EXE = os.path.join(APP, 'SystemInformer.exe')
LOGDIR = DIST
LLOG = os.path.join(LOGDIR, 'si_zh_launcher.log')
HLOG = os.path.join(LOGDIR, 'zh_hook.log')
SHOT = os.path.join(paths.SHOTS, 'shot_launcher.png')

WNDENUMPROC = ctypes.WINFUNCTYPE(BOOL, PV, PV)

user32.EnumWindows.argtypes = [WNDENUMPROC, PV]
user32.EnumChildWindows.argtypes = [PV, WNDENUMPROC, PV]
user32.GetWindowThreadProcessId.argtypes = [PV, ctypes.POINTER(DWORD)]
user32.GetClassNameW.argtypes = [PV, ctypes.c_wchar_p, ctypes.c_int]
user32.GetWindowTextW.argtypes = [PV, ctypes.c_wchar_p, ctypes.c_int]
user32.IsWindowVisible.argtypes = [PV]
user32.IsIconic.argtypes = [PV]
user32.IsWindow.argtypes = [PV]
user32.IsWindow.restype = BOOL
user32.BringWindowToTop.argtypes = [PV]
user32.SetForegroundWindow.argtypes = [PV]
user32.SendMessageW.argtypes = [PV, ctypes.c_uint, PV, PV]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.FindWindowExW.argtypes = [PV, PV, ctypes.c_wchar_p, ctypes.c_wchar_p]
user32.FindWindowExW.restype = PV
user32.ShowWindow.argtypes = [PV, ctypes.c_int]
user32.SetWindowPos.argtypes = [PV, PV, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
user32.PrintWindow.argtypes = [PV, PV, ctypes.c_uint]
user32.PrintWindow.restype = BOOL
user32.GetWindowDC.argtypes = [PV]
user32.GetWindowDC.restype = PV
user32.ReleaseDC.argtypes = [PV, PV]
user32.GetWindowRect.argtypes = [PV, PV]
psapi.EnumProcessModules.argtypes = [PV, PV, DWORD, ctypes.POINTER(DWORD)]
psapi.GetModuleBaseNameW.argtypes = [PV, PV, ctypes.c_wchar_p, DWORD]

# 64 位句柄一律显式声明 restype，否则 ctypes 默认 c_int 会把句柄截断成 32 位。
k32.OpenProcess.argtypes = [DWORD, BOOL, DWORD]
k32.OpenProcess.restype = PV
k32.CloseHandle.argtypes = [PV]
k32.CloseHandle.restype = BOOL
k32.QueryFullProcessImageNameW.argtypes = [PV, DWORD, ctypes.c_wchar_p, ctypes.POINTER(DWORD)]
k32.QueryFullProcessImageNameW.restype = BOOL

gdi32.CreateCompatibleDC.argtypes = [PV]
gdi32.CreateCompatibleDC.restype = PV
gdi32.CreateCompatibleBitmap.argtypes = [PV, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = PV
gdi32.SelectObject.argtypes = [PV, PV]
gdi32.SelectObject.restype = PV
gdi32.DeleteObject.argtypes = [PV]
gdi32.DeleteObject.restype = BOOL
gdi32.DeleteDC.argtypes = [PV]
gdi32.DeleteDC.restype = BOOL
gdi32.GetDIBits.argtypes = [PV, PV, ctypes.c_uint, ctypes.c_uint, PV, PV, ctypes.c_uint]
gdi32.GetDIBits.restype = ctypes.c_int
gdi32.BitBlt.argtypes = [PV, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, PV,
                         ctypes.c_int, ctypes.c_int, DWORD]
gdi32.BitBlt.restype = BOOL


class RECT(ctypes.Structure):
    _fields_ = [("l", ctypes.c_long), ("t", ctypes.c_long), ("r", ctypes.c_long), ("b", ctypes.c_long)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
                ("biPlanes", ctypes.c_ushort), ("biBitCount", ctypes.c_ushort),
                ("biCompression", DWORD), ("biSizeImage", DWORD),
                ("biXPelsPerMeter", ctypes.c_long), ("biYPelsPerMeter", ctypes.c_long),
                ("biClrUsed", DWORD), ("biClrImportant", DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", DWORD * 3)]


def write_png(path, w, h, bgra_bottomup):
    """纯 Python PNG：BGRA 自下而上 -> RGB 自上而下"""
    rowbytes = w * 4
    raw = bytearray()
    for y in range(h - 1, -1, -1):
        off = y * rowbytes
        row = bgra_bottomup[off:off + rowbytes]
        out = bytearray(w * 3)
        for x in range(w):
            i = x * 4
            out[x * 3] = row[i + 2]
            out[x * 3 + 1] = row[i + 1]
            out[x * 3 + 2] = row[i]
        raw.append(0)
        raw += out

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as fh:
        fh.write(png)


def proc_image(h):
    buf = ctypes.create_unicode_buffer(1024)
    n = DWORD(1024)
    if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
        return buf.value
    return ''


def find_main_window(verbose=True):
    """返回 (hwnd, pid, title)。

    取**面积最大**的那个 MainWindowClassName，而不是第一个 —— System Informer 会
    创建多个同类窗口，第一个往往是尚未布局的 0x0 隐藏窗口（上一轮就是栽在这里：
    窗口矩形读出来是 (0,0) 0x0，子控件也只有 23 个）。
    """
    out = []

    def cb(hwnd, lp):
        c = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, c, 256)
        if c.value != 'MainWindowClassName':
            return True
        pid = DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        h = k32.OpenProcess(0x1000, False, pid.value)
        if not h:
            return True
        img = proc_image(h)
        k32.CloseHandle(h)
        if img.lower() != EXE.lower():     # 只认测试副本，别抓用户真实的那个
            return True
        t = ctypes.create_unicode_buffer(512)
        user32.GetWindowTextW(hwnd, t, 512)
        rc = RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rc))
        out.append((hwnd, pid.value, t.value, rc, bool(user32.IsWindowVisible(hwnd))))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), None)
    if not out:
        return None
    if verbose:
        for hwnd, pid, title, rc, vis in out:
            print(f'      候选 hwnd=0x{hwnd:08X} vis={vis} '
                  f'rect=({rc.l},{rc.t},{rc.r},{rc.b}) title={title!r}')
    best = max(out, key=lambda x: max(0, x[3].r - x[3].l) * max(0, x[3].b - x[3].t))
    return best[0], best[1], best[2]


def dump_state(tag, pid=None):
    """采样：SystemInformer(测试副本) 有几个进程、目标 pid 有几个顶层窗口、各自什么状态。"""
    psapi.EnumProcesses.argtypes = [PV, DWORD, ctypes.POINTER(DWORD)]
    psapi.EnumProcesses.restype = BOOL

    arr = (DWORD * 4096)()
    need = DWORD(0)
    procs = []
    if psapi.EnumProcesses(ctypes.cast(arr, PV), ctypes.sizeof(arr), ctypes.byref(need)):
        n = need.value // 4
        for i in range(n):
            h = k32.OpenProcess(0x1000, False, arr[i])
            if not h:
                continue
            img = proc_image(h)
            k32.CloseHandle(h)
            if img.lower() == EXE.lower():
                procs.append(arr[i])

    wins = []

    def cb(hwnd, lp):
        p = DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if pid is None or p.value == pid:
            c = ctypes.create_unicode_buffer(256); user32.GetClassNameW(hwnd, c, 256)
            t = ctypes.create_unicode_buffer(512); user32.GetWindowTextW(hwnd, t, 512)
            rc = RECT(); user32.GetWindowRect(hwnd, ctypes.byref(rc))
            wins.append((p.value, hwnd, c.value, t.value, rc, bool(user32.IsWindowVisible(hwnd))))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), None)

    print(f'      [{tag}] 测试副本进程数={len(procs)} pids={procs}')
    if not wins:
        print(f'      [{tag}] 顶层窗口 = 无')
    for p, hwnd, cls, title, rc, vis in wins:
        print(f'      [{tag}] pid={p} hwnd=0x{hwnd:08X} {cls!r} vis={vis} '
              f'rect=({rc.l},{rc.t},{rc.r},{rc.b}) title={title[:44]!r}')


def capture(hwnd, path, crop_h=360):
    rc = RECT()
    print(f'      IsWindow={bool(user32.IsWindow(hwnd))} '
          f'可见={bool(user32.IsWindowVisible(hwnd))} 图标化={bool(user32.IsIconic(hwnd))}')
    # 强制显示：SW_RESTORE 消掉最小化态，SWP_SHOWWINDOW 才是真正「让它显示」的那一位。
    # 只发 SW_SHOWNORMAL 时实测遇到过「窗口隐藏且尺寸为 0」的状态。
    user32.ShowWindow(hwnd, 9)                          # SW_RESTORE
    user32.ShowWindow(hwnd, 5)                          # SW_SHOW
    sp = user32.SetWindowPos(hwnd, None, 40, 40, 1900, 1000,
                             0x0040 | 0x0004)            # SWP_SHOWWINDOW | SWP_NOZORDER
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    time.sleep(1.5)
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    w, h = rc.r - rc.l, rc.b - rc.t
    print(f'      SetWindowPos={sp} (err={ctypes.get_last_error()})')
    print(f'      窗口矩形 ({rc.l},{rc.t}) {w}x{h}')
    if w <= 0 or h <= 0:
        return False

    hdc_screen = user32.GetWindowDC(0)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
    hbm = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    gdi32.SelectObject(hdc_mem, hbm)

    ok = user32.PrintWindow(hwnd, hdc_mem, 2)   # PW_RENDERFULLCONTENT
    mode = 'PrintWindow(PW_RENDERFULLCONTENT)'
    if not ok:
        hdc_win = user32.GetWindowDC(hwnd)
        ok = gdi32.BitBlt(hdc_mem, 0, 0, w, h, hdc_win, 0, 0, 0x00CC0020 | 0x40000000)
        user32.ReleaseDC(hwnd, hdc_win)
        mode = 'BitBlt(窗口DC回退)'
    print(f'      抓取方式={mode} ok={ok}')

    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = w
    bmi.bmiHeader.biHeight = h
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * 4 * h)
    got = gdi32.GetDIBits(hdc_mem, hbm, 0, h, buf, ctypes.byref(bmi), 0)

    gdi32.DeleteObject(hbm)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(0, hdc_screen)

    if got != h:
        print(f'      GetDIBits 只拿到 {got} 行')
        return False

    # 行序很关键：biHeight 为正 -> buf 是“自下而上”的，图像的**顶部**在缓冲区末尾。
    # 取开头只能拿到图像最下面那几行（上一版就抓成了进程列表的中间段）。
    data = buf.raw[w * 4 * (h - crop_h):w * 4 * h]
    write_png(path, w, crop_h, data)
    print(f'      已写 {path}  ({w}x{crop_h})')
    return True


def read_controls(main_hwnd, pid):
    """直接向控件发消息读文本 —— 权威判据，不受截图/DWM 缓存影响"""
    HDM_GETITEMCOUNT = 0x1200
    HDM_GETITEMW = 0x120B
    TB_GETBUTTONCOUNT = 0x0418
    TB_GETBUTTONTEXTW = 0x044B

    class HDITEMW(ctypes.Structure):
        _fields_ = [("mask", ctypes.c_uint), ("cxy", ctypes.c_int), ("pszText", ctypes.c_wchar_p),
                    ("hbm", PV), ("cchTextMax", ctypes.c_int), ("fmt", ctypes.c_int),
                    ("lParam", ctypes.c_ssize_t), ("iImage", ctypes.c_int), ("iOrder", ctypes.c_int),
                    ("type", ctypes.c_uint), ("pvFilter", PV), ("state", ctypes.c_uint),
                    ("cFilter", ctypes.c_uint), ("pFilter", PV)]

    def send(h, m, w, l):
        return user32.SendMessageW(h, m, PV(w) if w else None, l)

    ctrls = []

    def cb(hwnd, lp):
        p = DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            c = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, c, 256)
            ctrls.append((hwnd, c.value))
        return True

    user32.EnumChildWindows(main_hwnd, WNDENUMPROC(cb), None)
    print(f'      主窗口下控件数 = {len(ctrls)}')

    for hwnd, cls in ctrls:
        if cls.lower() == 'sysheader32':
            n = send(hwnd, HDM_GETITEMCOUNT, 0, 0)
            titles = []
            for i in range(n if 0 < n < 64 else 0):
                b = ctypes.create_unicode_buffer(256)
                it = HDITEMW()
                it.mask = 0x0002
                it.pszText = ctypes.cast(b, ctypes.c_wchar_p)
                it.cchTextMax = 256
                if send(hwnd, HDM_GETITEMW, i, ctypes.cast(ctypes.byref(it), PV)):
                    titles.append(b.value)
            # 打印所有“有文本”的表头，不只看第一个
            if titles:
                print(f'      表头 hwnd=0x{hwnd:08X} {n} 列 = {titles}')

    for hwnd, cls in ctrls:
        if cls.lower() == 'toolbarwindow32':
            n = send(hwnd, TB_GETBUTTONCOUNT, 0, 0)
            if n <= 0:
                continue
            names = []
            for i in range(n if 0 < n < 64 else 0):
                b = ctypes.create_unicode_buffer(256)
                if send(hwnd, TB_GETBUTTONTEXTW, i, ctypes.cast(b, PV)) > 0:
                    names.append(b.value)
            print(f'      工具栏 hwnd=0x{hwnd:08X} {n} 个按钮 = {names}')


def tail(path, n=40, enc='utf-16'):
    if not os.path.exists(path):
        return f'(无 {os.path.basename(path)})'
    with open(path, 'rb') as fh:
        raw = fh.read()
    try:
        txt = raw.decode(enc, errors='replace')
    except Exception:
        txt = raw.decode('utf-8', errors='replace')
    lines = [l for l in txt.splitlines() if l.strip()]
    return '\n'.join(lines[:n]) if len(lines) <= n else '\n'.join(lines[:n]) + f'\n...（共 {len(lines)} 行）'


def main():
    # build.ps1 -Clean 会连 build\ 一起删掉，而截图落在 build\shots\ 下 ——
    # 不先建目录就会在写完窗口状态之后崩在 write_png 上（查到这一步之前，
    # 前面所有判定其实都已经过了，但整个脚本非零退出）。
    paths.ensure(paths.SHOTS)

    for p in (LLOG, HLOG, SHOT):
        if os.path.exists(p):
            os.remove(p)

    # 清掉测试副本里可能残留的代理 DLL，避免干扰
    for junk in ('winhttp.dll', 'winsta.dll', 'cfgmgr32.dll', 'bcrypt.dll', 'winsqlite3.dll'):
        j = os.path.join(APP, junk)
        if os.path.exists(j):
            os.remove(j)
            print(f'[i] 已从测试副本移除残留代理 {junk}')

    print(f'[+] 启动器 = {LAUNCHER}')
    print(f'[+] 目标   = {EXE}')
    proc = subprocess.Popen([LAUNCHER, '--log', '--app', EXE, '--no-wait'])
    print(f'[+] 启动器 pid = {proc.pid}')

    found = None
    noted = False
    for t in range(1, 16):
        time.sleep(2)
        if proc.poll() is not None and not noted:
            # --no-wait 下启动器「启动完就退出」是设计如此，不是失败
            noted = True
            print(f'[i] 启动器已退出 code={proc.returncode}（--no-wait 预期行为），继续等主窗口')
        found = find_main_window()
        if found:
            print(f'[+] t={t*2}s 主窗口出现 hwnd=0x{found[0]:08X} pid={found[1]} title={found[2]!r}')
            break
        print(f'    t={t*2}s 尚无主窗口')

    if not found:
        print('[!] 没找到主窗口，直接看日志')
    else:
        hwnd, pid, title = found
        # 界面是逐步建起来的：上一轮 t≈5s 就去枚举，表头和工具栏都还没建全，
        # 于是读出来「工具栏 0 个按钮」。给足 12 秒再读。
        print('      等待界面建全，每 2 秒采样一次窗口状态 ...')
        for k in range(1, 9):
            time.sleep(2)
            dump_state(f't={2+2*k}s', pid)
            if k == 4:
                capture(hwnd, SHOT)          # 先抓一张（此刻窗口通常还在）
        again = find_main_window(verbose=False)
        if again:
            if again[0] != hwnd:
                print(f'      [i] 主窗口句柄已变：0x{hwnd:08X} -> 0x{again[0]:08X}，改用新句柄')
            hwnd, pid, title = again
        found = (hwnd, pid, title)
        print('\n=== 模块检查（zh_hook.dll 是否在目标进程里）===')
        h = k32.OpenProcess(0x0410, False, pid)   # QUERY_INFORMATION | VM_READ
        if h:
            arr = (PV * 2048)()
            need = DWORD(0)
            if psapi.EnumProcessModules(h, ctypes.cast(arr, PV), ctypes.sizeof(arr), ctypes.byref(need)):
                n = need.value // ctypes.sizeof(PV)
                names = []
                for i in range(min(n, 2048)):
                    nm = ctypes.create_unicode_buffer(1024)
                    if psapi.GetModuleBaseNameW(h, arr[i], nm, 1024):
                        names.append(nm.value)
                print(f'      模块总数 = {len(names)}')
                print(f'      含 zh_hook.dll = {"zh_hook.dll" in [x.lower() for x in names]}')
            k32.CloseHandle(h)

        print('\n=== 控件实际文本（权威判据）===')
        read_controls(hwnd, pid)

        print('\n=== 截图 ===')
        capture(hwnd, SHOT)

    print('\n================ si_zh_launcher.log ================')
    print(tail(LLOG, 40, 'utf-16'))
    print('\n================ zh_hook.log（前 40 行）================')
    print(tail(HLOG, 40, 'utf-16'))

    # 收尾
    if found:
        subprocess.run(['taskkill', '/F', '/PID', str(found[1])], capture_output=True)
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    print('\n[+] 已收尾')


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    main()

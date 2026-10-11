# -*- coding: utf-8 -*-
r"""
真机端到端验证 —— 用**真实安装目录**、走**双击路径（IFEO -> 启动器 -> 注入）**。

为什么单独一个脚本（不动 verify_dict.py）：
  verify_dict.py 跑的是 test_app 副本、用 taskkill 强杀收尾。本轮要验的两件事
  恰恰只有真机 + 正常退出才能验：
    1. 退出时未命中收集是否**真的落到真实安装目录**（写路径是不是 _zhcn）；
    2. 退出钩子写出来的文件里**没有字段不全的行**（控制字符净化是否生效）。

步骤：
  1. 备份并临时改真实 settings：AllowOnlyOneInstance=0（否则第二个实例直接退出）、
     StartHidden=0（否则窗口藏在托盘，截不到图）。
  2. 删掉旧的 _zhcn\zh_dict.missed.txt，保证看到的是**本次运行**写出来的。
  3. 直接启动 D:\...\SystemInformer.exe —— 走 IFEO，不手动调启动器。
     同时在环境里塞 ZHHK_LOG / ZHHK_LOGALL：启动器只在带 --log 时才设 ZHHK_LOG，
     不带时它不覆盖，所以这里设的会被钩子继承到（用来数命中）。
  4. 截图主窗口 -> 右击进程列表（验本轮的 发送到 / 使用...扫描）
     -> WM_COMMAND 10223(ID_TAB_NEXT) ×2 切到网络页（验 启动类型/远程地址/...）。
  5. **正常关闭**（WM_CLOSE），等进程自己退出 —— 这一步才触发退出钩子。
  6. 检查 _zhcn\zh_dict.missed.txt：存在、行数、字段不全行数。
  7. 复原 settings，清理临时日志。
"""
import ctypes, json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_dict as V          # 复用枚举/截图/等待工具

import paths                   # 所有路径集中在 tools\paths.py
REAL  = paths.SI_DIR
APP   = os.path.join(REAL, "SystemInformer.exe")
ZHDIR = os.path.join(REAL, "_zhcn")
SETT  = APP + ".settings.json"
MISS  = os.path.join(ZHDIR, "zh_dict.missed.txt")
OUT   = paths.SHOTS
TMPLOG = os.path.join(os.environ.get("TEMP", REAL), "zh_hook_final.log")
BAK    = SETT + ".vfybak"      # 改 settings 前的原始值快照（见 main 里的说明）

ID_TAB_NEXT = 10223
WM_CONTEXTMENU, WM_KEYDOWN = 0x007B, 0x0100


def png(bmp):
    """BMP -> PNG（有 PIL 就转，没有就留 BMP 并说明）"""
    if not bmp or not os.path.exists(bmp):
        return None
    try:
        from PIL import Image
    except Exception:
        return bmp
    p = bmp[:-4] + ".png"
    try:
        Image.open(bmp).save(p)
        return p
    except Exception:
        return bmp


def read_miss(path):
    """-> (数据行数, 字段不全行数, 前几行样例)"""
    if not os.path.exists(path):
        return None
    lines = [l for l in open(path, encoding="utf-8-sig", errors="replace").read().splitlines()
             if l and not l.startswith("#")]
    bad = [l for l in lines if len(l.split("\t")) < 3]
    return len(lines), len(bad), lines[:6]


def rightclick_menu(main_h, pid, shots):
    """右击进程列表 -> 抓含菜单的截图；返回是否有弹出窗"""
    kids = V.enum_children(main_h)
    classes = {}
    for h in kids:
        classes.setdefault(V.win_class(h), []).append(h)
    tree = None
    for name in ("PhTreeNew", "SysListView32"):
        if name in classes:
            tree = classes[name][0]
            break
    if not tree:
        print("  !! 没找到进程列表控件，跳过右击")
        return False

    cr = V.wintypes.RECT()
    V.u32.GetClientRect.argtypes = [V.wintypes.HWND, ctypes.POINTER(V.wintypes.RECT)]
    V.u32.GetClientRect(tree, ctypes.byref(cr))
    cx, cy = 60, max(60, int((cr.bottom - cr.top) * 0.25))
    lp = (cy << 16) | (cx & 0xFFFF)

    V.u32.PostMessageW(tree, 0x0200, 0, lp)
    time.sleep(0.3)
    V.u32.PostMessageW(tree, 0x0204, 2, lp)
    time.sleep(0.3)
    V.u32.PostMessageW(tree, 0x0205, 0, lp)
    time.sleep(2.5)

    popup = None
    for h in V.enum_windows():
        if V.win_pid(h) == pid and V.u32.IsWindowVisible(h) and h != main_h:
            if V.win_class(h) == "#32768":
                popup = h
                break
    mx, my, mw, mh = V.win_rect(main_h)
    if popup:
        px, py, pw, ph = V.win_rect(popup)
        x0, y0 = min(mx, px), min(my, py)
        x1, y1 = max(mx + mw, px + pw), max(my + mh, py + ph)
    else:
        x0, y0, x1, y1 = mx, my, mx + mw + 520, my + mh
        print("  !! 未发现 #32768 弹出窗")
    b = os.path.join(shots, "11_menu_real.bmp")
    _, msg = V.capture_screen(x0, y0, min(x1 - x0, 1900), min(y1 - y0, 1300), b)
    print("  右键菜单截图 -> %s (%s)" % (png(b), msg))
    V.u32.PostMessageW(main_h, WM_KEYDOWN, 0x1B, 0)   # ESC
    time.sleep(1.2)
    return bool(popup)


def restore_settings():
    """从快照复原 settings 并删掉快照。幂等：没有快照就什么都不做。"""
    if not os.path.exists(BAK):
        return False
    with open(BAK, encoding="utf-8") as f:
        snap = json.load(f)
    with open(SETT, "w", encoding="utf-8") as f:
        json.dump(snap, f, ensure_ascii=False)
    os.remove(BAK)
    return True


def main():
    # --restore：上一次运行中途失败（被强杀 / 抛异常）后的恢复入口。
    #
    # 这里为什么必须用**独立快照文件**、而不是「下次运行时重新读 settings 当备份」：
    # 脚本自己一旦中途死掉，内存里那份备份就没了；下一次运行会把**已经被改过的值**
    # 当成原始值存下来，跑完再「复原」成改过的值 —— 错误就这样被永久固化。
    # 这个坑真踩过一次：AllowOnlyOneInstance 和 StartHidden 被留在 0（原始是 1），
    # 靠 4 月的 settings.json-bak 交叉核对才发现。所以：
    #   1) 改 settings 之前先把快照落盘；
    #   2) 有快照在 = 上次没跑完，直接拒绝启动，逼先复原；
    #   3) 正常/异常退出都走 finally 复原。
    if "--restore" in sys.argv:
        print("已从快照复原 settings" if restore_settings() else "没有待复原的快照")
        return 0
    if os.path.exists(BAK):
        print("!! 发现上次未复原的快照：%s" % BAK)
        print("   说明上一次运行中途失败了。先执行：")
        print("     python verify_final.py --restore")
        print("   它会用快照覆盖 settings，然后才允许重跑。")
        return 2

    os.makedirs(OUT, exist_ok=True)
    for p in (MISS, TMPLOG):
        if os.path.exists(p):
            os.remove(p)

    if not os.path.exists(SETT):
        print("!! 找不到 settings: %s" % SETT)
        return 1
    with open(SETT, encoding="utf-8") as f:
        snapshot = json.load(f)
    with open(BAK, "w", encoding="utf-8") as f:
        json.dump(snapshot, f, ensure_ascii=False)      # 先落盘快照，再动 settings

    try:
        return _run(snapshot)
    finally:
        if restore_settings():                          # 正常返回 / 抛异常 / Ctrl+C 都会到
            print("\nsettings 已复原（快照已删除）")


def _run(backup):
    cfg = dict(backup)
    for k in ("AllowOnlyOneInstance", "EnableStartAsAdmin", "StartHidden"):
        cfg[k] = "0"
    with open(SETT, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False)
    print("settings 已临时改：AllowOnlyOneInstance=0 EnableStartAsAdmin=0 StartHidden=0")

    env = dict(os.environ)
    env["ZHHK_LOG"] = TMPLOG
    env["ZHHK_LOGALL"] = "1"
    print("启动（走 IFEO 双击路径，未手动调启动器）: %s" % APP)
    proc = subprocess.Popen([APP], cwd=REAL, env=env, close_fds=True)

    def find_main():
        for h in V.enum_windows():
            if V.win_class(h) == "MainWindowClassName" and V.u32.IsWindowVisible(h):
                try:
                    if V.proc_path(V.win_pid(h)).lower() == APP.lower():
                        return h
                except Exception:
                    pass
        return None

    main_h = V.wait_for(find_main, 90)
    if not main_h:
        print("!! 没等到主窗口。检查 IFEO 是否还在：")
        print(subprocess.run(["reg", "query",
                              r"HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
                              r"\Image File Execution Options\SystemInformer.exe"],
                             capture_output=True, text=True, errors="replace").stdout)
        return 1                              # settings 由 finally 复原

    pid = V.win_pid(main_h)
    print("主窗口 hwnd=%#x pid=%d title=%s" % (main_h, pid, V.win_text(main_h)))
    time.sleep(4)
    b = os.path.join(OUT, "10_main_real.bmp")
    _, msg = V.capture(main_h, b)
    print("主窗口截图 -> %s (%s)" % (png(b), msg))

    V.u32.SetForegroundWindow(main_h)
    time.sleep(1)
    rightclick_menu(main_h, pid, OUT)

    # 切到网络页：Processes -> Services -> Network
    print("WM_COMMAND %d (ID_TAB_NEXT) ×2 -> 网络页" % ID_TAB_NEXT)
    for _ in range(2):
        V.u32.PostMessageW(main_h, 0x0111, ID_TAB_NEXT, 0)
        time.sleep(2.5)
    time.sleep(2)
    b = os.path.join(OUT, "12_network_real.bmp")
    _, msg = V.capture(main_h, b)
    print("网络页截图 -> %s (%s)" % (png(b), msg))

    # ---- 正常关闭，触发退出钩子 ----
    print("\n正常关闭: WM_CLOSE")
    V.u32.PostMessageW(main_h, 0x0010, 0, 0)
    exit_ok = V.wait_for(lambda: not _alive(pid), 35) is not None
    if not exit_ok:
        leftovers = [h for h in V.enum_windows()
                     if V.win_pid(h) == pid and V.u32.IsWindowVisible(h)]
        print("  !! 35 秒未退出，可见窗口 %d 个：%s"
              % (len(leftovers), [(V.win_class(h), V.win_text(h)[:40]) for h in leftovers[:6]]))
    else:
        print("  已退出（走的是退出钩子，不是强杀）")

    # ---- 检查真实安装目录里的收集文件 ----
    print("\n===== _zhcn\\zh_dict.missed.txt =====")
    r = read_miss(MISS)
    if r is None:
        print("  !! 文件不存在 —— 退出钩子没写 / 写到了别处")
    else:
        n, bad, sample = r
        print("  数据行 %d / 字段不全 %d 行" % (n, bad))
        for l in sample:
            print("    " + l[:110])

    # ---- 钩子日志 ----
    if os.path.exists(TMPLOG):
        txt = open(TMPLOG, encoding="utf-16", errors="ignore").read().splitlines()
        hit = [l for l in txt if l.startswith("[HIT ]")]
        miss = [l for l in txt if l.startswith("[miss]")]
        print("\n===== 钩子日志 =====")
        print("  行数 %d / HIT %d / miss %d" % (len(txt), len(hit), len(miss)))
        for l in txt:
            if "[dict]" in l or "退出刷新" in l or "[miss] 已载入" in l:
                print("    " + l[:150])
        newkeys = {"Start type", "Remote address", "Remote port", "Protocol",
                   "Owner", "Sen&d to", "Scan with"}
        seen = set()
        for l in hit:
            for k in newkeys:
                if '"%s"' % k in l and k not in seen:
                    seen.add(k)
                    print("    命中本轮新增: " + l[:120])
        print("  本轮新增条目命中 %d / %d 个" % (len(seen), len(newkeys)))
    else:
        print("\n!! 没有钩子日志（ZHHK_LOG 没被继承？）")

    # ---- 收尾：settings 由 main() 的 finally 统一复原，这里不再重复写 ----
    try:
        proc.terminate()
    except Exception:
        pass
    print("临时日志 %s%s" % (TMPLOG, "" if os.path.exists(TMPLOG) else "（无）"))
    return 0


def _alive(pid):
    import ctypes as _c
    h = V.k32.OpenProcess(0x1000, False, pid)     # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    V.k32.CloseHandle(h)
    return True


if __name__ == "__main__":
    sys.exit(main())

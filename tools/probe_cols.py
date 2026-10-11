# -*- coding: utf-8 -*-
r"""逐页签列头诊断：切到每个页签后，列出进程里所有 SysHeader32 的列数与其父类。

目的：搞清「树的行内容读不到列头」到底是列头不存在、还是列头列数为 0。
"""
import ctypes, json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_dict as V
import probe_headers as P

APP = P.APP
SETT = P.SETT
u32 = V.u32
ID_TAB_NEXT = 10223

TABS = ["进程", "服务", "网络", "磁盘", "防火墙", "设备"]


def all_of(clsname, h, acc):
    for c in P.children(h):
        if P.cls(c) == clsname:
            acc.append(c)
        all_of(clsname, c, acc)


def snap(tag):
    print("\n--- %s ---" % tag)
    # 每个顶层窗口各自找
    for h in V.enum_windows():
        try:
            if V.proc_path(V.win_pid(h)).lower() != APP.lower():
                continue
        except Exception:
            continue
        if P.cls(h) != "MainWindowClassName":
            continue
        hdrs, trees = [], []
        all_of("SysHeader32", h, hdrs)
        all_of("PhTreeNew", h, trees)
        print("  主窗口 %#x: PhTreeNew=%d, SysHeader32=%d" % (h, len(trees), len(hdrs)))
        for t in trees:
            par = u32.GetParent(t)
            print("     tree %#x 可见=%s 父=%s" % (t, bool(u32.IsWindowVisible(t)),
                                                   P.cls(par) if par else "-"))
        for hh in hdrs:
            par = u32.GetParent(hh)
            n = u32.SendMessageW(hh, P.HDM_GETITEMCOUNT, 0, 0)
            print("     header %#x 列数=%d 父=%s(%#x)"
                  % (hh, n, P.cls(par) if par else "-", par or 0))


def main():
    cfg = json.load(open(SETT, encoding="utf-8-sig"))
    keep = {k: cfg.get(k) for k in ("StartHidden", "AllowOnlyOneInstance", "EnableStartAsAdmin")}
    cfg["StartHidden"] = 0
    cfg["AllowOnlyOneInstance"] = 0
    cfg["EnableStartAsAdmin"] = 0
    json.dump(cfg, open(SETT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    try:
        subprocess.Popen([P.LAUNCHER, "--app", APP], cwd=P.DIST)
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
        time.sleep(5)
        for name in TABS:
            u32.PostMessageW(main_h, 0x0111, ID_TAB_NEXT, 0)
            time.sleep(3.0)
            snap(name)
        V.u32.PostMessageW(main_h, 0x0010, 0, 0)
        time.sleep(2)
    finally:
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
        print("\nsettings 已复原 %s" % keep)


if __name__ == "__main__":
    main()

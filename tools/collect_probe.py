# -*- coding: utf-8 -*-
r"""
采集「来源指纹」的一次性探针。

背景：zh_dict.missed.txt 里绝大多数是列表内容（进程名 / 服务名 / 设备名），
旧版本只记了「窗口类」，而这些都是 SysListView32，分不出谁是谁。新版钩子会连
**列表列头签名**和**所在列**一起记下来；这个脚本负责制造「各页都绘制一遍」的
机会，好让每块列表的指纹都被采到。

默认跑 **test_app 副本**（_work\hijack\test_app，与真实安装同版本），不碰
用户正在跑的实例：
  1. 用 dist\si_zh_launcher.exe --log --app <test_app exe> 启动（dist\zh_hook.dll 是新版）
  2. 用 WM_COMMAND 10223(ID_TAB_NEXT) 依次切过 6 个页签，每页停一下让它画完
  3. 右击进程列表（菜单那条路径）
  4. WM_CLOSE 正常退出（触发退出钩子）
  5. 按（控件链 + 列头 + 所在列）分组打印采集结果

settings 只改 StartHidden / AllowOnlyOneInstance / EnableStartAsAdmin 三个键，
原值先写进独立快照文件；有残留快照就拒绝启动，finally 里复原。
"""
import ctypes, json, os, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_dict as V

import paths                   # 所有路径集中在 tools\paths.py
DIST  = paths.DIST
REAL_DIR = paths.SI_DIR

# 默认跑 test_app 副本（不碰真实安装）。--real 跑真实安装，走 IFEO（等价于双击）。
USE_REAL = "--real" in sys.argv
VERIFY = "--verify" in sys.argv      # 只验证：不动已采集的未命中文件
ZHDIR = os.path.join(REAL_DIR, "_zhcn") if USE_REAL else DIST
TARGET_DIR = paths.SI_DIR if USE_REAL else paths.SI_TEST_DIR
APP   = os.path.join(TARGET_DIR, "SystemInformer.exe")
SETT  = APP + ".settings.json"
MISS  = os.path.join(ZHDIR, "zh_dict.missed.txt")
LAUNCHER = os.path.join(DIST, "si_zh_launcher.exe")
LOG   = os.path.join(ZHDIR, "zh_hook.collect.log")
SNAP  = os.path.join(ZHDIR, ".collect_probe.snapshot.json")
SHOTS = paths.SHOTS

KEYS = ("StartHidden", "AllowOnlyOneInstance", "EnableStartAsAdmin")

ID_TAB_NEXT = 10223
WM_COMMAND, WM_CLOSE, WM_CONTEXTMENU = 0x0111, 0x0010, 0x007B

TABS = ["进程", "服务", "网络", "磁盘", "防火墙", "设备"]


def png(bmp):
    """BMP -> PNG（有 PIL 就转）。"""
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


def snapshot():
    if os.path.exists(SNAP):
        print("!! 发现残留快照 %s —— 上次没跑完，settings 可能仍是被改过的。" % SNAP)
        print("   先跑 collect_probe.py --restore 复原。")
        sys.exit(2)
    d = json.load(open(SETT, encoding="utf-8-sig"))
    keep = {k: d.get(k) for k in KEYS}
    json.dump(keep, open(SNAP, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    return keep


def apply_settings():
    d = json.load(open(SETT, encoding="utf-8-sig"))
    d["StartHidden"] = 0
    d["AllowOnlyOneInstance"] = 0
    d["EnableStartAsAdmin"] = 0
    json.dump(d, open(SETT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)


def restore():
    if not os.path.exists(SNAP):
        print("没有快照，无需复原")
        return
    keep = json.load(open(SNAP, encoding="utf-8"))
    d = json.load(open(SETT, encoding="utf-8-sig"))
    for k, v in keep.items():
        if v is not None:
            d[k] = v
    json.dump(d, open(SETT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    os.remove(SNAP)
    print("已复原 settings: %s" % keep)


def find_main():
    for h in V.enum_windows():
        if V.win_class(h) == "MainWindowClassName" and V.u32.IsWindowVisible(h):
            try:
                if V.proc_path(V.win_pid(h)).lower() == APP.lower():
                    return h
            except Exception:
                pass
    return None


def kill_app():
    """测试副本收尾（找不到窗口时的兜底）。"""
    for h in V.enum_windows():
        try:
            if V.proc_path(V.win_pid(h)).lower() == APP.lower():
                V.u32.PostMessageW(h, WM_CLOSE, 0, 0)
        except Exception:
            pass


def report():
    if not os.path.exists(MISS):
        print("!! 收集文件没写出来: %s" % MISS)
        return
    lines = [l for l in open(MISS, encoding="utf-8-sig").read().splitlines()
             if l and not l.startswith("#")]
    print("\n===== 采集结果 =====")
    print("文件 %s" % MISS)
    print("数据行 %d" % len(lines))
    groups = {}
    for l in lines:
        p = l.split("\t")
        if len(p) < 4:
            continue
        chain = p[3]
        cols = p[4] if len(p) > 4 else ""
        col = p[5] if len(p) > 5 else ""
        groups.setdefault((chain, cols, col), []).append(p[0])
    print("不同（控件链 + 列头 + 所在列）组合: %d 种\n" % len(groups))
    for (chain, cols, col), items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print("-" * 76)
        print("控件链 : %s" % chain)
        print("列头   : %s" % (cols or "(无)"))
        print("所在列 : %-6s 条数 %d" % (col or "-", len(items)))
        print("样例   : %s" % ", ".join(items[:7]))

    if os.path.exists(LOG):
        txt = open(LOG, encoding="utf-16", errors="ignore").read().splitlines()
        hit = sum(1 for l in txt if l.startswith("[HIT ]"))
        print("\n钩子日志: 行数 %d / HIT %d" % (len(txt), hit))
        for l in txt:
            if "退出刷新" in l or "已载入" in l or "[dict]" in l:
                print("  " + l[:170])


def main():
    if not os.path.exists(SETT):
        print("找不到 settings: %s" % SETT)
        sys.exit(1)
    keep = snapshot()
    print("目标: %s" % APP)
    print("原始值快照: %s" % keep)

    try:
        apply_settings()
        print("临时置 StartHidden=0 / AllowOnlyOneInstance=0 / EnableStartAsAdmin=0")

        if os.path.exists(MISS) and not VERIFY:
            arch = MISS + ".before-fingerprint.txt"
            shutil.copy2(MISS, arch)
            os.remove(MISS)
            print("旧收集文件已归档 -> %s" % arch)

        for p in (LOG, os.path.join(DIST, "si_zh_launcher.log")):
            if os.path.exists(p):
                os.remove(p)

        if USE_REAL:
            env = dict(os.environ)
            env["ZHHK_LOG"] = LOG
            print("\n启动: 直接跑真实安装的 SystemInformer.exe（走 IFEO，等价双击）")
            subprocess.Popen([APP], cwd=TARGET_DIR, env=env, close_fds=True)
        else:
            print("\n启动: si_zh_launcher.exe --log --app <test_app>")
            subprocess.Popen([LAUNCHER, "--log", "--app", APP], cwd=DIST, close_fds=True)

        main_h = V.wait_for(find_main, 90)
        if not main_h:
            print("!! 没等到主窗口")
            return
        pid = V.win_pid(main_h)
        print("主窗口 hwnd=%#x pid=%d title=%s" % (main_h, pid, V.win_text(main_h)))
        time.sleep(3)

        for name in TABS:
            V.u32.PostMessageW(main_h, WM_COMMAND, ID_TAB_NEXT, 0)
            print("  切到页签 %-6s ..." % name)
            time.sleep(2.2)

        # 回到进程页再右击，把菜单那条路径也走到
        V.u32.PostMessageW(main_h, WM_COMMAND, ID_TAB_NEXT, 0)
        time.sleep(2.0)
        try:
            V.u32.SetForegroundWindow(main_h)
            time.sleep(0.5)
            b = os.path.join(SHOTS, "13_final_real.bmp" if USE_REAL else "13_final_test.bmp")
            _, msg = V.capture(main_h, b)
            print("  截图 -> %s (%s)" % (png(b), msg))
        except Exception as e:
            print("  截图失败（不影响采集）: %s" % e)
        for h in V.enum_children(main_h):
            c = V.win_class(h)
            if ("List" in c) or ("Tree" in c) or ("PhTree" in c):
                lp = (100 << 16) | 60
                V.u32.PostMessageW(h, 0x0200, 0, lp)
                time.sleep(0.3)
                V.u32.PostMessageW(h, 0x0204, 2, lp)
                time.sleep(0.3)
                V.u32.PostMessageW(h, 0x0205, 0, lp)
                print("  右击 %s 展开菜单" % c)
                time.sleep(2.5)
                V.u32.PostMessageW(main_h, 0x0100, 0x1B, 0)      # ESC
                break
        time.sleep(1.0)

        print("\n正常关闭（WM_CLOSE）...")
        V.u32.PostMessageW(main_h, WM_CLOSE, 0, 0)
        for _ in range(40):
            if not V.u32.IsWindow(main_h):
                break
            time.sleep(0.5)
        print("  已退出: %s" % (not V.u32.IsWindow(main_h)))
        time.sleep(1.0)
    finally:
        kill_app()
        restore()

    report()


if __name__ == "__main__":
    if "--restore" in sys.argv:
        restore()
    else:
        main()

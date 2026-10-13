# -*- coding: utf-8 -*-
r"""
verify_ifeo.py —— 验 si_zh_launcher.exe 的 --install / --uninstall

为什么要有它：这两个开关动的是 IFEO（恶意软件常用的持久化位置），写错一次就可能
让 System Informer **再也起不来**。所以每一档都要有可判读的落点，而且判据不能是
reg.exe 的退出码 —— 一律用 winreg 回读，值必须逐字符相等。

默认在 HKCU 下跑（靠 ZHHK_IFEO_ROOT 把键根换掉），所以**不需要管理员权限**，
也不会碰到真实的 IFEO。加 --real 才动真键（那时需要管理员）。

用法：
    python tools\verify\verify_ifeo.py           # 沙箱（HKCU），日常回归
    python tools\verify\verify_ifeo.py --real    # 真实键（HKLM），需要管理员
"""
import os
import subprocess
import sys
import winreg

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # tools\paths.py
import paths

LAUNCHER = os.path.join(paths.DIST, 'si_zh_launcher.exe')
BACKUP = os.path.join(paths.DIST, 'ifeo_backup.txt')   # 启动器目录 = dist

# 沙箱键根。真实的键根是 HKLM\...\Image File Execution Options（--real 时才用）
SANDBOX_ROOT = r'HKCU\Software\ZhhkVerify\IFEO'
REAL_ROOT = r'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'

HIVES = {
    'HKLM': winreg.HKEY_LOCAL_MACHINE,
    'HKEY_LOCAL_MACHINE': winreg.HKEY_LOCAL_MACHINE,
    'HKCU': winreg.HKEY_CURRENT_USER,
    'HKEY_CURRENT_USER': winreg.HKEY_CURRENT_USER,
}

# 一个**故意带空格和中文**的目标路径：键名要取它的文件名，值里要带引号
TARGET = r'D:\App\系统辅助\System Informer\SystemInformer.exe'

fails = []
n_ok = 0


def check(name, cond, detail=''):
    global n_ok
    if cond:
        n_ok += 1
        print('  [OK]   %s' % name)
    else:
        fails.append(name)
        print('  [FAIL] %s   %s' % (name, detail))


def split_root(key):
    hive, _, sub = key.partition('\\')
    return HIVES[hive.upper()], sub


def read_value(key, name):
    """返回 (键是否存在, 值是否存在, 值)。用 API，值不会被代码页糟蹋。"""
    hive, sub = split_root(key)
    try:
        with winreg.OpenKey(hive, sub, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            try:
                v, _ = winreg.QueryValueEx(k, name)
                return True, True, v
            except FileNotFoundError:
                return True, False, None
    except FileNotFoundError:
        return False, False, None


def key_info(key):
    """(值个数, 子键个数)；键不在则 (0, 0)"""
    hive, sub = split_root(key)
    try:
        with winreg.OpenKey(hive, sub, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            return winreg.QueryInfoKey(k)[1], winreg.QueryInfoKey(k)[0]
    except FileNotFoundError:
        return 0, 0


def delete_tree(key):
    hive, sub = split_root(key)
    try:
        winreg.DeleteKey(hive, sub)
    except OSError:
        pass
    # 逐级往上清空壳
    parts = sub.split('\\')
    for i in range(len(parts) - 1, 0, -1):
        try:
            winreg.DeleteKey(hive, '\\'.join(parts[:i]))
        except OSError:
            break


def set_value(key, name, value):
    hive, sub = split_root(key)
    with winreg.CreateKeyEx(hive, sub, 0, winreg.KEY_WRITE | winreg.KEY_WOW64_64KEY) as k:
        winreg.SetValueEx(k, name, 0, winreg.REG_SZ, value)


def launcher(root, *args, target=TARGET):
    """跑一次启动器。--elevated 让它跳过自我提权那一跳（否则会弹 UAC 卡住）。"""
    env = dict(os.environ)
    env['ZHHK_IFEO_ROOT'] = root
    cmd = [LAUNCHER, '--elevated', '--quiet'] + list(args)
    if target:
        cmd += ['--app', target]
    p = subprocess.run(cmd, env=env, capture_output=True)
    return p.returncode


def backup_text():
    if not os.path.exists(BACKUP):
        return None
    with open(BACKUP, 'rb') as fh:
        raw = fh.read()
    for enc in ('utf-8-sig', 'utf-8', 'gbk'):
        try:
            return raw.decode(enc).strip()
        except UnicodeDecodeError:
            continue
    return raw.decode('latin-1').strip()


def main():
    real = '--real' in sys.argv
    root = REAL_ROOT if real else SANDBOX_ROOT
    key = root + r'\SystemInformer.exe'
    quoted = '"%s"' % LAUNCHER

    # 动真键之前先把现状记下来，跑完原样写回（沙箱模式则一律删干净）
    orig_exists, orig_has, orig_val = read_value(key, 'Debugger')
    orig_extra = {}
    if real and orig_exists:
        hive, sub = split_root(key)
        with winreg.OpenKey(hive, sub, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            for i in range(winreg.QueryInfoKey(k)[1]):
                nm, vv, _ = winreg.EnumValue(k, i)
                if nm != 'Debugger':
                    orig_extra[nm] = vv

    print('[+] 模式   = %s' % ('真实键（HKLM）' if real else '沙箱（HKCU，无需管理员）'))
    print('[+] 键根   = %s' % root)
    print('[+] 启动器 = %s  (存在=%s)' % (LAUNCHER, os.path.exists(LAUNCHER)))
    print('[+] 目标   = %s' % TARGET)
    if not os.path.exists(LAUNCHER):
        print('!! 先跑 scripts\\build.ps1')
        return 2
    if real:
        print('[i] 真实键现状：键存在=%s 有 Debugger=%s 值=%r 其它值=%r'
              % (orig_exists, orig_has, orig_val, orig_extra))
        print('[i] 跑完会原样还原成上面这个状态')

    try:
        # ---------------------------------------------------------- 1 干净安装
        print('\n=== 1) 干净安装（键不存在）===')
        if os.path.exists(BACKUP):
            os.remove(BACKUP)
        delete_tree(key)
        rc = launcher(root, '--install')
        ke, ve, val = read_value(key, 'Debugger')
        check('退出码 = 0', rc == 0, 'rc=%s' % rc)
        check('键已创建', ke)
        check('值是带引号的启动器路径', val == quoted, '实际=%r' % (val,))
        check('备份记的是「安装前不存在该键」',
              backup_text() == '(安装前不存在该键)', '实际=%r' % (backup_text(),))

        # ---------------------------------------------------------- 2 键名跟目标走
        print('\n=== 2) 键名取目标 exe 的文件名 ===')
        # 目录含空格，文件名里也含空格 —— 键名必须与映像名逐字符相同（含空格），
        # 否则 Windows 根本不会把启动转交给调试器
        other_key = root + r'\Another App.exe'
        delete_tree(other_key)
        rc = launcher(root, '--install', target=r'C:\Tools\Sub Dir\Another App.exe')
        ke2, ve2, val2 = read_value(other_key, 'Debugger')
        check('生成了 "Another App.exe" 这个键', ke2 and ve2, '实际=%r' % (val2,))
        check('值仍是启动器路径', val2 == quoted, '实际=%r' % (val2,))
        delete_tree(other_key)

        # ---------------------------------------------------------- 3 保留附加参数
        print('\n=== 3) 已有附加参数要保留（别把 --collect 抹掉）===')
        delete_tree(key)
        set_value(key, 'Debugger', quoted + ' --collect')
        rc = launcher(root, '--install')
        ke, ve, val = read_value(key, 'Debugger')
        check('退出码 = 0', rc == 0, 'rc=%s' % rc)
        check('--collect 被保留', val == quoted + ' --collect', '实际=%r' % (val,))
        check('备份记下了原值',
              (backup_text() or '').endswith('--collect'), '实际=%r' % (backup_text(),))

        # ------------------------------------------------- 4 覆盖别的调试器要备份
        print('\n=== 4) 覆盖别人装的 Debugger ===')
        delete_tree(key)
        set_value(key, 'Debugger', r'M:\other\debugger.exe --flag')
        rc = launcher(root, '--install')
        ke, ve, val = read_value(key, 'Debugger')
        check('已被换成我们的路径', val == quoted, '实际=%r' % (val,))
        check('原值进了备份',
              backup_text() == r'原有 Debugger = M:\other\debugger.exe --flag',
              '实际=%r' % (backup_text(),))

        # ------------------------------------------------- 5 键在但没有 Debugger
        print('\n=== 5) 键存在、但没有 Debugger 值 ===')
        delete_tree(key)
        set_value(key, 'SomethingElse', 'x')
        rc = launcher(root, '--install')
        check('备份文案正确',
              backup_text() == '(该键存在但无 Debugger 值)', '实际=%r' % (backup_text(),))
        ke, ve, val = read_value(key, 'Debugger')
        check('Debugger 已写入', val == quoted, '实际=%r' % (val,))
        check('原有 SomethingElse 没被碰掉', read_value(key, 'SomethingElse')[1])
        delete_tree(key)

        # ---------------------------------------------------------- 6 卸载
        print('\n=== 6) 卸载（键会空，应连带删除）===')
        delete_tree(key)
        launcher(root, '--install')
        rc = launcher(root, '--uninstall')
        ke, ve, val = read_value(key, 'Debugger')
        check('退出码 = 0', rc == 0, 'rc=%s' % rc)
        check('Debugger 值已删除', not ve)
        check('空键已连带删除', not ke)

        # ------------------------------------------------- 7 卸载保留非空键
        print('\n=== 7) 卸载时键里还有别的值 → 保留该键 ===')
        delete_tree(key)
        set_value(key, 'Debugger', quoted)
        set_value(key, 'SomethingElse', 'keep-me')
        rc = launcher(root, '--uninstall')
        ke, ve, val = read_value(key, 'Debugger')
        check('退出码 = 0', rc == 0, 'rc=%s' % rc)
        check('Debugger 已删除', not ve)
        check('键被保留（因为还有别的值）', ke)
        check('别的值没被动', read_value(key, 'SomethingElse')[2] == 'keep-me')
        delete_tree(key)

        # ---------------------------------------------------------- 8 空卸载
        print('\n=== 8) 本来就没有 → 退出码 0，不报错 ===')
        delete_tree(key)
        rc = launcher(root, '--uninstall')
        check('退出码 = 0', rc == 0, 'rc=%s' % rc)
        check('没有凭空建出键', not read_value(key, 'Debugger')[0])

        # ------------------------------------------------- 9 默认目标名
        print('\n=== 9) 不给 --app 时，键名默认 SystemInformer.exe ===')
        delete_tree(key)
        rc = launcher(root, '--install', target=None)
        ke, ve, val = read_value(key, 'Debugger')
        check('退出码 = 0', rc == 0, 'rc=%s' % rc)
        check('键名正确', ke and ve, '实际=%r' % (val,))
        delete_tree(key)

        # ------------------------------------------------- 10 父→子的命令行接口
        print('\n=== 10) 提权那一跳传给子进程的整条命令行 ===')
        # HandleIfeo 拼出来的是这样一条命令行（AppendQuotedArg 负责给带空格的路径
        # 加引号）：
        #     "<launcher>" --install --elevated --quiet --app "<target>"
        # 把它当**字符串**直接交给 CreateProcess（subprocess 在 Windows 上收到字符串
        # 时是原样使用的，不重新拼引号），就等价于 ShellExecuteExW 传 lpParameters
        # 的那条路径 —— 于是「父进程拼参数 → 子进程解析」这一段被真正覆盖到了。
        delete_tree(key)
        raw = '"%s" --install --elevated --quiet --app "%s"' % (LAUNCHER, TARGET)
        env = dict(os.environ)
        env['ZHHK_IFEO_ROOT'] = root
        p = subprocess.run(raw, env=env, capture_output=True)
        ke, ve, val = read_value(key, 'Debugger')
        check('整条命令行能跑通（退出码 0）', p.returncode == 0, 'rc=%s' % p.returncode)
        check('键名仍取目标 exe 的文件名', ke and ve, '实际=%r' % (val,))
        check('值仍是带引号的启动器路径', val == quoted, '实际=%r' % (val,))
        delete_tree(key)

        # ------------------------------------------------- 11 参数拼错要能看出来
        print('\n=== 11) 目标路径没加引号（模拟拼错的命令行）===')
        # 这是反面用例：不加引号时路径在空格处被切成两半，键名会变成 "System"。
        # 记录下来是为了说明「为什么 Debugger 值必须带引号」不是洁癖。
        delete_tree(root + r'\System')
        delete_tree(key)
        raw = '"%s" --install --elevated --quiet --app %s' % (LAUNCHER, TARGET)
        p = subprocess.run(raw, env=env, capture_output=True)
        broken = read_value(root + r'\System', 'Debugger')
        check('不加引号确实会切坏（生成 "System" 键）', broken[1],
              '说明：这正是必须给路径加引号的原因')
        check('正确的键没有被建出来', not read_value(key, 'Debugger')[1])
        delete_tree(root + r'\System')

        # ------------------------------------------------- 12 老值未加引号
        print('\n=== 12) 老值没加引号（本机现状）→ 应升级成带引号 ===')
        delete_tree(key)
        set_value(key, 'Debugger', LAUNCHER)          # 未加引号，路径里还有空格
        rc = launcher(root, '--install')
        ke, ve, val = read_value(key, 'Debugger')
        check('退出码 = 0', rc == 0, 'rc=%s' % rc)
        check('已升级成带引号的形式', val == quoted, '实际=%r' % (val,))
        check('老值进了备份',
              backup_text() == '原有 Debugger = ' + LAUNCHER, '实际=%r' % (backup_text(),))
        delete_tree(key)

    finally:
        # 不论成败都把注册表恢复到进来时的样子 —— 包括真实模式
        delete_tree(key)
        if real and orig_exists:
            for nm, vv in orig_extra.items():
                set_value(key, nm, vv)
            if orig_has:
                set_value(key, 'Debugger', orig_val)
        print('\n[+] 已清理测试键%s' % ('（真实键已还原）' if real else ''))

    print('\n================ 结果 ================')
    print('通过 %d 项，失败 %d 项' % (n_ok, len(fails)))
    for f in fails:
        print('  FAIL: %s' % f)
    return 1 if fails else 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    sys.exit(main())

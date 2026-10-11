/*
 * si_zh_launcher.exe —— System Informer 中文显示层启动器
 * ---------------------------------------------------------------------------
 * 职责：以「已注入 zh_hook.dll、且钩子已启用」的状态把 SystemInformer.exe 启动起来。
 *
 * 为什么必须由启动器注入，而不是放一个同名 DLL 让程序自己加载：
 *   System Informer 在启动约 0.17s 时开启 ProcessSignaturePolicy
 *   = MicrosoftSignedOnly，之后任何未签名 DLL 的加载都以 0x241（577）硬失败；
 *   而且它不把程序目录纳入 DLL 搜索路径。唯一能抢在策略生效之前的动作，
 *   就是在目标进程还挂起的时候注入。
 *
 * 为什么创建时要用 DEBUG_ONLY_THIS_PROCESS：
 *   IFEO 的 Debugger 语义是「把靶子替换成调试器」。调试器再 CreateProcessW 靶子时
 *   IFEO 会再次套用 —— 靶子又被换成调试器本身，于是无限自我复制、靶子永远起不来。
 *   实测：plain 与 CREATE_SUSPENDED 两臂都会递归，只有带 DEBUG_ONLY_THIS_PROCESS
 *   的创建方式会跳过 IFEO 的 Debugger 替换。它不是「顺手加的旗标」，
 *   而是 IFEO 方案能否成立的唯一支点。
 *
 * 时序保证（本文件存在的意义）：
 *   1. DEBUG_ONLY_THIS_PROCESS 创建 -> 主线程停在初始断点，目标自己的代码一行未执行；
 *   2. 在这个窗口里注入 zh_hook.dll 并完成 Ready/Go/Done 握手，等钩子真正启用；
 *   3. 再分离调试器、放行主线程。
 *   与之相对的做法（注入后立刻 ResumeThread、钩子自己在后台 Sleep 一阵再启用）
 *   是让「界面还没建出来」跟「钩子已启用」赛跑 —— 本启动器把这段顺序变成确定性的。
 *
 * 用法：
 *   si_zh_launcher.exe                    启动 <启动器目录>\..\SystemInformer.exe
 *   si_zh_launcher.exe <target.exe> [args] (IFEO 就是这样调用我们的)
 *   si_zh_launcher.exe --app <path> [args]
 *   选项： --log      写启动器日志与钩子日志（排障用，平时别开）
 *          --no-wait  启动完就退出，不驻留等待 System Informer 结束
 *          --check    只显示解析到的配置，不启动（用来自检）
 *
 * 图标与版本信息见 si_zh_launcher.rc；--check 的提示框用 MB_USERICON 显示它。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "resource.h"

#define EV_READY L"Local\\ZhHook_Ready"
#define EV_GO    L"Local\\ZhHook_Go"
#define EV_DONE  L"Local\\ZhHook_Done"

#define T_READY_MS 20000
#define T_DONE_MS  30000
#define T_DEBUG_MS 20000

static wchar_t g_dir[MAX_PATH];         /* 启动器所在目录（zh_hook.dll / zh_dict.txt 在此） */
static wchar_t g_logPath[MAX_PATH];
static BOOL    g_logOn = FALSE;

/* ------------------------------------------------------------------ 工具 */

static void DirOf(const wchar_t* path, wchar_t* out, DWORD cap)
{
    wchar_t* s;
    wcsncpy_s(out, cap, path, _TRUNCATE);
    s = wcsrchr(out, L'\\');
    if (s) *s = L'\0';
}

static BOOL Exists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void Join(wchar_t* out, DWORD cap, const wchar_t* dir, const wchar_t* name)
{
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", dir, name);
}

static void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[4096], line[4300];
    va_list ap;
    HANDLE h;
    DWORD w;

    if (!g_logOn) return;

    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    _snwprintf_s(line, _countof(line), _TRUNCATE, L"[%8llu] %s\r\n",
                 (unsigned long long)GetTickCount64(), buf);

    h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &w, NULL);
    CloseHandle(h);
}

/* 主窗口是否已出现（用来证明「钩子装好时界面还没建」） */
static BOOL g_foundWin;
static DWORD g_wantPid;

static BOOL CALLBACK EnumCb(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    (void)lp;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == g_wantPid && GetWindow(hwnd, GW_OWNER) == NULL && IsWindowVisible(hwnd))
        g_foundWin = TRUE;
    return TRUE;
}

static BOOL HasVisibleTopWindow(DWORD pid)
{
    g_foundWin = FALSE;
    g_wantPid = pid;
    EnumWindows(EnumCb, 0);
    return g_foundWin;
}

/* 目标进程里已经加载了多少模块 —— 用来判断「加载器走到哪一步了」 */
static int CountModules(HANDLE hProc)
{
    HMODULE mods[1024];
    DWORD need = 0;
    if (!hProc) return -1;
    if (!EnumProcessModules(hProc, mods, sizeof(mods), &need)) return -1;
    return (int)(need / sizeof(HMODULE));
}

/* ------------------------------------------------------------------ 注入 */

static BOOL InjectDll(HANDLE hProc, const wchar_t* dllPath, DWORD* outErr)
{
    SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote;
    HANDLE ht;
    DWORD code = 0;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC pLoad;

    *outErr = 0;

    remote = VirtualAllocEx(hProc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { *outErr = GetLastError(); return FALSE; }

    if (!WriteProcessMemory(hProc, remote, dllPath, bytes, NULL))
    {
        *outErr = GetLastError();
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return FALSE;
    }

    pLoad = GetProcAddress(k32, "LoadLibraryW");
    if (!pLoad) { *outErr = GetLastError(); VirtualFreeEx(hProc, remote, 0, MEM_RELEASE); return FALSE; }

    ht = CreateRemoteThread(hProc, NULL, 0, (LPTHREAD_START_ROUTINE)pLoad, remote, 0, NULL);
    if (!ht)
    {
        *outErr = GetLastError();
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return FALSE;
    }

    {
        DWORD w = WaitForSingleObject(ht, 30000);
        if (w != WAIT_OBJECT_0)
        {
            /* 远程 LoadLibraryW 没能在 30 秒内返回 —— 几乎总是「目标此刻持有加载器锁」。
             * 绝不能把它当成成功：那样握手会一路超时，而日志里看不出任何线索
             * （这正是只给 DEBUG_ONLY_THIS_PROCESS 时的实际表现）。 */
            *outErr = (w == WAIT_TIMEOUT) ? ERROR_TIMEOUT : GetLastError();
            CloseHandle(ht);
            VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
            return FALSE;
        }
    }
    GetExitCodeThread(ht, &code);
    CloseHandle(ht);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);

    if (code == 0) { *outErr = GetLastError(); return FALSE; }
    return TRUE;
}

/* ------------------------------------------------------------------ 主体 */

static int Run(int argc, wchar_t** argv)
{
    wchar_t target[MAX_PATH] = L"";
    wchar_t targetDir[MAX_PATH];
    wchar_t dllPath[MAX_PATH], dictPath[MAX_PATH];
    wchar_t cmdline[32768];
    wchar_t childArgs[16384];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DEBUG_EVENT de;
    HANDLE evReady = NULL, evGo = NULL, evDone = NULL;
    BOOL log = FALSE, noWait = FALSE, check = FALSE;
    DWORD err = 0;
    int i, rc = 0;

    /* ---- 参数 ---- */
    childArgs[0] = L'\0';
    {
        BOOL haveTarget = FALSE;
        for (i = 1; i < argc; i++)
        {
            if (_wcsicmp(argv[i], L"--log") == 0)        { log = TRUE; continue; }
            if (_wcsicmp(argv[i], L"--no-wait") == 0)    { noWait = TRUE; continue; }
            if (_wcsicmp(argv[i], L"--check") == 0)      { check = TRUE; continue; }
            if (_wcsicmp(argv[i], L"--app") == 0 && i + 1 < argc)
            {
                wcsncpy_s(target, MAX_PATH, argv[++i], _TRUNCATE);
                haveTarget = TRUE;
                continue;
            }
            if (!haveTarget)
            {
                wcsncpy_s(target, MAX_PATH, argv[i], _TRUNCATE);
                haveTarget = TRUE;
                continue;
            }
            /* 其余全部原样转给 System Informer */
            if (childArgs[0]) wcscat_s(childArgs, _countof(childArgs), L" ");
            wcscat_s(childArgs, _countof(childArgs), argv[i]);
        }
    }

    /* ---- 解析路径 ---- */
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        DirOf(self, g_dir, MAX_PATH);
        Join(g_logPath, MAX_PATH, g_dir, L"si_zh_launcher.log");
    }
    g_logOn = log;
    if (g_logOn) DeleteFileW(g_logPath);

    if (!target[0])
    {
        wchar_t up[MAX_PATH];
        Join(up, MAX_PATH, g_dir, L"..");
        Join(target, MAX_PATH, up, L"SystemInformer.exe");
        if (!Exists(target))
            Join(target, MAX_PATH, g_dir, L"SystemInformer.exe");
    }

    if (GetEnvironmentVariableW(L"ZHHK_DLL", dllPath, MAX_PATH) == 0)
        Join(dllPath, MAX_PATH, g_dir, L"zh_hook.dll");
    if (GetEnvironmentVariableW(L"ZHHK_DICT", dictPath, MAX_PATH) == 0)
        Join(dictPath, MAX_PATH, g_dir, L"zh_dict.txt");

    DirOf(target, targetDir, MAX_PATH);

    Log(L"===== si_zh_launcher 启动 =====");
    Log(L"启动器目录 = %s", g_dir);
    Log(L"目标       = %s", target);
    Log(L"钩子 DLL   = %s (存在=%d)", dllPath, Exists(dllPath));
    Log(L"词典       = %s (存在=%d)", dictPath, Exists(dictPath));
    Log(L"子参数     = [%s]", childArgs);

    if (!Exists(target))
    {
        Log(L"!! 目标不存在，放弃");
        if (check) MessageBoxW(NULL, target, L"si_zh_launcher: 找不到 SystemInformer.exe", MB_ICONERROR);
        return 2;
    }
    if (!Exists(dllPath))
    {
        Log(L"!! 钩子 DLL 不存在，放弃");
        if (check) MessageBoxW(NULL, dllPath, L"si_zh_launcher: 找不到 zh_hook.dll", MB_ICONERROR);
        return 3;
    }

    if (check)
    {
        wchar_t msg[2048];
        _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                     L"目标：\n%s\n\n钩子 DLL：\n%s\n\n词典：\n%s\n\n日志：%s",
                     target, dllPath, dictPath, log ? L"开" : L"关");
        MessageBoxW(NULL, msg, L"si_zh_launcher --check", MB_OK | MB_USERICON);
        return 0;
    }

    /* ---- 子进程环境：这正是旧 POC 靠 os.environ 传、容易漏的东西 ---- */
    SetEnvironmentVariableW(L"ZHHK_DICT", dictPath);
    SetEnvironmentVariableW(L"ZHHK_TRANSLATE", L"1");
    if (log)
    {
        wchar_t hookLog[MAX_PATH];
        Join(hookLog, MAX_PATH, g_dir, L"zh_hook.log");
        DeleteFileW(hookLog);
        SetEnvironmentVariableW(L"ZHHK_LOG", hookLog);
    }

    /* ---- 握手事件：必须在注入之前建好，DLL 只会 OpenEvent 不会 Create ---- */
    evReady = CreateEventW(NULL, TRUE, FALSE, EV_READY);
    evGo    = CreateEventW(NULL, TRUE, FALSE, EV_GO);
    evDone  = CreateEventW(NULL, TRUE, FALSE, EV_DONE);
    if (!evReady || !evGo || !evDone)
    {
        Log(L"!! 创建握手事件失败 err=%lu", GetLastError());
        return 4;
    }

    _snwprintf_s(cmdline, _countof(cmdline), _TRUNCATE, L"\"%s\"%s%s",
                 target, childArgs[0] ? L" " : L"", childArgs);

    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    /* 两个旗标缺一不可：
     *   DEBUG_ONLY_THIS_PROCESS —— 绕过 IFEO 的 Debugger 二次替换（lab/exp2 实测，
     *                              plain 与 suspended 两臂都会递归，只有它不会）
     *   CREATE_SUSPENDED        —— 把主线程停在「加载器初始化之前」这个点
     *
     * 只给 DEBUG 而不给 CREATE_SUSPENDED 会踩一个很隐蔽的坑（实测踩过）：
     * 主线程已经跑进加载器、停在初始断点上，**加载器锁此时被持有**；我们再远程
     * LoadLibraryW 就会被永久挡住 —— 现象是启动器日志停在「调试事件 code=3」之后
     * 再无任何输出，而 zh_hook.log 根本没被创建（DllMain 从未执行）。
     * 加上 CREATE_SUSPENDED 后，主线程连加载器都还没进，锁是空的，注入立刻成功。 */
    if (!CreateProcessW(target, cmdline, NULL, NULL, FALSE,
                        DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED, NULL, targetDir, &si, &pi))
    {
        err = GetLastError();
        Log(L"!! CreateProcessW 失败 err=%lu", err);
        return 5;
    }
    Log(L"已创建（调试态 + 主线程挂起）pid=%lu tid=%lu", pi.dwProcessId, pi.dwThreadId);

    /* ---- 关键顺序：**先分离调试器，再注入** ----
     * 反过来做会永久卡死：只要进程还处在「调试态且调试事件未被消费」的状态，内核就
     * 压着它，我们的远程 LoadLibraryW 永远不返回（现象：日志停在“注入前状态”之后
     * 再无输出，zh_hook.log 根本不生成）。实测 lab/injmodes.py：
     *   纯 DEBUG                 -> 注入卡死 >8s，探针 DllMain 从未执行
     *   DEBUG | CREATE_SUSPENDED -> 同样卡死
     *   分离之后立刻重试          -> 4ms 返回
     *
     * 分离**不会**放行 CREATE_SUSPENDED 的主线程 —— 同一实验的 E 臂证明：分离后
     * 0.8s 目标仍未执行它 wmain 的第一行。所以「先分离、再注入、最后放行」既解掉
     * 了卡死，又保住了时序。 */
    ZeroMemory(&de, sizeof(de));
    de.dwThreadId = pi.dwThreadId;

    if (!DebugActiveProcessStop(pi.dwProcessId))
    {
        Log(L"分离失败 err=%lu，改为先消费一次调试事件再分离", GetLastError());
        if (WaitForDebugEvent(&de, 2000))
        {
            Log(L"调试事件 code=%lu tid=%lu", de.dwDebugEventCode, de.dwThreadId);
            if (de.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && de.u.CreateProcessInfo.hFile)
                CloseHandle(de.u.CreateProcessInfo.hFile);
            ContinueDebugEvent(pi.dwProcessId, de.dwThreadId, DBG_CONTINUE);
        }
        if (!DebugActiveProcessStop(pi.dwProcessId))
            Log(L"!! 二次分离仍失败 err=%lu", GetLastError());
        else
            Log(L"已分离（二次）");
    }
    else
    {
        Log(L"已分离（调试态解除，主线程仍挂起）");
    }

    Log(L"注入前状态：有可见主窗口=%d，已加载模块数=%d，目标第一行代码尚未执行",
        HasVisibleTopWindow(pi.dwProcessId), CountModules(pi.hProcess));

    /* ---- 注入 ---- */
    if (!InjectDll(pi.hProcess, dllPath, &err))
    {
        Log(L"!! 注入失败 err=%lu", err);
        if (err == ERROR_TIMEOUT)
        {
            /* 加载器锁被占死，目标已经救不回来 —— 结束它，别留一个卡死的 System Informer */
            Log(L"!! 目标加载器锁被占死，终止目标以防留下卡死进程");
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            return 7;
        }
        Log(L"照常放行（只是没有中文）");
        goto release;
    }
    Log(L"注入成功，等待 DLL 建钩 (Ready) ...");

    /* ---- 握手：Ready -> Go -> Done ---- */
    {
        DWORD r1 = WaitForSingleObject(evReady, T_READY_MS);
        DWORD r2, r3;
        Log(L"Ready r=%lu (%s)", r1, r1 == WAIT_OBJECT_0 ? L"钩子已建好" : L"超时");
        if (r1 != WAIT_OBJECT_0)
        {
            Log(L"!! DLL 未就绪，照常放行");
            goto release;
        }
        SetEvent(evGo);
        r2 = WaitForSingleObject(evDone, T_DONE_MS);
        (void)r2;
        r3 = WaitForSingleObject(evDone, 0);
        Log(L"Done r=%lu —— 钩子已启用；此刻有可见主窗口=%d（应为 0）",
            r2, HasVisibleTopWindow(pi.dwProcessId));
    }

release:
    /* 到这里才放行主线程：目标此时才第一次执行自己的代码，而钩子早已启用 */
    {
        DWORD rs = ResumeThread(pi.hThread);
        if (rs == (DWORD)-1)
            Log(L"!! ResumeThread 失败 err=%lu", GetLastError());
        else
            Log(L"主线程已放行（旧挂起计数=%lu）", rs);
    }

    CloseHandle(evReady); CloseHandle(evGo); CloseHandle(evDone);

    if (noWait)
    {
        Log(L"--no-wait：立即退出，pid=%lu 继续独立运行", pi.dwProcessId);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 0;
    }

    Log(L"等待目标退出 ...");
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, (DWORD*)&rc);
    Log(L"目标已退出 code=%d", rc);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return rc;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmd, int show)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc;

    (void)hi; (void)hp; (void)cmd; (void)show;

    if (!argv) return 1;
    rc = Run(argc, argv);
    LocalFree(argv);
    return rc;
}

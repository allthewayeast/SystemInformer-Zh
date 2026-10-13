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
 *   选项： --install    装 IFEO 劫持（写注册表），不启动程序
 *          --uninstall  卸 IFEO 劫持（删注册表），不启动程序
 *          --log      写启动器日志与钩子日志（排障用，平时别开）
 *          --no-wait  启动完就退出，不驻留等待 System Informer 结束
 *          --check    只显示解析到的配置，不启动（用来自检）
 *          --collect  打开「运行时未命中收集」（默认关，见下）
 *          --quiet    不弹结果对话框，只回退出码（给脚本用）
 *
 * --install / --uninstall（详见下面「IFEO 劫持的装与卸」一节）
 *
 * --collect 与运行时未命中收集
 *   收集器会把界面上出现过、但词典里没有的串持续追加到 zh_dict.missed.txt。
 *   那是采集行为，不该「装好就一直在写」，所以**默认关**，只有带 --collect
 *   启动时才开：本参数把子进程的 ZHHK_MISS 设成 1，钩子据此打开收集。
 *   （不带参数的默认是 ZHHK_MISS=0；不走启动器直接加载 DLL 时同样默认关。）
 *
 *   走 IFEO 双击启动时，参数写在 Debugger 值里即可。IFEO 的语义是「把
 *   Debugger 整串插到原命令行前面」（Raymond Chen《Image File Execution
 *   Options just inserts the debugger in front of the command line》），
 *   所以末尾追加参数是合法的：
 *
 *     Debugger = "D:\...\_zhcn\si_zh_launcher.exe" --collect
 *
 *   脚本做法见 scripts\collect.ps1（-On / -Off），它只改这一个值。
 *   注意 Debugger 一旦指向不存在的文件，程序会直接起不来（不会回退去启动
 *   原 exe）—— 所以改完务必用 scripts\verify-ifeo.ps1 自检。
 *
 * IFEO 劫持的装与卸
 *   以前这一步只能靠 scripts\install.ps1（要先自己开一个管理员 PowerShell）。
 *   现在启动器自己能做：
 *
 *     si_zh_launcher.exe --install      装（会弹一次 UAC）
 *     si_zh_launcher.exe --uninstall    卸
 *
 *   装的时候读的、写的都是同一个键：
 *
 *     HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\
 *         <目标映像名>
 *       Debugger = "<本启动器完整路径>" [原有附加参数]
 *
 *   键名取**目标 exe 的文件名**（默认 SystemInformer.exe，可用 --app 指定别的），
 *   与 install.ps1 的语义一致。原有附加参数会被保留 —— 例如先用
 *   scripts\collect.ps1 挂上了 --collect，再跑 --install 不会把它抹掉。
 *   改之前的老值会备份到 <启动器目录>\ifeo_backup.txt，格式与 install.ps1 相同
 *   （卸载脚本认得它，会据此提示该恢复成什么）。
 *
 *   三个刻意的设计选择：
 *     1. 对注册表的**改动**一律走 reg.exe（见下面实现处的注释），读取走 Win32 API；
 *     2. 写之前先判令牌，没提权就 runas 把自己拉起来，所以「双击 → 点 UAC」即可，
 *        不需要用户自己去找管理员命令行；
 *     3. 写完**回读校验**才报成功 —— reg.exe 的退出码不是唯一判据。
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

/* --------------------------------------------------------- IFEO 劫持注册
 *
 * --install / --uninstall 都在这里。三个刻意的选择，先讲清楚免得被当成随手写的：
 *
 * 1) **改动走 reg.exe，读取走 Win32 API。**
 *    改动用 reg.exe 是刻意的：每一步都是一条看得见、能照抄、能手动复现的命令行，
 *    排障时不用猜程序背地里写了什么。但读取不能走 reg.exe —— `reg query` 的输出是
 *    控制台代码页编码的，而值里含中文路径；解析它既不可靠又没必要，API 是精确的。
 *    另外要求 JIT 提权判定也本来就要 advapi32，所以这不引入新的依赖面。
 *
 * 2) **写完必须回读校验。** reg.exe 的退出码不是判据：组策略禁止注册表工具、
 *    或安全软件拦 IFEO 写入时，退出码可能好看而值没落地。回读通过才算成功。
 *
 * 3) **键名取目标 exe 的文件名**，与 install.ps1 一致；已有附加参数会被保留，
 *    免得「先 collect.ps1 On 再 --install」把 --collect 悄悄抹掉。
 */
#define IFEO_ROOT_DEFAULT \
    L"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options"
#define IFEO_VALUE_NAME L"Debugger"
#define IFEO_BACKUP     L"ifeo_backup.txt"

static const wchar_t* BaseName(const wchar_t* p)
{
    const wchar_t* a = wcsrchr(p, L'\\');
    const wchar_t* b = wcsrchr(p, L'/');
    const wchar_t* s = (a && b) ? (a > b ? a : b) : (a ? a : b);
    return s ? s + 1 : p;
}

/* 键根。ZHHK_IFEO_ROOT 是**测试钩子**：把它指到 HKCU 下就能在没有管理员权限的
 * 情况下跑完整流程（本机就是这种情况）。正常使用不要设它。 */
static const wchar_t* IfeoRoot(void)
{
    static wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(L"ZHHK_IFEO_ROOT", buf, _countof(buf));
    if (n > 0 && n < _countof(buf)) return buf;
    return IFEO_ROOT_DEFAULT;
}

/* 把 "HKLM\\SOFTWARE\\..." 拆成 HKEY_* 与子键路径；不认识的根返回 NULL。 */
static HKEY SplitRoot(const wchar_t* path, wchar_t* sub, size_t subCap)
{
    static const struct { const wchar_t* name; HKEY h; } map[] = {
        { L"HKLM", HKEY_LOCAL_MACHINE }, { L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE },
        { L"HKCU", HKEY_CURRENT_USER  }, { L"HKEY_CURRENT_USER",  HKEY_CURRENT_USER  },
        { L"HKCR", HKEY_CLASSES_ROOT  }, { L"HKEY_CLASSES_ROOT",  HKEY_CLASSES_ROOT  },
        { L"HKU",  HKEY_USERS         }, { L"HKEY_USERS",         HKEY_USERS         },
    };
    size_t i;

    for (i = 0; i < _countof(map); i++)
    {
        size_t n = wcslen(map[i].name);
        if (_wcsnicmp(path, map[i].name, n) == 0 && (path[n] == L'\\' || path[n] == L'\0'))
        {
            const wchar_t* s = path + n;
            while (*s == L'\\') s++;
            wcsncpy_s(sub, subCap, s, _TRUNCATE);
            return map[i].h;
        }
    }
    return NULL;
}

/* 读：键在不在、值在不在、值是什么。都用 API，见上面的第 1 条。 */
static void ProbeIfeo(const wchar_t* keyPath, const wchar_t* valueName,
                      BOOL* keyExists, BOOL* valueExists, wchar_t* value, DWORD valueCap)
{
    HKEY hive, hk = NULL;
    wchar_t sub[1024];

    if (keyExists)   *keyExists = FALSE;
    if (valueExists) *valueExists = FALSE;
    if (value && valueCap) value[0] = L'\0';

    hive = SplitRoot(keyPath, sub, _countof(sub));
    if (!hive) return;
    if (RegOpenKeyExW(hive, sub, 0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS) return;
    if (keyExists) *keyExists = TRUE;

    if (value && valueCap)
    {
        DWORD type = 0, cb = valueCap * (DWORD)sizeof(wchar_t);
        LONG r = RegQueryValueExW(hk, valueName, NULL, &type, (LPBYTE)value, &cb);
        if ((r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) ||
            r == ERROR_MORE_DATA)                 /* 值在，只是比我们的缓冲长 */
        {
            if (valueExists) *valueExists = TRUE;
        }
        else
        {
            value[0] = L'\0';
        }
    }
    RegCloseKey(hk);
}

/* 键下既无值也无子键 —— 与 uninstall.ps1 的 ValueCount/SubKeyCount 判据等价 */
static BOOL KeyIsEmpty(const wchar_t* keyPath)
{
    HKEY hive, hk = NULL;
    wchar_t sub[1024];
    DWORD nSub = 0, nVal = 0;

    hive = SplitRoot(keyPath, sub, _countof(sub));
    if (!hive) return FALSE;
    if (RegOpenKeyExW(hive, sub, 0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS)
        return TRUE;                             /* 键已经没了，视作空 */
    RegQueryInfoKeyW(hk, NULL, NULL, NULL, &nSub, NULL, NULL, &nVal, NULL, NULL, NULL, NULL);
    RegCloseKey(hk);
    return (nSub == 0 && nVal == 0);
}

static BOOL IsElevated(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    DWORD n = 0;
    BOOL r = FALSE;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
    {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n))
            r = (el.TokenIsElevated != 0);
        CloseHandle(tok);
    }
    return r;
}

/* 把 s 当成一个**独立命令行参数**追加到 dst：外面套引号，内部的 " 用 \" 转义，
 * 且引号前的连续 \ 要翻倍 —— MSVC CRT（也就是 reg.exe、CommandLineToArgvW）
 * 的解析规则。不这么做，值里的引号会被悄悄吃掉：实测 `/d ""C:\a b\x.exe""`
 * 落到注册表里是没有引号的 `C:\a b\x.exe`，而我们要的正是带引号的形式。 */
static void AppendQuotedArg(wchar_t* dst, size_t cap, const wchar_t* s)
{
    size_t n = wcslen(dst);
    size_t bs = 0;
    const wchar_t* p;

    #define PUT(ch) do { if (n + 1 < cap) dst[n++] = (ch); } while (0)

    PUT(L'"');
    for (p = s; *p; ++p)
    {
        if (*p == L'\\') { bs++; continue; }
        if (*p == L'"')
        {
            size_t k;
            for (k = 0; k < bs * 2 + 1; k++) PUT(L'\\');   /* 2n 个 \ ＋ 转义那一个 */
            PUT(L'"');
            bs = 0;
            continue;
        }
        while (bs > 0) { PUT(L'\\'); bs--; }               /* 不在引号前，\ 原样 */
        PUT(*p);
    }
    while (bs > 0) { PUT(L'\\'); PUT(L'\\'); bs--; }       /* 末尾的 \ 要翻倍，免得吃掉收尾引号 */
    PUT(L'"');
    if (n < cap) dst[n] = L'\0';

    #undef PUT
}

/* 把一个 Debugger 值拆成「exe 路径」与「后面的参数」。未加引号又有空格的老写法
 * 在这里只能切到第一个空格 —— 那正是未加引号本身的歧义，我们据此保守处理：
 * 切出来的路径与本启动器不符就不继承参数。 */
static void SplitExeAndArgs(const wchar_t* value, wchar_t* exe, size_t exeCap, const wchar_t** restOut)
{
    const wchar_t* p = value;
    size_t n = 0;

    while (*p == L' ' || *p == L'\t') p++;
    if (*p == L'"')
    {
        p++;
        while (*p && *p != L'"' && n + 1 < exeCap) exe[n++] = *p++;
        if (*p == L'"') p++;
    }
    else
    {
        while (*p && *p != L' ' && *p != L'\t' && n + 1 < exeCap) exe[n++] = *p++;
    }
    exe[n] = L'\0';
    while (*p == L' ' || *p == L'\t') p++;
    *restOut = p;
}

static void RegExePath(wchar_t* out, DWORD cap)
{
    wchar_t sys[MAX_PATH];
    if (GetSystemDirectoryW(sys, _countof(sys)) == 0)
        wcsncpy_s(sys, _countof(sys), L"C:\\Windows\\System32", _TRUNCATE);
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\reg.exe", sys);
}

/* 跑一条命令行并把 stdout+stderr 收回来。reg.exe 的输出远小于管道缓冲，所以
 * 「先读到 EOF 再等退出」不会死锁。返回 FALSE = 进程压根没起来。 */
static BOOL RunCapture(const wchar_t* cmdline, wchar_t* outText, DWORD outCap, DWORD* outExit)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, nul = INVALID_HANDLE_VALUE;
    char* raw;
    wchar_t* line;
    DWORD total = 0, got = 0;
    BOOL ok;

    if (outText && outCap) outText[0] = L'\0';
    if (outExit) *outExit = (DWORD)-1;

    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 8192)) return FALSE;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   /* 读端若被继承，我们永远等不到 EOF */
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, NULL);

    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = (nul == INVALID_HANDLE_VALUE) ? NULL : nul;
    si.hStdOutput = wr;
    si.hStdError  = wr;

    line = (wchar_t*)malloc((wcslen(cmdline) + 1) * sizeof(wchar_t));
    if (line) wcscpy_s(line, wcslen(cmdline) + 1, cmdline);

    ZeroMemory(&pi, sizeof(pi));
    ok = (line != NULL) && CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                          NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    free(line);

    if (!ok) { CloseHandle(rd); return FALSE; }

    raw = (char*)malloc(65536);
    if (raw)
    {
        while (total < 65535)
        {
            if (!ReadFile(rd, raw + total, 65535 - total, &got, NULL) || got == 0) break;
            total += got;
        }
        raw[total] = '\0';
    }
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, 60000);
    if (outExit) GetExitCodeProcess(pi.hProcess, outExit);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);

    if (raw && outText && outCap > 1)
    {
        UINT cp = GetConsoleOutputCP();
        int n;
        if (cp == 0) cp = GetACP();                 /* 没有控制台时按 ANSI 代码页 */
        n = MultiByteToWideChar(cp, 0, raw, (int)total, outText, (int)outCap - 1);
        if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, raw, (int)total, outText, (int)outCap - 1);
        if (n < 0 || n > (int)outCap - 1) n = 0;
        outText[n] = L'\0';
        while (n > 0 && (outText[n - 1] == L'\r' || outText[n - 1] == L'\n' ||
                         outText[n - 1] == L' '  || outText[n - 1] == L'\t'))
            outText[--n] = L'\0';
    }
    free(raw);
    return TRUE;
}

/* 备份/读取 ifeo_backup.txt —— 格式与 install.ps1 / uninstall.ps1 互通 */
static void WriteBackupFile(const wchar_t* dir, const wchar_t* text)
{
    wchar_t path[MAX_PATH];
    char utf8[4096];
    int n;
    HANDLE h;
    DWORD w;

    Join(path, MAX_PATH, dir, IFEO_BACKUP);
    n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, (int)sizeof(utf8), NULL, NULL);
    if (n <= 1) return;
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, utf8, (DWORD)(n - 1), &w, NULL);       /* 不写结尾的 NUL */
    CloseHandle(h);
}

static void ReadBackupHint(const wchar_t* dir, wchar_t* out, DWORD cap)
{
    wchar_t path[MAX_PATH];
    char raw[4096];
    DWORD got = 0;
    HANDLE h;

    out[0] = L'\0';
    Join(path, MAX_PATH, dir, IFEO_BACKUP);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    ReadFile(h, raw, sizeof(raw) - 1, &got, NULL);
    CloseHandle(h);
    raw[got] = '\0';

    {
        char* p = raw;
        int n;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
            p += 3;
        n = MultiByteToWideChar(CP_UTF8, 0, p, -1, out, (int)cap - 1);
        if (n <= 0) MultiByteToWideChar(CP_ACP, 0, p, -1, out, (int)cap - 1);
        out[cap - 1] = L'\0';
        for (wchar_t* q = out; *q; ++q)
            if (*q == L'\r' || *q == L'\n' || *q == L'\t') *q = L' ';
    }
}

static int HandleIfeo(const wchar_t* selfDir, const wchar_t* target,
                      BOOL install, BOOL quiet, BOOL noElevate)
{
    wchar_t selfExe[MAX_PATH], regExe[MAX_PATH], keyPath[1200], backupPath[MAX_PATH];
    wchar_t cmd[8192], out[4096], msg[4096];
    wchar_t oldValue[2048], newValue[2048];
    wchar_t dllPath[MAX_PATH];
    const wchar_t* keyName;
    const wchar_t* title = install ? L"si_zh_launcher --install" : L"si_zh_launcher --uninstall";
    BOOL keyExists = FALSE, valueExists = FALSE;
    DWORD ec = 0;
    int rc = 0;

    GetModuleFileNameW(NULL, selfExe, MAX_PATH);
    RegExePath(regExe, MAX_PATH);
    Join(backupPath, MAX_PATH, selfDir, IFEO_BACKUP);
    keyName = BaseName(target);
    _snwprintf_s(keyPath, _countof(keyPath), _TRUNCATE, L"%s\\%s", IfeoRoot(), keyName);

    /* ---- 提权：IFEO 在 HKLM 下，不提权写不进去 ---- */
    Log(L"IFEO: install=%d target=%s", install, target);
    Log(L"IFEO: 键 = %s", keyPath);

    if (!IsElevated() && !noElevate)
    {
        SHELLEXECUTEINFOW sei;
        wchar_t params[4096];
        DWORD childRc = (DWORD)-1;

        params[0] = L'\0';
        wcscat_s(params, _countof(params), install ? L"--install" : L"--uninstall");
        wcscat_s(params, _countof(params), L" --elevated");
        if (quiet) wcscat_s(params, _countof(params), L" --quiet");
        wcscat_s(params, _countof(params), L" --app ");
        AppendQuotedArg(params, _countof(params), target);

        Log(L"IFEO: 未提权，runas 拉起自己：%s", params);

        ZeroMemory(&sei, sizeof(sei));
        sei.cbSize = sizeof(sei);
        /* SEE_MASK_NOASYNC：GUI 程序必须加，否则不等 UAC 走完就可能退出 */
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb = L"runas";
        sei.lpFile = selfExe;
        sei.lpParameters = params;
        sei.nShow = SW_SHOWNORMAL;

        if (!ShellExecuteExW(&sei))
        {
            DWORD e = GetLastError();
            Log(L"IFEO: 提权失败 err=%lu", e);
            if (!quiet)
            {
                _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                             L"这一步要写 HKLM，需要管理员权限。\n\n%s\n\n"
                             L"（提权失败：%lu）\n\n"
                             L"也可以右键 si_zh_launcher.exe「以管理员身份运行」，\n"
                             L"或用管理员 PowerShell 跑 scripts\\%s.ps1。",
                             (e == ERROR_CANCELLED) ? L"你取消了 UAC 提示。"
                                                    : L"没能拉起提权进程。",
                             e, install ? L"install" : L"uninstall");
                MessageBoxW(NULL, msg, title, MB_OK | MB_ICONERROR);
            }
            return 8;
        }

        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &childRc);
        CloseHandle(sei.hProcess);
        Log(L"IFEO: 提权实例退出 code=%lu", childRc);
        return (int)childRc;
    }

    ProbeIfeo(keyPath, IFEO_VALUE_NAME, &keyExists, &valueExists, oldValue, _countof(oldValue));
    Log(L"IFEO: 键存在=%d 值存在=%d 旧值=[%s]", keyExists, valueExists, oldValue);

    if (install)
    {
        /* ---- 1) 备份旧值（格式与 install.ps1 一致）---- */
        if (!keyExists)
            WriteBackupFile(selfDir, L"(安装前不存在该键)\r\n");
        else if (!valueExists || !oldValue[0])
            WriteBackupFile(selfDir, L"(该键存在但无 Debugger 值)\r\n");
        else
        {
            wchar_t t[2048 + 64];
            _snwprintf_s(t, _countof(t), _TRUNCATE, L"原有 Debugger = %s\r\n", oldValue);
            WriteBackupFile(selfDir, t);
        }

        /* ---- 2) 新值：带引号的自身路径，并保留原有附加参数 ---- */
        newValue[0] = L'\0';
        AppendQuotedArg(newValue, _countof(newValue), selfExe);
        if (valueExists && oldValue[0])
        {
            wchar_t oldExe[MAX_PATH * 2];
            const wchar_t* rest = NULL;
            SplitExeAndArgs(oldValue, oldExe, _countof(oldExe), &rest);
            if (_wcsicmp(oldExe, selfExe) == 0 && rest && *rest)
            {
                wcscat_s(newValue, _countof(newValue), L" ");
                wcscat_s(newValue, _countof(newValue), rest);
                Log(L"IFEO: 保留原有附加参数 [%s]", rest);
            }
        }

        /* ---- 3) reg add ---- */
        _snwprintf_s(cmd, _countof(cmd), _TRUNCATE,
                     L"\"%s\" add \"%s\" /v %s /t REG_SZ /d ",
                     regExe, keyPath, IFEO_VALUE_NAME);
        AppendQuotedArg(cmd, _countof(cmd), newValue);
        wcscat_s(cmd, _countof(cmd), L" /f");
        Log(L"IFEO: %s", cmd);

        if (!RunCapture(cmd, out, _countof(out), &ec))
        {
            Log(L"IFEO: reg.exe 没跑起来");
            if (!quiet)
            {
                _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                             L"reg.exe 没能启动：\n%s\n\n命令：\n%s", regExe, cmd);
                MessageBoxW(NULL, msg, title, MB_OK | MB_ICONERROR);
            }
            return 9;
        }
        Log(L"IFEO: reg 退出码=%lu 输出=[%s]", ec, out);

        /* ---- 4) 回读校验 ---- */
        {
            BOOL k2 = FALSE, v2 = FALSE;
            wchar_t now[2048];
            ProbeIfeo(keyPath, IFEO_VALUE_NAME, &k2, &v2, now, _countof(now));

            if (v2 && wcscmp(now, newValue) == 0)
            {
                Join(dllPath, MAX_PATH, selfDir, L"zh_hook.dll");
                Log(L"IFEO: 装好了，回读一致");
                if (!quiet)
                {
                    _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                                 L"IFEO 劫持已安装（已回读校验）。\n\n"
                                 L"注册表\n%s\n    Debugger = %s\n\n"
                                 L"目标程序\n%s%s\n\n"
                                 L"现在双击、或走快捷方式启动，都会先经过本启动器"
                                 L"（界面为中文）。\n\n"
                                 L"卸载：si_zh_launcher.exe --uninstall\n"
                                 L"自检：scripts\\verify-ifeo.ps1\n\n"
                                 L"改动前的旧值已备份到\n%s",
                                 keyPath, now, target,
                                 Exists(dllPath) ? L"" :
                                     L"\n\n!! 注意：同目录下没有 zh_hook.dll，"
                                     L"这样装上去程序能起来，但不会有中文。",
                                 backupPath);
                    MessageBoxW(NULL, msg, title, MB_OK | MB_USERICON);
                }
                rc = 0;
            }
            else
            {
                Log(L"IFEO: 回读不一致 —— 期望 [%s] 实际 [%s]", newValue, v2 ? now : L"(无)");
                if (!quiet)
                {
                    _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                                 L"reg.exe 跑完了（退出码 %lu），但回读看到的不是我们要写的值 ——"
                                 L"这次改动**没有生效**。\n\n"
                                 L"期望：\n    Debugger = %s\n"
                                 L"实际：\n    %s\n\n"
                                 L"reg.exe 输出：\n%s\n\n"
                                 L"命令（可手动复现）：\n%s",
                                 ec, newValue, v2 ? now : L"(该值不存在)",
                                 out[0] ? out : L"(无输出)", cmd);
                    MessageBoxW(NULL, msg, title, MB_OK | MB_ICONERROR);
                }
                rc = 10;
            }
        }
        return rc;
    }

    /* ================================ 卸载 ================================ */
    if (!keyExists)
    {
        Log(L"IFEO: 键本来就不存在");
        if (!quiet)
        {
            _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                         L"没有需要卸载的东西。\n\n%s\n这个键本来就不存在，"
                         L"说明 System Informer 一直是原生启动的。", keyPath);
            MessageBoxW(NULL, msg, title, MB_OK | MB_USERICON);
        }
        return 0;
    }

    if (!valueExists || !oldValue[0])
    {
        Log(L"IFEO: 键在，但没有 Debugger 值");
        if (!quiet)
        {
            _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                         L"启动路径上没有拦截。\n\n%s\n这个键存在，但没有 Debugger 值，"
                         L"不需要卸载。", keyPath);
            MessageBoxW(NULL, msg, title, MB_OK | MB_USERICON);
        }
        return 0;
    }

    /* ---- 1) 删值（先删它，这一步做完启动路径就不再被拦）---- */
    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE,
                 L"\"%s\" delete \"%s\" /v %s /f", regExe, keyPath, IFEO_VALUE_NAME);
    Log(L"IFEO: %s", cmd);
    if (!RunCapture(cmd, out, _countof(out), &ec))
    {
        if (!quiet)
        {
            _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                         L"reg.exe 没能启动：\n%s\n\n命令：\n%s", regExe, cmd);
            MessageBoxW(NULL, msg, title, MB_OK | MB_ICONERROR);
        }
        return 9;
    }
    Log(L"IFEO: reg 退出码=%lu 输出=[%s]", ec, out);

    {
        BOOL v2 = FALSE;
        wchar_t now[2048];
        wchar_t hint[2048];
        BOOL keyGone = FALSE;

        ProbeIfeo(keyPath, IFEO_VALUE_NAME, NULL, &v2, now, _countof(now));
        if (v2)
        {
            Log(L"IFEO: Debugger 仍在 [%s]", now);
            if (!quiet)
            {
                _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                             L"卸载**没有生效** —— 回读仍然看得到 Debugger 值。\n\n"
                             L"现值：%s\n\nreg.exe 退出码 %lu，输出：\n%s\n\n"
                             L"命令（可手动复现）：\n%s", now, ec, out[0] ? out : L"(无输出)", cmd);
                MessageBoxW(NULL, msg, title, MB_OK | MB_ICONERROR);
            }
            return 11;
        }

        /* ---- 2) 键若已空，一并删掉，别留垃圾（与 uninstall.ps1 一致）---- */
        if (KeyIsEmpty(keyPath))
        {
            _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\" delete \"%s\" /f", regExe, keyPath);
            Log(L"IFEO: %s", cmd);
            RunCapture(cmd, out, _countof(out), &ec);
            keyGone = KeyIsEmpty(keyPath);
        }

        ReadBackupHint(selfDir, hint, _countof(hint));
        Log(L"IFEO: 卸完了 keyGone=%d", keyGone);

        if (!quiet)
        {
            _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                         L"IFEO 劫持已卸载（已回读校验）。\n\n"
                         L"已删除：\n%s\n    Debugger\n%s\n\n"
                         L"System Informer 已回到原生启动方式；程序文件仍在\n%s\n\n"
                         L"安装时的备份记录：\n%s",
                         keyPath,
                         keyGone ? L"（该键已空，连带删除）"
                                 : L"（键内仍有其他内容，保留该键）",
                         selfDir,
                         hint[0] ? hint : L"(没有找到 ifeo_backup.txt)");
            MessageBoxW(NULL, msg, title, MB_OK | MB_USERICON);
        }
        rc = 0;
    }
    return rc;
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
    BOOL log = FALSE, noWait = FALSE, check = FALSE, collect = FALSE;
    BOOL install = FALSE, uninstall = FALSE, quiet = FALSE, elevatedMark = FALSE;
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
            if (_wcsicmp(argv[i], L"--collect") == 0)    { collect = TRUE; continue; }
            /* 装/卸：Windows 上 /install 与 --install 都收，省得用户猜 */
            if (_wcsicmp(argv[i], L"--install") == 0   || _wcsicmp(argv[i], L"/install") == 0   ||
                _wcsicmp(argv[i], L"-install") == 0)     { install = TRUE; continue; }
            if (_wcsicmp(argv[i], L"--uninstall") == 0 || _wcsicmp(argv[i], L"/uninstall") == 0 ||
                _wcsicmp(argv[i], L"-uninstall") == 0)   { uninstall = TRUE; continue; }
            /* 只收长名。短名（-q 之类）有意不收：其余参数会被原样转给
             * System Informer，多认一个短名就多一分把它的参数吃掉的风险。 */
            if (_wcsicmp(argv[i], L"--quiet") == 0)      { quiet = TRUE; continue; }
            /* --elevated 是内部标记：runas 重启自己时由我们加上，用来防止
             * 万一"提权判定"出错时无限自我重启（见 HandleIfeo）。 */
            if (_wcsicmp(argv[i], L"--elevated") == 0)   { elevatedMark = TRUE; continue; }
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
    Log(L"未命中收集 = %s", collect ? L"开（--collect）" : L"关（默认；需要时加 --collect）");

    /* ---- --install / --uninstall：只动注册表，不启动程序 ----
     * 放在「目标是否存在」的检查之前：从 dist\ 里跑 --install（`..\SystemInformer.exe`
     * 还不存在）也要能装，键名照样取得到。 */
    if (install || uninstall)
    {
        if (install && uninstall) Log(L"!! 同时给了 --install 与 --uninstall，按 --install 处理");
        return HandleIfeo(g_dir, target, install, quiet, elevatedMark);
    }

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
                     L"目标：\n%s\n\n钩子 DLL：\n%s\n\n词典：\n%s\n\n"
                     L"日志：%s\n\n未命中收集：%s",
                     target, dllPath, dictPath, log ? L"开" : L"关",
                     collect ? L"开（--collect）" : L"关（默认）");
        MessageBoxW(NULL, msg, L"si_zh_launcher --check", MB_OK | MB_USERICON);
        return 0;
    }

    /* ---- 子进程环境：这正是旧 POC 靠 os.environ 传、容易漏的东西 ---- */
    SetEnvironmentVariableW(L"ZHHK_DICT", dictPath);
    SetEnvironmentVariableW(L"ZHHK_TRANSLATE", L"1");

    /* 运行时未命中收集：默认关，只有 --collect 才开（详见文件头）。
     * 两个值都显式写：子进程的状态完全由本次启动参数决定，
     * 不受启动器自身所处环境里有没有 ZHHK_MISS 影响。 */
    SetEnvironmentVariableW(L"ZHHK_MISS", collect ? L"1" : L"0");
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

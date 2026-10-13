/*
 * zh_hook.dll —— System Informer 中文显示层（运行时文本替换）
 * ===========================================================================
 *
 * 它做什么
 *   在本进程内接管所有「把文字画到界面上」的出口，查词典把英文换成中文。
 *   不改 SystemInformer.exe 一个字节、不动它的资源，所以升级、文件校验、
 *   数字签名一律不受影响。
 *
 * 为什么能生效（先看清 SI 是怎么画字的）
 *   SI 的界面文字有四个来源，各自对应一条拦截路径：
 *     1. 控件文本（对话框、按钮、选项页条目、树与列表项）
 *        -> SendMessageW(WM_SETTEXT / CB_ADDSTRING / TVM_INSERTITEMW …)
 *     2. 列表与表头（PhTreeNew 自绘，列头是 SysHeader32）
 *        -> SendMessageW(HDM_INSERTITEMW / LVM_INSERTCOLUMNW …)
 *     3. 工具栏与菜单栏（ToolStatus 插件自绘）
 *        -> SendMessageW(TB_ADDBUTTONS / TB_SETBUTTONINFO)
 *     4. 自绘文本（上述控件真正落笔时）
 *        -> DrawTextW / DrawTextExW / TextOutW / ExtTextOutW（及对应的 A 版）
 *
 *   1~3 是「写入型」消息：字符串在消息参数里，改参数即可。它们的 Unicode 版本
 *   必须显式声明常量 —— commctrl.h 里的 CM_TB_ADDBUTTONS 之类是历史遗留名，
 *   默认解析到 ANSI 数值（0x0418 等），而真实的 Unicode 消息是 0x0403/0x0432/0x0434。
 *   用错常量会让整类消息落进 default 被丢弃，症状是「表头能汉化、工具栏完全无效」。
 *
 *   4 是「绘制型」调用：文字已在内存里，只能改写指针或就地覆盖缓冲区。
 *   DrawTextW / TextOutW 内部都会再调 ExtTextOutW，若不加闸同一串会被处理两次 ——
 *   结果虽仍正确（译文再查词典必然落空），但日志里出现重复记录会让人误判载体，
 *   排障时这种假象是致命的。所以用一个线程本地的 RealGuard 静音。
 *
 * 拦截手段（三路并用，缺一路就会漏）
 *   A. MinHook 打 user32/gdi32 的导出层钩子 —— 覆盖所有调用者，含后加载的插件
 *   B. 本进程模块的 IAT 改写 —— 补充手段，抢在延迟加载完成前生效
 *   C. 本线程的 WH_CALLWNDPROC —— 兜底。CWPSTRUCT 是值拷贝，只能原地改内存
 *
 * 翻译表（两张，都是 UTF-8 文本、启动时读入，改词不必重编译本 DLL）
 *
 *   1) zh_dict.txt          精确匹配。每行「原文<TAB>译文」，# 开头为注释。
 *                           译文为空的条目会被跳过，所以自动收集来的待翻译
 *                           条目（译文留空）留在里面不影响运行。
 *
 *   2) zh_dict.pattern.txt  通配符匹配。**可选**，没有就不启用。
 *                           词典是精确匹配，匹配不了 System Informer 里大量
 *                           「运行期拼出来」的串（状态栏 "CPU usage: 1.82%"、
 *                           进程气泡 "Console application: svchost.exe (2180)"
 *                           ……），这些只能靠模式。格式与匹配规则见下方
 *                           「通配符模式翻译」一节。
 *
 *   3) zh_font.txt          字体替换表。**可选**，没有就不换字体。
 *                           System Informer 的应用字体（页签/菜单/表头/迷你信息窗）
 *                           写死成 Microsoft Sans Serif 8 号，设置项改不到它；
 *                           这张表在字体被创建时把 LOGFONT 换掉（换名 + 等比换字号）。
 *                           格式与理由见下方「字体替换」一节。
 *
 * 环境变量
 *   ZHHK_DICT      词典路径。默认 = 本 DLL 同目录的 zh_dict.txt
 *   ZHHK_PATTERN   模式表路径。默认 = 词典同目录的 zh_dict.pattern.txt
 *   ZHHK_FONT      字体表路径。默认 = 词典同目录的 zh_font.txt
 *                  取 0/off/false = 关掉字体替换（A/B 对照用）
 *   ZHHK_FONTLOG   1 = 逐条记录每个被创建的字体（[fontall] 行），不改变行为
 *   ZHHK_FONTFACE  1 = 核对 GDI 真正解析到的字体名（[fontface] 行），默认关
 *   ZHHK_TRANSLATE 1 = 真正替换文本（默认 0 = 只观察不改）
 *   ZHHK_LOG       日志文件路径（UTF-16LE）。不设则不记日志
 *   ZHHK_LOGALL    1 = 记录所有绘制调用（排障用，日志会很大）
 *   ZHHK_HOOKS     只启用指定类别，如 "SM,DT,TO"（排障用）
 *   ZHHK_TRACETB   1 = 追踪工具栏/表头范围的消息（排障用）
 *
 *   —— 以下两组只在「运行时未命中收集」打开时才有意义 ——
 *   ZHHK_MISS      收集开关。**默认关闭**：未设 = 关。
 *                  正常用法是给启动器加 --collect（见 si_zh_launcher.c），
 *                  由它把这里设成 1。取值：
 *                    未设 / 0 / off / false  -> 关
 *                    1 / on / true           -> 开，文件落在词典同目录
 *                    其它值                  -> 开，且该值就是文件路径
 *   ZHHK_MISSFLUSH 周期刷新间隔（秒，默认 15，0 = 关闭）
 *   ZHHK_MISSALL   1 = 关掉「像不像界面文字」过滤，未命中收集全都收
 *
 * 运行时未命中收集
 *   见下方「运行时未命中收集」一节 —— 把界面上出现过、词典里没有的串写到专有
 *   文件，由 tools/merge_missed.py 显式合并回词典。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include "MinHook.h"

#define MAX_ENTRIES  8192
#define MAX_TEXTLEN  4096
#define LOG_CAP      4000

// ---------------------------------------------------------------------------
// 控件消息的 Unicode 变体必须显式声明。
//
// 踩坑记录：commctrl.h 里的 CM_TB_ADDBUTTONS / CM_TB_SETBUTTONINFO /
// CM_TB_GETBUTTONTEXT 是「历史遗留名」，默认解析到 ANSI 数值（0x0418 等），
// 而真实 Unicode 消息是 TB_ADDBUTTONS(0x0403) / TB_SETBUTTONINFO(0x0432) /
// TB_GETBUTTONTEXT(0x0434)。用错常量会让这些消息全部落进 default 被丢弃 ——
// 实测中表现为「表头能汉化、工具栏完全无效」。
// ---------------------------------------------------------------------------
#define CM_LVM_FIRST        0x1000
#define CM_LVM_INSERTCOLUMNW (CM_LVM_FIRST + 29)   // 0x101D
#define CM_LVM_SETCOLUMNW    (CM_LVM_FIRST + 31)   // 0x101F
#define CM_LVM_INSERTITEMW   (CM_LVM_FIRST + 77)   // 0x104D
#define CM_LVM_SETITEMW      (CM_LVM_FIRST + 76)   // 0x104C

#define CM_HDM_FIRST         0x1200
#define CM_HDM_INSERTITEMW   (CM_HDM_FIRST + 10)   // 0x120A
#define CM_HDM_SETITEMW      (CM_HDM_FIRST + 12)   // 0x120C

#define CM_TB_ADDBUTTONS     0x0403
#define CM_TB_SETBUTTONINFO  0x0432
#define CM_TB_GETBUTTONTEXT  0x0434
#define CM_TB_BUTTONSTRUCTSIZE 0x041E

#define CM_SB_SETTEXTW       0x040B

// ---------------------------------------------------------------- 状态

static wchar_t        g_logPath[MAX_PATH];
static wchar_t        g_dictPath[MAX_PATH];
static HANDLE         g_log = INVALID_HANDLE_VALUE;
static BOOL           g_doTranslate = FALSE;
static BOOL           g_logAll = FALSE;
static CRITICAL_SECTION g_cs;
static volatile LONG  g_initDone = 0;
static volatile LONG  g_nLog = 0;      // 日志行数上限（节流）
static BOOL           g_needSM = TRUE;
static BOOL           g_needDT = TRUE;
static BOOL           g_needTO = TRUE;
static BOOL           g_needDE = TRUE;
static BOOL           g_traceTb = FALSE;   // 追踪工具栏/表头范围消息，带窗口类名

static void*          g_realSendMessageW = NULL;
static void*          g_realDrawTextW    = NULL;
static void*          g_realDrawTextExW  = NULL;
static void*          g_realTextOutW     = NULL;
static void*          g_realExtTextOutW  = NULL;
static void*          g_realDrawTextA    = NULL;
static void*          g_realExtTextOutA  = NULL;
static void*          g_realTextOutA     = NULL;

// ExtTextOutW 是 GDI 最底层的文本出口：user32 内部画菜单栏、以及自绘控件
// （PhTabNew 标签栏这类）往往绕过 DrawTextW 直接走它。
// 这也是「菜单栏/标签栏联不通、表头却联得通」的最可能解释。
static BOOL           g_needXO   = TRUE;
static BOOL           g_needAnsi = TRUE;   // DrawTextA / ExtTextOutA / TextOutA

// DrawTextW / TextOutW 内部都会再调 ExtTextOutW。若不加这道闸，同一串文字会被
// 处理两次：结果虽仍正确（译文再查词典必然落空），但日志里会出现两条同串记录，
// 让人误判「ExtTextOutW 才是载体」。做诊断时这个假象是致命的，所以必须静音。
static __declspec(thread) int g_inReal = 0;

struct RealGuard
{
    RealGuard()  { g_inReal++; }
    ~RealGuard() { g_inReal--; }
};

// ---------------------------------------------------------------------------
// 运行时未命中收集
//
// 目的：把「界面上确实出现过、但词典里没有」的串收集起来，进程退出时落到一个
//       专有文件，供 _work\dict\merge_missed.py 合并回 zh_dict.txt。
//
// 两条设计决定都是有原因的：
//
// 1) 不直接写进 zh_dict.txt。
//    zh_dict.txt 是启动时被读的词典。运行期往里追加，进程一旦被强杀就会留下
//    半行，下次加载直接读到脏数据；而且它是 build_zh_dict.py 的生成物，
//    下次重跑就被覆盖。所以写到同目录的 zh_dict.missed.txt，
//    由 merge_missed.py 显式合并（合并时统一做去重、分类、格式校验）。
//
// 2) 不等退出才写。
//    TerminateProcess / 任务管理器结束进程 都不走 DLL_PROCESS_DETACH，
//    只在退出时写会把这些场景的数据全丢掉。所以首次见到某个新串就立即追加一行；
//    干净退出时再把整个文件按最终计数重写一遍（临时文件 + 原子替换）。
//
// 去重键是「原文」而不是哈希：哈希表只负责定位，落盘前再做一次精确比较，
// 避免「撞哈希就当成见过」的误判（旧实现 1024 槽只存哈希，就是这个毛病）。
#define MISS_MAX      4096
#define MISS_TEXTLEN  200
#define MISS_CLSLEN   96     // 控件链（最多 4 级：类名#控件ID<父<祖父<曾祖）
#define MISS_COLLEN   160    // 列表列头签名（最多 10 列）
#define MISS_SLOT     8192

// 一条未命中的「身份」。前三个字段就是「来源指纹」——合并侧靠它把字符串
// 分到「进程列表 / 服务列表 / 设备列表 / 某个对话框…」，进而判断它是
// 界面文字还是列表里的**数据**（进程名、服务名、设备名）。
struct MissRec
{
    wchar_t text[MISS_TEXTLEN];
    wchar_t chain[MISS_CLSLEN];   // SysListView32#1234<SysTabControl32#1000<#32770
    wchar_t cols[MISS_COLLEN];    // Name|PID|CPU|Private bytes|User name
    int     col;                  // 文本落在第几列（-1 = 判不出 / 不适用）
    DWORD   count;
    char    api[32];
};

static MissRec* g_miss      = NULL;      // 堆分配：4096 * sizeof(MissRec) ≈ 2.3 MB
static int      g_missSlot[MISS_SLOT];   // 0 = 空槽，否则 g_miss 下标 + 1
static int      g_missCount = 0;
static HANDLE   g_missFile  = INVALID_HANDLE_VALUE;
static wchar_t  g_missPath[MAX_PATH];
// **默认关闭**。收集会在真实使用中持续往磁盘写文件，属于排障/采集行为，
// 不该是「装好就一直在写」的默认状态；要收集就显式给启动器加 --collect。
static BOOL     g_missEnabled = FALSE;
static BOOL     g_missDone    = FALSE;   // 退出刷新只做一次（DETACH 与钩子可能都触发）
static BOOL     g_missAll     = FALSE;   // ZHHK_MISSALL=1 时不做「像不像界面文字」过滤
static LONG     g_missSkipped = 0;       // 被过滤掉的次数（只用于日志）
// 指纹采集会向控件发消息（LVM_GETHEADER / HDM_GETITEMW），控件处理消息时可能
// 触发重绘、再次进到本文件里的绘制钩子 —— 那就又可能调用 MissNote，无限递归。
// 线程本地闸：闸内的 MissNote 直接放弃这次记录（该字符串下次重绘还会再来）。
static __declspec(thread) LONG g_probeGuard = 0;

// 绘制期「当前正在画的窗口」。见 HookBeginPaint 上面的说明 —— System Informer
// 的列表是自绘的，画在内存 DC 上，WindowFromDC() 查不到窗口。
#define PAINT_STACK 8
static __declspec(thread) HWND g_paintStack[PAINT_STACK];
static __declspec(thread) int  g_paintDepth = 0;
static DWORD    g_missFlushSec = 15;     // 周期检查点间隔（秒），ZHHK_MISSFLUSH 可改

static DWORD HashStr(const wchar_t* s);      // 定义在下方，这里先声明
static void  Logf(const wchar_t* fmt, ...);  // 同上
static BOOL  ReadWholeFileW(const wchar_t* path, char** data, DWORD* size);

static void MissWriteRaw(HANDLE h, const char* bytes)
{
    DWORD written = 0;
    if (h && h != INVALID_HANDLE_VALUE && bytes)
        WriteFile(h, bytes, (DWORD)strlen(bytes), &written, NULL);
}

// 数据行: 原文 <TAB> 次数 <TAB> API <TAB> 控件链 <TAB> 列表列头 <TAB> 所在列
static void MissFormatLine(const MissRec* r, char* line, size_t cap)
{
    char en[MISS_TEXTLEN * 3];
    char chain[MISS_CLSLEN * 3];
    char cols[MISS_COLLEN * 3];
    char col[16];

    if (WideCharToMultiByte(CP_UTF8, 0, r->text, -1, en, sizeof(en), NULL, NULL) <= 0)
        en[0] = '\0';
    if (WideCharToMultiByte(CP_UTF8, 0, r->chain, -1, chain, sizeof(chain), NULL, NULL) <= 0)
        chain[0] = '\0';
    if (WideCharToMultiByte(CP_UTF8, 0, r->cols, -1, cols, sizeof(cols), NULL, NULL) <= 0)
        cols[0] = '\0';
    if (r->col >= 0)
        _snprintf_s(col, sizeof(col), _TRUNCATE, "%d", r->col);
    else
        strcpy_s(col, sizeof(col), "-");

    _snprintf_s(line, cap, _TRUNCATE, "%s\t%lu\t%s\t%s\t%s\t%s\r\n",
                en, (unsigned long)r->count, r->api[0] ? r->api : "?",
                chain[0] ? chain : "-", cols[0] ? cols : "-", col);
}

static void MissAppendLine(const MissRec* r)
{
    char line[MISS_TEXTLEN * 4];

    if (!g_missFile || g_missFile == INVALID_HANDLE_VALUE)
        return;
    MissFormatLine(r, line, _countof(line));
    MissWriteRaw(g_missFile, line);
}

static const char MISS_HEADER[] =
    "\xEF\xBB\xBF"                                            // UTF-8 BOM，记事本可直接打开
    "# System Informer 中文词典 · 运行时未命中收集\r\n"
    "# 由 zh_hook.dll 在运行期自动追加维护。本文件【不会】被词典加载器读取，\r\n"
    "# 合并进词典请运行: python tools\\merge_missed.py\r\n"
    "#\r\n"
    "# 只有启动时带了 --collect 的运行才会写入本文件（默认不收集）。\r\n"
    "# 字段: 原文 <TAB> 次数 <TAB> API <TAB> 控件链 <TAB> 列表列头 <TAB> 所在列\r\n"
    "# 同一原文只占一行；跨次运行不重复（启动时会先读回已有键和指纹）。\r\n"
    "#\r\n"
    "# 「控件链」形如 SysListView32#1234<SysTabControl32#1000<#32770：从画出文字的\r\n"
    "#   控件逐级向上到顶层窗口，每级是「类名#控件ID」。\r\n"
    "# 「列表列头」是列表控件的列标题按列序用 | 连接。**这是区分来源的关键**：\r\n"
    "#   进程列表 / 服务列表 / 设备列表的控件类和父链都一样，只有列不一样。\r\n"
    "# 「所在列」是文字落在第几列（0 起）。列表第 0 列通常是对象名（进程名、服务名\r\n"
    "#   之类的**数据**），后面的列才可能是状态之类的界面枚举值。\r\n"
    "#\r\n";

static void MissLoadExisting(void)
{
    char* raw = NULL;
    DWORD size = 0;
    char* line;
    char* nl;

    if (!g_missEnabled || !*g_missPath)
        return;
    if (!ReadWholeFileW(g_missPath, &raw, &size))
        return;                                   // 首次运行：文件还不存在

    if (!g_miss)
        g_miss = (MissRec*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                     sizeof(MissRec) * MISS_MAX);
    if (!g_miss)
    {
        HeapFree(GetProcessHeap(), 0, raw);
        g_missEnabled = FALSE;
        return;
    }

    line = raw;
    while (line && *line)
    {
        size_t n;

        nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; }
        n = strlen(line);
        if (n > 0 && line[n - 1] == '\r')
            line[n - 1] = '\0';

        if (line[0] != '#' && line[0] != '\0' && strchr(line, '\t') != NULL)
        {
            // 按 TAB 切成最多 6 段：原文 / 次数 / API / 控件链 / 列头 / 所在列
            char* f[6];
            int nf = 0;
            char* p;
            f[nf++] = line;
            for (p = line; nf < 6 && (p = strchr(p, '\t')) != NULL; )
            {
                *p = '\0';
                f[nf++] = ++p;
            }

            if (g_missCount < MISS_MAX)
            {
                MissRec* r = &g_miss[g_missCount];
                wchar_t w[MISS_TEXTLEN];
                if (MultiByteToWideChar(CP_UTF8, 0, f[0], -1, w, MISS_TEXTLEN) > 0)
                {
                    DWORD h;
                    DWORD i, probe;
                    wcsncpy_s(r->text, MISS_TEXTLEN, w, _TRUNCATE);

                    // ★ 旧版本这里只把 api 写成 "-"、cls 写成 "?" 就算读完了 ——
                    //   于是每次重启后第一次重写文件，全表指纹都被抹成 -/?，
                    //   来源信息永久丢失（实测：734 条全部退化成 api=- cls=?）。
                    //   现在逐字段读回。旧格式（4 列）读不到指纹，留空即可，
                    //   由 MissNote 里的「自愈」在下次见到同一条时补上。
                    r->count = (nf > 1) ? (DWORD)strtoul(f[1], NULL, 10) : 1;
                    if (r->count == 0)
                        r->count = 1;
                    if (nf > 2 && f[2][0] && strcmp(f[2], "-") != 0)
                        strncpy_s(r->api, sizeof(r->api), f[2], _TRUNCATE);
                    if (nf > 3 && f[3][0] && strcmp(f[3], "-") != 0 && strcmp(f[3], "?") != 0)
                        MultiByteToWideChar(CP_UTF8, 0, f[3], -1, r->chain, MISS_CLSLEN);
                    if (nf > 4 && f[4][0] && strcmp(f[4], "-") != 0)
                        MultiByteToWideChar(CP_UTF8, 0, f[4], -1, r->cols, MISS_COLLEN);
                    r->col = (nf > 5 && f[5][0] != '-') ? atoi(f[5]) : -1;

                    h = HashStr(r->text) | 1u;
                    i = h & (MISS_SLOT - 1);
                    for (probe = 0; probe < MISS_SLOT; probe++)
                    {
                        if (g_missSlot[i] == 0)
                        {
                            g_missSlot[i] = g_missCount + 1;
                            g_missCount++;
                            break;
                        }
                        i = (i + 1) & (MISS_SLOT - 1);
                    }
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }

    HeapFree(GetProcessHeap(), 0, raw);
    Logf(L"[miss] 已载入历史未命中 %d 条: %s", g_missCount, g_missPath);
}

static BOOL MissOpen(void)
{
    if (!g_missEnabled || !*g_missPath)
        return FALSE;

    g_missFile = CreateFileW(g_missPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_missFile == INVALID_HANDLE_VALUE)
    {
        Logf(L"[miss] 无法打开未命中文件: %s (err=%lu)", g_missPath, GetLastError());
        g_missPath[0] = L'\0';
        return FALSE;
    }
    if (GetFileSize(g_missFile, NULL) == 0)
        MissWriteRaw(g_missFile, MISS_HEADER);
    return TRUE;
}

// ---------------------------------------------------------------------------
// 落盘：三种触发，覆盖三种死法
//
//   1. 首次见到就追加一行        —— 强杀 / 崩溃 / 断电，数据也已经在了
//   2. 周期检查点（默认 15 秒）  —— 重写整个文件，把计数刷新成最终值。
//                                   这是**真正管用的那条**：外部 taskkill /F
//                                   调的是别的进程里的 NtTerminateProcess，
//                                   进程内钩子根本不会被调用。
//   3. 退出钩子 + DLL_PROCESS_DETACH —— 正常退出时立刻刷新一次
//
// 为什么要专门钩 NtTerminateProcess：
//   实测发现 System Informer 退出时**根本不会走 DLL_PROCESS_DETACH**
//   （日志里一条 teardown 记录都没有）。查源码找到原因 ——
//   phlib\apistubs.cpp 自己实现了 ExitProcess_Stub / TerminateProcess_Stub，
//   绕开 kernel32 直接调 ntdll!NtTerminateProcess：
//       ExitProcess        -> ExitProcess_Stub      -> PhExitApplication
//       TerminateProcess   -> TerminateProcess_Stub -> NtTerminateProcess
//       PhExitApplication  -> RtlExitUserProcess 或 NtTerminateProcess
//   所以钩 kernel32 是白钩，必须钩 ntdll 这两个。
// ---------------------------------------------------------------------------

static void MissCloseAppendHandle(void)
{
    if (g_missFile != INVALID_HANDLE_VALUE && g_missFile != NULL)
    {
        CloseHandle(g_missFile);
        g_missFile = INVALID_HANDLE_VALUE;
    }
}

// 原子重写（.tmp + MoveFileEx）。调用方需持有 g_cs。
static void MissRewrite(void)
{
    wchar_t tmp[MAX_PATH];
    HANDLE h;
    int i;
    SYSTEMTIME st;
    char line[MISS_TEXTLEN * 4];

    if (!*g_missPath || !g_miss)
        return;

    _snwprintf_s(tmp, _countof(tmp), _TRUNCATE, L"%s.tmp", g_missPath);
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        Logf(L"[miss] 检查点: 建临时文件失败 err=%lu tmp=%s", GetLastError(), tmp);
        return;
    }

    MissWriteRaw(h, MISS_HEADER);
    GetLocalTime(&st);
    _snprintf_s(line, _countof(line), _TRUNCATE,
                "# 最后一次刷新: %04d-%02d-%02d %02d:%02d:%02d  进程 pid=%lu  共 %d 条\r\n#\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                (unsigned long)GetCurrentProcessId(), g_missCount);
    MissWriteRaw(h, line);

    for (i = 0; i < g_missCount; i++)
    {
        MissFormatLine(&g_miss[i], line, _countof(line));
        MissWriteRaw(h, line);
    }
    FlushFileBuffers(h);
    CloseHandle(h);

    if (!MoveFileExW(tmp, g_missPath, MOVEFILE_REPLACE_EXISTING))
    {
        Logf(L"[miss] 检查点: 替换失败 err=%lu", GetLastError());
        DeleteFileW(tmp);
    }
}

// 周期检查点：重写并继续追加
static void MissCheckpoint(void)
{
    if (!g_missEnabled || !*g_missPath || !g_miss)
        return;

    EnterCriticalSection(&g_cs);
    MissCloseAppendHandle();
    MissRewrite();
    MissOpen();                       // 重新挂上追加句柄，继续收集
    LeaveCriticalSection(&g_cs);
}

// 退出：重写并关闭。可重入安全 —— 多个退出路径可能都被触发。
static void MissFlushAndClose(void)
{
    if (g_missDone)
        return;
    g_missDone = TRUE;

    Logf(L"[miss] 退出刷新: count=%d 已过滤=%ld path=%s",
         g_missCount, (long)g_missSkipped, g_missPath);

    EnterCriticalSection(&g_cs);
    MissCloseAppendHandle();
    MissRewrite();
    LeaveCriticalSection(&g_cs);
}

// 周期刷新线程。间隔由 ZHHK_MISSFLUSH 控制（秒，0 = 关闭，默认 15）。
static DWORD WINAPI MissCheckpointThread(LPVOID param)
{
    (void)param;
    for (;;)
    {
        DWORD ms = g_missFlushSec * 1000;
        Sleep(ms ? ms : 15000);
        if (!g_missEnabled)
            return 0;
        MissCheckpoint();
    }
}

// ntdll 的两个退出入口（见上面「为什么要专门钩 NtTerminateProcess」）
// 用 LONG 而不是 NTSTATUS：WIN32_LEAN_AND_MEAN 下 NTSTATUS 不保证已声明，
// 而这两个函数的返回值/参数在二进制层面就是 32 位状态码，LONG 完全等价。
typedef LONG (NTAPI *NtTerminateProcess_t)(HANDLE, LONG);
typedef VOID (NTAPI *RtlExitUserProcess_t)(LONG);
static void* g_realNtTerminateProcess = NULL;
static void* g_realRtlExitUserProcess = NULL;

static LONG NTAPI HookNtTerminateProcess(HANDLE ProcessHandle, LONG ExitStatus)
{
    NtTerminateProcess_t real = (NtTerminateProcess_t)g_realNtTerminateProcess;

    __try
    {
        // 只在自己要死的时候刷；别的进程来杀我们（taskkill /F）钩子不会被调用，
        // 那种情况靠周期检查点兜底。
        if (ProcessHandle == (HANDLE)-1 || ProcessHandle == GetCurrentProcess())
            MissFlushAndClose();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }

    if (real)
        return real(ProcessHandle, ExitStatus);
    return ExitStatus;
}

static VOID NTAPI HookRtlExitUserProcess(LONG ExitStatus)
{
    RtlExitUserProcess_t real = (RtlExitUserProcess_t)g_realRtlExitUserProcess;

    __try { MissFlushAndClose(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }

    if (real)
        real(ExitStatus);             // 正常情况不返回
}

// 形如 "smss.exe" / "ntdll.dll" 的进程名、模块名、文件名 —— 是数据，不是界面文字。
// 判据：不带空格 + 以已知扩展名收尾。
static BOOL MissLooksLikeFileName(const wchar_t* s)
{
    static const wchar_t* exts[] = {
        L"exe", L"dll", L"sys", L"ocx", L"cpl", L"msc", L"bat", L"cmd",
        L"com", L"scr", L"drv", L"mui", L"log", L"tmp", L"ini", L"dat", NULL
    };
    const wchar_t* dot;
    int i;

    if (wcschr(s, L' '))
        return FALSE;                        // 带空格的基本是句子
    dot = wcsrchr(s, L'.');
    if (!dot || dot == s || !dot[1])
        return FALSE;
    for (i = 0; exts[i]; i++)
    {
        if (_wcsicmp(dot + 1, exts[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

// 含小数（数字 . 数字）—— 几乎总是格式化出来的数值读数。
// 界面标签里不会出现 "30.29" 这种东西，但状态栏每刷新一次就是一个新串。
static BOOL MissHasDecimal(const wchar_t* s)
{
    for (; s[0] && s[1] && s[2]; s++)
    {
        if (s[0] >= L'0' && s[0] <= L'9' && s[1] == L'.' &&
            s[2] >= L'0' && s[2] <= L'9')
            return TRUE;
    }
    return FALSE;
}

// 只收集「看起来需要翻译的界面文字」。
//
// 为什么必须有这道闸：列表里的内存占用、CPU 百分比、PID、用户名这些**数据**
// 每秒都在变，每一个都是一条全新的「未命中」。不滤的话 4096 个槽位几分钟就被
// 这些数值占满，真正的 UI 文字反而被挤掉 —— 第一版实测就是这样，409 条里
// 有 380 多条是 "95.17" / "4.06 MB" / "NT AUTHORITY\\SYSTEM" 这类东西。
//
// 判据（宁少勿多 —— 漏掉的下一轮还会再出现，误收的会永久污染词表）：
//   * 已经是中文（含 CJK）      -> 不需要翻译
//   * 一个字母都没有            -> 纯数字/符号
//   * 含反斜杠                  -> DOMAIN\\user 之类的数据
//   * 不含数字                  -> 基本可以肯定是文字，收
//   * 含数字的：字母段很短（MB / kB/s）或数字+符号占比过高 -> 当作数值，丢
//
// ZHHK_MISSALL=1 可关掉这道闸（排查「某个串为什么没被收集」时用）。
static BOOL MissLooksTranslatable(const wchar_t* s)
{
    int len = 0, letters = 0, digits = 0, cjk = 0, punct = 0;
    int run = 0, maxRun = 0, runs = 0;

    for (const wchar_t* p = s; *p; p++)
    {
        wchar_t c = *p;
        len++;

        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))
        {
            letters++;
            run++;
            if (run > maxRun)
                maxRun = run;
        }
        else
        {
            if (run > 0) { runs++; run = 0; }
            if (c >= L'0' && c <= L'9')        digits++;
            else if (c >= 0x2E80)              cjk++;     // CJK 及以后
            else if (c > L' ')                 punct++;
        }
    }
    if (run > 0)
        runs++;

    if (len < 2)             return FALSE;
    if (cjk > 0)             return FALSE;
    if (letters == 0)        return FALSE;
    if (wcschr(s, L'\\'))    return FALSE;
    if (MissLooksLikeFileName(s)) return FALSE;
    if (MissHasDecimal(s))   return FALSE;
    if (digits == 0)         return TRUE;

    if (maxRun <= 3 && runs <= 2)
        return FALSE;                                  // "4.06 MB" / "977.98 MB/s"
    if ((digits + punct) * 100 > len * 45)
        return FALSE;                                  // "Physical memory: 30.19 GB (47.41%)"
    return TRUE;
}

// ---------------------------------------------------------------------------
// 来源指纹采集
//
// 目的：记下「这条字符串是哪个界面画出来的」，好让合并侧判断它是**界面文字**
// 还是列表里的**数据**（进程名 / 服务名 / 设备名 / 用户名）。
//
// 三个信号，缺一不可：
//   1. 控件链 —— 从画字的控件逐级向上到顶层窗口，区分「主窗口里的列表」和
//      「某个对话框里的列表」。
//   2. 列头签名 —— **最关键的一个**。进程列表 / 服务列表 / 设备列表的控件类
//      都是 SysListView32、父链也一模一样，只有列头不同。
//   3. 所在列 —— 第 0 列是对象名（数据），往后的列才可能是状态之类的枚举。
//
// 代价：2 和 3 要向控件发消息（LVM_GETHEADER / HDM_GETITEMW / HDM_GETITEMRECT）。
// 控件处理消息时可能触发重绘，重绘会再次进入本文件的绘制钩子 → 又调 MissNote。
// 所以整段包在 g_probeGuard 里，闸内的 MissNote 直接放弃记录。
// ---------------------------------------------------------------------------

static void MissIdent(HWND hwnd, wchar_t* out, size_t cap)
{
    wchar_t cls[64] = L"";
    if (!hwnd)
    {
        wcsncpy_s(out, cap, L"?", _TRUNCATE);
        return;
    }
    GetClassNameW(hwnd, cls, _countof(cls));
    cls[_countof(cls) - 1] = L'\0';
    _snwprintf_s(out, cap, _TRUNCATE, L"%s#%d", cls, GetDlgCtrlID(hwnd));
}

static void MissChain(HWND hwnd, wchar_t* out, size_t cap)
{
    wchar_t one[80];
    int depth = 0;
    out[0] = L'\0';
    while (hwnd && depth < 4)
    {
        MissIdent(hwnd, one, _countof(one));
        if (wcslen(out) + wcslen(one) + 2 >= cap)
            break;
        if (out[0])
            wcscat_s(out, cap, L"<");
        wcscat_s(out, cap, one);
        hwnd = GetParent(hwnd);
        depth++;
    }
}

// 找列头控件。三种情况都要管：
//   1. 传进来的本身就是列头（列标题就画在它上面）
//   2. 标准列表控件：LVM_GETHEADER
//   3. 自绘列表（PhTreeNew）：列头是它的**子窗口**，LVM_GETHEADER 不管用
// 不处理第 3 种，进程/服务/设备列表就全都读不到列 —— 而列头正是区分它们的关键。
static HWND MissHeaderOf(HWND hwnd)
{
    wchar_t cls[64] = L"";
    HWND hh, h, best = NULL;
    int bestN = -1;

    if (!hwnd)
        return NULL;
    GetClassNameW(hwnd, cls, _countof(cls));
    if (_wcsicmp(cls, L"SysHeader32") == 0)
        return hwnd;
    hh = (HWND)SendMessageW(hwnd, LVM_GETHEADER, 0, 0);
    if (hh)
        return hh;

    // ★ 取**列数最多**的那个，不是第一个匹配到的。
    //
    // 实测（probe_cols.py 的输出）：System Informer 每个 PhTreeNew 挂了**两个**
    // SysHeader32 —— 先是列数=0 的空壳，然后才是真列头（列数 6 / 9 / 14 / 15）。
    // 取第一个匹配到的就是空壳，于是所有行内容的列头签名都变成 '-'，来源也就
    // 分不出来了（第一版实测 70 条 chain 有、cols 空）。
    //
    // 只扫**直接子窗口**：不能用 EnumChildWindows —— 它连孙子辈一起枚举，那样
    // 主窗口也会「找到」进程列表的列头，把主窗口上画的每个字都误判成列表内容。
    for (h = GetWindow(hwnd, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT))
    {
        int n;
        GetClassNameW(h, cls, _countof(cls));
        if (_wcsicmp(cls, L"SysHeader32") != 0)
            continue;
        n = (int)SendMessageW(h, HDM_GETITEMCOUNT, 0, 0);
        if (n > bestN)
        {
            bestN = n;
            best = h;
        }
    }
    return best;
}

// 找出「文本所属的列表控件」：先看自己，再看父窗口。
static HWND MissListOf(HWND hwnd)
{
    HWND par;

    if (!hwnd)
        return NULL;
    if (MissHeaderOf(hwnd))
        return hwnd;
    par = GetParent(hwnd);
    if (par && MissHeaderOf(par))
        return par;
    return NULL;
}

static void MissCols(HWND hwnd, wchar_t* out, size_t cap)
{
    HWND hh;
    int n, i;

    out[0] = L'\0';
    hh = MissHeaderOf(MissListOf(hwnd));
    if (!hh)
        return;
    n = (int)SendMessageW(hh, HDM_GETITEMCOUNT, 0, 0);
    if (n <= 0)
        return;
    if (n > 10)
        n = 10;

    for (i = 0; i < n; i++)
    {
        wchar_t buf[80] = L"";
        HDITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = HDI_TEXT;
        it.pszText = buf;
        it.cchTextMax = _countof(buf);
        SendMessageW(hh, HDM_GETITEMW, i, (LPARAM)&it);
        if (wcslen(out) + wcslen(buf) + 2 >= cap)
            break;
        if (out[0])
            wcscat_s(out, cap, L"|");
        wcscat_s(out, cap, buf);
    }
}

// 文字落在第几列。-1 = 判不出（不是列表 / 控件枚举为空）。
static int MissColumnAt(HWND hwnd, LONG x)
{
    HWND hh;
    int n, i;
    RECT rc;

    if (!hwnd || x < 0)
        return -1;
    hh = MissHeaderOf(MissListOf(hwnd));
    if (!hh)
        return -1;
    n = (int)SendMessageW(hh, HDM_GETITEMCOUNT, 0, 0);

    for (i = 0; i < n; i++)
    {
        ZeroMemory(&rc, sizeof(rc));
        rc.left = i;                      // HDM_GETITEMRECT 用 left 成员传索引（文档如此）
        if (!SendMessageW(hh, HDM_GETITEMRECT, i, (LPARAM)&rc))
            continue;
        if (x >= rc.left && x < rc.right)
            return i;
    }
    return -1;
}

// 采集指纹并**择优合并**进记录。
//
// 为什么是合并而不是直接覆盖：同一条字符串可能在「控件还没初始化完」的时候
// 就先画过一次（实测有 70 条在列头还是 0 列时被首次画出，指纹就永远停在 '-'）。
// 所以后来采到的更全的结果要能补进去，而更差的不能把好的冲掉。
// 判据用「更长者胜」：列头多一个列名就更长，控件链多一级也更长。
static void MissFillProbe(MissRec* r, HWND hwnd, HDC hdc, LONG x)
{
    MissRec tmp;

    if (!hwnd && hdc)
        hwnd = WindowFromDC(hdc);          // 窗口 DC：直接查得到
    if (!hwnd && g_paintDepth > 0)
        hwnd = g_paintStack[g_paintDepth - 1];   // 内存 DC：退回「当前绘制窗口」

    ZeroMemory(&tmp, sizeof(tmp));
    g_probeGuard++;
    MissChain(hwnd, tmp.chain, MISS_CLSLEN);
    MissCols(hwnd, tmp.cols, MISS_COLLEN);
    tmp.col = MissColumnAt(hwnd, x);
    g_probeGuard--;

    if (wcslen(tmp.chain) > wcslen(r->chain))
        wcsncpy_s(r->chain, MISS_CLSLEN, tmp.chain, _TRUNCATE);
    if (wcslen(tmp.cols) > wcslen(r->cols))
        wcsncpy_s(r->cols, MISS_COLLEN, tmp.cols, _TRUNCATE);
    if (tmp.col >= 0 && r->col < 0)
        r->col = tmp.col;
    if (!r->chain[0])
        wcsncpy_s(r->chain, MISS_CLSLEN, L"?", _TRUNCATE);
}

// 记一条未命中。api 是载体的函数名（ASCII）；hwnd 可直接给，也可给 hdc 由本函数反查窗口。
// x 是文字在 DC 坐标里的横坐标（-1 = 不知道），只用来判「在第几列」。
static void MissNote(const char* api, const wchar_t* s, HWND hwnd, HDC hdc, LONG x = -1)
{
    wchar_t clean[MISS_TEXTLEN];
    const wchar_t* key;
    DWORD h, i, probe;
    MissRec* r;
    int k;

    if (!g_missEnabled || !s || !*s)
        return;

    // 先净化，再判长、再哈希、再去重 —— 顺序很重要。
    //
    // 为什么必须净化控制字符（TAB / CR / LF -> 空格）：收集文件是「一行一条、TAB 分隔」，
    //   1) 多行界面文字（实测 "…are displayed.\r\nExamples:\r\nWdi*"）不净化会把一行
    //      拆成三行，中间几行没有 TAB 字段，合并时被静默丢弃 —— 真正要收的那句反而丢了；
    //   2) 更糟的是：文本里若含 TAB，等于让**运行期数据**往词典里注入一条翻译，
    //      而词典的格式是「原文<TAB>译文」，这就能凭空造出一条恶意映射。
    // 净化必须在哈希/去重之前做，否则同一条串每次都会被当成新条目重复记录。
    for (k = 0; k < MISS_TEXTLEN - 1 && s[k]; k++)
        clean[k] = (s[k] < L' ') ? L' ' : s[k];
    clean[k] = L'\0';
    if (k == 0 || s[k] != L'\0')
        return;                              // 空串，或原串超长（会被截断）
    key = clean;

    if (!g_missAll)
    {
        if (!MissLooksTranslatable(key))
        {
            g_missSkipped++;
            return;
        }
    }
    if (!g_miss)
        g_miss = (MissRec*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                     sizeof(MissRec) * MISS_MAX);
    if (!g_miss)
    {
        g_missEnabled = FALSE;
        return;
    }

    // 整段加锁：这张表是多线程共用的（每个绘制线程都会进来）。
    // CRITICAL_SECTION 可重入，所以里面的 Logf 再进一次 g_cs 是安全的。
    EnterCriticalSection(&g_cs);

    h = HashStr(key) | 1u;
    i = h & (MISS_SLOT - 1);

    for (probe = 0; probe < MISS_SLOT; probe++)
    {
        int slot = g_missSlot[i];

        if (slot == 0)
        {
            if (g_missCount >= MISS_MAX)
                break;
            if (g_probeGuard)
                break;                       // 正在采集指纹：放弃本次，下次重绘还会再来

            r = &g_miss[g_missCount];
            wcsncpy_s(r->text, MISS_TEXTLEN, key, _TRUNCATE);
            r->count = 1;
            strncpy_s(r->api, sizeof(r->api), api ? api : "?", _TRUNCATE);
            r->chain[0] = L'\0';
            r->cols[0] = L'\0';
            r->col = -1;

            MissFillProbe(r, hwnd, hdc, x);

            g_missSlot[i] = g_missCount + 1;     // 槽位后发布：读者看到的永远是填好的记录
            g_missCount++;
            MissAppendLine(r);                   // 立即落盘：强杀也不丢
            Logf(L"[miss] %hs col=%d chain=%s cols=%s \"%s\"",
                 r->api, r->col, r->chain, r->cols, r->text);
            break;
        }

        r = &g_miss[slot - 1];
        if (_wcsicmp(r->text, key) == 0)
        {
            r->count++;
            // 指纹自愈 / 升级。
            //
            // 只在第一次见到时采一次是不够的：实测有 70 条是在「列头还没填好」
            // 的时刻被首次画出来的，cols 就永远停在 '-'；另有 57 条停在一列。
            // 所以在计数为 2 的幂时重采（2/4/8/…/256，最多 8 次），
            // MissFillProbe 按「更长者胜」合并 —— 采全之后就不用再采了。
            if (!g_probeGuard && r->count >= 2 && r->count <= 256 &&
                (r->count & (r->count - 1)) == 0)
                MissFillProbe(r, hwnd, hdc, x);
            break;
        }
        i = (i + 1) & (MISS_SLOT - 1);
    }

    LeaveCriticalSection(&g_cs);
}

static HHOOK          g_callWndHook = NULL;

struct DictEntry { const wchar_t* en; const wchar_t* zh; DWORD hash; };
static DictEntry     g_entries[MAX_ENTRIES];
static int           g_entryCount = 0;
static wchar_t       g_pool[MAX_ENTRIES * 128];
static DWORD         g_poolUsed = 0;

static DWORD HashStr(const wchar_t* s)
{
    DWORD h = 2166136261u;
    while (*s)
    {
        wchar_t c = *s++;
        if (c >= L'A' && c <= L'Z')
            c += 32;
        h ^= (DWORD)c;
        h *= 16777619u;
    }
    return h;
}

static const wchar_t* PoolDup(const wchar_t* s)
{
    size_t n = wcslen(s) + 1;
    wchar_t* dst;

    if (g_poolUsed + n > _countof(g_pool))
        return NULL;
    dst = &g_pool[g_poolUsed];
    g_poolUsed += (DWORD)n;
    wcscpy_s(dst, n, s);
    return dst;
}

static void AddEntry(const wchar_t* en, const wchar_t* zh)
{
    if (!en || !zh || !*en || !*zh)
        return;
    if (g_entryCount >= MAX_ENTRIES)
        return;

    g_entries[g_entryCount].en = PoolDup(en);
    g_entries[g_entryCount].zh = PoolDup(zh);
    if (!g_entries[g_entryCount].en || !g_entries[g_entryCount].zh)
        return;

    g_entries[g_entryCount].hash = HashStr(en);
    g_entryCount++;
}

// 最近一次命中的查询缓存

// ---------------------------------------------------------------- 工具

static BOOL PtrReadable(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || (uintptr_t)p < (uintptr_t)0x10000)
        return FALSE;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
        return FALSE;
    if (mbi.State != MEM_COMMIT)
        return FALSE;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return FALSE;
    return (mbi.Protect & PAGE_READONLY) || (mbi.Protect & PAGE_READWRITE) ||
           (mbi.Protect & PAGE_WRITECOPY) || (mbi.Protect & PAGE_EXECUTE_READ) ||
           (mbi.Protect & PAGE_EXECUTE_READWRITE) || (mbi.Protect & PAGE_EXECUTE_WRITECOPY);
}

// 从任意指针安全取宽字符串（原地拷贝，不分配、不阻塞）
static int SafeText(const wchar_t* p, wchar_t* buf, int cap)
{
    int len = 0;

    if (!p || cap < 2)
        return 0;
    if ((uintptr_t)p < (uintptr_t)0x10000 || (uintptr_t)p > (uintptr_t)0x00007FFFFFFFFFFF)
        return 0;
    if (!PtrReadable(p))
        return 0;

    __try
    {
        while (len < cap - 1 && p[len] != L'\0')
            len++;
        for (int i = 0; i < len; i++)
            buf[i] = p[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }

    buf[len] = L'\0';
    return len;
}

// ---------------------------------------------------------------- 日志

static void LogLine(const wchar_t* text)
{
    wchar_t line[MAX_TEXTLEN + 256];
    static const wchar_t bom[2] = { 0xFEFF, 0 };
    int n;

    if (!g_log || g_log == INVALID_HANDLE_VALUE || !text)
        return;

    n = _snwprintf_s(line, _countof(line), _TRUNCATE, L"%s\r\n", text);
    if (n <= 0)
        return;

    EnterCriticalSection(&g_cs);
    if (GetFileSize(g_log, NULL) == 0)
    {
        DWORD written = 0;
        WriteFile(g_log, bom, 2, &written, NULL);
    }
    {
        DWORD written = 0;
        WriteFile(g_log, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &written, NULL);
    }
    LeaveCriticalSection(&g_cs);
}

static void Logf(const wchar_t* fmt, ...)
{
    wchar_t buf[MAX_TEXTLEN + 256];
    va_list ap;

    if (InterlockedIncrement(&g_nLog) > LOG_CAP)
        return;

    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    LogLine(buf);
}

// ---------------------------------------------------------------- 词典

static BOOL ReadWholeFileW(const wchar_t* path, char** data, DWORD* size)
{
    HANDLE h;
    DWORD sz, got;

    *data = NULL;
    *size = 0;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;

    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > 8u * 1024 * 1024)
    {
        CloseHandle(h);
        return FALSE;
    }

    *data = (char*)HeapAlloc(GetProcessHeap(), 0, sz + 1);
    if (!*data)
    {
        CloseHandle(h);
        return FALSE;
    }

    if (!ReadFile(h, *data, sz, &got, NULL))
    {
        HeapFree(GetProcessHeap(), 0, *data);
        *data = NULL;
        CloseHandle(h);
        return FALSE;
    }

    (*data)[got] = '\0';
    *size = got;
    CloseHandle(h);
    return TRUE;
}

static void AddEntry(const wchar_t* en, const wchar_t* zh);

static void LoadDict(void)
{
    char* raw = NULL;
    DWORD size = 0;
    char* line;
    char* next;
    int lines = 0;
    int nEmpty = 0, nComment = 0, nNoTab = 0, nConvFail = 0;

    if (!*g_dictPath)
    {
        Logf(L"[dict] 未设置词典路径（ZHHK_DICT 缺失）");
        return;
    }
    if (!ReadWholeFileW(g_dictPath, &raw, &size))
    {
        Logf(L"[dict] 无法读取词典: %s (err=%lu)", g_dictPath, GetLastError());
        return;
    }

    Logf(L"[dict] path=%s", g_dictPath);
    Logf(L"[dict] 读到 %lu 字节",
         size, (BYTE)raw[0], (BYTE)raw[1], (BYTE)raw[2], (BYTE)raw[3],
         (BYTE)raw[4], (BYTE)raw[5], (BYTE)raw[6], (BYTE)raw[7]);

    {
        int nTab = 0;
        for (DWORD i = 0; i < size; i++)
        {
            if (raw[i] == '\t') nTab++;
        }
        Logf(L"[dict] TAB 数 = %d", nTab);
    }

    line = raw;
    while (line && *line)
    {
        char* nl;
        char* tab;

        next = line;
        nl = strchr(line, '\n');
        if (nl)
        {
            *nl = '\0';
            next = nl + 1;
        }
        else
        {
            next = NULL;
        }

        lines++;

        if (line[0] == '\0') { nEmpty++; }
        else if (line[0] == '#') { nComment++; }

        if (line[0] != '\0' && line[0] != '#')
        {
            if (line[0] == (char)0xEF && line[1] == (char)0xBB && line[2] == (char)0xBF)
                line += 3;

            tab = strchr(line, '\t');
            if (tab)
            {
                wchar_t en[MAX_TEXTLEN];
                wchar_t zh[MAX_TEXTLEN];

                *tab = '\0';
                if (MultiByteToWideChar(CP_UTF8, 0, line, -1, en, MAX_TEXTLEN) > 0 &&
                    MultiByteToWideChar(CP_UTF8, 0, tab + 1, -1, zh, MAX_TEXTLEN) > 0)
                {
                    size_t n = wcslen(en);
                    if (n > 0 && en[n - 1] == L'\r')
                        en[n - 1] = L'\0';
                    n = wcslen(zh);
                    if (n > 0 && zh[n - 1] == L'\r')
                        zh[n - 1] = L'\0';

                    AddEntry(en, zh);
                    if (g_entryCount == 1)
                        Logf(L"[dict] 首条: \"%s\" -> \"%s\"", en, zh);
                }
                else
                {
                    nConvFail++;
                    Logf(L"[dict] 第 %d 行转换失败 err=%lu", lines, GetLastError());
                }
            }
            else
            {
                nNoTab++;
                if (nNoTab <= 3)
                    Logf(L"[dict] 第 %d 行无 TAB: \"%hs\"", lines, line);
            }
        }

        line = next;
    }

    HeapFree(GetProcessHeap(), 0, raw);
    Logf(L"[dict] 扫描 %d 行（空 %d / 注释 %d / 无TAB %d / 转换失败 %d），载入 %d 条",
         lines, nEmpty, nComment, nNoTab, nConvFail, g_entryCount);
}

// ===========================================================================
// 通配符模式翻译
// ===========================================================================
//
// 为什么需要单独一套
//   词典是**精确匹配**，而 System Informer 里有大量「运行期拼出来」的串：
//
//     状态栏   L"CPU usage: %s"                 ->  "CPU usage: 1.82%"
//     进程气泡 L"Console application: %s (%lu)"  ->  "Console application: svchost.exe (2180)"
//     列表菜单 L"Copy \"%s\""                    ->  'Copy "explorer.exe"'
//     插件菜单 L"Save for %s"
//
//   这些串每次运行内容都不一样，精确匹配永远命中不了 —— 收集器只能把它们反复
//   写进未命中文件（实测：某次运行 352 条收集里 344 条是这类碎片，主因就是
//   进程气泡那一整串）。
//
// 格式（独立文件 zh_dict.pattern.txt，与词典同样的「原文<TAB>译文」）
//   原文里的通配符：
//     *        任意长度（可为空）的片段，并**捕获**它
//     ?        恰好一个字符，不捕获
//     \* \? \\ 转义，表示字面量
//   译文里的占位符：
//     *        依次取「还没被用过的」捕获（第 1 个 * 取捕获 1，以此类推）
//     \1..\9   显式取第 N 个捕获（可换序、可重复引用）
//     \\       字面反斜杠
//   译文中没用到的捕获会按顺序追加到末尾 —— 宁可译文有点啰嗦，也不让界面上的
//   数据悄悄消失。占位符比通配符少会在启动日志里告警，tools/check_pattern.py
//   也能在构建期查出来。
//
// 应用方式：**反复过所有模式，直到某一遍完全没改动**（上限 PAT_PASS 遍）。
//   为什么不是「命中一条就返回」：进程气泡那种
//     "… Notes: … Signer: … Console application: … Flags: …"
//   是整串画出来的，一条模式只能换掉其中一个字段标签，多遍才能全换完。
//
// 写模式时必须知道的四个性质
//   1. `*` 取**最短**可行片段，不是贪婪。所以 '*A: *' 里第一个 * 只吃到 A: 之前
//      那一段，不会把整串吞掉。
//   2. 首尾都带 *（如 '*Process is *'）等价于「子串替换」。
//   3. 顺序有意义：文件里**靠前的先匹配**。多遍应用时，宽泛的模式若排在具体
//      模式前面，会把后者的目标文本先改掉 —— 具体写前面，宽泛写后面。
//   4. **多行文本不套模式**（见 MatchPatterns 里的说明）：那种串的行距是调用方
//      按原文宽度算好的，换成中文宽度就画错。所以气泡那类整串多行的文字，
//      模式救不了 —— 它只能靠 zh_dict.txt 精确匹配，或者干脆不译。
//
// 性能：模式只在**精确匹配落空之后**才试；先用「字面前缀」和「长度下界」两道
//   预筛排除掉绝大多数模式（进程气泡那 350 条碎片的经验是：平均一条串只真正
//   进入匹配 1~2 条模式）。匹配本身还有步数预算兜底，杜绝病态模式把界面拖死。

#define PAT_MAX        256      // 最多多少条模式
#define PAT_TEXTLEN    512      // 单条原文/译文的长度上限
#define PAT_MAXCAP     8        // 单条模式里最多几个捕获
#define PAT_PFX        48       // 预筛用的字面前缀长度上限
#define PAT_INPUT_MAX  1024     // 超过这个长度就不试模式了（界面文字不会这么长）
#define PAT_BUDGET     20000    // 单次匹配的递归步数预算（防病态模式）
#define PAT_PASS       4        // 同一条模式最多反复应用几轮
#define PAT_LOG_HITS   200      // 模式命中日志最多记多少条（防日志爆炸）

struct PatEntry
{
    wchar_t pat[PAT_TEXTLEN];
    wchar_t zh[PAT_TEXTLEN];
    wchar_t pfx[PAT_PFX];   // 第一个通配符之前的字面前缀（预筛用）
    int     pfxLen;
    int     literal;        // 通配符之外的字面字符数 = 匹配长度的下界
    int     caps;           // 原文里的 * 个数
    int     holes;          // 译文里的占位符个数
};

static PatEntry g_pat[PAT_MAX];
static int      g_patCount = 0;
static wchar_t  g_patPath[MAX_PATH];
static volatile LONG g_patHits = 0;

// 大小写处理：只折 ASCII。词典侧用的是 _wcsicmp，这里保持一致的口径就够 ——
// 界面原文都是英文，非 ASCII 一律按原样比较。
static wchar_t PatUp(wchar_t c)
{
    return (c >= L'a' && c <= L'z') ? (wchar_t)(c - 32) : c;
}

static BOOL PatSame(wchar_t a, wchar_t b)
{
    return PatUp(a) == PatUp(b);
}

// 算出一条模式的预筛信息（字面前缀 / 长度下界 / 通配符数 / 占位符数）。
static void PatAnalyze(PatEntry* e)
{
    int i, n = 0;
    BOOL wild = FALSE;

    e->caps = 0;
    e->literal = 0;

    for (i = 0; e->pat[i]; i++)
    {
        wchar_t c = e->pat[i];

        if (c == L'\\' && (e->pat[i + 1] == L'*' || e->pat[i + 1] == L'?' || e->pat[i + 1] == L'\\'))
        {
            c = e->pat[++i];               // 转义：当字面量
        }
        else if (c == L'*' || c == L'?')
        {
            if (c == L'*')
                e->caps++;
            wild = TRUE;                   // 前缀到此为止
            continue;
        }

        e->literal++;
        if (!wild && n < PAT_PFX - 1)
            e->pfx[n++] = c;
    }

    e->pfx[n] = L'\0';
    e->pfxLen = n;

    e->holes = 0;
    for (i = 0; e->zh[i]; i++)
    {
        if (e->zh[i] == L'\\' && e->zh[i + 1] == L'\\')
        {
            i++;
            continue;
        }
        if (e->zh[i] == L'\\' && e->zh[i + 1] >= L'1' && e->zh[i + 1] <= L'9')
        {
            e->holes++;
            i++;
            continue;
        }
        if (e->zh[i] == L'*')
            e->holes++;
    }
}

struct PatCap { int start; int len; };

struct PatTry
{
    PatCap caps[PAT_MAXCAP];
    int    nCaps;
    int    budget;
};

// p = 模式，s = 当前文本位置，base = 文本起点（捕获存相对 base 的偏移）。
// 递归深度 = 模式里 * 的段数（连续的 * 已折叠），有界；回溯规模由 budget 兜底。
static BOOL PatMatch(const wchar_t* p, const wchar_t* s, const wchar_t* base, PatTry* t)
{
    while (*p)
    {
        if (t->budget-- <= 0)
            return FALSE;

        if (*p == L'\\' && (p[1] == L'*' || p[1] == L'?' || p[1] == L'\\'))
        {
            if (!*s || !PatSame(*s, p[1]))
                return FALSE;
            p += 2;
            s++;
            continue;
        }

        if (*p == L'*')
        {
            int idx;
            const wchar_t* q;

            while (*p == L'*')                 // ** 与 * 等价
                p++;

            idx = (t->nCaps < PAT_MAXCAP) ? t->nCaps++ : -1;

            for (q = s; ; q++)                 // 最短优先：先试空片段
            {
                int save = t->nCaps;

                if (idx >= 0)
                {
                    t->caps[idx].start = (int)(s - base);
                    t->caps[idx].len = (int)(q - s);
                }
                if (PatMatch(p, q, base, t))
                    return TRUE;

                t->nCaps = save;
                if (!*q)
                    break;                     // 已试到串尾，没有别的可能了
            }
            return FALSE;
        }

        if (*p == L'?')
        {
            if (!*s)
                return FALSE;
            p++;
            s++;
            continue;
        }

        if (!*s || !PatSame(*p, *s))
            return FALSE;
        p++;
        s++;
    }

    return *s == L'\0';
}

// 把译文里的占位符换成捕获。返回 FALSE = 结果放不下（调用方按「不匹配」处理）。
static BOOL PatExpand(const PatEntry* e, const wchar_t* s, const PatTry* t,
                      wchar_t* out, int outCap)
{
    int used[PAT_MAXCAP];
    int i, o = 0, k;

    for (i = 0; i < PAT_MAXCAP; i++)
        used[i] = 0;

    for (i = 0; e->zh[i]; i++)
    {
        const wchar_t* piece;
        int pieceLen;

        if (e->zh[i] == L'\\' && e->zh[i + 1] == L'\\')
        {
            if (o + 2 > outCap)
                return FALSE;
            out[o++] = L'\\';
            i++;
            continue;
        }

        if (e->zh[i] == L'\\' && e->zh[i + 1] >= L'1' && e->zh[i + 1] <= L'9')
        {
            k = e->zh[i + 1] - L'1';
            i++;
        }
        else if (e->zh[i] == L'*')
        {
            k = -1;
            for (int j = 0; j < t->nCaps; j++)
            {
                if (!used[j]) { k = j; break; }
            }
            if (k < 0)
                continue;                      // 捕获已用完：这个 * 输出空
        }
        else
        {
            if (o + 2 > outCap)
                return FALSE;
            out[o++] = e->zh[i];
            continue;
        }

        piece = (k < t->nCaps) ? (s + t->caps[k].start) : NULL;
        pieceLen = (k < t->nCaps) ? t->caps[k].len : 0;
        if (k < t->nCaps)
            used[k] = 1;

        if (pieceLen > 0)
        {
            if (o + pieceLen + 1 > outCap)
                return FALSE;
            wmemcpy(out + o, piece, (size_t)pieceLen);
            o += pieceLen;
        }
    }

    // 译文没引用到的捕获追加到末尾 —— 不静默丢数据。
    {
        BOOL any = FALSE;

        for (i = 0; i < t->nCaps; i++)
        {
            if (!used[i] && t->caps[i].len > 0) { any = TRUE; break; }
        }
        if (any)
        {
            if (o + 2 > outCap)
                return FALSE;
            out[o++] = L' ';
            for (i = 0; i < t->nCaps; i++)
            {
                if (used[i] || t->caps[i].len <= 0)
                    continue;
                if (o + t->caps[i].len + 2 > outCap)
                    return FALSE;
                wmemcpy(out + o, s + t->caps[i].start, (size_t)t->caps[i].len);
                o += t->caps[i].len;
                out[o++] = L' ';
            }
            o--;                                // 去掉最后那个空格
        }
    }

    out[o] = L'\0';
    return TRUE;
}

// 对一条模式应用一次。返回 TRUE = 命中（且译文与原文不同）。
static BOOL PatApply(const PatEntry* e, const wchar_t* in, wchar_t* out, int outCap)
{
    wchar_t cur[MAX_TEXTLEN];
    PatTry t;
    int i, slen;

    slen = (int)wcslen(in);
    if (slen <= 0 || slen > PAT_INPUT_MAX)
        return FALSE;
    if (slen < e->literal)                     // 预筛 1：长度下界
        return FALSE;
    if (slen < e->pfxLen)
        return FALSE;

    for (i = 0; i < e->pfxLen; i++)            // 预筛 2：字面前缀
    {
        if (!PatSame(in[i], e->pfx[i]))
            return FALSE;
    }

    t.nCaps = 0;
    t.budget = PAT_BUDGET;

    if (!PatMatch(e->pat, in, in, &t))
        return FALSE;
    if (!PatExpand(e, in, &t, cur, MAX_TEXTLEN))
        return FALSE;
    if (wcscmp(cur, in) == 0)
        return FALSE;                          // 译文没引用任何捕获：等于没变

    wcsncpy_s(out, outCap, cur, _TRUNCATE);
    return TRUE;
}

// 模式匹配总入口。
//
// 一遍又一遍地过**所有**模式，直到某一遍完全没改动（上限 PAT_PASS 遍）。
// 为什么不是「命中一条就返回」：进程气泡那种串是
//   "… Notes: … Signer: … Console application: … Flags: …"
// 一整串画出来的，一条模式只能换掉其中一个字段标签，必须多遍才能全换完。
//
// s 与 out 可以是同一块缓冲区：整个过程都在 cur 里做，最后才写 out。
static const wchar_t* MatchPatterns(const wchar_t* s, wchar_t* out, int outCap)
{
    wchar_t cur[MAX_TEXTLEN];
    wchar_t nxt[MAX_TEXTLEN];
    BOOL changed = FALSE;
    int pass, i;

    if (g_patCount <= 0 || !s || !*s)
        return NULL;

    // 多行文本一律不套模式。
    //
    // 为什么：进程提示气泡那种「一整串多行文字」是 SI 自己算好行距再逐行画的，
    // 它按**原文**的宽度算布局。我们把其中几个字段标签换成中文，字的宽度就变了
    // （"Signer:" 6 个半角 vs "签名者：" 4 个全角），SI 那边并不知道 —— 实测结果是
    // 气泡的几行叠在一起、糊成一团。这是**画错**，比不翻译糟糕得多。
    //
    // 精确匹配的词典不受这条限制：词典命中的是整串一一对应，本来就命中不了
    // 这种拼出来的多行串，两者互不影响。
    if (wcschr(s, L'\n') || wcschr(s, L'\r'))
        return NULL;

    wcsncpy_s(cur, MAX_TEXTLEN, s, _TRUNCATE);

    for (pass = 0; pass < PAT_PASS; pass++)
    {
        BOOL any = FALSE;

        for (i = 0; i < g_patCount; i++)
        {
            if (!PatApply(&g_pat[i], cur, nxt, MAX_TEXTLEN))
                continue;

            wcsncpy_s(cur, MAX_TEXTLEN, nxt, _TRUNCATE);
            any = TRUE;
            changed = TRUE;

            if (InterlockedIncrement(&g_patHits) <= PAT_LOG_HITS)
                Logf(L"[pat] #%d 第 %d 遍改写为 \"%s\"", i, pass + 1, cur);
        }

        if (!any)
            break;
    }

    if (!changed)
        return NULL;

    wcsncpy_s(out, outCap, cur, _TRUNCATE);
    return out;
}

static void LoadPatterns(void)
{
    char* raw = NULL;
    DWORD size = 0;
    char* line;
    char* next;
    int lines = 0, nBad = 0, nWarn = 0;

    if (!*g_patPath)
        return;
    if (!ReadWholeFileW(g_patPath, &raw, &size))
    {
        Logf(L"[pat] 没有模式表（可选，不是错误）: %s", g_patPath);
        return;
    }

    Logf(L"[pat] path=%s (%lu 字节)", g_patPath, size);

    line = raw;
    while (line && *line)
    {
        char* nl;
        char* tab;

        next = line;
        nl = strchr(line, '\n');
        if (nl)
        {
            *nl = '\0';
            next = nl + 1;
        }
        else
        {
            next = NULL;
        }

        lines++;

        if (line[0] == (char)0xEF && line[1] == (char)0xBB && line[2] == (char)0xBF)
            line += 3;

        if (line[0] != '\0' && line[0] != '#')
        {
            tab = strchr(line, '\t');
            if (!tab)
            {
                nBad++;
                if (nBad <= 3)
                    Logf(L"[pat] 第 %d 行没有 TAB，已忽略: \"%hs\"", lines, line);
            }
            else
            {
                wchar_t pat[PAT_TEXTLEN], zh[PAT_TEXTLEN];

                *tab = '\0';
                if (MultiByteToWideChar(CP_UTF8, 0, line, -1, pat, PAT_TEXTLEN) <= 0 ||
                    MultiByteToWideChar(CP_UTF8, 0, tab + 1, -1, zh, PAT_TEXTLEN) <= 0)
                {
                    nBad++;
                    if (nBad <= 3)
                        Logf(L"[pat] 第 %d 行 UTF-8 转换失败", lines);
                }
                else
                {
                    size_t n = wcslen(pat);
                    if (n > 0 && pat[n - 1] == L'\r') pat[n - 1] = L'\0';
                    n = wcslen(zh);
                    if (n > 0 && zh[n - 1] == L'\r') zh[n - 1] = L'\0';

                    if (!pat[0] || !zh[0])
                    {
                        nBad++;
                    }
                    else if (g_patCount >= PAT_MAX)
                    {
                        nBad++;
                        if (nBad <= 3)
                            Logf(L"[pat] 模式数超过上限 %d，多余的被忽略", PAT_MAX);
                    }
                    else
                    {
                        PatEntry* e = &g_pat[g_patCount];

                        wcscpy_s(e->pat, PAT_TEXTLEN, pat);
                        wcscpy_s(e->zh, PAT_TEXTLEN, zh);
                        PatAnalyze(e);

                        if (e->caps == 0)
                        {
                            nWarn++;
                            Logf(L"[pat] 第 %d 行没有任何通配符，应放进 zh_dict.txt 走精确匹配: \"%s\"",
                                 lines, pat);
                        }
                        else if (e->holes < e->caps)
                        {
                            nWarn++;
                            Logf(L"[pat] 第 %d 行译文占位符(%d)少于原文通配符(%d)，"
                                 L"没被引用的捕获会追加到译文末尾: \"%s\"",
                                 lines, e->holes, e->caps, pat);
                        }

                        g_patCount++;
                    }
                }
            }
        }

        line = next;
    }

    HeapFree(GetProcessHeap(), 0, raw);
    Logf(L"[pat] 载入 %d 条模式（扫描 %d 行 / 坏行 %d / 告警 %d）",
         g_patCount, lines, nBad, nWarn);
}

// ---------------------------------------------------------------- 字体替换
//
// 为什么需要它：SI 的界面字体有两个来源，都写死在 exe 里，设置项管不到 ——
//   · 应用字体 PhApplicationFont = PhCreateApplicationFont() → PhInitializeFont()
//     → PhCreateFontHandle(L"Microsoft Sans Serif", 8, ...)   ← 8 号写死在 guisup.c
//     它管：页签（tabnew/theme）、菜单栏文字、表头大字号(16/22)、迷你信息窗、查找对话框
//   · 列表字体 PhCreateTreeWindowFont() 读设置项 "Font"，没设就用
//     SPI_GETICONTITLELOGFONT（Win10/11 = Segoe UI 9）
//   设置项「应用程序字体」只改第二条。所以那个 8 号只能从代码侧改。
//
// 走哪条路：**在字体被创建时改，不在画文字时改**
//   SI 的字体全部从两个入口出来：
//     PhCreateFontHandle(...)    → CreateFont(...)          （应用字体/等宽/迷你窗/图）
//     PhCreateIconTitleFont 等   → CreateFontIndirect(&lf)  （列表字体/消息字体/字体副本）
//   在这一层换掉 LOGFONT，SI **自己量文字时用的也是新字体**（GetTextMetrics /
//   GetTextExtent 都走同一个 HFONT），列宽、页签高、表头高会跟着重算，所以不会出现
//   「量的是旧字体、画的是新字体」那种错位。
//   反过来，如果在 DrawText/TextOut 钩子里临时 SelectObject 一个新字体：SI 是拿旧字体
//   量好矩形才来画的，换掉之后文字会被裁掉或压到相邻元素上 —— 那条路不走。
//
//   还没覆盖的：GetStockObject(DEFAULT_GUI_FONT)。系统库存字体不是 CreateFont 造出来的，
//   钩不到。它只作为兜底出现（有 PhApplicationFont 时基本用不到），先记录在案。
//
// 格式（独立文件 zh_font.txt，与词典同样的 TAB 分隔）
//   原字体[|原字号]<TAB>新字体[|新字号]
//     · 只给字体名   —— 只换名字，字号不动
//     · 两侧都给字号 —— 整个字族按「新/原」等比缩放 lfHeight。缩放的是像素高度，
//       所以 DPI 缩放自动跟随，不需要知道当前是多少 DPI（96 DPI 下 8 号 = -11px，
//       乘 9/8 后 = -12px，正是 9 号）。
//     · **没列出来的字体一律不动** —— 等宽字体（Lucida Console/Consolas）和插件自己的
//       字体因此天然安全，这也是用白名单而不是黑名单的原因。
//     · 新字体名必须是 ASCII：A 版入口（CreateFontA/CreateFontIndirectA）要把新名字转回
//       ANSI，非 ASCII 会让那条路静默失效。载入时直接把这类行判为坏行。
//
// 开关：ZHHK_FONT=0/off/false 关掉（A/B 对照用）；=其它值是路径。
// 想看 SI 到底造了哪些字体：ZHHK_FONTLOG=1（逐条记录，不改变行为）。要拿「替换前」的
// 清单就同时配 ZHHK_FONT=0。

#define FONT_MAX        64
#define FONT_TEXTLEN    160
#define FONT_LOG_MAX    60      // 逐条替换日志上限，防日志爆炸
#define FONT_LOG_ALLMAX 400     // ZHHK_FONTLOG=1 时逐条记录的上限

struct FontEntry
{
    wchar_t from[LF_FACESIZE];
    wchar_t to[LF_FACESIZE];
    int     fromPt;
    int     toPt;
};

static FontEntry g_font[FONT_MAX];
static int      g_fontCount = 0;
static wchar_t  g_fontPath[MAX_PATH];
static BOOL     g_doFont = TRUE;
static BOOL     g_fontLog = FALSE;
static volatile LONG g_fontSubst = 0;
static volatile LONG g_fontAll = 0;

// 只折 ASCII 的比较。口径与 PatUp 一致：界面字体名都是 ASCII，非 ASCII 按原样比。
static BOOL FontSame(const wchar_t* a, const wchar_t* b)
{
    for (; *a && *b; a++, b++)
    {
        if (PatUp(*a) != PatUp(*b))
            return FALSE;
    }
    return *a == L'\0' && *b == L'\0';
}

// 新字体名必须是纯 ASCII（见上面的说明）
static BOOL FontAscii(const wchar_t* s)
{
    for (; *s; s++)
    {
        if ((unsigned)*s > 0x7F)
            return FALSE;
    }
    return TRUE;
}

// 解析 "字体名" 或 "字体名|字号"。字号不在 4..200 就整行判坏。
static BOOL FontParseSide(const wchar_t* s, wchar_t* face, int faceCap, int* pt)
{
    const wchar_t* bar;
    const wchar_t* end;
    size_t n;

    *pt = 0;

    for (bar = s; *bar && *bar != L'|'; bar++)
        ;

    end = bar;
    while (s < end && (*s == L' ' || *s == L'\t')) s++;
    while (end > s && (end[-1] == L' ' || end[-1] == L'\t')) end--;

    n = (size_t)(end - s);
    if (n == 0 || n >= (size_t)faceCap)
        return FALSE;

    wcsncpy_s(face, (size_t)faceCap, s, n);
    face[n] = L'\0';

    if (*bar == L'|')
    {
        long v = wcstol(bar + 1, NULL, 10);
        if (v < 4 || v > 200)
            return FALSE;
        *pt = (int)v;
    }

    return TRUE;
}

// 核心：按白名单换掉字体名（给了字号就同时等比缩放像素高度），并把字符集归到
// DEFAULT_CHARSET。三个入口共用。
//
// 关于字符集：把拉丁字体换成中文界面字体时，字符集跟着归到 DEFAULT_CHARSET 才是语义
// 正确的（中文直接由该字体渲染，不走字体链接）。SI 自己建字体用的就是 DEFAULT_CHARSET
// （guisup.c 的 PhCreateFontHandle），这里与它保持同一口径。
//
// ★ 别把「选项页文字画成鬼画符」记到字符集头上 —— 我一开始就是这么误判的。
//   真正原因是「把 Tahoma/Segoe UI 这类系统字体也替换了」：GDI 把对话框的
//   "MS Shell Dlg 2" 映射成 Tahoma 去建字体，换掉它之后标准控件（Edit/Static）取到的
//   字形就是错的，表现为 "SystemInformer" 画成 "Pvpbj Rkd g bo"、中文反而正常。
//   实测对照：规则集加回 Tahoma/Segoe UI 就复现；只留 MS Sans Serif 一条就正常
//   （截图 build\shots\font_cmp_probeF.png）。详见 dict\zh_font.txt 顶部的第三条红线。
//
// ★ SYMBOL_CHARSET 的字体一律不动。那类字体（Marlett、各种图标字体）的「字符」其实是
//   字形序号，换掉字体名等于换掉整套图标，必须整族跳过。
//
// ★ 权重为 FW_DONTCARE(0) 的一律不动。
//   GDI 的字体链接（font linking）在解析「MS Shell Dlg 2」这类占位字体、以及为各语系
//   找替代字体时，会**用 CreateFont/CreateFontIndirect 真的把候选字体造一遍**。
//   实测这些探针长这样：字重=0（FW_DONTCARE）、高度=+10（单元格高）、字体名横跨
//   宋体 / 新宋体 / Microsoft JhengHei UI / Meiryo UI / Malgun Gothic / Yu Gothic UI /
//   Arial / Segoe UI / Tahoma —— 一看就是「各语系候选表」，不是界面要用的字体。
//   换掉它们会污染 GDI 的字体关联表。SI 自己要用的字体永远带真实字重
//   （FW_NORMAL=400 / FW_BOLD=700），所以「字重为 0 就跳过」这条判据精确且不误伤。
//   Marlett 恰好也是 0，多一层保护。
static BOOL FontSubst(wchar_t* face, size_t faceCap, LONG* height, int* charset, LONG weight,
                      BOOL log)
{
    int i;

    if (!g_fontCount || !face[0])
        return FALSE;

    if (weight == FW_DONTCARE)
        return FALSE;                       // GDI 字体链接的探针，不是界面字体

    if (charset && *charset == SYMBOL_CHARSET)
        return FALSE;                       // 图标字体：整族跳过，连名字都不换

    for (i = 0; i < g_fontCount; i++)
    {
        const FontEntry* e = &g_font[i];
        wchar_t from[LF_FACESIZE];
        LONG h0, h1;
        int cs0;

        if (!FontSame(face, e->from))
            continue;

        wcsncpy_s(from, LF_FACESIZE, face, _TRUNCATE);

        cs0 = charset ? *charset : -1;

        h0 = *height;
        h1 = h0;
        if (e->fromPt > 0 && e->toPt > 0 && h0 != 0)
        {
            // 64 位算：h0 只有几百，但别留溢出的余地
            h1 = (LONG)(((LONGLONG)h0 * e->toPt) / e->fromPt);
            if (h1 == 0)
                h1 = h0;
            *height = h1;
        }

        wcsncpy_s(face, faceCap, e->to, _TRUNCATE);
        if (charset)
            *charset = DEFAULT_CHARSET;

        if (log)
        {
            LONG n = InterlockedIncrement(&g_fontSubst);

            if (n <= FONT_LOG_MAX)
                Logf(L"[font] #%d \"%s\" h=%d cs=%d w=%d -> \"%s\" h=%d cs=%d",
                     (int)n, from, (int)h0, cs0, (int)weight, e->to, (int)h1, (int)DEFAULT_CHARSET);
            else if (n == FONT_LOG_MAX + 1)
                Logf(L"[font] 替换已超过 %d 条，之后不再逐条记录", FONT_LOG_MAX);
        }

        return TRUE;
    }

    return FALSE;
}

// ZHHK_FONTLOG=1 时把每个被创建的字体记一行（不改变任何行为）。
static void FontLogAll(const wchar_t* api, const wchar_t* face, LONG h, LONG w)
{
    LONG n;

    if (!g_fontLog)
        return;

    n = InterlockedIncrement(&g_fontAll);
    if (n <= FONT_LOG_ALLMAX)
        Logf(L"[fontall] %s \"%s\" h=%d w=%d", api, face ? face : L"", (int)h, (int)w);
}

// ZHHK_FONTFACE=1 时的「事后核对」：把刚建好的字体选进一个内存 DC，用 GetTextFace
// 读出 GDI **真正解析到**的字体名。
//
// 为什么要这一步：CreateFont 成功只说明「请求被接受」，不代表「用的是这个字体」。
// 字体名写错、字体没装、或者 GDI 觉得该做字体链接，都会悄悄换一个 —— 只看我们
// 传进去的名字是看不出来的。GetTextFace 读的是实际选中的那个，这是唯一能证的。
//
// 默认关闭：它要在钩子里建临时 DC，正常情况下没必要承担这个风险。只在做字体
// 验证时开，最多核对 FONT_FACE_MAX 次。
#define FONT_FACE_MAX   20

static BOOL g_fontFace = FALSE;
static volatile LONG g_fontFaceDone = 0;
static volatile LONG g_inFaceCheck = 0;

static void FontCheckRealized(HFONT hf, const wchar_t* want)
{
    HDC dc;
    HGDIOBJ old;
    wchar_t got[LF_FACESIZE];

    if (!g_fontFace || !hf)
        return;
    if (InterlockedCompareExchange(&g_inFaceCheck, 1, 0) != 0)
        return;                                  // 已经在这条链上，别递归下去
    if (InterlockedIncrement(&g_fontFaceDone) > FONT_FACE_MAX)
    {
        InterlockedExchange(&g_inFaceCheck, 0);
        return;
    }

    __try
    {
        dc = CreateCompatibleDC(NULL);
        if (dc)
        {
            got[0] = L'\0';
            old = SelectObject(dc, hf);
            if (!GetTextFaceW(dc, LF_FACESIZE, got))
                wcsncpy_s(got, LF_FACESIZE, L"(读不到)", _TRUNCATE);
            SelectObject(dc, old);
            DeleteDC(dc);
            Logf(L"[fontface] 请求 \"%s\"，GDI 实际给的是 \"%s\"", want, got);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    InterlockedExchange(&g_inFaceCheck, 0);
}

typedef HFONT (WINAPI *CreateFontW_t)(int, int, int, int, int, DWORD, DWORD, DWORD,
                                      DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR);typedef HFONT (WINAPI *CreateFontA_t)(int, int, int, int, int, DWORD, DWORD, DWORD,
                                      DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR);
typedef HFONT (WINAPI *CreateFontIndirectW_t)(const LOGFONTW*);
typedef HFONT (WINAPI *CreateFontIndirectA_t)(const LOGFONTA*);

static void* g_realCreateFontW = NULL;
static void* g_realCreateFontA = NULL;
static void* g_realCreateFontIndirectW = NULL;
static void* g_realCreateFontIndirectA = NULL;

static HFONT WINAPI HookCreateFontW(int cHeight, int cWidth, int cEscapement, int cOrientation,
                                    int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut,
                                    DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
                                    DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName)
{
    wchar_t face[LF_FACESIZE];
    LONG h = cHeight;
    int cs = (int)iCharSet;
    HFONT hf;
    BOOL sub = FALSE;

    face[0] = L'\0';
    if (pszFaceName)
        wcsncpy_s(face, LF_FACESIZE, pszFaceName, _TRUNCATE);

    FontLogAll(L"CreateFont", face, (LONG)cHeight, (LONG)cWeight);

    if (g_doFont)
        sub = FontSubst(face, LF_FACESIZE, &h, &cs, (LONG)cWeight, TRUE);
    if (sub)
    {
        cHeight = (int)h;
        iCharSet = (DWORD)cs;
    }

    hf = ((CreateFontW_t)g_realCreateFontW)(cHeight, cWidth, cEscapement, cOrientation,
        cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision,
        iQuality, iPitchAndFamily, face[0] ? face : pszFaceName);

    if (sub)
        FontCheckRealized(hf, face);

    return hf;
}

static HFONT WINAPI HookCreateFontA(int cHeight, int cWidth, int cEscapement, int cOrientation,
                                    int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut,
                                    DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
                                    DWORD iQuality, DWORD iPitchAndFamily, LPCSTR pszFaceName)
{
    char faceA[LF_FACESIZE];
    wchar_t face[LF_FACESIZE];
    LONG h = cHeight;
    int cs = (int)iCharSet;

    faceA[0] = '\0';
    if (pszFaceName)
        strncpy_s(faceA, LF_FACESIZE, pszFaceName, _TRUNCATE);

    if (g_doFont && faceA[0] &&
        MultiByteToWideChar(CP_ACP, 0, faceA, -1, face, LF_FACESIZE) > 0 &&
        FontSubst(face, LF_FACESIZE, &h, &cs, (LONG)cWeight, TRUE))
    {
        // 新名字转不回 ANSI 就整个放弃（名字没换，高度/字符集也不能换，否则是不一致的字体）
        if (WideCharToMultiByte(CP_ACP, 0, face, -1, faceA, LF_FACESIZE, NULL, NULL) > 0)
        {
            cHeight = (int)h;
            iCharSet = (DWORD)cs;
        }
    }

    return ((CreateFontA_t)g_realCreateFontA)(cHeight, cWidth, cEscapement, cOrientation,
        cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision,
        iQuality, iPitchAndFamily, faceA[0] ? faceA : pszFaceName);
}

static HFONT WINAPI HookCreateFontIndirectW(const LOGFONTW* lplf)
{
    LOGFONTW lf;
    HFONT hf;
    BOOL sub = FALSE;

    if (!lplf)
        return ((CreateFontIndirectW_t)g_realCreateFontIndirectW)(lplf);

    __try
    {
        lf = *lplf;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return ((CreateFontIndirectW_t)g_realCreateFontIndirectW)(lplf);
    }

    FontLogAll(L"CreateFontIndirect", lf.lfFaceName, lf.lfHeight, lf.lfWeight);

    if (g_doFont)
    {
        int cs = (int)lf.lfCharSet;

        sub = FontSubst(lf.lfFaceName, LF_FACESIZE, &lf.lfHeight, &cs, lf.lfWeight, TRUE);
        if (sub)
            lf.lfCharSet = (BYTE)cs;
    }

    // 传副本，不改调用方的缓冲区：有的调用方拿同一个 LOGFONT 反复建字体/算尺寸，
    // 就地改掉会让它后面的判断也跟着变。
    hf = ((CreateFontIndirectW_t)g_realCreateFontIndirectW)(&lf);

    if (sub)
        FontCheckRealized(hf, lf.lfFaceName);

    return hf;
}

static HFONT WINAPI HookCreateFontIndirectA(const LOGFONTA* lplf)
{
    LOGFONTA la;
    wchar_t face[LF_FACESIZE];
    LONG h;

    if (!lplf)
        return ((CreateFontIndirectA_t)g_realCreateFontIndirectA)(lplf);

    __try
    {
        la = *lplf;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return ((CreateFontIndirectA_t)g_realCreateFontIndirectA)(lplf);
    }

    if (!g_doFont || !la.lfFaceName[0])
        return ((CreateFontIndirectA_t)g_realCreateFontIndirectA)(&la);

    if (MultiByteToWideChar(CP_ACP, 0, la.lfFaceName, -1, face, LF_FACESIZE) <= 0)
        return ((CreateFontIndirectA_t)g_realCreateFontIndirectA)(&la);

    h = la.lfHeight;
    {
        int cs = (int)la.lfCharSet;

        if (FontSubst(face, LF_FACESIZE, &h, &cs, la.lfWeight, TRUE))
        {
            if (WideCharToMultiByte(CP_ACP, 0, face, -1, la.lfFaceName, LF_FACESIZE, NULL, NULL) > 0)
            {
                la.lfHeight = h;
                la.lfCharSet = (BYTE)cs;
            }
        }
    }

    return ((CreateFontIndirectA_t)g_realCreateFontIndirectA)(&la);
}

static void LoadFonts(void)
{
    char* raw = NULL;
    DWORD size = 0;
    char* line;
    char* next;
    int lines = 0, nBad = 0, nDup = 0;

    if (!g_doFont)
    {
        Logf(L"[font] 字体替换已关闭（ZHHK_FONT）");
        return;
    }

    if (!*g_fontPath)
        return;

    if (!ReadWholeFileW(g_fontPath, &raw, &size))
    {
        Logf(L"[font] 没有字体表（可选，不是错误）: %s", g_fontPath);
        return;
    }

    Logf(L"[font] path=%s (%lu 字节)", g_fontPath, size);

    line = raw;
    while (line && *line)
    {
        char* nl;
        char* tab;

        next = line;
        nl = strchr(line, '\n');
        if (nl)
        {
            *nl = '\0';
            next = nl + 1;
        }
        else
        {
            next = NULL;
        }

        lines++;

        if (line[0] == (char)0xEF && line[1] == (char)0xBB && line[2] == (char)0xBF)
            line += 3;

        if (line[0] != '\0' && line[0] != '#')
        {
            tab = strchr(line, '\t');
            if (!tab)
            {
                nBad++;
                if (nBad <= 3)
                    Logf(L"[font] 第 %d 行没有 TAB，已忽略: \"%hs\"", lines, line);
            }
            else
            {
                wchar_t left[FONT_TEXTLEN], right[FONT_TEXTLEN];
                wchar_t from[LF_FACESIZE], to[LF_FACESIZE];
                int fromPt = 0, toPt = 0;
                size_t n;

                *tab = '\0';
                if (MultiByteToWideChar(CP_UTF8, 0, line, -1, left, FONT_TEXTLEN) <= 0 ||
                    MultiByteToWideChar(CP_UTF8, 0, tab + 1, -1, right, FONT_TEXTLEN) <= 0)
                {
                    nBad++;
                    if (nBad <= 3)
                        Logf(L"[font] 第 %d 行 UTF-8 转换失败", lines);
                    line = next;
                    continue;
                }

                n = wcslen(left);
                if (n > 0 && left[n - 1] == L'\r') left[n - 1] = L'\0';
                n = wcslen(right);
                if (n > 0 && right[n - 1] == L'\r') right[n - 1] = L'\0';

                if (!FontParseSide(left, from, LF_FACESIZE, &fromPt) ||
                    !FontParseSide(right, to, LF_FACESIZE, &toPt))
                {
                    nBad++;
                    if (nBad <= 3)
                        Logf(L"[font] 第 %d 行格式不合法（应为「原字体[|字号]<TAB>新字体[|字号]」，"
                             L"字号 4~200）", lines);
                }
                else if (!FontAscii(to))
                {
                    nBad++;
                    if (nBad <= 3)
                        Logf(L"[font] 第 %d 行的新字体名不是 ASCII，A 版入口会失效，已忽略: \"%s\"",
                             lines, to);
                }
                else if (g_fontCount >= FONT_MAX)
                {
                    nBad++;
                    if (nBad <= 3)
                        Logf(L"[font] 规则数超过上限 %d，多余的被忽略", FONT_MAX);
                }
                else
                {
                    int i;
                    BOOL dup = FALSE;

                    for (i = 0; i < g_fontCount; i++)
                    {
                        if (FontSame(g_font[i].from, from))
                        {
                            dup = TRUE;
                            break;
                        }
                    }

                    if (dup)
                    {
                        // 靠前的先用；重复的原字体名一律忽略并记数，免得两条规则
                        // 抢同一个字体、结果取决于文件顺序而没人发现。
                        nDup++;
                        Logf(L"[font] 第 %d 行的原字体 \"%s\" 已出现过，已忽略", lines, from);
                    }
                    else
                    {
                        FontEntry* e = &g_font[g_fontCount];

                        wcscpy_s(e->from, LF_FACESIZE, from);
                        wcscpy_s(e->to, LF_FACESIZE, to);
                        e->fromPt = fromPt;
                        e->toPt = toPt;
                        g_fontCount++;
                    }
                }
            }
        }

        line = next;
    }

    HeapFree(GetProcessHeap(), 0, raw);
    Logf(L"[font] 载入 %d 条替换规则（扫描 %d 行 / 坏行 %d / 重复 %d）",
         g_fontCount, lines, nBad, nDup);
}

// ---------------------------------------------------------------- 翻译

// 词典查找（输入已在 key 中）——哈希比对，未命中才做全等比较
static const wchar_t* LookupDict(const wchar_t* key, int keyLen, wchar_t* out, int outCap)
{
    DWORD h = HashStr(key);

    for (int i = 0; i < g_entryCount; i++)
    {
        if (g_entries[i].hash != h)
            continue;
        if (_wcsicmp(g_entries[i].en, key) != 0)
            continue;

        wcsncpy_s(out, outCap, g_entries[i].zh, _TRUNCATE);
        return out;
    }
    (void)keyLen;
    return NULL;
}

// 输入安全拷贝到 out，返回长度；不做翻译

// 翻译入口。
//
// 性能设计（必须）：列表/树每帧都会把同一批字符串重新送进来，若每次都做
// 字符串拷贝 + 哈希，在 5000 条词典下主界面会明显卡顿。这里做一个
// 「指针 → 结果」的直接映射 memo：
//   * 命中且文本未变  -> 零字典查询，零分配
//   * 同一指针但文本变了（缓冲区复用）-> 用精确比较发现并重新计算
#define MEMO_SIZE 4096
struct MemoSlot { const void* ptr; DWORD hash; const wchar_t* out; };
static MemoSlot g_memo[MEMO_SIZE];

static const wchar_t* TryTranslate(const wchar_t* s, wchar_t* out, int outCap)
{
    MemoSlot* slot;
    DWORD h;
    int len;
    const wchar_t* r;

    if (!s || !*s || g_entryCount == 0 || outCap < 2)
        return NULL;

    slot = &g_memo[(((uintptr_t)s) >> 3) & (MEMO_SIZE - 1)];

    len = SafeText(s, out, outCap);
    if (len <= 0)
        return NULL;

    if (slot->ptr == (const void*)s && slot->out && _wcsicmp(slot->out, out) == 0)
    {
        if (!slot->out[0])
            return NULL;                       // 缓存了「未命中」
        wcsncpy_s(out, outCap, slot->out, _TRUNCATE);
        return out;
    }

    h = HashStr(out);

    // 1) 精确匹配
    r = LookupDict(out, len, out, outCap);

    // 去 & 后的形态：词典允许以「去掉 & 加速键前缀」的形式书写，
    // 模式表同理（源码里不少菜单串是 L"&Copy\bCtrl+C" 这种）。
    {
        wchar_t stripped[MAX_TEXTLEN];
        BOOL hasAmp = FALSE;
        int k = 0;

        for (int i = 0; out[i]; i++)
        {
            if (out[i] == L'&')
                hasAmp = TRUE;
            else
                stripped[k++] = out[i];
        }
        stripped[k] = L'\0';

        // 2) 精确匹配（去 & 形态）
        if (!r && hasAmp && k > 0)
            r = LookupDict(stripped, k, out, outCap);

        // 3) 通配符模式。运行期拼出来的串只能靠这一步（见「通配符模式翻译」）。
        //    src 是必需的：MatchPatterns 会把结果写回 out，而那正是它的输入。
        if (!r && g_patCount > 0)
        {
            wchar_t src[MAX_TEXTLEN];

            wcsncpy_s(src, MAX_TEXTLEN, out, _TRUNCATE);
            r = MatchPatterns(src, out, outCap);

            if (!r && hasAmp && k > 0)
                r = MatchPatterns(stripped, out, outCap);
        }
    }

    (void)h;

    // 写入 memo（存指针，不存文本 —— 零分配）
    slot->ptr = (const void*)s;
    slot->hash = h;
    slot->out = r ? PoolDup(out) : L"";

    return r;
}

// ---------------------------------------------------------------- 消息处理

static const char* MsgName(UINT msg)
{
    switch (msg)
    {
    case CM_LVM_INSERTCOLUMNW:  return "CM_LVM_INSERTCOLUMNW";
    case CM_LVM_SETCOLUMNW:     return "CM_LVM_SETCOLUMNW";
    case CM_HDM_INSERTITEMW:    return "CM_HDM_INSERTITEMW";
    case CM_HDM_SETITEMW:       return "CM_HDM_SETITEMW";
    case CM_TB_ADDBUTTONS:      return "TB_ADDBUTTONS";
    case CM_TB_SETBUTTONINFO:   return "TB_SETBUTTONINFO";
    case CM_TB_GETBUTTONTEXT:   return "TB_GETBUTTONTEXT";
    case CM_TB_BUTTONSTRUCTSIZE:return "TB_BUTTONSTRUCTSIZE";
    case CM_SB_SETTEXTW:        return "CM_SB_SETTEXTW";
    case WM_SETTEXT:            return "WM_SETTEXT";
    case CM_LVM_INSERTITEMW:    return "CM_LVM_INSERTITEMW";
    case CM_LVM_SETITEMW:       return "CM_LVM_SETITEMW";
    default:                    return NULL;
    }
}

// 处理「结构体里带字符串指针」的消息；返回 TRUE 表示已替换（无需调用真实函数再改）
static BOOL HandleStructMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT* outMsg)
{
    const wchar_t* text = NULL;
    const wchar_t* newText = NULL;
    wchar_t translated[MAX_TEXTLEN];
    wchar_t oldText[MAX_TEXTLEN];

    if (msg == CM_LVM_INSERTCOLUMNW || msg == CM_LVM_SETCOLUMNW)
    {
        LVCOLUMNW* col;
        if (!lParam) return FALSE;
        col = (LVCOLUMNW*)lParam;
        __try { text = (col->mask & LVCF_TEXT) ? col->pszText : NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { text = NULL; }
        if (!text) return FALSE;
        SafeText(text, oldText, MAX_TEXTLEN);
        if (!g_doTranslate) { Logf(L"[hit] %hs hwnd=%p \"%s\"", MsgName(msg), hwnd, oldText); return FALSE; }
        newText = TryTranslate(text, translated, MAX_TEXTLEN);
        Logf(L"[%s] %hs hwnd=%p \"%s\" -> \"%s\"", newText ? L"HIT " : L"MISS", MsgName(msg), hwnd, oldText, newText ? newText : L"-");
        if (!newText) { MissNote(MsgName(msg), oldText, hwnd, NULL); return FALSE; }
        __try { col->pszText = (LPWSTR)newText; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
        return TRUE;
    }

    if (msg == CM_HDM_INSERTITEMW || msg == CM_HDM_SETITEMW)
    {
        HDITEMW* it;
        if (!lParam) return FALSE;
        it = (HDITEMW*)lParam;
        __try { text = (it->mask & HDI_TEXT) ? it->pszText : NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { text = NULL; }
        if (!text) return FALSE;
        SafeText(text, oldText, MAX_TEXTLEN);
        if (!g_doTranslate) { Logf(L"[hit] %hs hwnd=%p \"%s\"", MsgName(msg), hwnd, oldText); return FALSE; }
        newText = TryTranslate(text, translated, MAX_TEXTLEN);
        Logf(L"[%s] %hs hwnd=%p \"%s\" -> \"%s\"", newText ? L"HIT " : L"MISS", MsgName(msg), hwnd, oldText, newText ? newText : L"-");
        if (!newText) { MissNote(MsgName(msg), oldText, hwnd, NULL); return FALSE; }
        __try { it->pszText = (LPWSTR)newText; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
        return TRUE;
    }

    if (msg == CM_TB_ADDBUTTONS)
    {
        TBBUTTON* arr = (TBBUTTON*)lParam;
        int count = (int)wParam;
        int i;
        if (!arr || count <= 0 || count > 4096) return FALSE;

        for (i = 0; i < count; i++)
        {
            const wchar_t* s = NULL;
            __try
            {
                INT_PTR v = arr[i].iString;
                if (v > 0x10000 && (v & 1) == 0)
                    s = (const wchar_t*)v;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { s = NULL; }

            if (!s) continue;
            SafeText(s, oldText, MAX_TEXTLEN);
            if (!g_doTranslate) { Logf(L"[hit] CM_TB_ADDBUTTONS[%d] hwnd=%p \"%s\"", i, hwnd, oldText); continue; }
            newText = TryTranslate(s, translated, MAX_TEXTLEN);
            Logf(L"[%s] CM_TB_ADDBUTTONS[%d] hwnd=%p \"%s\" -> \"%s\"", newText ? L"HIT " : L"MISS", i, hwnd, oldText, newText ? newText : L"-");
            if (!newText) { MissNote("TB_ADDBUTTONS", oldText, hwnd, NULL); continue; }
            __try { arr[i].iString = (INT_PTR)(PVOID)newText; }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        return TRUE;
    }

    if (msg == CM_TB_SETBUTTONINFO)
    {
        TBBUTTONINFOW* bi = (TBBUTTONINFOW*)lParam;
        if (!bi) return FALSE;
        __try
        {
            if ((bi->dwMask & TBIF_TEXT) && bi->cbSize >= RTL_SIZEOF_THROUGH_FIELD(TBBUTTONINFOW, pszText))
                text = bi->pszText;
            else
                text = NULL;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { text = NULL; }
        if (!text) return FALSE;
        SafeText(text, oldText, MAX_TEXTLEN);
        if (!g_doTranslate) { Logf(L"[hit] CM_TB_SETBUTTONINFO hwnd=%p \"%s\"", hwnd, oldText); return FALSE; }
        newText = TryTranslate(text, translated, MAX_TEXTLEN);
        Logf(L"[%s] CM_TB_SETBUTTONINFO hwnd=%p \"%s\" -> \"%s\"", newText ? L"HIT " : L"MISS", hwnd, oldText, newText ? newText : L"-");
        if (!newText) { MissNote("TB_SETBUTTONINFO", oldText, hwnd, NULL); return FALSE; }
        __try { bi->pszText = (LPWSTR)newText; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
        return TRUE;
    }

    (void)outMsg;
    return FALSE;
}

// 处理「lParam 就是字符串指针」的消息
static LPARAM HandleDirectText(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    const wchar_t* text = (const wchar_t*)lParam;
    wchar_t oldText[MAX_TEXTLEN];
    wchar_t translated[MAX_TEXTLEN];
    const wchar_t* newText;

    if (!SafeText(text, oldText, MAX_TEXTLEN))
        return lParam;

    if (!g_doTranslate)
    {
        Logf(L"[hit] %hs hwnd=%p \"%s\"", MsgName(msg), hwnd, oldText);
        return lParam;
    }

    newText = TryTranslate(text, translated, MAX_TEXTLEN);
    Logf(L"[%s] %hs hwnd=%p \"%s\" -> \"%s\"", newText ? L"HIT " : L"MISS", MsgName(msg), hwnd, oldText, newText ? newText : L"-");
    if (!newText)
    {
        MissNote(MsgName(msg), oldText, hwnd, NULL);
        return lParam;
    }

    return (LPARAM)newText;
}

// ---------------------------------------------------------------- Hook: SendMessageW

typedef LRESULT (WINAPI *SendMessageW_t)(HWND, UINT, WPARAM, LPARAM);
// ---------------------------------------------------------------- Hook: BeginPaint/EndPaint
//
// 维护「当前正在绘制的窗口」。为什么需要：System Informer 的列表（PhTreeNew）
// 是**自绘**的 —— 在自己的 WM_PAINT 里建一个内存 DC，画完再 BitBlt 上去。
// 内存 DC 用 WindowFromDC() 查不到窗口，于是行内容全都丢了窗口上下文
// （第一版实测：178 条里 161 条 chain='-'），而「这条字符串属于哪个列表」
// 正是要靠这个上下文才分得出来。
//
// 用线程本地的栈记下 BeginPaint 的窗口：绘制期间（哪怕画在内存 DC 上）都算
// 在这个窗口名下；嵌套 BeginPaint 取最内层。
typedef HDC  (WINAPI *BeginPaint_t)(HWND, LPPAINTSTRUCT);
typedef BOOL (WINAPI *EndPaint_t)(HWND, const PAINTSTRUCT*);

static void* g_realBeginPaint = NULL;     // 与 g_realDrawTextW 同样用 void*，配合钩子表
static void* g_realEndPaint   = NULL;

static HDC WINAPI HookBeginPaint(HWND hwnd, LPPAINTSTRUCT ps)
{
    HDC r = ((BeginPaint_t)g_realBeginPaint)(hwnd, ps);
    if (g_paintDepth < PAINT_STACK)
        g_paintStack[g_paintDepth++] = hwnd;
    return r;
}

static BOOL WINAPI HookEndPaint(HWND hwnd, const PAINTSTRUCT* ps)
{
    if (g_paintDepth > 0)
        g_paintDepth--;
    return ((EndPaint_t)g_realEndPaint)(hwnd, ps);
}

typedef int (WINAPI *DrawTextW_t)(HDC, LPCWSTR, int, LPRECT, UINT);
typedef int (WINAPI *DrawTextExW_t)(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
typedef BOOL (WINAPI *TextOutW_t)(HDC, int, int, LPCWSTR, int);
typedef BOOL (WINAPI *ExtTextOutW_t)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
typedef int (WINAPI *DrawTextA_t)(HDC, LPCSTR, int, LPRECT, UINT);
typedef BOOL (WINAPI *ExtTextOutA_t)(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
typedef BOOL (WINAPI *TextOutA_t)(HDC, int, int, LPCSTR, int);

// 把一个 ANSI 串转成宽字符，取回可读长度（不含结尾 NUL）。失败返回 -1。
static int AnsiToWide(LPCSTR text, int count, wchar_t* out, int outCch)
{
    int n;

    if (!text || outCch < 2)
        return -1;

    n = MultiByteToWideChar(CP_ACP, 0, text, count < 0 ? -1 : count, out, outCch - 1);
    if (n <= 0)
        return -1;
    if (count < 0)
        n--;                    // 上面按包含 NUL 转换，这里去掉
    if (n <= 0)
        return -1;
    out[n] = 0;
    return n;
}

static LRESULT WINAPI HookSendMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SendMessageW_t real = (SendMessageW_t)g_realSendMessageW;

    if (g_traceTb && msg >= 0x0400 && msg <= 0x0460)
    {
        wchar_t cls[128] = L"?";
        wchar_t txt[MAX_TEXTLEN];
        int tl;
        GetClassNameW(hwnd, cls, 128);
        tl = SafeText((const wchar_t*)lParam, txt, 200);
        Logf(L"[TRACE] class=%s msg=0x%04X wParam=%p text=\"%s\"",
             cls, msg, (void*)wParam, tl > 0 ? txt : L"");
    }

    if (!real)
        return 0;

    switch (msg)
    {
    case CM_LVM_INSERTCOLUMNW: case CM_LVM_SETCOLUMNW:
    case CM_HDM_INSERTITEMW:   case CM_HDM_SETITEMW:
    case CM_TB_ADDBUTTONS:    case CM_TB_SETBUTTONINFO:
        HandleStructMsg(hwnd, msg, wParam, lParam, NULL);
        return real(hwnd, msg, wParam, lParam);

    case CM_SB_SETTEXTW:
    case WM_SETTEXT:
        lParam = HandleDirectText(hwnd, msg, wParam, lParam);
        return real(hwnd, msg, wParam, lParam);

    case CM_TB_GETBUTTONTEXT:
    {
        LRESULT r = real(hwnd, msg, wParam, lParam);
        if (g_doTranslate && r > 0 && lParam)
        {
            wchar_t* dst = (wchar_t*)lParam;
            wchar_t oldText[MAX_TEXTLEN];
            wchar_t translated[MAX_TEXTLEN];
            const wchar_t* nt;
            if (SafeText(dst, oldText, MAX_TEXTLEN) && *oldText)
            {
                nt = TryTranslate(dst, translated, MAX_TEXTLEN);
                Logf(L"[%s] CM_TB_GETBUTTONTEXT hwnd=%p \"%s\" -> \"%s\"", nt ? L"HIT " : L"MISS", hwnd, oldText, nt ? nt : L"-");
                if (nt)
                {
                    size_t cap = (size_t)r + 1;
                    __try
                    {
                        wcsncpy_s(dst, cap, translated, _TRUNCATE);
                        // 同 DrawTextW 的道理：译文比原文短时若仍返回旧的 r，
                        // 调用方按 r 去读就会读到缓冲区里残留的英文。
                        r = (LRESULT)wcslen(dst);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) { }
                }
                else
                {
                    MissNote("TB_GETBUTTONTEXT", oldText, hwnd, NULL);
                }
            }
        }
        return r;
    }

    default:
        // 观测模式：把控制消息范围全部记下来，用于定位工具栏走的是哪条路径
        if (g_logAll && msg >= WM_USER && msg <= WM_USER + 200)
            Logf(L"[msg] hwnd=%p msg=0x%04X (WM_USER+%u) wParam=%p lParam=%p",
                 hwnd, msg, msg - WM_USER, (void*)wParam, (void*)lParam);
        return real(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------- Hook: 自绘文本

static int WINAPI HookDrawTextW(HDC hdc, LPCWSTR text, int count, LPRECT rect, UINT format)
{
    wchar_t oldText[MAX_TEXTLEN];
    wchar_t translated[MAX_TEXTLEN];
    const wchar_t* nt;
    const wchar_t* use = text;
    int useCount = count;

    if (!g_realDrawTextW)
        return 0;

    if (SafeText(text, oldText, MAX_TEXTLEN) <= 0)
        return ((DrawTextW_t)g_realDrawTextW)(hdc, text, count, rect, format);

    if (!g_doTranslate)
    {
        if (g_logAll)
            Logf(L"[draw] \"%s\"", oldText);
    }
    else if ((nt = TryTranslate(text, translated, MAX_TEXTLEN)) != NULL)
    {
        // 关键：必须传「译文的长度」，绝不能用调用方给的原文长度。
        // comctl32 画表头时传显式 count（= 英文原文长度），而译文（中文）通常更短；
        // 沿用原 count 会让 DrawTextW 越过译文结尾继续往后读，把同一缓冲区里
        // SafeText 先拷进去、wcsncpy_s 没覆盖掉的英文残尾一起画出来 ——
        // 现象就是「中文后面拖着英文，像没替换完」。
        use = translated;
        useCount = (int)wcslen(translated);
        Logf(L"[HIT ] DrawTextW \"%s\" -> \"%s\" (count=%d->%d)", oldText, use, count, useCount);
    }
    else
    {
        MissNote("DrawTextW", oldText, NULL, hdc, rect ? rect->left : -1);
    }

    {
        RealGuard zh_guard;
        return ((DrawTextW_t)g_realDrawTextW)(hdc, use, useCount, rect, format);
    }
}

static BOOL WINAPI HookTextOutW(HDC hdc, int x, int y, LPCWSTR text, int count)
{
    wchar_t oldText[MAX_TEXTLEN];
    wchar_t translated[MAX_TEXTLEN];
    const wchar_t* nt;
    const wchar_t* use = text;
    int useCount = count;

    if (!g_realTextOutW)
        return FALSE;

    if (SafeText(text, oldText, MAX_TEXTLEN) <= 0)
        return ((TextOutW_t)g_realTextOutW)(hdc, x, y, text, count);

    if (g_doTranslate && (nt = TryTranslate(text, translated, MAX_TEXTLEN)) != NULL)
    {
        use = translated;
        useCount = (int)wcslen(translated);
        Logf(L"[HIT ] TextOutW \"%s\" -> \"%s\"", oldText, use);
    }
    else if (g_doTranslate)
    {
        MissNote("TextOutW", oldText, NULL, hdc, x);
    }

    {
        RealGuard zh_guard;
        return ((TextOutW_t)g_realTextOutW)(hdc, x, y, use, useCount);
    }
}

// DrawTextExW —— 带 DRAWTEXTPARAMS 的变体，自绘代码常用
static int WINAPI HookDrawTextExW(HDC hdc, LPWSTR text, int count, LPRECT rect,
                                  UINT format, LPDRAWTEXTPARAMS params)
{
    wchar_t oldText[MAX_TEXTLEN];
    wchar_t translated[MAX_TEXTLEN];
    const wchar_t* nt;
    const wchar_t* use = text;
    int useCount = count;

    if (!g_realDrawTextExW)
        return 0;

    if (SafeText(text, oldText, MAX_TEXTLEN) <= 0)
        return ((DrawTextExW_t)g_realDrawTextExW)(hdc, text, count, rect, format, params);

    if (g_doTranslate && (nt = TryTranslate(text, translated, MAX_TEXTLEN)) != NULL)
    {
        use = translated;
        useCount = -1;
        Logf(L"[HIT ] DrawTextExW \"%s\" -> \"%s\"", oldText, use);
    }
    else if (g_doTranslate)
    {
        MissNote("DrawTextExW", oldText, NULL, hdc, rect ? rect->left : -1);
    }
    else if (g_logAll)
    {
        Logf(L"[drawEx] \"%s\"", oldText);
    }

    {
        RealGuard zh_guard;
        return ((DrawTextExW_t)g_realDrawTextExW)(hdc, (LPWSTR)use, useCount, rect, format, params);
    }
}

// ExtTextOutW —— GDI 最底层的文本出口。user32 内部画菜单栏、以及自绘控件
// （PhTabNew 标签栏这类）往往绕过 DrawTextW 直接走它，这正是菜单栏/标签栏
// 联不通、而表头联得通的最可能解释。
//
// 三条护栏，全部遵循「宁可漏译，不可画错」：
//   1. ETO_GLYPH_INDEX 时字符串是字形索引而不是字符，翻译它是灾难；
//   2. count==0 或 text==NULL 是「只用 ETO_OPAQUE 擦背景」的调用，没有文字；
//   3. lpDx 是「相邻字符原点间距」数组，长度按 count 计。译文若比原文长，
//      就会越过数组末尾读脏数据 —— 所以只在「译文不长于原文」时才替换。
static BOOL WINAPI HookExtTextOutW(HDC hdc, int x, int y, UINT options, const RECT* lprect,
                                   LPCWSTR text, UINT count, const INT* lpDx)
{
    wchar_t oldText[MAX_TEXTLEN];
    wchar_t translated[MAX_TEXTLEN];
    const wchar_t* nt;
    int n;

    if (!g_realExtTextOutW)
        return FALSE;

    if (g_inReal || !text || count == 0 || (options & ETO_GLYPH_INDEX))
        return ((ExtTextOutW_t)g_realExtTextOutW)(hdc, x, y, options, lprect, text, count, lpDx);

    if (SafeText(text, oldText, MAX_TEXTLEN) <= 0)
        return ((ExtTextOutW_t)g_realExtTextOutW)(hdc, x, y, options, lprect, text, count, lpDx);

    if (!g_doTranslate)
    {
        if (g_logAll)
            Logf(L"[draw] ExtTextOutW \"%s\"", oldText);
        return ((ExtTextOutW_t)g_realExtTextOutW)(hdc, x, y, options, lprect, text, count, lpDx);
    }

    nt = TryTranslate(text, translated, MAX_TEXTLEN);
    if (!nt)
    {
        MissNote("ExtTextOutW", oldText, NULL, hdc, x);
        return ((ExtTextOutW_t)g_realExtTextOutW)(hdc, x, y, options, lprect, text, count, lpDx);
    }

    n = (int)wcslen(translated);
    if (lpDx && n > (int)count)
    {
        Logf(L"[skip] ExtTextOutW \"%s\" -> \"%s\" 译文更长且带 lpDx, 放弃替换", oldText, translated);
        return ((ExtTextOutW_t)g_realExtTextOutW)(hdc, x, y, options, lprect, text, count, lpDx);
    }

    Logf(L"[HIT ] ExtTextOutW \"%s\" -> \"%s\"", oldText, translated);
    return ((ExtTextOutW_t)g_realExtTextOutW)(hdc, x, y, options, lprect, translated, (UINT)n, lpDx);
}

// DrawTextA —— ANSI 变体，兜底用
static int WINAPI HookDrawTextA(HDC hdc, LPCSTR text, int count, LPRECT rect, UINT format)
{
    wchar_t w[MAX_TEXTLEN];
    wchar_t translated[MAX_TEXTLEN];
    char a[MAX_TEXTLEN * 2];
    const wchar_t* nt;
    int r;

    if (!g_realDrawTextA)
        return 0;

    if (g_inReal || AnsiToWide(text, count, w, MAX_TEXTLEN) <= 0)
        return ((DrawTextA_t)g_realDrawTextA)(hdc, text, count, rect, format);

    if (!g_doTranslate)
    {
        if (g_logAll)
            Logf(L"[draw] DrawTextA \"%s\"", w);
        return ((DrawTextA_t)g_realDrawTextA)(hdc, text, count, rect, format);
    }

    nt = TryTranslate(w, translated, MAX_TEXTLEN);
    if (!nt)
    {
        MissNote("DrawTextA", w, NULL, hdc, rect ? rect->left : -1);
        return ((DrawTextA_t)g_realDrawTextA)(hdc, text, count, rect, format);
    }

    r = WideCharToMultiByte(CP_ACP, 0, translated, -1, a, (int)sizeof(a), NULL, NULL);
    if (r <= 0)
        return ((DrawTextA_t)g_realDrawTextA)(hdc, text, count, rect, format);

    Logf(L"[HIT ] DrawTextA \"%s\" -> \"%s\" (count=%d->%d)", w, translated, count, r - 1);
    return ((DrawTextA_t)g_realDrawTextA)(hdc, a, r - 1, rect, format);
}

static BOOL WINAPI HookExtTextOutA(HDC hdc, int x, int y, UINT options, const RECT* lprect,
                                   LPCSTR text, UINT count, const INT* lpDx)
{
    wchar_t w[MAX_TEXTLEN];
    wchar_t translated[MAX_TEXTLEN];
    char a[MAX_TEXTLEN * 2];
    const wchar_t* nt;
    int r;

    if (!g_realExtTextOutA)
        return FALSE;

    if (g_inReal || !text || count == 0 || (options & ETO_GLYPH_INDEX) ||
        AnsiToWide(text, (int)count, w, MAX_TEXTLEN) <= 0)
        return ((ExtTextOutA_t)g_realExtTextOutA)(hdc, x, y, options, lprect, text, count, lpDx);

    if (!g_doTranslate)
    {
        if (g_logAll)
            Logf(L"[draw] ExtTextOutA \"%s\"", w);
        return ((ExtTextOutA_t)g_realExtTextOutA)(hdc, x, y, options, lprect, text, count, lpDx);
    }

    nt = TryTranslate(w, translated, MAX_TEXTLEN);
    if (!nt)
    {
        MissNote("ExtTextOutA", w, NULL, hdc, x);
        return ((ExtTextOutA_t)g_realExtTextOutA)(hdc, x, y, options, lprect, text, count, lpDx);
    }

    r = WideCharToMultiByte(CP_ACP, 0, translated, -1, a, (int)sizeof(a), NULL, NULL);
    if (r <= 0 || (lpDx && r - 1 > (int)count))
        return ((ExtTextOutA_t)g_realExtTextOutA)(hdc, x, y, options, lprect, text, count, lpDx);

    Logf(L"[HIT ] ExtTextOutA \"%s\" -> \"%s\"", w, translated);
    return ((ExtTextOutA_t)g_realExtTextOutA)(hdc, x, y, options, lprect, a, (UINT)(r - 1), lpDx);
}

static BOOL WINAPI HookTextOutA(HDC hdc, int x, int y, LPCSTR text, int count)
{
    wchar_t w[MAX_TEXTLEN];
    wchar_t translated[MAX_TEXTLEN];
    char a[MAX_TEXTLEN * 2];
    const wchar_t* nt;
    int r;

    if (!g_realTextOutA)
        return FALSE;

    if (g_inReal || AnsiToWide(text, count, w, MAX_TEXTLEN) <= 0)
        return ((TextOutA_t)g_realTextOutA)(hdc, x, y, text, count);

    if (!g_doTranslate)
        return ((TextOutA_t)g_realTextOutA)(hdc, x, y, text, count);

    nt = TryTranslate(w, translated, MAX_TEXTLEN);
    if (!nt)
    {
        MissNote("TextOutA", w, NULL, hdc, x);
        return ((TextOutA_t)g_realTextOutA)(hdc, x, y, text, count);
    }

    r = WideCharToMultiByte(CP_ACP, 0, translated, -1, a, (int)sizeof(a), NULL, NULL);
    if (r <= 0)
        return ((TextOutA_t)g_realTextOutA)(hdc, x, y, text, count);

    Logf(L"[HIT ] TextOutA \"%s\" -> \"%s\"", w, translated);
    return ((TextOutA_t)g_realTextOutA)(hdc, x, y, a, r - 1);
}

// ---------------------------------------------------------------- WH_CALLWNDPROC 兜底
static LRESULT CALLBACK CallWndProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_doTranslate && lParam)
    {
        CWPSTRUCT* c = (CWPSTRUCT*)lParam;
        switch (c->message)
        {
        case CM_SB_SETTEXTW:
        case WM_SETTEXT:
        {
            wchar_t oldText[MAX_TEXTLEN];
            wchar_t translated[MAX_TEXTLEN];
            const wchar_t* nt;
            if (SafeText((const wchar_t*)c->lParam, oldText, MAX_TEXTLEN) && *oldText)
            {
                nt = TryTranslate((const wchar_t*)c->lParam, translated, MAX_TEXTLEN);
                if (nt)
                {
                    Logf(L"[HIT ] CWP %hs \"%s\" -> \"%s\"", MsgName(c->message), oldText, translated);
                    c->lParam = (LPARAM)nt;   // CWPSTRUCT 里的字符串在发送方内存中，可原地改
                }
            }
            break;
        }
        default:
            break;
        }
    }
    return CallNextHookEx(g_callWndHook, code, wParam, lParam);
}

// ---------------------------------------------------------------- IAT Hook

static BOOL SameModuleName(const char* name, const char* want)
{
    const char* base = name;
    const char* dot;
    size_t n;

    for (const char* p = name; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;

    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);

    return _strnicmp(base, want, n) == 0 && want[n] == '\0';
}

static void* PickHook(const char* dll, const char* func)
{
    if (SameModuleName(dll, "USER32"))
    {
        if (_stricmp(func, "SendMessageW") == 0) return (void*)&HookSendMessageW;
        if (_stricmp(func, "DrawTextW") == 0)    return (void*)&HookDrawTextW;
        if (_stricmp(func, "DrawTextA") == 0)    return (void*)&HookDrawTextA;
        if (_stricmp(func, "DrawTextExW") == 0)  return (void*)&HookDrawTextExW;
    }
    else if (SameModuleName(dll, "GDI32") || SameModuleName(dll, "GDI32FULL"))
    {
        if (_stricmp(func, "TextOutW") == 0)     return (void*)&HookTextOutW;
        if (_stricmp(func, "TextOutA") == 0)     return (void*)&HookTextOutA;
        if (_stricmp(func, "ExtTextOutW") == 0)  return (void*)&HookExtTextOutW;
        if (_stricmp(func, "ExtTextOutA") == 0)  return (void*)&HookExtTextOutA;
        if (_stricmp(func, "CreateFontW") == 0)         return (void*)&HookCreateFontW;
        if (_stricmp(func, "CreateFontA") == 0)         return (void*)&HookCreateFontA;
        if (_stricmp(func, "CreateFontIndirectW") == 0) return (void*)&HookCreateFontIndirectW;
        if (_stricmp(func, "CreateFontIndirectA") == 0) return (void*)&HookCreateFontIndirectA;
    }
    return NULL;
}

static void* PickReal(const char* dll, const char* func)
{
    if (SameModuleName(dll, "USER32"))
    {
        if (_stricmp(func, "SendMessageW") == 0) return g_realSendMessageW;
        if (_stricmp(func, "DrawTextW") == 0)    return g_realDrawTextW;
        if (_stricmp(func, "DrawTextA") == 0)    return g_realDrawTextA;
        if (_stricmp(func, "DrawTextExW") == 0)  return g_realDrawTextExW;
    }
    else if (SameModuleName(dll, "GDI32") || SameModuleName(dll, "GDI32FULL"))
    {
        if (_stricmp(func, "TextOutW") == 0)     return g_realTextOutW;
        if (_stricmp(func, "TextOutA") == 0)     return g_realTextOutA;
        if (_stricmp(func, "ExtTextOutW") == 0)  return g_realExtTextOutW;
        if (_stricmp(func, "ExtTextOutA") == 0)  return g_realExtTextOutA;
        if (_stricmp(func, "CreateFontW") == 0)         return g_realCreateFontW;
        if (_stricmp(func, "CreateFontA") == 0)         return g_realCreateFontA;
        if (_stricmp(func, "CreateFontIndirectW") == 0) return g_realCreateFontIndirectW;
        if (_stricmp(func, "CreateFontIndirectA") == 0) return g_realCreateFontIndirectA;
    }
    return NULL;
}

// IAT 扫描的候选函数名。以前这里是手写的三重 && 判断，加钩子极易漏改 ——
// 漏改的后果是「该模块整体被跳过」，且完全没有日志。改成表的唯一来源。
static const char* const kHookNames[] = {
    "SendMessageW", "DrawTextW", "DrawTextA", "DrawTextExW",
    "TextOutW", "TextOutA", "ExtTextOutW", "ExtTextOutA",
    "CreateFontW", "CreateFontA", "CreateFontIndirectW", "CreateFontIndirectA",
    NULL
};

static BOOL ModuleHasHookTarget(const char* dllName)
{
    const char* const* n;
    for (n = kHookNames; *n; n++)
        if (PickHook(dllName, *n))
            return TRUE;
    return FALSE;
}

static void PatchIatEntry(void** slot, void* hook, void* real, const char* dllName, const char* funcName)
{
    DWORD oldProtect;

    if (*slot != real)
        return;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
}

static void HookModuleImports(HMODULE mod)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nt;
    IMAGE_IMPORT_DESCRIPTOR* imp;
    DWORD rva;

    if (!mod)
        return;

    __try
    {
        dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;

        rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva) return;
        imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva);

        for (; imp->Name; imp++)
        {
            const char* dllName = (const char*)(base + imp->Name);
            IMAGE_THUNK_DATA* oft;
            IMAGE_THUNK_DATA* ft;
            DWORD idx = 0;

            if (!ModuleHasHookTarget(dllName))
                continue;

            if (!imp->OriginalFirstThunk) continue;
            oft = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
            ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);

            for (; oft->u1.AddressOfData; oft++, ft++, idx++)
            {
                IMAGE_IMPORT_BY_NAME* ibn;
                void* hook;
                void* real;

                if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
                ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
                hook = PickHook(dllName, (const char*)ibn->Name);
                if (!hook) continue;
                real = PickReal(dllName, (const char*)ibn->Name);
                if (!real) continue;
                PatchIatEntry((void**)&ft->u1.Function, hook, real, dllName, (const char*)ibn->Name);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static int ArmModulesBefore(void* imageBase)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    int armed = 0;

    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return 0;

    int n = (int)(needed / sizeof(HMODULE));
    if (n > 1024) n = 1024;

    for (int i = 0; i < n; i++)
    {
        if ((void*)mods[i] == imageBase)
        {
            HookModuleImports(mods[i]);
            armed++;
        }
    }
    return armed;
}

// ---------------------------------------------------------------- 入口

static const wchar_t* MhStr(MH_STATUS st)
{
    switch (st)
    {
    case MH_OK:                                  return L"MH_OK";
    case MH_ERROR_ALREADY_INITIALIZED:           return L"ALREADY_INITIALIZED";
    case MH_ERROR_NOT_INITIALIZED:               return L"NOT_INITIALIZED";
    case MH_ERROR_ALREADY_CREATED:               return L"ALREADY_CREATED";
    case MH_ERROR_NOT_CREATED:                   return L"NOT_CREATED";
    case MH_ERROR_ENABLED:                       return L"ENABLED";
    case MH_ERROR_DISABLED:                      return L"DISABLED";
    case MH_ERROR_NOT_EXECUTABLE:                return L"NOT_EXECUTABLE";
    case MH_ERROR_UNSUPPORTED_FUNCTION:          return L"UNSUPPORTED_FUNCTION";
    case MH_ERROR_MEMORY_ALLOC:                   return L"MEMORY_ALLOC";
    case MH_ERROR_MEMORY_PROTECT:                 return L"MEMORY_PROTECT";
    case MH_ERROR_MODULE_NOT_FOUND:               return L"MODULE_NOT_FOUND";
    case MH_ERROR_FUNCTION_NOT_FOUND:             return L"FUNCTION_NOT_FOUND";
    default:                                      return L"UNKNOWN";
    }
}

// 打钩。放在 DllMain 里调用最理想：插件的加载发生在「创建工具栏」之前，
// 而本模块需要的一切（user32/gdi32/kernel32）此时必然已加载完毕。
// 唯一不能做的是 LoadLibrary 新库 —— MinHook 只做 VirtualProtect + 代码改写。
// 打钩（只做「建钩」，不启用）。
//
// 为什么不能在这里直接 MH_EnableHook：
//   MH_EnableHook 会遍历并挂起进程内所有线程。若在 DllMain 里调用，主线程此刻
//   正卡在 LdrLoadDll 内部持有加载器锁；一旦被挂起，进程就再也走不到插件初始化。
//   实测表现：注入了，但 ToolStatus.dll 永远不加载，工具栏与菜单栏全部消失。
//   因此「启用」动作交给启动器在「恢复主线程之前」触发 —— 见 EnableHooks / EnableThread。
static void InstallHooks(void)
{
    MH_STATUS st = MH_Initialize();

    Logf(L"[minhook] MH_Initialize = %d (%s)", (int)st, MhStr(st));

    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return;

    {
        // 表驱动：加钩子只改这一张表。以前是 s1..s5 手写序列，每加一个都要
        // 同时改三处（建钩、日志、IAT 表），漏一处就静默失效。
        static const struct {
            const wchar_t* dll; const char* fn; LPVOID hook; void** real; BOOL on;
        } t[] = {
            { L"user32.dll", "SendMessageW", (LPVOID)&HookSendMessageW, &g_realSendMessageW, TRUE     },
            { L"user32.dll", "DrawTextW",    (LPVOID)&HookDrawTextW,    &g_realDrawTextW,    g_needDT },
            { L"user32.dll", "DrawTextExW",  (LPVOID)&HookDrawTextExW,  &g_realDrawTextExW,  g_needDE },
            { L"user32.dll", "DrawTextA",    (LPVOID)&HookDrawTextA,    &g_realDrawTextA,    g_needAnsi },
            { L"gdi32.dll",  "TextOutW",     (LPVOID)&HookTextOutW,     &g_realTextOutW,     g_needTO },
            { L"gdi32.dll",  "TextOutA",     (LPVOID)&HookTextOutA,     &g_realTextOutA,     g_needAnsi },
            { L"gdi32.dll",  "ExtTextOutW",  (LPVOID)&HookExtTextOutW,  &g_realExtTextOutW,  g_needXO },
            { L"gdi32.dll",  "ExtTextOutA",  (LPVOID)&HookExtTextOutA,  &g_realExtTextOutA,  g_needAnsi },
            // 字体替换（表在 zh_font.txt）。CreateFont 与 CreateFontIndirect **两条都要挂**：
            // SI 的应用字体走 CreateFont（guisup.c 的 PhCreateFontHandle），列表字体走
            // CreateFontIndirect —— 只挂一条会漏掉一半界面。关掉替换（ZHHK_FONT=0）时
            // 仍要建钩子，否则 ZHHK_FONTLOG 采集不到「替换前」的清单。
            { L"gdi32.dll",  "CreateFontW",         (LPVOID)&HookCreateFontW,         &g_realCreateFontW,         (g_doFont || g_fontLog) },
            { L"gdi32.dll",  "CreateFontA",         (LPVOID)&HookCreateFontA,         &g_realCreateFontA,         (g_doFont || g_fontLog) },
            { L"gdi32.dll",  "CreateFontIndirectW", (LPVOID)&HookCreateFontIndirectW, &g_realCreateFontIndirectW, (g_doFont || g_fontLog) },
            { L"gdi32.dll",  "CreateFontIndirectA", (LPVOID)&HookCreateFontIndirectA, &g_realCreateFontIndirectA, (g_doFont || g_fontLog) },
            // 退出钩子：SI 自己实现了 ExitProcess/TerminateProcess 的桩函数
            // （phlib\apistubs.cpp），绕开 kernel32 直接进 ntdll，所以只能钩这里。
            { L"ntdll.dll",  "NtTerminateProcess", (LPVOID)&HookNtTerminateProcess, &g_realNtTerminateProcess, TRUE },
            { L"ntdll.dll",  "RtlExitUserProcess", (LPVOID)&HookRtlExitUserProcess, &g_realRtlExitUserProcess, TRUE },
            // 只用来维护「当前绘制窗口」，不改变任何行为（见 HookBeginPaint 上方说明）
            { L"user32.dll", "BeginPaint",   (LPVOID)&HookBeginPaint,    &g_realBeginPaint,   TRUE },
            { L"user32.dll", "EndPaint",     (LPVOID)&HookEndPaint,      &g_realEndPaint,     TRUE },
        };
        wchar_t line[640];
        int off = 0;
        size_t i;

        for (i = 0; i < _countof(t); i++)
        {
            MH_STATUS s = MH_OK;
            int w;

            if (t[i].on)
                s = MH_CreateHookApi(t[i].dll, t[i].fn, t[i].hook, t[i].real);

            w = _snwprintf_s(line + off, _countof(line) - (size_t)off, _TRUNCATE,
                             L"%hs=%d(%s) ", t[i].fn, (int)s, MhStr(s));
            if (w < 0)
                break;
            off += w;
        }

        Logf(L"[minhook] %s", line);
    }
}

static void EnableHooks(void)
{
    MH_STATUS se = MH_EnableHook(MH_ALL_HOOKS);

    Logf(L"[minhook] EnableHook(ALL) = %d (%s)", (int)se, MhStr(se));
    Logf(L"[addr] SMW=%p DTW=%p DTEW=%p DTA=%p TOW=%p TOA=%p XTOW=%p XTOA=%p",
         g_realSendMessageW, g_realDrawTextW, g_realDrawTextExW, g_realDrawTextA,
         g_realTextOutW, g_realTextOutA, g_realExtTextOutW, g_realExtTextOutA);
}

// 与启动器握手：Ready=钩已建好；Go=启动器允许启用；Done=已启用完成
#define EV_READY L"Local\\ZhHook_Ready"
#define EV_GO    L"Local\\ZhHook_Go"
#define EV_DONE  L"Local\\ZhHook_Done"

static DWORD WINAPI EnableThread(LPVOID param)
{
    HANDLE evReady, evGo, evDone;
    DWORD r;

    (void)param;

    evReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, EV_READY);
    evGo    = OpenEventW(SYNCHRONIZE,       FALSE, EV_GO);
    evDone  = OpenEventW(EVENT_MODIFY_STATE, FALSE, EV_DONE);

    Logf(L"[sync] events ready=%p go=%p done=%p", (void*)evReady, (void*)evGo, (void*)evDone);

    if (evReady)
    {
        SetEvent(evReady);
        Logf(L"[sync] 已通知启动器：钩子建好");
    }

    if (evGo)
    {
        r = WaitForSingleObject(evGo, 30000);
        Logf(L"[sync] 等到 Go (r=%lu)", r);
    }
    else
    {
        // 【劫持模式】没有启动器，也就没人给我们发 Go。
        // 关键是「等加载器锁释放」：本线程由 DllMain（延迟加载辅助函数内部）创建，
        // 要等主线程从 LdrLoadDll 返回后才会真正被调度。多给一小段时间，
        // 让加载器初始化彻底走完，再 EnableHook 才安全。
        Logf(L"[sync] 无 Go 事件（劫持模式），等待加载器锁释放");
        Sleep(400);
        Logf(L"[sync] 兜底路径：自行启用钩子");
    }

    EnableHooks();

    if (evDone)
        SetEvent(evDone);

    Logf(L"[scan] 关键模块: user32=%p gdi32=%p ToolStatus=%p",
         (void*)GetModuleHandleW(L"user32.dll"), (void*)GetModuleHandleW(L"gdi32.dll"),
         (void*)GetModuleHandleW(L"ToolStatus.dll"));

    g_callWndHook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, NULL, GetCurrentThreadId());
    Logf(L"[hook] WH_CALLWNDPROC=%p (err=%lu)", (void*)g_callWndHook, g_callWndHook ? 0 : GetLastError());

    InterlockedExchange(&g_initDone, 1);
    return 0;
}

static void ReadEnv(void)
{
    DWORD n;

    n = GetEnvironmentVariableW(L"ZHHK_LOG", g_logPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        g_logPath[0] = L'\0';

    n = GetEnvironmentVariableW(L"ZHHK_DICT", g_dictPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        g_dictPath[0] = L'\0';

    n = GetEnvironmentVariableW(L"ZHHK_PATTERN", g_patPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        g_patPath[0] = L'\0';

    // 字体表：未设 = 用默认路径（与词典同目录）；0/off/false = 关掉字体替换（A/B 对照）；
    // 其它值 = 显式路径。关掉时仍然建钩子，好让 ZHHK_FONTLOG 能采到「替换前」的清单。
    n = GetEnvironmentVariableW(L"ZHHK_FONT", g_fontPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        g_fontPath[0] = L'\0';
    }
    else if (_wcsicmp(g_fontPath, L"0") == 0 || _wcsicmp(g_fontPath, L"off") == 0 ||
             _wcsicmp(g_fontPath, L"false") == 0)
    {
        g_doFont = FALSE;
        g_fontPath[0] = L'\0';
    }

    g_doTranslate = GetEnvironmentVariableW(L"ZHHK_TRANSLATE", NULL, 0) > 0;
    g_traceTb = GetEnvironmentVariableW(L"ZHHK_TRACETB", NULL, 0) > 0;

    {   // 钩子集合（默认 SM+DT+TO+DE）
        wchar_t hooks[64];
        DWORD n = GetEnvironmentVariableW(L"ZHHK_HOOKS", hooks, 64);
        if (n > 0 && n < 64)
        {
            g_needSM = wcsstr(hooks, L"SM") != NULL;
            g_needDT = wcsstr(hooks, L"DT") != NULL;
            g_needTO = wcsstr(hooks, L"TO") != NULL;
            g_needDE = wcsstr(hooks, L"DE") != NULL;
        }
    }
    g_logAll = GetEnvironmentVariableW(L"ZHHK_LOGALL", NULL, 0) > 0;
    g_missAll = GetEnvironmentVariableW(L"ZHHK_MISSALL", NULL, 0) > 0;
    g_fontLog = GetEnvironmentVariableW(L"ZHHK_FONTLOG", NULL, 0) > 0;
    g_fontFace = GetEnvironmentVariableW(L"ZHHK_FONTFACE", NULL, 0) > 0;

    if (!g_dictPath[0] && g_logPath[0])
    {
        wcscpy_s(g_dictPath, MAX_PATH, g_logPath);
        wchar_t* slash = wcsrchr(g_dictPath, L'\\');
        if (slash)
        {
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_dictPath), L"zh_dict.txt");
        }
    }

    // 模式表默认与词典同目录。它是可选的：文件不在就只是没有模式匹配。
    if (!g_patPath[0] && g_dictPath[0])
    {
        wchar_t* slash;
        wcscpy_s(g_patPath, MAX_PATH, g_dictPath);
        slash = wcsrchr(g_patPath, L'\\');
        if (slash)
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_patPath), L"zh_dict.pattern.txt");
    }

    // 字体表默认也与词典同目录。同样可选：文件不在就只是不做字体替换。
    if (g_doFont && !g_fontPath[0] && g_dictPath[0])
    {
        wchar_t* slash;
        wcscpy_s(g_fontPath, MAX_PATH, g_dictPath);
        slash = wcsrchr(g_fontPath, L'\\');
        if (slash)
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_fontPath), L"zh_font.txt");
    }

    // ------------------------------------------------------------------
    // 未命中收集：**默认关闭**。
    //
    // 它是采集/排障行为（会持续往磁盘追加文件），不该「装好就一直在写」。
    // 正常用法是给启动器加 --collect，由启动器把 ZHHK_MISS 设成 1；
    // 直接加载本 DLL（不走启动器）时不设任何东西就是关的。
    //
    // ZHHK_MISS 的取值：未设/0/off/false = 关；1/on/true = 开（默认路径）；
    // 其它值 = 开，且该值就是文件路径（向后兼容旧写法）。
    // ------------------------------------------------------------------
    {
        wchar_t v[MAX_PATH];

        n = GetEnvironmentVariableW(L"ZHHK_MISS", v, MAX_PATH);

        if (n == 0 || n >= MAX_PATH)
        {
            g_missEnabled = FALSE;
        }
        else if (_wcsicmp(v, L"0") == 0 || _wcsicmp(v, L"off") == 0 || _wcsicmp(v, L"false") == 0)
        {
            g_missEnabled = FALSE;
            g_missPath[0] = L'\0';
        }
        else if (_wcsicmp(v, L"1") == 0 || _wcsicmp(v, L"on") == 0 || _wcsicmp(v, L"true") == 0)
        {
            g_missEnabled = TRUE;                  // 路径下面派生
        }
        else
        {
            g_missEnabled = TRUE;
            wcsncpy_s(g_missPath, MAX_PATH, v, _TRUNCATE);
        }
    }

    // 未命中收集文件：默认落在词典同目录，命名为 zh_dict.missed.txt。
    // 注意：这个文件是**程序自己维护**的，不要手写；手写的内容 merge_missed.py
    // 会当作「未命中原文」处理。
    if (g_missEnabled && !g_missPath[0])
    {
        if (g_dictPath[0])
        {
            wchar_t* slash;
            wcscpy_s(g_missPath, MAX_PATH, g_dictPath);
            slash = wcsrchr(g_missPath, L'\\');
            if (slash)
                wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_missPath), L"zh_dict.missed.txt");
        }
        else
        {
            g_missEnabled = FALSE;                 // 词典路径都没有，无从派生
        }
    }

    // 周期检查点间隔（秒）。0 = 关闭周期刷新（只靠追加 + 退出钩子）。
    {
        wchar_t v[32];
        DWORD m = GetEnvironmentVariableW(L"ZHHK_MISSFLUSH", v, 32);
        if (m > 0 && m < 32)
            g_missFlushSec = (DWORD)wcstoul(v, NULL, 10);
    }
}

// ===========================================================================
// 入口
// ===========================================================================

static void Teardown(void)
{
    if (g_callWndHook)
    {
        UnhookWindowsHookEx(g_callWndHook);
        g_callWndHook = NULL;
    }

    // 未命中收集：按最终计数整体重写并关闭。
    // 放在关日志之前 —— 这样万一刷新失败，日志里还留有记录。
    MissFlushAndClose();

    if (g_log != INVALID_HANDLE_VALUE && g_log != NULL)
    {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
}

// DllMain 的签名与链接特性由系统固定（带 static 会被编译器忽略），这里不写 static。
BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(self);
        InitializeCriticalSection(&g_cs);

        ReadEnv();

        if (g_logPath[0])
        {
            g_log = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (g_log == INVALID_HANDLE_VALUE)
                g_logPath[0] = L'\0';
        }

        LoadDict();

        // 通配符模式表（可选）。必须在 LoadDict 之后：g_patPath 的默认值是从
        // g_dictPath 派生的。
        LoadPatterns();

        // 字体替换表（可选）。同样要在 LoadDict 之后（g_fontPath 的默认值来自 g_dictPath），
        // 且必须在 InstallHooks 之前 —— 钩子表的 on 标志要读 g_doFont/g_fontLog。
        LoadFonts();

        // 运行时未命中收集：先读回历史（跨次运行不重复），再开追加句柄。
        // 这两步在 g_missEnabled 为假时直接返回 —— 默认就是关的。
        MissLoadExisting();
        MissOpen();

        // 周期检查点线程。外部 taskkill /F 调的是别的进程里的 NtTerminateProcess，
        // 我们的进程内钩子不会被调用 —— 这是那种情况下唯一的保险。
        // 用裸线程：本函数返回后它仍在跑，进程退出时自然消失。
        if (g_missEnabled)
            CreateThread(NULL, 0, MissCheckpointThread, NULL, 0, NULL);

        // 关键：在 DllMain 内打钩。插件加载早于工具栏创建，这样才抢得到
        // TB_ADDBUTTONS / TB_SETBUTTONINFO 这两条「一次性」消息。
        InstallHooks();

        // 同时把自身模块的 IAT 也改掉（补充手段）
        ArmModulesBefore((void*)self);

        if (!CreateThread(NULL, 0, EnableThread, NULL, 0, NULL))
            return FALSE;
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        Logf(L"[boot] DLL_PROCESS_DETACH (reserved=%p)", reserved);
        Teardown();
    }

    return TRUE;
}


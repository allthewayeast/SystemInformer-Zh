<#
.SYNOPSIS
    打开 / 关闭「运行时未命中收集」，做法是在 IFEO 的 Debugger 值里增删 --collect。

.DESCRIPTION
    收集功能**默认关闭**。它由启动器的 --collect 启动参数控制；而走 IFEO 双击
    启动时，参数只能写在 Debugger 值里。IFEO 的语义是「把 Debugger 整串插到
    原命令行前面」（Raymond Chen: Image File Execution Options just inserts the
    debugger in front of the command line），所以末尾追加参数是合法的：

        Debugger = "<...>\_zhcn\si_zh_launcher.exe" --collect

    本脚本只改这一个值：别的参数原样保留，并且**统一把启动器路径加上引号**。
    引号这件事很要紧 —— 安装目录通常含空格，未加引号的路径交给命令行解析是有
    歧义的：Windows 只能按「模糊名字」规则逐个空格切出候选名去试。本脚本读旧值
    时也照同一套规则还原，所以老的未加引号写法能正确读出来并升级成带引号的。

    *** 改完会自动试启一次并验证 ***
    IFEO 的 Debugger 一旦指向不存在的东西，System Informer 会**直接起不来**
    （不会回退去启动原 exe）。所以本脚本写完注册表后会立刻启动一次目标程序，
    确认真有进程起来：
        起来了   -> 成功，注册表保留；
        没起来   -> 说明这个形态在本机不被接受，**自动还原成改之前的值**并报错。
    跳过这次试启：-NoTest（不推荐）。

.PARAMETER Action
    On      加上 --collect
    Off     去掉 --collect
    Status  只看当前状态，不改任何东西（不需要管理员）

.PARAMETER AppDir
    System Informer 的安装目录。不传就从启动器路径推上一级。

.PARAMETER NoTest
    不做试启验证。只在你明确知道后果时用。

.EXAMPLE
    pwsh -File scripts\collect.ps1 Status
    pwsh -File scripts\collect.ps1 On
    pwsh -File scripts\collect.ps1 Off
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('On', 'Off', 'Status')]
    [string]$Action,
    [string]$AppDir,
    [string]$SubDir = '_zhcn',
    [switch]$NoTest
)

$ErrorActionPreference = 'Stop'
$PSStyle.OutputRendering = 'PlainText'

$IFEO_ROOT = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
$KEY       = "$IFEO_ROOT\SystemInformer.exe"
$TOKEN     = '--collect'

# 把 Debugger 值拆成「程序路径 + 其余参数」。
#
# 未加引号时按 Windows 的做法还原：从最短候选开始，逐个空格往前拼，取第一个
# 真实存在的文件。安装目录含空格时，不这样做就会把
#   D:\App\系统辅助\System Informer\_zhcn\si_zh_launcher.exe
# 切成 "D:\App\系统辅助\System" + "Informer\..." —— 那是错的。
function Resolve-Debugger {
    param([string]$cmd)

    $cmd = $cmd.Trim()

    if ($cmd.StartsWith('"')) {
        $i = $cmd.IndexOf('"', 1)
        if ($i -gt 0) {
            return [pscustomobject]@{
                Exe  = $cmd.Substring(1, $i - 1)
                Args = $cmd.Substring($i + 1).Trim()
            }
        }
    }

    $idx = 0
    while ($true) {
        $sp   = $cmd.IndexOf(' ', $idx)
        $cand = if ($sp -lt 0) { $cmd } else { $cmd.Substring(0, $sp) }
        foreach ($p in @($cand, "$cand.exe")) {
            if (Test-Path -LiteralPath $p -PathType Leaf) {
                return [pscustomobject]@{
                    Exe  = $p
                    Args = if ($sp -lt 0) { '' } else { $cmd.Substring($sp + 1).Trim() }
                }
            }
        }
        if ($sp -lt 0) { break }
        $idx = $sp + 1
    }

    # 还原不出来：按第一个空格切，至少不崩
    $p0 = ($cmd -split '\s+')[0]
    return [pscustomobject]@{ Exe = $p0; Args = '' }
}

$dbg = (Get-ItemProperty -Path $KEY -ErrorAction SilentlyContinue).Debugger
if (-not $dbg) {
    throw @"
没找到 IFEO 的 Debugger 值（$KEY）。
说明还没安装，或者安装方式不是 IFEO。先跑：
  pwsh -File scripts\install.ps1 -AppDir '<System Informer 安装目录>'
"@
}

$d       = Resolve-Debugger $dbg
$exe     = $d.Exe
$argRaw  = $d.Args
$on      = [bool][regex]::IsMatch($argRaw, "(^|\s)$([regex]::Escape($TOKEN))(\s|$)")

Write-Output "键          : $KEY"
Write-Output "Debugger    : $dbg"
Write-Output "启动器      : $exe  (存在=$(Test-Path -LiteralPath $exe))"
Write-Output "其它参数    : $(if ($argRaw) { $argRaw } else { '(无)' })"
Write-Output "收集当前状态: $(if ($on) { '开' } else { '关' })"

if (-not (Test-Path -LiteralPath $exe)) {
    Write-Warning "Debugger 指向的启动器不存在：$exe"
    Write-Warning '先修好这个（重跑 install.ps1），再动 --collect。'
    if ($Action -ne 'Status') { throw '启动器路径无效，已中止（没有改注册表）' }
}

if ($Action -eq 'Status') {
    Write-Output ''
    Write-Output '没有改动任何东西（Status 是只读的）。'
    if ($on) { Write-Output '关闭：pwsh -File scripts\collect.ps1 Off' }
    else     { Write-Output '打开：pwsh -File scripts\collect.ps1 On' }
    return
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '需要管理员权限（IFEO 在 HKLM 下）。请用管理员身份的 PowerShell 运行。'
}

# 重建：带引号的程序路径 + 其它参数 + （要开就加）--collect
$others = @($argRaw -split '\s+' | Where-Object { $_ -and $_ -ne $TOKEN })
$new = '"' + $exe + '"'
if ($others.Count) { $new += ' ' + ($others -join ' ') }
if ($Action -eq 'On') { $new += ' ' + $TOKEN }

if ($new -eq $dbg) {
    Write-Output ''
    Write-Output '已经是目标状态，注册表未改动。'
    return
}

$appRoot = if ($AppDir) { $AppDir } else { Split-Path -Parent (Split-Path -Parent $exe) }
$target  = Join-Path $appRoot 'SystemInformer.exe'
if (-not (Test-Path -LiteralPath $target)) { $target = $null }

Set-ItemProperty -Path $KEY -Name Debugger -Value $new -Type String
Write-Output ''
Write-Output "已改：$new"

if ($NoTest -or -not $target) {
    if (-not $target) { Write-Output "!! 没找到 $appRoot\SystemInformer.exe，跳过试启验证" }
    Write-Output "复核：$((Get-ItemProperty -Path $KEY).Debugger)"
} else {
    Write-Output ''
    Write-Output "试启验证：启动 $target"

    $before = @(Get-Process SystemInformer, si_zh_launcher -ErrorAction SilentlyContinue |
                Select-Object -ExpandProperty Id)
    $err = $null
    try { Start-Process -FilePath $target -ErrorAction Stop } catch { $err = $_.Exception.Message }

    $ok = $false
    if (-not $err) {
        for ($i = 0; $i -lt 40; $i++) {
            Start-Sleep -Milliseconds 500
            $fresh = @(Get-Process SystemInformer, si_zh_launcher -ErrorAction SilentlyContinue |
                       Where-Object { $before -notcontains $_.Id })
            if ($fresh.Count -gt 0) { $ok = $true; break }
        }
    }

    if ($ok) {
        Write-Output '  通过 —— 启动器和目标都起来了，注册表保留。'
        Write-Output '  （刚拉起的实例可以直接关掉，不影响设置。）'
    } else {
        Write-Output "  失败 —— $(if ($err) { $err } else { '20 秒内没出现启动器或目标进程' })"
        Set-ItemProperty -Path $KEY -Name Debugger -Value $dbg -Type String
        Write-Output "  已还原 Debugger：$((Get-ItemProperty -Path $KEY).Debugger)"
        Write-Output ''
        Write-Output '  说明「往 Debugger 值里追加参数」在这台机器上不被接受。替代做法：'
        Write-Output '  建一个快捷方式指向下面这行，用快捷方式启动（不走 IFEO）：'
        Write-Output "    `"$exe`" --collect"
        throw '试启验证失败，已还原注册表'
    }
}

if ($Action -eq 'On') {
    $sub = if ($AppDir) { Join-Path $AppDir $SubDir } else { Split-Path -Parent $exe }
    Write-Output ''
    Write-Output '收集已打开。启动 System Informer 后开始记录，落盘到：'
    Write-Output "  $sub\zh_dict.missed.txt"
    Write-Output '收集够了就跑：pwsh -File scripts\collect.ps1 Off'
} else {
    Write-Output ''
    Write-Output '收集已关闭，不会再往 zh_dict.missed.txt 写新内容（已收集的保留）。'
}

Write-Output ''
Write-Output '自检：pwsh -File scripts\verify-ifeo.ps1'

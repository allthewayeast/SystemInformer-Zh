<#
.SYNOPSIS
    安装「双击即中文」：把构建产物铺进程序目录，并用 IFEO 把 SystemInformer.exe
    的启动转交给启动器。

.DESCRIPTION
    做三件事：
      1. 把 dist\ 里的 si_zh_launcher.exe / zh_hook.dll / zh_dict.txt /
         zh_dict.pattern.txt 铺到 <AppDir>\_zhcn\（独立子目录，不与程序自带文件混在一起）；
      2. 把该目录下原有的 Debugger 值备份到 _zhcn\ifeo_backup.txt；
      3. 写 IFEO 的 Debugger 值，让双击 SystemInformer.exe 时改由启动器接管。

    IFEO 是什么 / 为什么用它：
      HKLM\...\Image File Execution Options\<映像名> 下的 Debugger 值会让 Windows
      在启动该映像时改为启动你指定的程序，并把原映像路径作为首个参数传过去。
      这是「不改 exe 文件、不改快捷方式、双击照常」的唯一办法。
      代价：它同时也是恶意软件常用的持久化位置，个别安全软件会对此报警；
            而且一旦 Debugger 指向的东西有问题，程序就起不来 —— 所以务必保留
            uninstall.ps1 的一键还原。

    第 3 步（写注册表）也可以由启动器自己做：`si_zh_launcher.exe --install`，
    它会自己弹 UAC、写完回读校验，不需要管理员 PowerShell。两者写的是同一个值，
    只有一处差别：启动器那条路会**保留** Debugger 里已有的附加参数（例如
    collect.ps1 挂上的 --collect），而本脚本是直接覆盖成「带引号的启动器路径」。
    如果你正在用 --collect 收集未命中，重新安装后记得再跑一次 collect.ps1 -On。

.PARAMETER AppDir
    System Informer 的安装目录。不传就自动找：优先正在运行的实例，其次
    常见安装位置。

.PARAMETER Dist
    构建产物目录，默认 <仓库>\dist（先跑 scripts\build.ps1）。

.PARAMETER NoIfeo
    只铺文件、不动注册表。这样可以直接双击 si_zh_launcher.exe 使用。

.EXAMPLE
    pwsh -File scripts\install.ps1
    pwsh -File scripts\install.ps1 -AppDir 'D:\App\系统辅助\System Informer'
    pwsh -File scripts\install.ps1 -NoIfeo
#>
[CmdletBinding()]
param(
    [string]$AppDir,
    [string]$Dist,
    [string]$SubDir = '_zhcn',
    [switch]$NoIfeo
)

$ErrorActionPreference = 'Stop'
$PSStyle.OutputRendering = 'PlainText'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Dist) { $Dist = Join-Path $repo 'dist' }

$IFEO_ROOT = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
$KEY       = "$IFEO_ROOT\SystemInformer.exe"
$FILES     = 'si_zh_launcher.exe', 'zh_hook.dll', 'zh_dict.txt', 'zh_dict.pattern.txt'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '需要管理员权限（IFEO 在 HKLM 下）。请用管理员身份的 PowerShell 运行。'
    }
}

function Resolve-AppDir {
    param([string]$Given)

    $cands = @()
    if ($Given) { $cands += $Given }

    # 正在运行的实例最可靠 —— 用户就是从这里启动的
    $cands += @(Get-Process SystemInformer -ErrorAction SilentlyContinue |
                Where-Object { $_.Path } | ForEach-Object { Split-Path -Parent $_.Path })

    $cands += Join-Path $env:ProgramFiles 'SystemInformer'
    if (${env:ProgramFiles(x86)}) { $cands += Join-Path ${env:ProgramFiles(x86)} 'SystemInformer' }

    foreach ($c in $cands) {
        if ($c -and (Test-Path (Join-Path $c 'SystemInformer.exe'))) { return $c }
    }

    if ($Given) { throw "在 $Given 里找不到 SystemInformer.exe" }
    throw @'
找不到 System Informer 的安装目录。
请用 -AppDir 显式指定，例如：
  -AppDir 'D:\App\系统辅助\System Informer'
'@
}

# ---------------------------------------------------------------- 前置检查
Assert-Admin
$AppDir = Resolve-AppDir -Given $AppDir

foreach ($f in $FILES) {
    if (-not (Test-Path (Join-Path $Dist $f))) {
        throw "缺少产物 $Dist\$f —— 请先跑 pwsh -File scripts\build.ps1"
    }
}
Write-Output "程序目录 : $AppDir"
Write-Output "产物目录 : $Dist"

$running = Get-Process SystemInformer -ErrorAction SilentlyContinue |
           Where-Object { $_.Path -eq (Join-Path $AppDir 'SystemInformer.exe') }
if ($running) {
    Write-Output "!! System Informer 正在运行（pid $($running.Id -join ',')）。"
    Write-Output "   注册表可以照改，但正在跑的那个进程不会变中文。稍后重启它即可。"
}

# ---------------------------------------------------------------- 1) 铺文件
$sub = Join-Path $AppDir $SubDir
New-Item -ItemType Directory -Force -Path $sub | Out-Null
foreach ($f in $FILES) {
    Copy-Item (Join-Path $Dist $f) (Join-Path $sub $f) -Force
}
$launcher = Join-Path $sub 'si_zh_launcher.exe'
Write-Output "[1/3] 已铺到 $sub"
Get-ChildItem $sub -File |
    Where-Object { $_.Name -in $FILES } |
    ForEach-Object { "        {0,-22} {1,8} bytes" -f $_.Name, $_.Length }

# ---------------------------------------------------------------- 2) 备份旧值
$backup = Join-Path $sub 'ifeo_backup.txt'
if (Test-Path $KEY) {
    $old = (Get-ItemProperty -Path $KEY -ErrorAction SilentlyContinue).Debugger
    if ($old) {
        "原有 Debugger = $old" | Set-Content -Path $backup -Encoding UTF8
        Write-Output "[2/3] 已备份原有 Debugger 到 $backup"
    } else {
        '(该键存在但无 Debugger 值)' | Set-Content -Path $backup -Encoding UTF8
        Write-Output '[2/3] 该键已存在但没有 Debugger 值，已记录'
    }
} else {
    '(安装前不存在该键)' | Set-Content -Path $backup -Encoding UTF8
    Write-Output '[2/3] 安装前不存在 IFEO 键，已记录'
}

if ($NoIfeo) {
    Write-Output '[3/3] -NoIfeo：跳过注册表。可直接双击 si_zh_launcher.exe 使用。'
    return
}

# ---------------------------------------------------------------- 3) 写 IFEO
# Debugger 值一律写成**带引号**的形式。安装目录通常含空格，未加引号的路径交给
# 命令行解析是有歧义的（CreateProcess 会逐个空格切出候选名去试）；带引号是文档
# 明确支持的形式，也才谈得上在后面安全地追加参数（scripts\collect.ps1 就是往
# 这个值末尾加 --collect）。
New-Item -Path $KEY -Force | Out-Null
Set-ItemProperty -Path $KEY -Name Debugger -Value ('"' + $launcher + '"') -Type String
Write-Output '[3/3] IFEO 已写入'
Write-Output "        $KEY"
Write-Output "        Debugger = $((Get-ItemProperty -Path $KEY).Debugger)"

Write-Output ''
Write-Output '完成。现在双击 / 快捷方式启动 System Informer 都会走启动器（界面应为中文）。'
Write-Output '自检：pwsh -File scripts\verify-ifeo.ps1'
Write-Output '卸载：pwsh -File scripts\uninstall.ps1'

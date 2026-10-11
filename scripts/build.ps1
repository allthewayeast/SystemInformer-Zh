<#
.SYNOPSIS
    构建 SystemInformer-Zh 的全部产物到 dist\。

.DESCRIPTION
    产物：
      dist\si_zh_launcher.exe   启动器（含 assets\systemInformer-zh.ico 图标）
      dist\zh_hook.dll          显示层钩子，由启动器注入到 SystemInformer.exe
      dist\zh_dict.txt          词典

    只依赖两样东西：Visual Studio 的 C++ 工具链（自动定位），以及仓库里
    src\third_party\minhook 自带的 MinHook 源码。不需要 CMake、不需要 vcpkg。

.PARAMETER VcVars
    vcvars64.bat 的完整路径。不传就自动找（优先用 vswhere）。

.PARAMETER Clean
    先删掉 build\ 与 dist\ 再构建。

.EXAMPLE
    pwsh -File scripts\build.ps1
    pwsh -File scripts\build.ps1 -Clean
    pwsh -File scripts\build.ps1 -VcVars 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
#>
[CmdletBinding()]
param(
    [string]$VcVars,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$PSStyle.OutputRendering = 'PlainText'

# ---------------------------------------------------------------- 路径
$repo = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $repo 'dist'
$obj  = Join-Path $repo 'build\obj'

$hookSrc = Join-Path $repo 'src\hook\zh_hook.cpp'
$lchSrc  = Join-Path $repo 'src\launcher\si_zh_launcher.c'
$lrcSrc  = Join-Path $repo 'src\launcher\si_zh_launcher.rc'
$assets  = Join-Path $repo 'assets'
$mhRoot  = Join-Path $repo 'src\third_party\minhook'
$mhInc   = Join-Path $mhRoot 'include'
$mhSrc   = Join-Path $mhRoot 'src'
$dictSrc = Join-Path $repo 'dict\zh_dict.txt'

foreach ($p in $hookSrc, $lchSrc, $lrcSrc, $dictSrc) {
    if (-not (Test-Path $p)) { throw "缺少源文件：$p" }
}
if (-not (Test-Path (Join-Path $mhSrc 'hook.c'))) { throw "缺少 MinHook 源码：$mhSrc" }
if (-not (Test-Path (Join-Path $assets 'systemInformer-zh.ico'))) { throw "缺少图标：$assets\systemInformer-zh.ico" }

# ---------------------------------------------------------------- 找工具链
function Find-VcVars {
    param([string]$Explicit)

    if ($Explicit) {
        if (Test-Path $Explicit) { return $Explicit }
        throw "指定的 -VcVars 不存在：$Explicit"
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $inst = & $vswhere -latest -products * `
                    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                    -property installationPath 2>$null
        if ($inst) {
            $c = Join-Path $inst.Trim() 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $c) { return $c }
        }
    }

    $glob = @(
        'C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat'
        'C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat'
    )
    $hit = Get-ChildItem -Path $glob -ErrorAction SilentlyContinue | Sort-Object FullName -Descending
    if ($hit) { return $hit[0].FullName }

    throw @'
找不到 vcvars64.bat。
请安装 Visual Studio 的「使用 C++ 的桌面开发」工作负载，
或用 -VcVars 显式指定，例如：
  -VcVars 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
'@
}

$vcvars = Find-VcVars -Explicit $VcVars
Write-Output "工具链 : $vcvars"

# ---------------------------------------------------------------- 目录
if ($Clean) {
    Remove-Item (Join-Path $repo 'build'), $dist -Recurse -Force -ErrorAction SilentlyContinue
    Write-Output '已清理 build\ 与 dist\'
}
New-Item -ItemType Directory -Force -Path $dist, $obj | Out-Null
Get-ChildItem $obj -File -ErrorAction SilentlyContinue | Remove-Item -Force

# 在 cmd 里跑一条命令；失败即抛。
function Invoke-Vc {
    param([string]$Line, [string]$What)
    Write-Output ''
    Write-Output "--- $What ---"
    $out = cmd /c "call `"$vcvars`" >nul 2>&1 && $Line" 2>&1
    $out | Where-Object { $_ -notmatch '^\s*$' } | ForEach-Object { Write-Output $_ }
    if ($LASTEXITCODE -ne 0) { throw "$What 失败（exit=$LASTEXITCODE）" }
}

Push-Location $obj
try {
    # ------------------------------------------------------------ 1) 资源
    # 单独跑 rc.exe（而不是交给 cl），这样 /I 明确指向 assets，图标路径不依赖工作目录。
    $res = Join-Path $obj 'si_zh_launcher.res'
    Invoke-Vc "rc /nologo /I `"$assets`" /fo `"$res`" `"$lrcSrc`"" '[1/4] 编译启动器资源（图标 + 版本信息）'

    # ------------------------------------------------------------ 2) MinHook
    $c = '/nologo /c /O2 /MT /W3 /DNDEBUG /DWIN32_LEAN_AND_MEAN'
    Invoke-Vc ("cl $c /I `"$mhInc`" /I `"$mhSrc`" /I `"$mhSrc\hde`" " +
               "`"$mhSrc\buffer.c`" `"$mhSrc\hook.c`" `"$mhSrc\trampoline.c`" `"$mhSrc\hde\hde64.c`"") `
              '[2/4] 编译 MinHook'

    # ------------------------------------------------------------ 3) 钩子 DLL
    $cpp = '/nologo /LD /O2 /MT /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE'
    Invoke-Vc ("cl $cpp /I `"$mhInc`" `"$hookSrc`" " +
               "buffer.obj hook.obj trampoline.obj hde64.obj " +
               "/Fe:`"$dist\zh_hook.dll`" /link user32.lib kernel32.lib gdi32.lib psapi.lib comctl32.lib") `
              '[3/4] 编译 zh_hook.dll'

    # ------------------------------------------------------------ 4) 启动器
    Invoke-Vc ("cl /nologo /O2 /MT /W3 /utf-8 /DUNICODE /D_UNICODE `"$lchSrc`" `"$res`" " +
               "/Fe:`"$dist\si_zh_launcher.exe`" " +
               "/link /SUBSYSTEM:WINDOWS user32.lib kernel32.lib shell32.lib psapi.lib") `
              '[4/4] 编译 si_zh_launcher.exe'
}
finally {
    Pop-Location
}

# ---------------------------------------------------------------- 词典
Copy-Item $dictSrc (Join-Path $dist 'zh_dict.txt') -Force

Remove-Item $obj -Recurse -Force -ErrorAction SilentlyContinue

# ---------------------------------------------------------------- 报告
Write-Output ''
Write-Output '==================== dist ===================='
Get-ChildItem $dist -File | Sort-Object Name | ForEach-Object {
    '{0,-24} {1,9} bytes' -f $_.Name, $_.Length
}

# 图标是否真的进了 EXE：从 exe 里把关联图标抽出来看一眼。
$exe = Join-Path $dist 'si_zh_launcher.exe'
try {
    Add-Type -AssemblyName System.Drawing -ErrorAction Stop
    $ico = [System.Drawing.Icon]::ExtractAssociatedIcon($exe)
    if ($ico) {
        Write-Output ''
        Write-Output ("图标自检 : OK  {0}x{1}（已内嵌并可从 EXE 抽出）" -f $ico.Width, $ico.Height)
        $ico.Dispose()
    } else {
        throw 'ExtractAssociatedIcon 返回空'
    }
} catch {
    Write-Output ''
    Write-Output "图标自检 : 跳过（$($_.Exception.Message)）"
}

# 子系统必须保留：启动器是 GUI 子系统（双击不闪黑框），DLL 无所谓。
$pe = [System.IO.File]::ReadAllBytes($exe)
$peOff = [BitConverter]::ToInt32($pe, 0x3C)
$sub = [BitConverter]::ToUInt16($pe, $peOff + 0x5C)
Write-Output ("子系统   : {0}（{1}）" -f $sub, $(if ($sub -eq 2) { 'GUI，正确' } else { '不是 GUI！双击会闪黑框' }))
if ($sub -ne 2) { throw '启动器子系统不是 GUI' }

Write-Output ''
Write-Output '构建完成。安装：pwsh -File scripts\install.ps1 -AppDir <System Informer 安装目录>'

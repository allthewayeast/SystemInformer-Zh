<#
.SYNOPSIS
    撤销 install.ps1 做的一切。

.DESCRIPTION
    设计原则：**先保证 System Informer 能照常启动**，再谈清理。
    所以第一步永远是删掉 Debugger 值 —— 那一步做完就不再有任何东西拦在启动路径上。

    只摘注册表这一步也可以由启动器做：`si_zh_launcher.exe --uninstall`（会自己弹
    UAC）。本脚本仍然是**兜底手段**：它不依赖 _zhcn 目录是否存在，所以在目录已经
    被删掉、启动器已经没了的情况下，它照样能把 System Informer 救回原生启动。

.PARAMETER AppDir
    System Informer 的安装目录。不传就自动找（同 install.ps1）。

.PARAMETER RemoveFiles
    连 _zhcn 目录一起删。不加则只摘 IFEO，程序文件保留。

.EXAMPLE
    pwsh -File scripts\uninstall.ps1
    pwsh -File scripts\uninstall.ps1 -RemoveFiles
#>
[CmdletBinding()]
param(
    [string]$AppDir,
    [string]$SubDir = '_zhcn',
    [switch]$RemoveFiles
)

$ErrorActionPreference = 'Stop'
$PSStyle.OutputRendering = 'PlainText'

$IFEO_ROOT = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
$KEY       = "$IFEO_ROOT\SystemInformer.exe"

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '需要管理员权限（IFEO 在 HKLM 下）。'
}

# ---------------------------------------------------------------- 1) 摘 IFEO
if (Test-Path $KEY) {
    $dbg = (Get-ItemProperty -Path $KEY -ErrorAction SilentlyContinue).Debugger
    if ($dbg) {
        Write-Output "检测到 Debugger = $dbg"
        Remove-ItemProperty -Path $KEY -Name Debugger -Force
        Write-Output '[1/3] Debugger 值已删除 —— 启动路径上已经没有拦截了'
    } else {
        Write-Output '[1/3] 该键下没有 Debugger 值'
    }

    # 键里若已空（无值、无子键），一并清掉，别留垃圾
    $p = Get-Item $KEY
    if ($p.ValueCount -eq 0 -and $p.SubKeyCount -eq 0) {
        Remove-Item $KEY -Force
        Write-Output '       键已空，连带删除'
    } else {
        Write-Output "       键内仍有其他内容（值 $($p.ValueCount) / 子键 $($p.SubKeyCount)），保留"
    }
} else {
    Write-Output '[1/3] IFEO 键本来就不存在'
}

# ---------------------------------------------------------------- 2) 备份还原提示
# AppDir 允许不存在（可能只想摘 IFEO）；这时跳过文件部分。
$sub = $null
if ($AppDir) {
    $sub = Join-Path $AppDir $SubDir
} else {
    $run = Get-Process SystemInformer -ErrorAction SilentlyContinue | Where-Object { $_.Path } | Select-Object -First 1
    if ($run) { $sub = Join-Path (Split-Path -Parent $run.Path) $SubDir }
}

if ($sub -and (Test-Path $sub)) {
    $backup = Join-Path $sub 'ifeo_backup.txt'
    if (Test-Path $backup) {
        Write-Output "[2/3] 安装时的备份记录：$(Get-Content $backup -Raw -Encoding UTF8)"
        if ((Get-Content $backup -Raw -Encoding UTF8) -match '^原有 Debugger = (.+)$') {
            Write-Output "       若你本来就有别的 Debugger，请手动恢复为：$($Matches[1])"
        }
    } else {
        Write-Output '[2/3] 没有找到备份记录'
    }
} else {
    Write-Output '[2/3] 没有定位到 _zhcn 目录，跳过备份检查（可用 -AppDir 指定）'
}

# ---------------------------------------------------------------- 3) 文件
if ($RemoveFiles) {
    if ($sub -and (Test-Path $sub)) {
        Remove-Item $sub -Recurse -Force
        Write-Output "[3/3] 已删除 $sub"
    } else {
        Write-Output '[3/3] 没有需要删除的 _zhcn 目录'
    }
} elseif ($sub) {
    Write-Output "[3/3] 程序文件保留在 $sub（要删就加 -RemoveFiles）"
} else {
    Write-Output '[3/3] 未定位到程序文件目录'
}

# 确认
$left = Test-Path $KEY
Write-Output ''
Write-Output ("IFEO 键仍存在 = " + $left + "  ->  " +
              $(if ($left) { '注意：仍有 IFEO 项' } else { 'System Informer 已回到原生启动方式' }))

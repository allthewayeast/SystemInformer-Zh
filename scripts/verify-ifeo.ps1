# verify-ifeo.ps1 —— IFEO 端到端验收（用测试副本，不动真实安装）
#
# 验的是「双击照常」这条路真的通：
#   1. 挂上 IFEO 后，用普通方式启动 SystemInformer.exe（相当于双击）
#   2. 被启动的到底是不是 System Informer（而不是调试器自己）
#   3. 启动器进程**只有一个** —— 这一条是防递归的硬判据。
#      若 IFEO 二次套用，启动器会自我复制出成串进程，数量会 >1。
#   4. 目标进程里确实有 zh_hook.dll
#   5. 结束即还原 IFEO，不留副作用
#
# 需要一个 System Informer 副本当靶子（不碰你正在用的那份）。用 -TestExe 指定，
# 或设环境变量 SI_TEST_DIR。默认 <仓库>\test-app。
#
# 用法：
#   pwsh -File scripts\verify-ifeo.ps1               # 提权 shell 直接启动
#   pwsh -File scripts\verify-ifeo.ps1 -ViaExplorer   # 经 explorer 启动 = 非提权 = 真·双击

[CmdletBinding()]
param(
    [switch]$ViaExplorer,
    [string]$TestExe
)

$ErrorActionPreference = 'Stop'
$PSStyle.OutputRendering = 'PlainText'

$repo     = Split-Path -Parent $PSScriptRoot
$dist     = Join-Path $repo 'dist'
$launcher = Join-Path $dist 'si_zh_launcher.exe'
if (-not $TestExe) {
    $TestExe = if ($env:SI_TEST_DIR) { Join-Path $env:SI_TEST_DIR 'SystemInformer.exe' }
               else { Join-Path $repo 'test-app\SystemInformer.exe' }
}
$testExe = $TestExe

$IFEO_ROOT = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
$KEY       = "$IFEO_ROOT\SystemInformer.exe"
$LLOG      = Join-Path $dist 'si_zh_launcher.log'

function Kill-Test {
    Get-Process SystemInformer -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $testExe } | Stop-Process -Force -ErrorAction SilentlyContinue
    Get-Process si_zh_launcher -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

Kill-Test
Remove-Item $LLOG -Force -ErrorAction SilentlyContinue
if (-not (Test-Path $testExe)) { throw "找不到测试副本 $testExe" }

$hadKey = Test-Path $KEY
$oldVal = if ($hadKey) { (Get-ItemProperty -Path $KEY -ErrorAction SilentlyContinue).Debugger } else { $null }
Write-Output ("安装前 IFEO 键存在 = $hadKey ; Debugger = " + $(if ($oldVal) { $oldVal } else { '(无)' }))

try {
    New-Item -Path $KEY -Force | Out-Null
    # 带上 --log 便于取证；启动器每次运行都会把日志截断重建，不会无限增长
    Set-ItemProperty -Path $KEY -Name Debugger -Value ('"' + $launcher + '" --log') -Type String
    Write-Output ("已挂 IFEO: Debugger = " + (Get-ItemProperty -Path $KEY).Debugger)
    Write-Output ''

    if ($ViaExplorer) {
        Write-Output '[1] 经 explorer.exe 启动（非提权，等价于双击）...'
        Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$testExe`"" | Out-Null
    } else {
        Write-Output '[1] 用普通方式启动 SystemInformer.exe（本 shell 已提权）...'
        Start-Process -FilePath $testExe | Out-Null
    }

    for ($i = 1; $i -le 10; $i++) {
        Start-Sleep -Seconds 2
        $app = Get-Process SystemInformer -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $testExe }
        if ($app) { Write-Output "    t=$($i*2)s 目标进程已出现 pid=$($app.Id)"; break }
        Write-Output "    t=$($i*2)s 目标进程尚未出现"
    }
    Start-Sleep -Seconds 8

    Write-Output ''
    Write-Output '[2] 进程事实'
    $app  = @(Get-Process SystemInformer -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $testExe })
    $lch  = @(Get-Process si_zh_launcher -ErrorAction SilentlyContinue)
    Write-Output ("    SystemInformer(测试副本) 实例数 = " + $app.Count)
    Write-Output ("    si_zh_launcher 实例数          = " + $lch.Count + "   <-- 必须是 1；>1 就是 IFEO 递归")
    if ($app.Count -ge 1) {
        Write-Output ("    目标 pid = " + $app[0].Id + "  标题 = " + $app[0].MainWindowTitle)
        $mods = @($app[0].Modules | Where-Object { $_.ModuleName -ieq 'zh_hook.dll' })
        Write-Output ("    目标进程含 zh_hook.dll = " + ($mods.Count -gt 0))
    }
    Write-Output ("    递归判据 = " + $(if ($lch.Count -le 1) { '通过（未递归）' } else { '失败（发生了自我复制）' }))

    Write-Output ''
    Write-Output '[3] 启动器日志'
    if (Test-Path $LLOG) {
        $txt = Get-Content $LLOG -Encoding Unicode
        $starts = @($txt | Where-Object { $_ -match 'si_zh_launcher 启动' })
        Write-Output ("    「启动」次数 = " + $starts.Count + "   <-- 1 表示只跑了一遍")
        $txt | Where-Object { $_ -match '已创建|已分离|注入|Ready|Done|主线程已放行|!!' } |
            ForEach-Object { '    ' + $_ }
    } else {
        Write-Output '    (没有日志 -> 启动器没被调用？)'
    }
}
finally {
    Kill-Test
    if ($hadKey) {
        if ($oldVal) { Set-ItemProperty -Path $KEY -Name Debugger -Value $oldVal -Type String }
        else { Remove-ItemProperty -Path $KEY -Name Debugger -Force -ErrorAction SilentlyContinue }
    } else {
        Remove-Item $KEY -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Output ''
    Write-Output ("[还原] IFEO 键存在 = " + (Test-Path $KEY) + " ; Debugger = " +
                  $(if (Test-Path $KEY) { (Get-ItemProperty -Path $KEY -ErrorAction SilentlyContinue).Debugger } else { '(无)' }))
}

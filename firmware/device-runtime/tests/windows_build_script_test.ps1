# Windows构建入口确定性回归：只提取实际时钟检查函数，不触发VS安装或完整编译。
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop' # 任何断言失败返回非零。
$scriptPath = Join-Path $PSScriptRoot '../scripts/build-windows.ps1' # 被测正式入口。
$tokens = $null; $errors = $null # PowerShell语法解析结果。
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$errors) # 不执行入口的环境变更。
if ($errors.Count -gt 0) { throw 'Windows build script has parse errors' }
$definition = $ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Assert-BuildClock'}, $true) # 精确选择实际函数。
if (-not $definition) { throw 'Build clock preflight is missing' }
Invoke-Expression $definition.Extent.Text
$testRoot = Join-Path $env:TEMP ('ur-clock-test-' + [Guid]::NewGuid().ToString('N')) # 本轮独占回归目录。
$file = Join-Path $testRoot 'clock-input.txt' # 不更改实际源码时间戳。
$checks = 0 # 已通过的真实断言数量。
New-Item -ItemType Directory $testRoot | Out-Null
try {
    'clock-test' | Set-Content $file
    Assert-BuildClock $file
    $checks++
    [IO.File]::SetLastWriteTimeUtc($file, [DateTime]::UtcNow.AddHours(1))
    $rejected = $false # 必须观察到未来输入被拒绝，不接受仅无崩溃。
    try { Assert-BuildClock $file } catch { $rejected = $_.Exception.Message -match 'build clock is earlier' }
    if (-not $rejected) { throw 'Future input was not rejected' }
    $checks++
    [IO.File]::SetLastWriteTimeUtc($file, [DateTime]::UtcNow.AddHours(-1))
    Assert-BuildClock $file
    $checks++
    $rejected = $false
    try { Assert-BuildClock (Join-Path $testRoot 'missing.txt') } catch { $rejected = $true }
    if (-not $rejected) { throw 'Missing build input was not rejected' }
    $checks++
    Write-Output "Windows build-script assertions passed: $checks"
} finally {
    Remove-Item $file -ErrorAction SilentlyContinue
    Remove-Item $testRoot -ErrorAction SilentlyContinue
}

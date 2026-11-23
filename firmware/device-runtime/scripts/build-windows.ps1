# Windows/MSVC统一构建入口：固定vcpkg基线，执行C++/传输测试，再打ZIP与安装程序。
[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path, # 源码根目录。
    [string]$BuildRoot, # 本次构建、测试和产物目录。
    [string]$VcpkgRoot, # 固定版本的Windows依赖工具目录。
    [string]$InnoCompiler = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe', # 安装器编译工具。
    [ValidateRange(1, 16)][int]$Jobs = 2 # 编译并发上限。
)
$ErrorActionPreference = 'Stop' # 任何阶段失败都禁止继续交付部分产物。
if (-not $BuildRoot) { $BuildRoot = Join-Path $RepoRoot '.temp/device-runtime/windows' }
if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path $RepoRoot '.reference/vcpkg' }
$source = Join-Path $RepoRoot 'firmware/device-runtime' # 可移植设备源码。
$vcpkgCommit = 'e06564091c10e0b042900800c9fd48aad7f00643' # 与manifest一致的依赖基线。

# Invoke-Checked执行Program及Arguments，正常返回输出；非零退出码直接抛错。
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit $LASTEXITCODE" }
}

# Assert-BuildClock核对File时间，明显领先系统时钟时抛错，避免Ninja反复重生成后才失败。
function Assert-BuildClock([string]$File) {
    $item = Get-Item -LiteralPath $File -ErrorAction Stop # 必须存在的构建输入。
    if ($item.LastWriteTimeUtc -gt [DateTime]::UtcNow.AddSeconds(2)) {
        throw "Windows build clock is earlier than input: $File. Correct Windows/QEMU UTC clock before building."
    }
}
Assert-BuildClock (Join-Path $source 'CMakeLists.txt')

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe' # 官方工具链定位器。
if (-not (Test-Path $vswhere)) { throw 'Install Visual Studio 2022 C++ Build Tools first' }
$vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath # 具有x64工具链的VS2022目录。
if (-not $vs) { throw 'MSVC 2022 x64 toolchain is unavailable' }
$devcmd = Join-Path $vs 'Common7/Tools/VsDevCmd.bat' # 当前进程的MSVC环境初始化入口。
# 仅调用经vswhere解析的工具路径；环境输出用于当前进程，不写日志或发布配置。
if ($devcmd -match '["&|<>^]') { throw 'Unsupported Visual Studio installation path' }
$environment = & $env:ComSpec /d /s /c "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'MSVC environment setup failed' }
foreach ($entry in $environment) {
    if ($entry -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' # 官方VS附带的CMake。
$ninjaDir = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja' # 同一工具链的Ninja目录。
$env:PATH = "$ninjaDir;$env:PATH"
foreach ($program in @('git', 'python', 'node', 'mosquitto')) {
    if (-not (Get-Command $program -ErrorAction SilentlyContinue)) { throw "Missing test/build prerequisite: $program" }
}
if (-not (Test-Path $InnoCompiler)) { throw 'Install Inno Setup 6 before creating the installer' }
if (-not (Test-Path $VcpkgRoot)) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $VcpkgRoot) | Out-Null
    Invoke-Checked 'git' @('clone', '--filter=blob:none', 'https://github.com/microsoft/vcpkg.git', $VcpkgRoot)
    Invoke-Checked 'git' @('-C', $VcpkgRoot, 'checkout', '--detach', $vcpkgCommit)
}
$actualCommit = & git -C $VcpkgRoot rev-parse HEAD # 不覆盖用户已有的其他依赖版本。
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $vcpkgCommit) { throw 'vcpkg checkout differs from the locked baseline' }
Assert-BuildClock (Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake')
if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
    Invoke-Checked (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
}
$env:VCPKG_MAX_CONCURRENCY = [string]$Jobs
New-Item -ItemType Directory -Force $BuildRoot | Out-Null
Invoke-Checked $cmake @('-S', $source, '-B', $BuildRoot, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake", '-DVCPKG_TARGET_TRIPLET=x64-windows',
    '-DUR_DESKTOP_BROWSER=ON', '-DUR_WINDOWS_PACKAGE=ON')
# 同轮收集所有独立编译错误；仍保留非零退出即禁止测试/打包的门禁。
Invoke-Checked $cmake @('--build', $BuildRoot, '--parallel', [string]$Jobs, '--', '-k', '0')
Invoke-Checked (Join-Path (Split-Path $cmake) 'ctest.exe') @('--test-dir', $BuildRoot, '--output-on-failure')
# 每次打包使用新暂存目录，避免失败构建或旧依赖留下的DLL混入交付包。
$stage = Join-Path $BuildRoot ('stage-' + [Guid]::NewGuid().ToString('N')) # 当前打包专属暂存目录。
Invoke-Checked $cmake @('--install', $BuildRoot, '--prefix', $stage)
$packages = Join-Path $BuildRoot 'packages' # ZIP、EXE及散列的交付目录。
New-Item -ItemType Directory -Force $packages | Out-Null
Invoke-Checked (Join-Path (Split-Path $cmake) 'cpack.exe') @('--config', (Join-Path $BuildRoot 'CPackConfig.cmake'), '-B', $packages)
Invoke-Checked $InnoCompiler @("/DPackageDir=$stage", "/DOutputDir=$packages", (Join-Path $source 'packaging/windows/setup.iss'))
Get-ChildItem $packages -File | Get-FileHash -Algorithm SHA256 | Format-Table -AutoSize

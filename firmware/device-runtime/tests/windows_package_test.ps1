# 真实Windows安装包验收：静默安装、运行依赖、三个界面入口、升级与卸载。
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Installer) # 待验收的真实EXE安装器。
$ErrorActionPreference = 'Stop'
$testRoot = Join-Path $env:TEMP ('UR device test ' + [Guid]::NewGuid().ToString('N')) # 测试独占目录，特意包含空格。
$install = Join-Path $testRoot 'installed' # 安装和卸载的精确目标。
New-Item -ItemType Directory $testRoot | Out-Null
$installed = $false # 失败后是否需要回收已安装的软件。
$assertions = 0 # 仅统计实际执行成功的断言。
$originalPath = $env:PATH # 验收结束恢复调用者环境，测试时不借用开发工具的DLL搜索路径。
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"

# Assert-Package验证安装后的行为，错误立即停止，不只检查安装器退出码。
function Assert-Package([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:assertions++
    Write-Output "PASS $Message"
}

# Install-Package指定本次独占目录；同路径第二次运行用于实际覆盖升级验证。
function Install-Package {
    $p = Start-Process -FilePath $Installer -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR=`"$install`" /LOG=`"$testRoot\install.log`"" -Wait -PassThru
    Assert-Package ($p.ExitCode -eq 0) 'installer exit zero'
    $script:installed = $true
}

try {
    Install-Package
    foreach ($file in @('device_browser.exe', 'device_video_call.exe', 'device_voice_file.exe', 'launch-device.ps1', 'unins000.exe', 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'mosquitto.dll', 'libcrypto-3-x64.dll', 'libssl-3-x64.dll', 'opus.dll', 'pthreadVC3.dll')) {
        Assert-Package (Test-Path (Join-Path $install $file)) "installed $file"
    }
    Assert-Package (-not [bool](Get-ChildItem $install -Recurse -Filter '*tests.exe')) 'test programs excluded'
    Assert-Package (-not [bool](Get-ChildItem $install -Recurse -Filter '*.pdb')) 'debug symbols excluded'
    Assert-Package (-not [bool](Get-ChildItem $install -Recurse -Filter '*.lib')) 'development static libraries excluded'
    Assert-Package (-not [bool](Get-ChildItem $install -Recurse -Filter '*.h')) 'development headers excluded'
    foreach ($license in @('SOURCES.md', 'LICENSE.watcher', 'LICENSE.IXWebSocket', 'LICENSE.cJSON', 'LICENSE.mosquitto', 'LICENSE.opus', 'LICENSE.openssl', 'LICENSE.pthreads')) {
        Assert-Package (Test-Path (Join-Path $install "licenses\$license")) "installed license $license"
    }
    # Windows自带系统库不是应用可再分发依赖，不能因依赖扫描而混入安装包。
    foreach ($systemDll in @('kernel32.dll', 'ntdll.dll', 'user32.dll', 'advapi32.dll', 'ws2_32.dll', 'ucrtbase.dll')) {
        Assert-Package (-not (Test-Path (Join-Path $install $systemDll))) "Windows system library excluded: $systemDll"
    }
    $fileDevice = Start-Process (Join-Path $install 'device_voice_file.exe') -Wait -PassThru -RedirectStandardError (Join-Path $testRoot 'file.err')
    Assert-Package ($fileDevice.ExitCode -eq 2) 'installed native executable loads dependencies'
    $secretText = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('temporary-test-secret'))
    $secret = ConvertTo-SecureString $secretText -AsPlainText -Force
    $configPath = Join-Path $testRoot 'device.json'
    @{mqttHost='127.0.0.1'; mqttPort=1883; caFile=''; product='testproduct'; device='testdevice'; encryptedSecret=($secret | ConvertFrom-SecureString)} | ConvertTo-Json | Set-Content $configPath -Encoding UTF8
    $secret.Dispose()
    foreach ($mode in @('VoiceUdp', 'VoiceWebRTC', 'Video')) {
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
        $start.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$install\launch-device.ps1`" -Mode $mode -NoBrowser -ConfigPath `"$configPath`""
        $start.UseShellExecute = $false
        $start.RedirectStandardOutput = $true
        $process = [Diagnostics.Process]::Start($start)
        try {
            $read = $process.StandardOutput.ReadLineAsync()
            Assert-Package ($read.Wait(15000)) "$mode local bridge startup within budget"
            $url = $read.Result
            Assert-Package ($url -match '^http://127\.0\.0\.1:[0-9]+/') "$mode provides local URL"
            $page = Invoke-WebRequest -UseBasicParsing $url -TimeoutSec 5
            Assert-Package ($page.StatusCode -eq 200 -and $page.Content -match '<html') "$mode serves embedded interface"
        } finally {
            # 只终止本轮launcher的精确子设备进程，使父进程正常离开其finally并释放句柄。
            Get-CimInstance Win32_Process -Filter "ParentProcessId=$($process.Id)" | Where-Object { $_.ExecutablePath -like "$install\device_*.exe" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
            if (-not $process.WaitForExit(5000)) { $process.Kill(); $process.WaitForExit() }
            $process.Dispose()
        }
    }
    Install-Package
    Assert-Package (Test-Path (Join-Path $install 'device_browser.exe')) 'same version reinstall preserves executable'
    $uninstall = Start-Process (Join-Path $install 'unins000.exe') -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -Wait -PassThru
    Assert-Package ($uninstall.ExitCode -eq 0) 'uninstaller exit zero'
    $installed = $false
    Assert-Package (-not (Test-Path (Join-Path $install 'device_browser.exe'))) 'uninstall removes installed native executable'
    Write-Output "Windows installer assertions passed: $assertions"
} finally {
    $env:PATH = $originalPath
    if ($installed -and (Test-Path (Join-Path $install 'unins000.exe'))) {
        Start-Process (Join-Path $install 'unins000.exe') -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -Wait | Out-Null
    }
    # 本次随机目录由测试创建，仅删除短期设备密钥；安装与错误日志保留供失败审查。
    Remove-Item (Join-Path $testRoot 'device.json') -ErrorAction SilentlyContinue
    Write-Output "Installer evidence: $testRoot"
}

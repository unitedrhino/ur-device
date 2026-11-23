# 桌面设备启动入口：配置仅保存在当前用户目录，设备密钥使用Windows DPAPI加密。
[CmdletBinding()]
param(
    [ValidateSet('VoiceUdp', 'VoiceWebRTC', 'Video')][string]$Mode = 'VoiceUdp', # 选择语音媒体或视频入口。
    [switch]$Configure, # 仅配置接入参数，不启动设备进程。
    [switch]$NoBrowser, # 自动验收时不打开真实浏览器。
    [string]$ConfigPath = (Join-Path $env:LOCALAPPDATA 'UnitedRhino\DeviceSimulator\device.json') # 当前用户的加密接入配置。
)
$ErrorActionPreference = 'Stop'

# Save-DeviceConfiguration收集接入参数；长期密钥不写入明文配置或控制台。
function Save-DeviceConfiguration {
    $config = [ordered]@{
        mqttHost = (Read-Host 'MQTT host')
        mqttPort = [int](Read-Host 'MQTT port')
        caFile = (Read-Host 'CA file path (required for remote MQTT TLS)')
        product = (Read-Host 'Product ID')
        device = (Read-Host 'Device name')
        encryptedSecret = (Read-Host 'Device secret (base64)' -AsSecureString | ConvertFrom-SecureString)
    }
    if ($config.mqttPort -lt 1 -or $config.mqttPort -gt 65535) { throw 'Invalid MQTT port' }
    New-Item -ItemType Directory -Force (Split-Path -Parent $ConfigPath) | Out-Null
    $config | ConvertTo-Json | Set-Content -LiteralPath $ConfigPath -Encoding UTF8
}

if ($Configure -or -not (Test-Path -LiteralPath $ConfigPath)) {
    Save-DeviceConfiguration
    if ($Configure) { Write-Host 'Device configuration saved'; exit 0 }
}
$config = Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json # 已保存的接入参数。
if ([int]$config.mqttPort -lt 1 -or [int]$config.mqttPort -gt 65535 -or
    [string]::IsNullOrWhiteSpace($config.mqttHost) -or
    [string]::IsNullOrWhiteSpace($config.product) -or
    [string]::IsNullOrWhiteSpace($config.device)) { throw 'Invalid device configuration' }
$binary = Join-Path $PSScriptRoot $(if ($Mode -eq 'Video') { 'device_video_call.exe' } else { 'device_browser.exe' }) # 本安装目录的原生设备程序。
if (-not (Test-Path -LiteralPath $binary)) { throw 'Device executable is missing' }
# 本地端口租约只用于选取候选值，C++绑定失败仍会显式退出，不抢占其他监听者。
$lease = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
$lease.Start()
$port = $lease.LocalEndpoint.Port # 待交给C++桥的本地候选端口。
$lease.Stop()
$start = New-Object Diagnostics.ProcessStartInfo # 仅向子设备传递身份与媒体选项。
$start.FileName = $binary
$start.Arguments = [string]$port
$start.WorkingDirectory = $PSScriptRoot
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$start.EnvironmentVariables['UR_MQTT_HOST'] = [string]$config.mqttHost
$start.EnvironmentVariables['UR_MQTT_PORT'] = [string]$config.mqttPort
$start.EnvironmentVariables['UR_MQTT_CA'] = [string]$config.caFile
$start.EnvironmentVariables['UR_DEVICE_PRODUCT'] = [string]$config.product
$start.EnvironmentVariables['UR_DEVICE_NAME'] = [string]$config.device
$start.EnvironmentVariables['UR_MEDIA_TRANSPORT'] = if ($Mode -eq 'VoiceWebRTC') { 'webrtc' } else { 'udp' }
# 明文只进入子设备进程环境，不覆盖调用者的进程级环境变量。
$secret = ConvertTo-SecureString $config.encryptedSecret # 当前用户DPAPI解密结果。
$secretPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secret) # 仅用于一次子进程参数传递的短期缓冲。
try { $start.EnvironmentVariables['UR_DEVICE_SECRET'] = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($secretPointer) }
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($secretPointer); $secret.Dispose() }
$process = New-Object Diagnostics.Process # 本启动器唯一持有的设备进程。
$process.StartInfo = $start
$started = $false # 防止启动失败时读取无效的进程状态。
try {
    if (-not $process.Start()) { throw 'Device process could not start' }
    $started = $true
    $url = $process.StandardOutput.ReadLine()
    if ($url -notmatch '^http://127\.0\.0\.1:[0-9]+/(\?media=webrtc)?$') { throw 'Device bridge could not start' }
    Write-Host $url
    if (-not $NoBrowser) { Start-Process $url }
    while (-not $process.StandardOutput.EndOfStream) { Write-Host $process.StandardOutput.ReadLine() }
    $process.WaitForExit()
    exit $process.ExitCode
} finally {
    if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $process.Dispose()
    $start.EnvironmentVariables.Remove('UR_DEVICE_SECRET')
}

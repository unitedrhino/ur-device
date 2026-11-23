; Windows每用户安装包：独立C++设备程序、运行依赖和三种桌面启动入口。
#ifndef PackageDir
  #error PackageDir must point to the CMake install directory
#endif
#ifndef OutputDir
  #define OutputDir "output"
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
[Setup]
AppId={{6E6F1B34-7D38-4C24-93C1-57F09E9BEC63}
AppName=联犀设备模拟器
AppVersion={#AppVersion}
AppPublisher=UnitedRhino
DefaultDirName={localappdata}\Programs\UnitedRhinoDevice
DefaultGroupName=联犀设备模拟器
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=UR-Device-Simulator-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
UninstallDisplayName=联犀设备模拟器
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Icons]
Name: "{group}\AI 语音（MQTT+UDP）"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\launch-device.ps1"" -Mode VoiceUdp"; WorkingDir: "{app}"
Name: "{group}\AI 语音（WebRTC）"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\launch-device.ps1"" -Mode VoiceWebRTC"; WorkingDir: "{app}"
Name: "{group}\视频通话"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\launch-device.ps1"" -Mode Video"; WorkingDir: "{app}"
Name: "{group}\设备接入配置"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\launch-device.ps1"" -Configure"; WorkingDir: "{app}"
Name: "{group}\卸载"; Filename: "{uninstallexe}"

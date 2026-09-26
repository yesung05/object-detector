; Generated payload only: never package development state or the build folder.
#ifndef StageDir
  #error Build with scripts/build-installer.ps1 to provide StageDir
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0-alpha.1"
#endif
#ifndef NumericVersion
  #define NumericVersion "0.1.0.1"
#endif
[Setup]
AppId={{8B1A3B4E-3648-4926-9619-558A4C9D984A}
AppName=unmanned_detector
AppVersion={#AppVersion}
VersionInfoVersion={#NumericVersion}
DefaultDirName={localappdata}\Programs\unmanned_detector
DefaultGroupName=unmanned_detector
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=unmanned_detector-Setup-{#AppVersion}-x64
Compression=lzma2/normal
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
UninstallDisplayName=unmanned_detector {#AppVersion}
CloseApplications=yes
RestartApplications=no
AppMutex=Local\HUNIK.Installed.Launcher
[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"
[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
[Tasks]
Name: "desktopicon"; Description: "바탕 화면 바로 가기 만들기"; Flags: checkedonce
[Icons]
Name: "{autoprograms}\unmanned_detector\unmanned_detector 실행"; Filename: "{app}\unmanned_detector-launcher.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\unmanned_detector"; Filename: "{app}\unmanned_detector-launcher.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autoprograms}\unmanned_detector\사용 안내"; Filename: "{app}\README.txt"
Name: "{autoprograms}\unmanned_detector\기록 폴더"; Filename: "{localappdata}\unmanned_detector"
[Run]
Filename: "{app}\unmanned_detector-launcher.exe"; Description: "unmanned_detector 실행"; Flags: nowait postinstall skipifsilent unchecked
; User data is intentionally never removed by uninstall.

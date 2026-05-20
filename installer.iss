[Setup]
AppName=DatabaseTopit
AppVersion={#AppVersion}
AppPublisher=mironovnp
DefaultDirName={autopf}\DatabaseTopit
DefaultGroupName=DatabaseTopit
OutputDir=.\Output
OutputBaseFilename=DatabaseTopit-Setup
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
SetupIconFile=compiler:SetupClassicIcon.ico
UninstallDisplayIcon={app}\CaseChampGui.exe

[Files]
; The GUI executable and all its DLLs (published via dotnet publish --self-contained true or false)
Source: "staging\GUI\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; The C++ database server backend
Source: "staging\dbserver.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; Create shortcut in start menu
Name: "{group}\DatabaseTopit"; Filename: "{app}\CaseChampGui.exe"
Name: "{group}\Uninstall DatabaseTopit"; Filename: "{uninstallexe}"
; Create shortcut on desktop
Name: "{autodesktop}\DatabaseTopit"; Filename: "{app}\CaseChampGui.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Run]
Filename: "{app}\CaseChampGui.exe"; Description: "{cm:LaunchProgram,DatabaseTopit}"; Flags: nowait postinstall skipifsilent

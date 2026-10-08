; Inno Setup script for RD Desktop Software.
; Built by package.bat (which passes the version and the path of the built exe);
; can also be compiled directly:  ISCC.exe installer\RD_Desktop_Software.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceExe
  #define SourceExe "..\build\RD_Desktop_Software.exe"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
#define SourceDir ExtractFilePath(SourceExe)

#define AppName "RD Desktop Software"
#define AppPublisher "RepairDesk Desktop"
#define AppExeName "RD_Desktop_Software.exe"

[Setup]
AppId={{6B1E2C4A-7D52-4D41-9C64-2F0A3B5E8D17}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
OutputDir={#OutputDir}
OutputBaseFilename=RD_Desktop_Software-{#AppVersion}-setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
; Per-user install by default (no UAC prompt); the user may still pick "all users".
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DisableProgramGroupPage=yes
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion
; MinGW runtime DLLs that build.bat places next to the exe (libwinpthread-1.dll).
Source: "{#SourceDir}*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
// Settings live in %APPDATA%\RD Desktop Software; only removed if the user agrees.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  SettingsDir: String;
begin
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent then
  begin
    SettingsDir := ExpandConstant('{userappdata}\RD Desktop Software');
    if DirExists(SettingsDir) then
      if MsgBox('Also delete your settings and saved RepairDesk credentials (' + SettingsDir + ')?',
                mbConfirmation, MB_YESNO) = IDYES then
        DelTree(SettingsDir, True, True, True);
  end;
end;

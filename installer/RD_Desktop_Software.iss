; Inno Setup script for RD Desktop Software.
; Built by package.bat (which passes the version and the path of the built exe);
; can also be compiled directly:  ISCC.exe installer\RD_Desktop_Software.iss
; Optional code signing: package.bat defines SignToolName and registers the tool with /S.

#ifndef AppVersion
  ; Fallback: read the version from version.txt (the single source, also used by CMake).
  #define VersionFile FileOpen(AddBackslash(SourcePath) + "..\version.txt")
  #define AppVersion Trim(FileRead(VersionFile))
  #expr FileClose(VersionFile)
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
#define AppURL "https://github.com/Jeremiahstauffur/RD-Desktop-Software"
; Must never change: it is how Setup recognises an existing installation to update.
#define AppGuid "{6B1E2C4A-7D52-4D41-9C64-2F0A3B5E8D17}"
#define UninstallKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\" + AppGuid + "_is1"

[Setup]
AppId={{#AppGuid}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
AppCopyright=Copyright (C) 2026 Jeremiah Stauffur
; Version resource of setup.exe: antivirus heuristics distrust installers without one.
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} Setup
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
VersionInfoCopyright=Copyright (C) 2026 Jeremiah Stauffur
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
SetupIconFile=RDicon.ico
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
; Updating: reuse the install mode (per-user / all users), folder and shortcut choices
; of an existing installation instead of asking again or creating a second copy.
UsePreviousPrivileges=yes
UsePreviousAppDir=yes
UsePreviousTasks=yes
DirExistsWarning=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DisableProgramGroupPage=yes
CloseApplications=yes
RestartApplications=no
#ifdef SignToolName
SignTool={#SignToolName}
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion
; MinGW runtime DLLs that build.bat places next to the exe (libwinpthread-1.dll).
Source: "{#SourceDir}*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "RDicon.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\RDicon.ico"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\RDicon.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
; In-app update: the app starts Setup with /SILENT /FROMAPP=1, Setup closes the app
; (CloseApplications) and starts the new version when it is done.
Filename: "{app}\{#AppExeName}"; Flags: nowait postinstall skipifnotsilent; Check: LaunchedFromApp

[Code]
// ---------------------------------------------------------------------------
// Update detection. User preferences (accent colour, column layout, font size,
// OAuth credentials, ...) live in %APPDATA%\RD Desktop Software\settings.json,
// which Setup never writes or deletes, so an update keeps them automatically.
// ---------------------------------------------------------------------------
var
  PrevVersion: String;
  PrevDir: String;

function ReadPrevInstall(Root: Integer): Boolean;
begin
  Result := RegQueryStringValue(Root, '{#UninstallKey}', 'DisplayVersion', PrevVersion);
  if Result then
    RegQueryStringValue(Root, '{#UninstallKey}', 'InstallLocation', PrevDir);
end;

function IsUpdate: Boolean;
begin
  Result := PrevVersion <> '';
end;

// True when the running app started this Setup (see Updater.cpp).
function LaunchedFromApp: Boolean;
begin
  Result := ExpandConstant('{param:FROMAPP|0}') = '1';
end;

function InitializeSetup: Boolean;
begin
  PrevVersion := '';
  PrevDir := '';
  // Prefer the install mode Setup is running in; fall back to the other one so a
  // previous all-users install is still recognised from a per-user run and vice versa.
  if IsAdminInstallMode then
  begin
    if not ReadPrevInstall(HKLM) then ReadPrevInstall(HKCU);
  end
  else
  begin
    if not ReadPrevInstall(HKCU) then ReadPrevInstall(HKLM);
  end;
  Result := True;
end;

// When updating, the folder is already known: don't ask for it again.
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := IsUpdate and (PageID = wpSelectDir);
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  S: String;
begin
  S := '';
  if IsUpdate then
  begin
    S := S + 'Existing installation found:' + NewLine;
    S := S + Space + '{#AppName} ' + PrevVersion + ' -> {#AppVersion}' + NewLine;
    if PrevDir <> '' then S := S + Space + PrevDir + NewLine;
    S := S + NewLine;
    S := S + 'Your settings are kept:' + NewLine;
    S := S + Space + 'accent colour, theme, font size, table column layout,' + NewLine;
    S := S + Space + 'API key / OAuth credentials (' + ExpandConstant('{userappdata}\RD Desktop Software') + ')' + NewLine + NewLine;
  end;
  if MemoDirInfo <> '' then S := S + MemoDirInfo + NewLine + NewLine;
  if MemoGroupInfo <> '' then S := S + MemoGroupInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then S := S + MemoTasksInfo + NewLine;
  Result := S;
end;

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

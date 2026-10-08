#define MyAppName "Screen2NVR"
#define MyAppVersion "1.1.0"
#define MyAppPublisher "Screen2NVR"
#define MyAppExeName "Screen2NVR.exe"

[Setup]
AppId={{6D1A75C8-ED8D-4DD6-B3B9-75E38B925DB6}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\Screen2NVR
DefaultGroupName=Screen2NVR
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=Screen2NVR-Setup-x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.14393
PrivilegesRequired=admin
CloseApplications=yes
RestartApplications=no
AppMutex=Local\Screen2NVR.ScreenCapture
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupLogging=yes
SetupIconFile=..\assets\Screen2NVR.ico
ShowLanguageDialog=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
english.AutoStart=Start Screen2NVR when I sign in to Windows
russian.AutoStart=Запускать Screen2NVR при входе в Windows
english.AutoStartGroup=Startup:
russian.AutoStartGroup=Автозагрузка:
english.DesktopIcon=Create a desktop shortcut
russian.DesktopIcon=Создать ярлык на рабочем столе
english.ShortcutsGroup=Shortcuts:
russian.ShortcutsGroup=Ярлыки:
english.UninstallApp=Uninstall Screen2NVR
russian.UninstallApp=Удалить Screen2NVR
english.LaunchApp=Launch Screen2NVR
russian.LaunchApp=Запустить Screen2NVR
english.MissingMediaFoundation=Microsoft Media Foundation is not available in this edition of Windows.%n%nIf you use Windows 10 N or Windows 11 N, install the official Media Feature Pack from Settings > Apps > Optional features, restart the computer, and run the Screen2NVR installer again.
russian.MissingMediaFoundation=В этой редакции Windows отсутствует компонент Microsoft Media Foundation.%n%nЕсли используется Windows 10 N или Windows 11 N, установите официальный компонент «Media Feature Pack» через «Параметры» > «Приложения» > «Дополнительные компоненты», перезагрузите компьютер и повторите установку Screen2NVR.
english.MissingD3DCompiler=The standard Direct3D component D3DCompiler_47.dll is missing from Windows.%n%nInstall all Windows updates and run the Screen2NVR installer again.
russian.MissingD3DCompiler=В Windows отсутствует штатный компонент Direct3D D3DCompiler_47.dll.%n%nУстановите все обновления Windows и повторите установку Screen2NVR.

[Tasks]
Name: "autostart"; Description: "{cm:AutoStart}"; GroupDescription: "{cm:AutoStartGroup}"; Flags: checkedonce
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; GroupDescription: "{cm:ShortcutsGroup}"; Flags: unchecked

[Files]
Source: "..\x64\Release\Screen2NVR.exe"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{commonappdata}\Screen2NVR"; Permissions: users-modify
Name: "{commonappdata}\Screen2NVR\logs"; Permissions: users-modify

[Icons]
Name: "{group}\Screen2NVR"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallApp}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Screen2NVR"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Screen2NVR"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Screen2NVR"" dir=in action=allow program=""{app}\{#MyAppExeName}"" enable=yes profile=any"; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Parameters: "--set-autostart=1"; Flags: runhidden waituntilterminated runasoriginaluser; Check: ShouldEnableAutoStart
Filename: "{app}\{#MyAppExeName}"; Parameters: "--set-autostart=0"; Flags: runhidden waituntilterminated runasoriginaluser; Check: ShouldDisableAutoStart
Filename: "{app}\{#MyAppExeName}"; Parameters: "--set-language={code:AppLanguage}"; Flags: runhidden waituntilterminated runasoriginaluser
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM {#MyAppExeName}"; Flags: runhidden waituntilterminated; RunOnceId: "StopScreen2NVR"
Filename: "{app}\{#MyAppExeName}"; Parameters: "--set-autostart=0"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveUserAutoStart"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Screen2NVR"""; Flags: runhidden waituntilterminated; RunOnceId: "RemoveFirewallRule"
Filename: "{sys}\reg.exe"; Parameters: "delete HKCU\Software\Microsoft\Windows\CurrentVersion\Run /v Screen2NVR /f"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveAutoStart"

[Code]
function InitializeSetup(): Boolean;
begin
  Result := False;

  if not FileExists(ExpandConstant('{sys}\mfplat.dll')) then
  begin
    MsgBox(ExpandConstant('{cm:MissingMediaFoundation}'), mbCriticalError, MB_OK);
    Exit;
  end;

  if not FileExists(ExpandConstant('{sys}\D3DCompiler_47.dll')) then
  begin
    MsgBox(ExpandConstant('{cm:MissingD3DCompiler}'), mbCriticalError, MB_OK);
    Exit;
  end;

  Result := True;
end;

function InstallerParameter(const Name: String): String;
begin
  Result := ExpandConstant('{param:' + Name + '|}');
end;

function AppLanguage(Param: String): String;
begin
  if ActiveLanguage = 'russian' then Result := 'ru'
  else Result := 'en';
end;

function ShouldEnableAutoStart(): Boolean;
var
  Value: String;
begin
  Value := InstallerParameter('AUTOSTART');
  if Value = '1' then Result := True
  else if Value = '0' then Result := False
  else Result := WizardIsTaskSelected('autostart');
end;

function ShouldDisableAutoStart(): Boolean;
begin
  Result := not ShouldEnableAutoStart();
end;

procedure CurPageChanged(CurPageID: Integer);
var
  Value: String;
begin
  if CurPageID = wpSelectTasks then
  begin
    Value := InstallerParameter('AUTOSTART');
    if Value = '1' then WizardSelectTasks('autostart')
    else if Value = '0' then WizardSelectTasks('!autostart');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  RtspPort, OnvifPort, ConfigPath: String;
begin
  if CurStep = ssPostInstall then
  begin
    ConfigPath := ExpandConstant('{commonappdata}\Screen2NVR\config.ini');
    RtspPort := InstallerParameter('RTSPPORT');
    OnvifPort := InstallerParameter('ONVIFPORT');
    if RtspPort <> '' then SetIniString('Network', 'RtspPort', RtspPort, ConfigPath);
    if OnvifPort <> '' then SetIniString('Network', 'OnvifPort', OnvifPort, ConfigPath);
  end;
end;

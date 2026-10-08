#define MyAppName "Screen2NVR"
#define MyAppVersion "1.0.0"
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

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "autostart"; Description: "Запускать Screen2NVR при входе в Windows"; GroupDescription: "Автозагрузка:"; Flags: checkedonce
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; GroupDescription: "Ярлыки:"; Flags: unchecked

[Files]
Source: "..\x64\Release\Screen2NVR.exe"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{commonappdata}\Screen2NVR"; Permissions: users-modify
Name: "{commonappdata}\Screen2NVR\logs"; Permissions: users-modify

[Icons]
Name: "{group}\Screen2NVR"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Удалить Screen2NVR"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Screen2NVR"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Screen2NVR"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Screen2NVR"" dir=in action=allow program=""{app}\{#MyAppExeName}"" enable=yes profile=any"; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Parameters: "--set-autostart=1"; Flags: runhidden waituntilterminated runasoriginaluser; Check: ShouldEnableAutoStart
Filename: "{app}\{#MyAppExeName}"; Parameters: "--set-autostart=0"; Flags: runhidden waituntilterminated runasoriginaluser; Check: ShouldDisableAutoStart
Filename: "{app}\{#MyAppExeName}"; Description: "Запустить Screen2NVR"; Flags: nowait postinstall skipifsilent

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
    MsgBox(
      'В этой редакции Windows отсутствует компонент Microsoft Media Foundation.' + #13#10 + #13#10 +
      'Если используется Windows 10 N или Windows 11 N, установите официальный компонент ' +
      '«Media Feature Pack» через «Параметры» → «Приложения» → «Дополнительные компоненты», ' +
      'перезагрузите компьютер и повторите установку Screen2NVR.',
      mbCriticalError, MB_OK);
    Exit;
  end;

  if not FileExists(ExpandConstant('{sys}\D3DCompiler_47.dll')) then
  begin
    MsgBox(
      'В Windows отсутствует штатный компонент Direct3D D3DCompiler_47.dll.' + #13#10 + #13#10 +
      'Установите все обновления Windows и повторите установку Screen2NVR.',
      mbCriticalError, MB_OK);
    Exit;
  end;

  Result := True;
end;

function InstallerParameter(const Name: String): String;
begin
  Result := ExpandConstant('{param:' + Name + '|}');
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

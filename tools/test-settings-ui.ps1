param([string]$Executable = "$PSScriptRoot\..\x64\Release\Screen2NVR.exe", [switch]$Fixture)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SettingsUiTest {
    public delegate bool EnumProc(IntPtr window, IntPtr value);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr value);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetLastActivePopup(IntPtr window);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr WriteText(IntPtr window, uint message, IntPtr w, string l);
    public static void SetWindowText(IntPtr window, string value) { WriteText(window, 0xC, IntPtr.Zero, value); }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr ReadText(IntPtr window, uint message, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr dest, int x, int y, int w, int h, IntPtr source, int sx, int sy, uint rop);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left, top, right, bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    public static IntPtr Find(uint process) {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr w, IntPtr p) {
            uint id; GetWindowThreadProcessId(w, out id);
            if (id == process && GetDlgItem(w, 2000) != IntPtr.Zero) { result = w; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }
    public static void Tab(IntPtr window, int index) {
        IntPtr tab = GetDlgItem(window, 2000);
        // TCM_SETCURFOCUS lets the control generate notifications inside its own process.
        SendMessage(tab, 0x1330, new IntPtr(index), IntPtr.Zero);
    }
    public static string Text(IntPtr window) {
        var text = new StringBuilder(4096); ReadText(window, 0xD, new IntPtr(text.Capacity), text); return text.ToString();
    }
}
'@
function Assert([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
    Write-Output "PASS: $Message"
}
function Control([int]$Id) { [SettingsUiTest]::GetDlgItem($script:window, $Id) }
function Send([IntPtr]$Handle, [uint32]$Message, [int]$W = 0, [int]$L = 0) {
    [SettingsUiTest]::SendMessage($Handle, $Message, [IntPtr]$W, [IntPtr]$L).ToInt64()
}
function Command([int]$Id, [int]$Notification = 0) { $null = Send $script:window 0x111 ($Id -bor ($Notification -shl 16)) }
function SourceMode([int]$Index) {
    $null = Send (Control 2050) 0x14E $Index
    Command 2050 1
}
function ComboItems([int]$Id) {
    $control = Control $Id
    $count = Send $control 0x146
    for ($i = 0; $i -lt $count; $i++) {
        $text = New-Object Text.StringBuilder 512
        $null = [SettingsUiTest]::ReadText($control, 0x148, [IntPtr]$i, $text)
        $text.ToString()
    }
}
function SelectCombo([int]$Id, [int]$Index) {
    $null = Send (Control $Id) 0x14E $Index
    Command $Id 1
}
function Screenshot([string]$Name) {
    $rect = New-Object SettingsUiTest+Rect
    $null = [SettingsUiTest]::GetWindowRect($script:window, [ref]$rect)
    $bitmap = New-Object Drawing.Bitmap(($rect.right - $rect.left), ($rect.bottom - $rect.top))
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try { $null = [SettingsUiTest]::PrintWindow($script:window, $dc, 2) }
    finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
    try { $bitmap.Save((Join-Path $script:output $Name), [Drawing.Imaging.ImageFormat]::Png) }
    finally { $bitmap.Dispose() }
}
function PaintedClient([string]$Name) {
    # Read already-painted pixels. PrintWindow would trigger a repaint and can hide this regression.
    Start-Sleep -Milliseconds 120
    $rect = New-Object SettingsUiTest+Rect
    $null = [SettingsUiTest]::GetClientRect($script:window, [ref]$rect)
    $bitmap = New-Object Drawing.Bitmap($rect.right, $rect.bottom)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $destination = $graphics.GetHdc()
    $source = [SettingsUiTest]::GetDC($script:window)
    try { $null = [SettingsUiTest]::BitBlt($destination, 0, 0, $rect.right, $rect.bottom, $source, 0, 0, 0x00CC0020) }
    finally { $null = [SettingsUiTest]::ReleaseDC($script:window, $source); $graphics.ReleaseHdc($destination); $graphics.Dispose() }
    $bitmap.Save((Join-Path $script:output $Name), [Drawing.Imaging.ImageFormat]::Png)
    return $bitmap
}
$resolvedExe = (Resolve-Path -LiteralPath $Executable).Path
$script:output = [IO.Path]::GetFullPath("$PSScriptRoot\..\x64\ui-verification")
$null = New-Item -ItemType Directory -Path $script:output -Force
$app = if ($Fixture) { Start-Process -FilePath $resolvedExe -ArgumentList '--ui' -WindowStyle Hidden -PassThru }
       else { Start-Process -FilePath $resolvedExe -WindowStyle Hidden -PassThru }
$script:window = [IntPtr]::Zero
try {
    for ($i = 0; $i -lt 50; $i++) {
        if ($app.HasExited) { throw "Application exited: $($app.ExitCode)" }
        $script:window = [SettingsUiTest]::Find($app.Id)
        if ($script:window -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 200
    }
    Assert ($script:window -ne [IntPtr]::Zero) 'Settings window created'
    Command 1001
    Assert ([SettingsUiTest]::IsWindowVisible($script:window)) 'Settings opens after hidden launch'
    # Reproduce history-dependent painting before using PrintWindow anywhere.
    [SettingsUiTest]::Tab($script:window, 1)
    [SettingsUiTest]::Tab($script:window, 0)
    $painted = PaintedClient 'camera-after-video-actual.png'
    $painted.Dispose()
    Assert ((Send (Control 2000) 0x1304) -eq 5) 'Merged device tab leaves five tabs'
    foreach ($target in 0..4) {
        foreach ($previous in 0..4) {
            if ($target -eq $previous) { continue }
            [SettingsUiTest]::Tab($script:window, $previous)
            Start-Sleep -Milliseconds 35
            [SettingsUiTest]::Tab($script:window, $target)
            $painted = PaintedClient "tab-$target-after-$previous-actual.png"
            # Each tab's intentionally empty far-right area must be cleared, even over group boxes.
            $background = $painted.GetPixel(930, 550).ToArgb()
            $dirty = 0
            for ($y = 570; $y -lt 578; $y++) {
                for ($x = 700; $x -lt 900; $x++) {
                    if ($painted.GetPixel($x, $y).ToArgb() -ne $background) { $dirty++ }
                }
            }
            if ($target -eq 0) {
                for ($y = 550; $y -lt 555; $y++) {
                    for ($x = 700; $x -lt 900; $x++) {
                        if ($painted.GetPixel($x, $y).ToArgb() -ne $background) { $dirty++ }
                    }
                }
            }
            if ($target -eq 2) {
                for ($y = 500; $y -lt 530; $y++) {
                    for ($x = 595; $x -lt 605; $x++) {
                        if ($painted.GetPixel($x, $y).ToArgb() -ne $background) { $dirty++ }
                    }
                }
            }
            $painted.Dispose()
            Assert ($dirty -eq 0) "Tab $target after $previous has clean background"
        }
    }
    [SettingsUiTest]::Tab($script:window, 0)
    Assert ((Control 2002) -eq [IntPtr]::Zero -and (Control 2003) -eq [IntPtr]::Zero) 'Duplicated camera URL and copy button removed'
    Assert ([SettingsUiTest]::IsWindowVisible((Control 2001)) -and [SettingsUiTest]::IsWindowVisible((Control 2050))) 'Camera and source controls share Device tab'
    Assert (![SettingsUiTest]::IsWindowVisible((Control 2007))) 'Main RTSP path is not on Device tab'
    $initialRtspPort = [SettingsUiTest]::Text((Control 2006))
    $null = [SettingsUiTest]::SetWindowText((Control 2006), '9554')
    Screenshot 'device.png'
    [SettingsUiTest]::Tab($script:window, 1)
    Assert ([SettingsUiTest]::IsWindowVisible((Control 2007))) 'Main RTSP path is editable on Video tab'
    $null = [SettingsUiTest]::SetWindowText((Control 2007), 'ui-preview-test')
    $url = [SettingsUiTest]::Text((Control 2107))
    Write-Output "RTSP draft: $url"
    Assert ($url -match '^rtsp://.+:9554/ui-preview-test$') 'RTSP link tracks unsaved port and path'
    [SettingsUiTest]::Tab($script:window, 1)
    Assert (![SettingsUiTest]::IsWindowEnabled((Control 2101))) 'Disabled secondary stream fields are inactive'
    $null = Send (Control 2100) 0xF1 1
    Command 2100
    Assert ([SettingsUiTest]::IsWindowEnabled((Control 2101))) 'Enabling secondary stream activates fields'
    $null = [SettingsUiTest]::SetWindowText((Control 2106), 'preview-sub')
    Assert (([SettingsUiTest]::Text((Control 2108))) -match ':9554/preview-sub$') 'Secondary RTSP address follows draft settings'
    $initialMainResolution = [SettingsUiTest]::Text((Control 2008))
    Assert ((Control 2102) -eq [IntPtr]::Zero) 'Separate secondary height edit removed'
    Assert ((Send (Control 2008) 0x146) -gt 0 -and (Send (Control 2101) 0x146) -gt 0) 'Both resolutions use populated dropdown lists'
    SelectCombo 2101 0
    Assert (([SettingsUiTest]::Text((Control 2008))) -eq $initialMainResolution) 'Secondary resolution does not modify main resolution'
    $mainBefore = [SettingsUiTest]::Text((Control 2008)); $subBefore = [SettingsUiTest]::Text((Control 2101))
    foreach ($preset in 1..3) {
        SelectCombo 2040 $preset
        Assert (([SettingsUiTest]::Text((Control 2008))) -eq $mainBefore -and ([SettingsUiTest]::Text((Control 2101))) -eq $subBefore) "Preset $preset preserves both resolutions"
        $fps = @(0,8,12,25)[$preset]; $bitrate = @(0,1000,2000,4000)[$preset]; $gop = @(0,24,25,50)[$preset]
        Assert (([SettingsUiTest]::Text((Control 2010))) -eq "$fps" -and ([SettingsUiTest]::Text((Control 2011))) -eq "$bitrate" -and ([SettingsUiTest]::Text((Control 2012))) -eq "$gop") "Preset $preset changes only encoder rate parameters"
    }
    [SettingsUiTest]::SetWindowText((Control 2010), '17')
    Assert ((Send (Control 2040) 0x147) -eq 0) 'Manual FPS switches quality to Custom'
    SelectCombo 2040 0
    Assert (([SettingsUiTest]::Text((Control 2010))) -eq '17') 'Selecting Custom preserves manually edited FPS'
    Screenshot 'video-dual-stream.png'
    [SettingsUiTest]::Tab($script:window, 0)
    SourceMode 0
    Assert (![SettingsUiTest]::IsWindowVisible((Control 2056))) 'Window title hidden for whole monitor'
    Assert (![SettingsUiTest]::IsWindowVisible((Control 2052))) 'Region fields hidden for whole monitor'
    Assert ((Send (Control 2051) 0x146) -ge 1) 'Monitor selector populated'
    Screenshot 'source-monitor.png'
    SourceMode 1
    foreach ($field in @(@(2052,'0'), @(2053,'0'), @(2054,'1000'), @(2055,'800'))) {
        [SettingsUiTest]::SetWindowText((Control $field[0]), $field[1])
    }
    Command 2055 512
    [SettingsUiTest]::Tab($script:window, 1)
    $mainOptions = @(ComboItems 2008); $subOptions = @(ComboItems 2101)
    $times = [char]0xD7
    Assert ($mainOptions[0] -like "1000 $times 800*" -and $mainOptions[-1] -eq "720 $times 576") 'Main resolutions use the capture region, from native width down to 720'
    Assert (($subOptions -join ',') -eq "720 $times 576,640 $times 512,480 $times 384,320 $times 256") 'Four secondary choices follow the crop proportions, not monitor proportions'
    SelectCombo 2008 ($mainOptions.Count - 1)
    Assert (([SettingsUiTest]::Text((Control 2008))) -eq "720 $times 576") 'Main resolution can be chosen as a width-height pair'
    SelectCombo 2040 3
    Assert (([SettingsUiTest]::Text((Control 2008))) -eq "720 $times 576") 'Preset does not overwrite the chosen crop resolution'
    Screenshot 'video-crop-resolutions.png'
    [SettingsUiTest]::Tab($script:window, 0)
    Assert ([SettingsUiTest]::IsWindowVisible((Control 2052))) 'Region fields shown for region capture'
    Assert (![SettingsUiTest]::IsWindowVisible((Control 2056))) 'Window title hidden for region capture'
    $null = [SettingsUiTest]::SetWindowText((Control 2052), '111')
    $null = [SettingsUiTest]::SetWindowText((Control 2053), '222')
    Assert (([SettingsUiTest]::Text((Control 2052))) -eq '111' -and ([SettingsUiTest]::Text((Control 2053))) -eq '222') 'Region coordinates edit independently'
    Screenshot 'source-region.png'
    SourceMode 2
    Assert ([SettingsUiTest]::IsWindowVisible((Control 2056))) 'Window title shown for named window'
    Assert (![SettingsUiTest]::IsWindowVisible((Control 2052))) 'Region fields hidden for named window'
    SourceMode 3
    Assert (![SettingsUiTest]::IsWindowVisible((Control 2056))) 'Window title hidden for active window'
    foreach ($mode in @(0,1,2,3,1,0,2,0)) { SourceMode $mode }
    Screenshot 'source-after-switches.png'
    Command 2016
    Command 1001
    Assert (([SettingsUiTest]::Text((Control 2008))) -eq $initialMainResolution) 'Cancel restores original resolution without silently migrating existing settings'
    [SettingsUiTest]::Tab($script:window, 2)
    foreach ($removed in @(2013,2023,2025,2026,2095)) {
        Assert ((Control $removed) -eq [IntPtr]::Zero) "Obsolete overlay control $removed removed"
    }
    Assert (([SettingsUiTest]::Text((Control 2017))) -eq ([string][char]0x428 + [char]0x440 + [char]0x438 + [char]0x444 + [char]0x442)) 'Font button has its short caption'
    $templateBefore = [SettingsUiTest]::Text((Control 2080))
    $tokens = @('{camera}','{date}','{time}','{computer}','{user}')
    foreach ($index in 0..4) {
        [SettingsUiTest]::SetWindowText((Control 2080), 'AB')
        $null = Send (Control 2080) 0xB1 1 1
        $null = Send (Control (2110+$index)) 0xF5
        Assert (([SettingsUiTest]::Text((Control 2080))) -eq ('A'+$tokens[$index]+'B')) "Token $($tokens[$index]) inserts at caret"
    }
    [SettingsUiTest]::SetWindowText((Control 2080), 'replace me')
    $null = Send (Control 2080) 0xB1 0 7
    $null = Send (Control 2111) 0xF5
    Assert (([SettingsUiTest]::Text((Control 2080))) -eq '{date} me') 'Token link replaces selected text'
    $null = Send (Control 2080) 0xC7
    Assert (([SettingsUiTest]::Text((Control 2080))) -eq 'replace me') 'Token insertion supports Undo'
    [SettingsUiTest]::SetWindowText((Control 2080), $templateBefore)
    $initialMasks = Send (Control 2091) 0x18B
    Start-Sleep -Seconds 3
    Screenshot 'preview-live.png'
    $preview = Control 2024
    $rect = New-Object SettingsUiTest+Rect
    $null = [SettingsUiTest]::GetWindowRect($preview, [ref]$rect)
    Assert ((($rect.right - $rect.left) * 9) -eq (($rect.bottom - $rect.top) * 16)) 'Preview is exactly 16:9'
    if ($Fixture) {
        $marginBefore = [SettingsUiTest]::Text((Control 2030))
        $null = Send $preview 0x201 1 (6 -bor (6 -shl 16))
        $null = Send $preview 0x200 1 (110 -bor (60 -shl 16))
        $null = Send $preview 0x202 0 (110 -bor (60 -shl 16))
        $marginAfter = [SettingsUiTest]::Text((Control 2030))
        Assert ($marginAfter -ne $marginBefore) 'Dragging overlay text updates its position'
        Screenshot 'preview-text-moved.png'
        $null = Send $preview 0x201 1 (110 -bor (60 -shl 16))
        $null = Send $preview 0x200 1 (180 -bor (90 -shl 16))
        $null = Send $preview 0x100 27
        Assert (([SettingsUiTest]::Text((Control 2030))) -eq $marginAfter) 'Esc restores original text position'
    }
    Command 2094
    Command 2092
    $null = Send $preview 0x201 1 (100 -bor (160 -shl 16))
    $null = Send $preview 0x200 1 (230 -bor (230 -shl 16))
    $null = Send $preview 0x202 0 (230 -bor (230 -shl 16))
    Assert ((Send (Control 2091) 0x18B) -eq 1) 'Dragging adds a privacy mask'
    $null = Send $preview 0x201 1 (160 -bor (190 -shl 16))
    $null = Send $preview 0x200 1 (185 -bor (210 -shl 16))
    $null = Send $preview 0x202 0 (185 -bor (210 -shl 16))
    Assert ((Send (Control 2091) 0x18B) -eq 1) 'Moving a mask preserves mask count'
    Screenshot 'preview-mask.png'
    $null = Send (Control 2022) 0x405 1 10
    $null = Send $script:window 0x114
    Screenshot 'preview-opacity-10.png'
    $null = Send (Control 2022) 0x405 1 95
    $null = Send $script:window 0x114
    Screenshot 'preview-opacity-95.png'
    Command 2093
    Assert ((Send (Control 2091) 0x18B) -eq 0) 'Delete removes selected mask'
    [SettingsUiTest]::Tab($script:window, 3)
    Screenshot 'security.png'
    [SettingsUiTest]::Tab($script:window, 4)
    Screenshot 'status.png'
    Command 2016
    Command 1001
    Assert (([SettingsUiTest]::Text((Control 2006))) -eq $initialRtspPort) 'Cancel discards draft network settings'
    Assert ((Send (Control 2091) 0x18B) -eq $initialMasks) 'Cancel restores saved masks'
    if ($Fixture) {
        [SettingsUiTest]::Tab($script:window, 2)
        [SettingsUiTest]::SetWindowText((Control 2080), '')
        Command 2015
        Assert (![SettingsUiTest]::IsWindowVisible($script:window) -and !$app.HasExited) 'Empty template saves without a restart dialog or exit'
        Command 1001
        Assert (([SettingsUiTest]::Text((Control 2080))) -eq '') 'Empty template stays disabled on reopen'
        $null = Send (Control 2110) 0xF5
        $null = Send (Control 2022) 0x405 1 25
        $null = Send $script:window 0x114
        Command 2015
        Assert (!$app.HasExited -and ![SettingsUiTest]::IsWindowVisible($script:window)) 'Overlay edit saves without restarting the application'
        Command 1001
        Assert (([SettingsUiTest]::Text((Control 2080))) -eq '{camera}') 'Saved template replaces removed checkboxes'
        Assert (([SettingsUiTest]::Text((Control 2034))) -eq '25') 'Saved live opacity retained'
        Screenshot 'overlay-final.png'
        [SettingsUiTest]::Tab($script:window, 4)
        Assert ([SettingsUiTest]::IsWindowVisible((Control 2096)) -and (Send (Control 2096) 0xF0) -eq 0) 'File logging is visible on Status and disabled by default'
        $null = Send (Control 2096) 0xF1 1
        Command 2016
        Command 1001
        Assert ((Send (Control 2096) 0xF0) -eq 0) 'Cancel does not enable logging'
        foreach ($enabled in @(1,0)) {
            $null = Send (Control 2096) 0xF1 $enabled
            Command 2015
            Assert (!$app.HasExited -and ![SettingsUiTest]::IsWindowVisible($script:window)) "Logging=$enabled saves without restart or interruption"
            Command 1001
            Assert ((Send (Control 2096) 0xF0) -eq $enabled) "Logging=$enabled survives reopening"
        }
        Screenshot 'status-logging.png'
        [SettingsUiTest]::Tab($script:window, 0)
        SourceMode 1
        foreach ($field in @(@(2052,'0'), @(2053,'0'), @(2054,'1000'), @(2055,'800'))) {
            [SettingsUiTest]::SetWindowText((Control $field[0]), $field[1])
        }
        Command 2055 512
        [SettingsUiTest]::Tab($script:window, 1)
        SelectCombo 2008 ((Send (Control 2008) 0x146) - 1)
        $null = Send (Control 2100) 0xF1 1
        Command 2100
        SelectCombo 2101 2
        # The fixture saves in memory; decline the expected video restart, never launch another process.
        $null = [SettingsUiTest]::PostMessage($script:window, 0x111, [IntPtr]2015, [IntPtr]::Zero)
        $noButton = [IntPtr]::Zero
        for ($i = 0; $i -lt 40; $i++) {
            Start-Sleep -Milliseconds 50
            $popup = [SettingsUiTest]::GetLastActivePopup($script:window)
            if ($popup -ne $script:window) { $noButton = [SettingsUiTest]::GetDlgItem($popup, 7) }
            if ($noButton -ne [IntPtr]::Zero) { break }
        }
        Assert ($noButton -ne [IntPtr]::Zero) 'Video/crop change requests restart, not a validation error'
        $null = Send $noButton 0xF5
        Command 1001
        Assert (([SettingsUiTest]::Text((Control 2008))) -eq "720 $times 576" -and ([SettingsUiTest]::Text((Control 2101))) -eq "480 $times 384") 'Saved main/sub resolution pairs survive reopening'
        Assert ((Send (Control 2008) 0x146) -eq 3 -and (Send (Control 2101) 0x146) -eq 4) 'Saving a new resolution removes legacy-only entries'
    }
    Write-Output "Screenshots: $script:output"
} finally {
    if ($script:window -ne [IntPtr]::Zero -and !$app.HasExited) { Command 1002 }
    if (!$app.WaitForExit(10000)) { throw "Test application did not stop; PID $($app.Id)" }
}

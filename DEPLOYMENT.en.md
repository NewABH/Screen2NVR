# Screen2NVR — installation and usage

[Русский](DEPLOYMENT.md) · [Project overview](README.md)

Screen2NVR was developed by the OpenAI Codex AI assistant from the user's
requirements. The user defined the product and architectural decisions and
tested earlier builds on real PCs and a Hikvision NVR.

The native `Screen2NVR.exe` contains desktop capture, GPU processing, H.264
encoding, RTSP/RTP, ONVIF, discovery and tray controls. No FFmpeg, MediaMTX,
.NET runtime or background service is required.

## Installation and language

Download `Screen2NVR-Setup-x64.exe` from
[Releases](https://github.com/NewABH/Screen2NVR/releases) and run it as administrator.
Choose **English** or **Russian** in the initial language dialog. The same
language is selected for the application. Setup installs into
`C:\Program Files\Screen2NVR`, creates a Start menu shortcut and data directory,
adds an inbound firewall rule, and registers a Windows uninstaller. The tasks
page offers startup when the installing user signs in and an optional desktop shortcut.

After installation, normal operation does not require administrator rights.
The application runs in the interactive user's session. Right-click its tray
icon to start/stop streaming, restart, copy the main RTSP URL, open the log
directory, open Settings or exit. Double-click also opens Settings.
The entire icon changes color: green for streaming, yellow for startup, red
for an error and gray for a user-paused stream.

The **Device → Language** selection switches the interface after **Save** without
restarting the application or disconnecting viewers. User-authored camera names,
templates and standby messages are not translated. The built-in standby message
is translated when switching languages. Windows common dialogs use the selected
language when those Windows resources are installed.

## Settings tabs

**Device** combines the camera name, ONVIF/RTSP ports, language, startup and capture
source. Select a monitor, rectangular region, window title or active window;
only relevant fields are shown. Window capture records the visible part on the
selected monitor, including other windows covering it. Cursor visibility and
individual mouse-click highlights are separate options. Overlapping clicks
retain independent soft fade-in/fade-out animations. A standby message can
replace the desktop while it is unavailable.

ONVIF manufacturer, model and serial number come from the current motherboard's
SMBIOS Type 2 record, using Windows `GetSystemFirmwareTable`. Missing or placeholder
values become `Unknown`. These identifiers are not editable, are not displayed
in Settings and are not copied from another computer's INI file.

**Video** contains main and secondary stream settings and their RTSP URLs.
Quality presets change only main-stream FPS, bitrate and keyframe interval (GOP).
They do not change either resolution or the secondary stream's parameters.
Manual rate changes select the Custom preset.

Resolution dropdowns follow the actual captured region's aspect ratio, including
window size and clipping to the selected monitor. Main-stream choices run from
native size down to width 720; secondary choices use widths 720, 640, 480 and 320.
Small sources are not upscaled; valid dimensions are even for H.264. Very small
or unusually narrow regions may have fewer legal choices. A previously saved
custom resolution remains available so merely opening Settings does not migrate it.

The secondary stream is off by default. Initial settings are 640×360, 8 FPS,
512 kbit/s and GOP 16. Its resolution and FPS cannot exceed the main stream.
Both streams share capture and composition; secondary scaling is done on the GPU,
then a separate encoder publishes it. Enabling another encoder increases load.
If a driver cannot support two hardware encoding sessions, software fallback
is used only when it has been explicitly allowed.

**Overlay** has a 16:9 live preview, text and privacy masks. The template alone
controls displayed text; an empty template hides text and its background, not
privacy masks. Click `{camera}`, `{date}`, `{time}`, `{computer}` or `{user}`
to insert a token at the caret. Ordinary text and `\n` line breaks are supported.
The **Font** button opens the Windows family/style/size dialog. Choose a corner,
drag the text, or adjust margins, padding and background opacity with sliders.
The background area is calculated from the actual text dimensions.

To hide an area, click **Add** and draw a rectangle on the preview. Select or
move it with the mouse, remove it with **Delete**, or press Esc to cancel a drag.
Up to 32 masks are supported. Both streams receive the same text and masks.
The preview shows draft changes; **Save** applies them and **Cancel** discards them.
Changing output resolution rescales the masks. A draft capture-source change
does not change the preview until the source has restarted.

**Security** enables shared RTSP/ONVIF credentials and an IPv4 allowlist.
Separate addresses with commas or semicolons; an empty list allows all addresses.
The allowlist covers RTSP, ONVIF HTTP and discovery. Localhost is allowed for
watchdog checks but still requires authentication when enabled.

**Status** shows startup phase, uptime, GPU, monitor, encoder, measured FPS,
bitrate, H.264 counters and local RTSP checks. Viewers are grouped by source IP:
one NVR reading both streams is one device and two sessions. Only PLAY sessions
count; internal watchdog sessions do not. Multiple players behind the same IP
cannot be distinguished as separate devices. File logging can be enabled here.

## Applying settings

Language, overlay, masks, cursor/click effects, standby, adaptive FPS/load,
startup and logging apply after Save without replacing the encoder or RTSP sessions.
Camera identity, capture source, network/security and encoding changes require
the offered restart. A declined restart remains pending. When resolution changes
are pending, dependent text/mask coordinates also wait for restart.

## Connect a recorder or player

Default endpoints are:

```text
ONVIF:     http://<PC-LAN-IP>:80/onvif/device_service
Main:      rtsp://<PC-LAN-IP>:554/Streaming/Channels/101
Secondary: rtsp://<PC-LAN-IP>:554/Streaming/Channels/102
```

Existing settings such as port 8000 or path `pc-screen` are preserved on upgrades.
Always use the actual port shown by Screen2NVR. Port 80 is the project's default
ONVIF HTTP endpoint; ONVIF itself does not mandate that port.

On a Hikvision NVR choose **ONVIF**, the PC address, the ONVIF port and, when
enabled, the same username/password. The proprietary **HIKVISION** protocol is
not implemented. Alternatively configure custom RTSP for video only; it does
not obtain the camera's ONVIF name. Refresh the existing channel's camera details
after changing names/profiles; do not delete a channel containing recordings just
to refresh metadata. NVR firmware can retain its own channel name.

ONVIF exposes main `Profile_1` first and secondary `Profile_2` only when enabled.
Both URLs and encoder configurations are separate. Camera names are exposed
through discovery, scopes, hostname, profiles, video configurations and OSD reads.
Remote OSD/configuration writes are not implemented.

In VLC use **Media → Open Network Stream** (Ctrl+N) and copy a URL from Video.
URLs in Settings omit passwords; the player prompts when authentication is enabled.

## Authentication and network safety

RTSP and ONVIF accept Basic and Digest MD5/MD5-sess. ONVIF additionally accepts
WS-Security UsernameToken PasswordText/PasswordDigest. ONVIF advertises Digest
with `qop=auth`; RTSP advertises classic MD5 without qop for camera/NVR compatibility,
while still accepting properly signed qop=auth requests.

Digest checks the password, method, resource, nonce and peer IP. qop=auth checks
increasing nonce counts across reconnects. Classic no-qop MD5 has no nonce-count
replay protection. Nonces expire after five minutes. WS-Security PasswordDigest
also requires client clocks to agree within five minutes and rejects reused tokens.

Limited ONVIF time/discovery/capability requests are available before authentication
so recorders can synchronize and discover services; profiles, stream URLs and
video remain protected. Providing two authentication methods in one request
does not allow an invalid credential to be bypassed by another valid one.

These mechanisms do not encrypt HTTP, RTSP or video. Basic and PasswordText
expose credentials to a network observer. Use a trusted LAN or secured VPN,
not directly exposed Internet ports.

## Files and logging

Configuration is stored in `%ProgramData%\Screen2NVR\config.ini`. Saves use a
Unicode temporary file, flush it, and replace the original with a backup at
`config.ini.bak`. Locks and permission failures preserve the working file and
report the Windows error. A valid backup is loaded if the primary is damaged.
Do not publish either configuration file: they can contain passwords.

For Windows error 5, check read-only attributes and reinstall over the existing
installation to restore data-directory permissions. For error 32/33, close the
program holding the file and retry. Extract the portable ZIP into a permanent
directory before launching or enabling startup. Portable and installed builds
share the same configuration directory.

File logging is **disabled by default**. Enable **Status → Write log to file**,
then Save; restarting is not required. Capture error notifications and automatic
recovery still work while file logging is off. Old logs are not deleted when
logging is disabled. Enable it before a restart to collect encoder startup details.

The active `%ProgramData%\Screen2NVR\logs\Screen2NVR.log` is limited to 1 MiB,
with one rotated `.log.1` backup (about 2 MiB total). There is no per-frame logging.
RTSP stage diagnostics are rate-limited and omit passwords, authorization
headers, nonces and credential-bearing URLs. Sent-packet counters cannot prove
that a remote device actually decoded the stream.

## Compatibility and troubleshooting

Use Windows 10/11 x64 with an active monitor and a working D3D11/WDDM Intel,
AMD or NVIDIA driver. Complete Windows Update on a fresh PC; Microsoft Basic
Display Adapter alone does not guarantee desktop duplication or hardware H.264.
No NVIDIA-specific API is used. Software H.264 fallback increases CPU load.
Windows N editions require the official Media Feature Pack. No separate Visual
C++ Redistributable or legacy DirectX End-User Runtime is required.

Session 0, fully headless PCs and some Remote Desktop sessions do not provide
usable DXGI Desktop Duplication. Hardware encoder selection is tied to the capture
adapter, avoiding incompatible GPU surfaces on hybrid systems. Driver output
format changes are renegotiated instead of producing the old "unexpected encoder
output stream change" failure; unsupported or endlessly changing formats still
produce a diagnostic error and normal recovery.

The watchdog permits 60 seconds for startup and detects a 20-second pipeline/H.264
stall. It displays a reason, logs it if logging is enabled, and replaces the process.
Checks defer while the interactive desktop is unavailable. Every ten seconds
it also opens a local authenticated RTSP session and checks actual received RTP;
the secondary stream is checked separately when enabled.

For old iVMS-4200 gray-screen issues, see [the compatibility investigation](COMPATIBILITY.md).
Version 1.1.0 fixes an invalid AUD/SPS/PPS order without changing credentials,
resolution or encoder policy. Its effect on the reported iVMS 3.6.0.6/NVR combination
requires a field test. Test live view and a **new** recording after upgrading;
already stored archives cannot be rewritten by an application update.

## Silent installation

```bat
Screen2NVR-Setup-x64.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LANG=english /AUTOSTART=1 /RTSPPORT=554 /ONVIFPORT=80
```

Use `/LANG=russian` for Russian and `/AUTOSTART=0` to disable startup. Without
port overrides, existing configured ports are retained.

## Developer checks

From the project directory:

```powershell
.\tools\build-settings-tests.cmd
.\tools\build-rtsp-tests.cmd
.\tools\build-encoder-tests.cmd
.\x64\encoder-tests\EncoderTests.exe
.\x64\encoder-tests\EncoderTests.exe --decode
.\x64\encoder-tests\EncoderTests.exe --decode --software
.\tools\build-logging-tests.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test-security.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test-clients.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test-settings-ui.ps1 -Executable .\x64\settings-tests\SettingsTests.exe -Fixture
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test-dual-stream.ps1 -LegacyDigest -Live
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test-dual-stream.ps1 -Software -ContentBaseDigest
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test-dual-stream.ps1 -Desktop -LegacyDigest
```

UI/persistence fixtures do not save the deployed configuration or change startup.
Security fixtures use separate loopback ports and temporary test credentials.
Dual-stream tests use ONVIF 18002, RTSP 18556 and discovery 13704. Run these
network tests sequentially. The synthetic GPU test checks masks, SPS dimensions,
AUD/header ordering, timestamps, independent SSRCs, I/P frames and live updates.
`--decode` feeds actual encoded synthetic pictures to an independent Windows H.264 decoder.
`-Desktop` requires accessible desktop duplication; a successful synthetic test
does not prove desktop availability or a particular Intel driver's behavior.

Optional security `-Curl` requires a functioning Digest-capable `curl.exe` build;
it is a developer tool, not a runtime dependency. Windows SSPI or system policy
can prevent that client from creating a Digest response. Do not weaken server
authentication merely to make that optional test pass.

`tools\analyze.cmd` runs MSVC static analysis. `tools\test-stream.ps1` can run a
longer synthetic soak with the main application closed; it uses the configured
RTSP port and temporary credentials but does not save them.

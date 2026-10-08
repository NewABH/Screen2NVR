# AGENTS.md

## Project: Screen2NVR

This repository is part of the Screen2NVR project.

The goal is to turn a Windows 10/11 PC desktop into a lightweight IP-camera-like video source that can be recorded by a Hikvision NVR.

Codex should treat this file as the authoritative project context unless the user explicitly changes a decision.

## 0. Current architecture override — 15 August 2026

The user explicitly replaced the earlier multi-process MediaMTX architecture with a strict
single-executable, single-process requirement.

The production application is now:

```text
Screen2NVR.exe
    +-- DXGI / D3D11 desktop capture and GPU processing
    +-- Media Foundation hardware H.264 encoder
    +-- embedded RTSP/RTP server on TCP 554
    +-- embedded ONVIF HTTP service on TCP 80 (configurable)
    +-- embedded WS-Discovery service on UDP 3702
```

MediaMTX, FFmpeg, .NET, ScreenCapture.exe and Screen2ONVIF.exe are no longer separate
production processes. Historical MediaMTX sections below remain as project history only
and do not override this user-approved architecture.

The production application also owns a native Windows tray icon and settings window.
Shared settings/logs live under `%ProgramData%\Screen2NVR`, per-user autostart is controlled
through HKCU Run, and the x64 installer deploys the single EXE under Program Files.

As of October 2026, the application optionally exposes a secondary H.264 stream in the same
process. One composed GPU capture (including masks/text) feeds the main encoder and a GPU
downscale/secondary encoder. RTSP paths, codec state and ONVIF profiles are separate; listening
ports, source and security settings are shared. The secondary stream is disabled by default.

The current settings tabs are Device (Устройство), Video, Overlay, Security and Status.
Device combines the former Camera and Source tabs; both RTSP paths/URLs belong to Video.
ONVIF manufacturer/model/serial come from this PC's SMBIOS Type 2 motherboard information,
read with GetSystemFirmwareTable (no WMI/runtime dependency). Missing/placeholder values use
Unknown; old fixed VendorABH/ModelABH/serial INI fields are ignored, not hardware overrides.
Status counts unique active viewer IPs and separately main/sub RTSP sessions; the tagged
internal loopback watchdog is excluded, but real local players remain counted. Refresh the
statistics outside the encoded-frame callback. Several clients behind one IP count as one device.

Video quality presets change only main FPS/bitrate/GOP, never either stream's resolution.
Resolution dropdowns use the actual capture crop aspect (not necessarily the monitor): main
native size down to width 720; sub widths 720/640/480/320, with nearest even heights. Small
sources are not upscaled. CaptureGeometry.h shares window matching/clipping with the GPU crop.
Preserve existing off-list resolutions on opening settings, labelled as previously chosen;
explicit source/resolution changes select from the new aspect-correct options. Do not clamp
ultrawide sub heights back to the old 120-pixel minimum. Keep mask scaling/restart boundaries.

File logging is opt-in: AppSettings.loggingEnabled / [Logging] Enabled defaults to false,
including old INIs without this key. The Status tab checkbox applies after Save without
restarting capture; configure the shared logger directly even if capture is stalled/paused.
While disabled, do not create, append, rotate or migrate log files, and close any open log
handle. Retain existing files. Error notifications/watchdog recovery remain independent.
Enabled logging retains UTF-8 and the existing 1 MiB + one backup limit. Console output in
diagnostic test executables is separate from file logging. Preserve credential redaction.

The current defaults requested by the user are ONVIF TCP 80, RTSP TCP 554, main path
`Streaming/Channels/101`, and sub path `Streaming/Channels/102`. Existing saved ports/paths
are preserved on upgrade; nested RTSP paths must be accepted by the settings UI.
RTSP Digest accepts the stream base URI for SETUP of its own `trackID=0`, as used by some
camera/NVR clients, but must still reject another host/port/stream and verify the password.
RTSP challenges deliberately offer classic MD5 without `qop`: the NVR log from 7 October 2026
shows repeated DESCRIBE failures in the qop/nc/cnonce exchange, before SETUP. ONVIF HTTP
challenges still advertise `qop=auth`. Never bypass password/nonce validation to accommodate
a malformed response. Test challenge negotiation, not just a forced legacy response.
The next NVR log (`Screen2NVR (3).log`) confirms authenticated DESCRIBE and SETUP now succeed,
but PLAY fails with `Digest URI mismatch`; the original URIs are not present in that log.
Digest URI comparison now treats a stream's single trailing slash consistently with the RTSP
router, including PLAY/keepalive/TEARDOWN. Hash the exact signed URI, and do not generalize
this to other streams, track URLs, queries, different hosts/ports, or HTTP request paths.
The user subsequently confirmed Hikvision streaming with authentication works. Preserve this
Digest negotiation and URI compatibility in unrelated UI/identity changes.
Connection-stage diagnostics are bounded and must never contain Authorization, passwords,
Digest responses, nonces, or raw credential-bearing URLs.

Release 1.1.0 introduces Russian/English UI via Localization.h and [General] Language
(0=Russian, 1=English). Default follows Windows UI language. The installer offers both
languages and applies its selection using --set-language=ru/en. Switching UI language is
live, not a reason to restart capture. Preserve camera names, templates and user-authored
standby text; translate only the known built-in standby text. Rebuild localized controls
without losing the selected tab or saved/draft state. Keep documentation in both languages.

The user reports iVMS-4200 3.6.0.6 gray live/remote archives, while 3.13.0.5 and exported
recordings work. Published Hikvision notes document player/playctrl.dll updates but do not
identify the exact trigger. Do not claim field compatibility with old iVMS until checked.
The RTSP publisher previously sent SPS,PPS,AUD,SPS,PPS,IDR. H.264 requires an existing AUD
to be first. OrderAccessUnit now emits AUD first and supplies cached SPS/PPS only when
missing in an IDR; preserve original distinct parameter sets and coded picture bytes.
Keep last-packet markers, sample-derived 90 kHz timestamps and Digest behavior unchanged.
Do not add another allocation to every predicted picture just for ordering. Regression
tests are tools/build-rtsp-tests.cmd, dual-stream RTP timing/order checks, and independent
Windows H.264 decoding in EncoderTests --decode [--software]. See COMPATIBILITY.md.

---

## 1. Primary goal

Build a native Windows application that:

- captures the interactive user's desktop;
- uses GPU-native Windows APIs where possible;
- minimizes CPU usage;
- encodes the desktop as H.264;
- publishes the video into MediaMTX;
- exposes the stream to Hikvision as:

```text
rtsp://<PC-LAN-IP>:554/pc-screen
```

The final solution should be lightweight, reliable, easy to deploy, and suitable for unattended use on many Windows PCs.

---

## 2. Target systems

Main deployment targets:

- Windows 10 x64
- Windows 11 x64
- Intel Core i3 9th-generation-class PCs
- Intel integrated graphics, typically Intel UHD Graphics
- no discrete GPU should be assumed
- desktops are normally 1920x1080, but higher-resolution desktops may exist
- application must run in the logged-in interactive user session for screen capture

Development may happen on a more powerful PC with an NVIDIA GPU, but do not design the solution around NVIDIA-specific APIs.

---

## 3. Current high-level architecture

The intended final video path is:

```text
Windows desktop
    |
    v
DXGI Desktop Duplication
    |
    v
D3D11 GPU processing
    |
    +-- resize to 1920x1080 if necessary
    +-- pixel-format conversion to NV12
    +-- overlay local date/time
    |
    v
Media Foundation H.264 encoder
    |
    v
H.264 NAL units
    |
    v
RTP / UDP publisher
    |
    v
MediaMTX
    |
    v
RTSP
    |
    v
Hikvision NVR
```

FFmpeg is NOT part of the desired final ScreenCapture implementation.

MediaMTX remains responsible for RTSP serving and client handling.

---

## 4. Why FFmpeg is being replaced

An earlier working implementation used:

```text
gdigrab -> FFmpeg -> h264_qsv -> RTSP -> MediaMTX
```

Example of the previously working FFmpeg configuration:

```text
-f gdigrab
-framerate 12
-video_size 1920x1080
-i desktop
-vf drawtext + scale + format=nv12
-c:v h264_qsv
-profile:v baseline
-level 4.0
-b:v 2000k
-maxrate 4000k
-bufsize 8000k
-g 25
-an
-f rtsp
-rtsp_transport tcp
rtsp://127.0.0.1:554/pc-screen
```

It worked, including Intel Quick Sync encoding, but the project is moving away from FFmpeg because:

- GDI desktop capture consumes more CPU than desired;
- FFmpeg is a large dependency;
- the project should use native Windows GPU APIs;
- DXGI Desktop Duplication testing already showed substantially lower capture overhead.

Do not reintroduce FFmpeg unless the user explicitly asks for it.

---

## 5. Screen capture API decision

Use:

```text
DXGI Desktop Duplication
IDXGIOutput1::DuplicateOutput
IDXGIOutputDuplication::AcquireNextFrame
```

Do not use Xbox Game Bar / Win+Alt+R as the capture implementation.

Windows.Graphics.Capture was considered, but DXGI Desktop Duplication is preferred for this project because the desired behavior is continuous, unattended desktop capture without capture UI or visible border requirements.

Important requirements:

- D3D11 device must be created on the same adapter as the selected DXGI output.
- The capture code must handle `DXGI_ERROR_WAIT_TIMEOUT`.
- The final implementation must recover from `DXGI_ERROR_ACCESS_LOST`.
- Screen capture must run in the interactive user's session.
- Do not expect ordinary Session 0 Windows services to capture the logged-in user's desktop reliably.

---

## 6. Current ScreenCapture development status

The current ScreenCapture phase is native C++.

A minimal DXGI capture test has been prepared.

Its responsibilities are currently only:

1. enumerate DXGI adapters;
2. enumerate outputs;
3. choose the first active monitor;
4. create a D3D11 device on that monitor's adapter;
5. create `IDXGIOutputDuplication`;
6. call `AcquireNextFrame`;
7. inspect the returned `ID3D11Texture2D`;
8. print frame dimensions and DXGI format.

No encoding, RTP, Media Foundation, timestamp rendering, or MediaMTX publishing should be added until this DXGI capture stage is confirmed working, unless the user explicitly requests moving ahead.

Expected current output resembles:

```text
ScreenCapture DXGI test

Adapter 0: Intel(R) UHD Graphics 630
  Output 0: \\.\DISPLAY1 [ACTIVE]

Using adapter 0, output 0
D3D11 device created.

Capture started:
1920x1080
DXGI format: 87

Waiting for frames. Press Ctrl+C to stop.
```

Potential formats already observed during earlier experiments:

- DXGI format 87 = `DXGI_FORMAT_B8G8R8A8_UNORM`
- DXGI format 10 = `DXGI_FORMAT_R16G16B16A16_FLOAT` on HDR configurations

The final implementation must cope with realistic source formats rather than assuming one hard-coded desktop format.

---

## 7. Planned ScreenCapture implementation stages

Proceed incrementally.

### Stage 1 — DXGI capture

Already the current task.

Verify:

- selected adapter;
- selected output;
- desktop resolution;
- desktop texture format;
- stable frame acquisition.

### Stage 2 — GPU resize

Use D3D11 processing.

Target output:

```text
1920x1080
```

Avoid CPU readback for routine frame processing.

### Stage 3 — NV12 conversion

Convert the desktop GPU texture into an NV12 GPU surface suitable for H.264 encoding.

Prefer a D3D11 video processor or another native GPU path.

Avoid converting a full HD frame through CPU memory every frame.

### Stage 4 — timestamp overlay

Overlay local date and time on the video.

Equivalent appearance goal to the old FFmpeg overlay:

```text
DD.MM.YYYY HH:MM:SS
```

Near the top-left corner, with readable contrast.

Prefer GPU rendering.

Do not make the timestamp implementation force a full GPU-to-CPU-to-GPU round trip.

### Stage 5 — Media Foundation H.264

Use Media Foundation.

Target stream parameters:

```text
Codec:       H.264 / AVC
Resolution:  1920x1080
Frame rate:  12 fps
Bitrate:     about 2 Mbps
GOP:         25
Audio:       none
```

Hardware encoding is strongly preferred.

The main target is Intel integrated graphics.

Prefer hardware MFTs when available, while keeping a clear fallback/error path.

Do not silently fall back to an unexpectedly high-CPU software encoder without reporting it.

### Stage 6 — verify hardware encoder

At startup, log enough information to know whether a hardware H.264 encoder was selected.

The user cares about low CPU utilization.

### Stage 7 — RTP/H.264 publisher

Implement the minimum H.264 RTP packetizer required to feed MediaMTX.

Required concepts include:

- 90 kHz RTP clock;
- RTP sequence numbers;
- RTP timestamps;
- marker bit on the last packet of an access unit;
- single-NAL packets when small enough;
- FU-A fragmentation for large NAL units;
- correct handling of SPS/PPS and IDR access units.

Keep this module small and isolated.

### Stage 8 — MediaMTX integration

MediaMTX should receive the native application's stream and expose:

```text
rtsp://<PC-LAN-IP>:554/pc-screen
```

Hikvision connects to MediaMTX, not directly to ScreenCapture.

### Stage 9 — production robustness

Add:

- automatic recreation after `DXGI_ERROR_ACCESS_LOST`;
- restart/recovery if GPU device changes;
- graceful handling of monitor mode changes;
- logging;
- configuration file;
- hidden/background startup;
- protection against launching duplicate capture instances.

---

## 8. MediaMTX

Deployment directory is currently intended to be:

```text
C:\screen2nvr\
```

Typical contents:

```text
C:\screen2nvr\
    mediamtx.exe
    nssm.exe
    ScreenCapture.exe
    Screen2ONVIF.exe
    mediamtx.yml
    camera.ini
    logs\
```

Earlier versions also contained `ffmpeg.exe`, but it should disappear from the final native ScreenCapture solution.

MediaMTX is currently run as a Windows service through NSSM.

Typical service setup:

```bat
nssm install MediaMTX C:\screen2nvr\mediamtx.exe
nssm set MediaMTX AppDirectory C:\screen2nvr
nssm set MediaMTX AppParameters C:\screen2nvr\mediamtx.yml
nssm set MediaMTX AppStdout C:\screen2nvr\logs\mediamtx.out.log
nssm set MediaMTX AppStderr C:\screen2nvr\logs\mediamtx.err.log
nssm set MediaMTX Start SERVICE_AUTO_START

sc failure MediaMTX reset= 0 actions= restart/5000
sc description MediaMTX "MediaMTX RTSP streaming server"
net start MediaMTX
```

RTSP firewall rule:

```bat
netsh advfirewall firewall add rule name="MediaMTX RTSP streaming server" dir=in action=allow protocol=TCP localport=554 profile=any
```

The exact MediaMTX RTP ingest configuration should be implemented only after ScreenCapture is successfully producing valid H.264/RTP.

---

## 9. Previous duplicate-publisher problem

Earlier MediaMTX logs showed messages such as:

```text
closing existing publisher
```

This happened because more than one FFmpeg process was publishing to the same path:

```text
pc-screen
```

This was not a scaling problem.

The final application must prevent multiple simultaneous ScreenCapture instances from publishing the same stream.

Preferred solution:

- use a named Windows mutex at process startup;
- if another instance is already running, log the condition and exit.

---

## 10. ScreenCapture startup model

ScreenCapture must run in the interactive user's Windows session.

Earlier FFmpeg startup used the common Startup folder with a hidden VBS launcher.

A similar approach is acceptable initially for ScreenCapture.

Later, compile ScreenCapture as a Windows GUI-subsystem application if appropriate so it has no console window.

Do NOT convert desktop capture into a normal Session 0 service unless a session-aware architecture is specifically designed.

MediaMTX and ONVIF can run independently of the interactive session.

---

## 11. ONVIF component

There is a separate project named:

```text
Screen2ONVIF
```

Its purpose is to make the PC stream appear to Hikvision as an ONVIF network video device.

It is independent from ScreenCapture and should not be mixed into the capture implementation yet.

Current intended behavior:

- reads `C:\screen2nvr\camera.ini`;
- determines the PC's LAN IPv4;
- listens for WS-Discovery on UDP 3702;
- exposes ONVIF SOAP over TCP port 8000;
- advertises an RTSP URI:

```text
rtsp://<PC-LAN-IP>:554/pc-screen
```

ONVIF firewall rules:

```bat
netsh advfirewall firewall add rule name="Screen2ONVIF HTTP" dir=in action=allow protocol=TCP localport=8000 profile=any

netsh advfirewall firewall add rule name="Screen2ONVIF Discovery" dir=in action=allow protocol=UDP localport=3702 profile=any
```

A first C# implementation was created but produced a roughly 65 MB self-contained executable because it bundled the .NET runtime.

The decision was therefore made to create the ONVIF emulator in native C++ instead.

Do not return to .NET for this project unless explicitly requested.

---

## 12. ONVIF configuration

Current intended `camera.ini` structure:

```ini
[Camera]
Name=Касса №5
Manufacturer=Screen2NVR
Model=PC Screen Camera
Serial=SCREEN2NVR-001

[Network]
OnvifPort=8000
RtspPort=554
RtspPath=pc-screen
```

The stream is always published locally into MediaMTX, but ONVIF must return a LAN-reachable URI using the PC's actual network address:

```text
rtsp://<LAN-IP>:554/pc-screen
```

Never advertise:

```text
rtsp://127.0.0.1:554/pc-screen
```

to Hikvision.

---

## 13. ONVIF methods already considered

The minimal emulator is expected to support at least the subset commonly requested by an NVR:

```text
GetServices
GetDeviceInformation
GetCapabilities
GetProfiles
GetStreamUri
GetScopes
GetHostname
GetSystemDateAndTime
GetVideoSources
GetVideoSourceConfigurations
GetVideoEncoderConfigurations
GetVideoEncoderConfigurationOptions
```

The ONVIF implementation is not ONVIF-certified.

Compatibility must be tested against the actual Hikvision NVR.

Do not assume that one successful SOAP request means full NVR compatibility.

---

## 14. ONVIF naming caveat

The user wants Hikvision to discover the device with a meaningful camera name.

The ONVIF emulator should advertise the configured name in relevant scopes/profile metadata.

However, do not promise that every Hikvision model will use that value as the displayed channel name. Some Hikvision interfaces maintain their own local channel naming.

Treat this as something to verify experimentally.

---

## 15. Development environment

The user wants to work primarily in Visual Studio Code.

Native C++ compilation uses the Microsoft C++ toolchain from Visual Studio 2022 / Build Tools and the Windows SDK.

Preferred way to launch VS Code:

```bat
x64 Native Tools Command Prompt for VS 2022
cd /d D:\project1\ScreenCapture
code .
```

Verify MSVC:

```bat
cl
```

For a simple one-file build, an example command is:

```bat
cl /std:c++17 /O2 /EHsc ScreenCapture.cpp /Fe:ScreenCapture.exe d3d11.lib dxgi.lib
```

This is sufficient for early experiments.

As the project grows, migrate to CMake rather than relying on a large manual `cl` command.

Preferred eventual layout:

```text
ScreenCapture\
    AGENTS.md
    CMakeLists.txt
    src\
        main.cpp
        Capture.cpp
        Capture.h
        VideoProcessor.cpp
        VideoProcessor.h
        H264Encoder.cpp
        H264Encoder.h
        RtpSender.cpp
        RtpSender.h
        Config.cpp
        Config.h
```

Do not split files prematurely while the current subsystem is still being debugged.

---

## 16. Windows libraries likely required later

ScreenCapture may eventually need:

```text
d3d11.lib
dxgi.lib
dxguid.lib
mfplat.lib
mf.lib
mfuuid.lib
mfreadwrite.lib
wmcodecdspuuid.lib
ole32.lib
ws2_32.lib
```

Only add libraries when the implementation actually needs them.

Avoid unnecessary third-party dependencies.

---

## 17. Important performance rule

The central optimization rule is:

> Keep video frames on the GPU for as much of the pipeline as possible.

Avoid this:

```text
GPU desktop texture
 -> CPU RAM
 -> CPU conversion
 -> CPU overlay
 -> GPU upload
 -> encoder
```

Prefer:

```text
GPU desktop texture
 -> GPU resize/conversion
 -> GPU overlay
 -> hardware encoder
```

CPU work should mostly involve:

- control logic;
- timestamp text updates;
- encoded H.264 packet handling;
- RTP packetization;
- network transmission.

---

## 18. Frame-rate behavior

Target output is 12 fps.

Desktop Duplication may not return a new frame when nothing on screen changes.

The final stream still needs valid timing behavior for an NVR.

Design the capture/encoder loop so that:

- desktop changes are acquired efficiently;
- the last available frame can be reused when required;
- encoder timestamps progress monotonically;
- output timing remains approximately 12 fps.

Do not create a busy loop that consumes CPU when the screen is idle.

---

## 19. HDR and pixel formats

Earlier testing showed that HDR desktops may produce:

```text
DXGI_FORMAT_R16G16B16A16_FLOAT
```

while SDR desktops may commonly produce:

```text
DXGI_FORMAT_B8G8R8A8_UNORM
```

Do not assume BGRA8 in every environment.

If HDR support is not implemented initially, detect the source format and produce a clear error/log message rather than corrupt video.

A later implementation may tone-map HDR to SDR before NV12/H.264.

---

## 20. Timestamp requirement

The previous FFmpeg stream displayed local date and time.

Equivalent formatting:

```text
DD.MM.YYYY HH:MM:SS
```

Example:

```text
14.08.2026 23:46:02
```

The final native implementation should preserve this functionality.

Preferred placement:

- near top-left;
- approximately 10 px margin;
- readable white text;
- dark/transparent background or another readable contrast method.

Exact appearance is secondary to low overhead and readability.

---

## 21. Stream compatibility target

The previous working Hikvision-compatible H.264 settings were approximately:

```text
Resolution: 1920x1080
FPS:        12
Codec:      H.264
Profile:    Baseline
Level:      4.0
Bitrate:    2000 kbps
Maxrate:    4000 kbps
Buffer:     8000 kb
GOP:        25
Audio:      disabled
```

These values are a compatibility reference, not an absolute requirement.

When using Media Foundation, choose the closest sensible settings and verify Hikvision playback.

Do not sacrifice hardware encoding just to reproduce every FFmpeg flag literally.

---

## 22. Networking

MediaMTX currently serves RTSP on:

```text
TCP 554
```

The ONVIF service uses:

```text
TCP 8000
UDP 3702
```

The native RTP sender should use a fixed or configurable local UDP port chosen to match the MediaMTX RTP ingest configuration.

Do not hard-code unexplained ports in multiple files. Keep network settings centralized once configuration support is added.

---

## 23. Local IP selection

The PC may have multiple adapters:

- Ethernet;
- Wi-Fi;
- VPN;
- virtual adapters;
- Hyper-V;
- Docker;
- loopback-like interfaces.

Do not blindly pick the first IPv4 address for ONVIF advertisement.

The selected address should ideally correspond to the route/network that can reach the Hikvision NVR.

For early tests, route-based LAN IP detection is acceptable.

Later add an optional configured interface/IP override.

---

## 24. Logging

Production applications should log to:

```text
C:\screen2nvr\logs\
```

Useful ScreenCapture startup information:

```text
application version
selected adapter
selected GPU
selected monitor
desktop resolution
desktop DXGI format
output resolution
encoder name
hardware/software encoder status
bitrate
frame rate
RTP destination
MediaMTX target
```

Log errors with HRESULT values when applicable.

For HRESULT errors, print hexadecimal values.

---

## 25. Background execution

The final ScreenCapture application should have no visible interface during normal operation.

During development, a console build is useful for diagnostics.

For production, either:

- build with `/SUBSYSTEM:WINDOWS`; or
- use another clean hidden-start method.

Do not remove diagnostics until logging exists.

Screen2ONVIF can also run without a visible console.

---

## 26. Deployment philosophy

The user prefers:

- minimal resource consumption;
- few moving parts;
- no unnecessary runtime installations;
- native Windows binaries where practical;
- simple installation;
- predictable paths;
- full-file solutions rather than many small manual search-and-replace edits.

When changing code, prefer to provide or apply complete coherent files when practical.

Do not over-engineer a subsystem before the previous stage works.

---

## 27. Installer history

Earlier deployment used a WinRAR SFX installer.

Important cleanup lesson:

Before replacing files under:

```text
C:\screen2nvr
```

stop processes/services that may lock:

```text
ffmpeg.exe
mediamtx.exe
nssm.exe
ScreenCapture.exe
Screen2ONVIF.exe
```

Do not perform directory deletion while the command shell's current working directory is inside the directory being removed.

Change to a safe directory first, for example:

```bat
cd /d C:\
```

Do not indiscriminately kill all `wscript.exe` processes on a machine unless explicitly acceptable.

---

## 28. MediaMTX warning already seen

A MediaMTX message similar to:

```text
RTP packets are too big (1460 > 1440), remuxing...
```

was observed previously.

It was not the root cause of the old broken-pipe issue.

The actual root cause was duplicate publishers on the same path.

For the new native RTP packetizer, keep UDP/RTP packets within a safe MTU-sized payload to avoid unnecessary remuxing or fragmentation.

A conservative RTP packet size is preferred.

---

## 29. Threading expectations

Potential final threading model:

```text
Capture thread
    |
    v
GPU processing / encode pipeline
    |
    v
Encoded-frame queue
    |
    v
RTP/network thread
```

But begin simpler if Media Foundation naturally supports an efficient synchronous pipeline.

Avoid unbounded queues.

If the network falls behind, dropping old screen frames is usually preferable to growing latency indefinitely.

Low latency matters more than preserving every desktop frame.

---

## 30. Reliability expectations

This is intended for continuous operation.

Eventually handle:

- user lock/unlock;
- display sleep/wake;
- display mode change;
- GPU reset;
- Desktop Duplication access loss;
- MediaMTX restart;
- network interruptions;
- duplicate process launch.

The application should recover automatically where feasible rather than permanently exiting after a transient event.

During early development, errors may still exit the test executable, but record what production recovery will be required.

---

## 31. Security / privacy assumptions

This application captures the full user desktop.

Do not add remote-control functionality.

Do not add arbitrary remote command execution.

Do not expose additional HTTP endpoints unless needed for the project.

ONVIF currently exists only to identify/access the video source from the NVR.

---

## 32. Current immediate task for Codex

Unless the user explicitly says otherwise, the next action is:

1. inspect the existing `ScreenCapture` source;
2. build it with MSVC x64 Release;
3. fix compile errors if present;
4. run the DXGI capture test in the interactive desktop session;
5. report:
   - GPU adapter;
   - monitor/output;
   - capture resolution;
   - DXGI texture format;
   - any HRESULT errors;
6. do NOT yet implement Media Foundation or RTP until DXGI capture is confirmed.

If DXGI capture is already confirmed in the local repository, continue to Stage 2: GPU resize to 1920x1080.

---

## 33. Rules for Codex changes

When working in this repository:

1. Read this file before making architectural changes.
2. Inspect the existing code before rewriting it.
3. Build after meaningful code changes whenever the local environment allows it.
4. Do not claim compilation success without actually compiling when command execution is available.
5. Preserve x64 compatibility.
6. Prefer C++17 or newer if needed.
7. Use RAII and `Microsoft::WRL::ComPtr` for COM/D3D resources where appropriate.
8. Check every important HRESULT.
9. Keep the hot frame path free of unnecessary allocations.
10. Avoid per-frame creation/destruction of expensive D3D or Media Foundation objects.
11. Do not add third-party packages without discussing why they are necessary.
12. Do not replace MediaMTX with a custom RTSP server unless explicitly requested.
13. Do not reintroduce FFmpeg unless explicitly requested.
14. Do not merge Screen2ONVIF into ScreenCapture yet unless explicitly requested.
15. Prefer a working, measurable increment over a large speculative rewrite.

---

## 34. Success criteria for the final system

The project is successful when a deployment PC can:

```text
boot Windows
 -> MediaMTX starts
 -> user logs in
 -> ScreenCapture starts silently
 -> desktop is captured through DXGI
 -> processing/encoding is primarily GPU accelerated
 -> MediaMTX receives the stream
 -> Hikvision records rtsp://<PC-IP>:554/pc-screen
 -> ONVIF discovery presents the configured camera identity
```

while:

- CPU usage remains low;
- no FFmpeg process exists;
- no visible capture window appears;
- the stream survives normal desktop changes and long-running use.

---

## 35. Communication preference

When helping the user:

- respond in Russian unless asked otherwise;
- be direct;
- avoid unnecessary theory when a concrete next step is known;
- when providing code changes, prefer complete files over fragments when practical;
- do not ask the user to manually patch many scattered lines if a full corrected file can be supplied;
- explain build/run commands exactly;
- proceed one testable stage at a time.

# Screen2NVR

[Русский](README.ru.md) · [Releases](https://github.com/NewABH/Screen2NVR/releases)

Screen2NVR turns a Windows 10/11 desktop into a network H.264 camera for ONVIF or
RTSP recorders, including Hikvision NVRs.

All capture, encoding, RTSP, ONVIF, discovery and tray controls run in one native
`Screen2NVR.exe`. No FFmpeg, MediaMTX, .NET runtime or separate service is required.

This project was developed by the OpenAI Codex AI assistant from the user's
requirements. The user made product and architectural decisions and tested the
application on actual computers and a Hikvision recorder. See
[AI_DEVELOPMENT.md](AI_DEVELOPMENT.md).

## Features

- DXGI capture of a monitor, rectangular area, named window or active window.
- D3D11 scaling, rotation and HDR processing.
- Media Foundation hardware H.264 encoding with an explicitly permitted software fallback.
- Main and optional secondary RTSP streams with independent resolution, FPS, bitrate and GOP.
- Embedded RTSP/RTP, ONVIF and WS-Discovery servers.
- Hikvision-compatible RTSP Digest authentication and an IP allowlist.
- Text overlays, privacy masks and soft highlights for individual mouse clicks.
- Tray controls, Russian/English settings, optional sign-in startup and a stream watchdog.
- Optional bounded file logging, disabled by default.

Default endpoints:

```text
ONVIF:         http://<IP>:80/onvif/device_service
Main RTSP:     rtsp://<IP>:554/Streaming/Channels/101
Secondary:     rtsp://<IP>:554/Streaming/Channels/102
```

Existing custom ports and paths are preserved when upgrading.

## Install and use

Download `Screen2NVR-Setup-x64.exe` from
[Releases](https://github.com/NewABH/Screen2NVR/releases). Select English or Russian
at the start of installation. Setup installs into `Program Files`, adds an
inbound firewall rule and offers automatic startup when the current user signs in.

Right-click the tray icon to open **Settings**, or double-click it. The **Device**
tab also lets you change the interface language; this does not interrupt the stream.
Language selection does not translate your camera name or custom overlay text.

Settings and optional logs are stored in `%ProgramData%\Screen2NVR`.
For configuration, NVR setup, compatibility limits and troubleshooting, read
[the English guide](DEPLOYMENT.en.md) or [the Russian guide](DEPLOYMENT.md).
Changes in each release are documented in [CHANGELOG.md](CHANGELOG.md).

## Build

Use Windows 10/11 x64, Visual Studio 2022 Build Tools with **Desktop development
with C++**, and the Windows SDK.

```bat
build-release.cmd
```

Output: `x64\Release\Screen2NVR.exe`. To build the installer, also install
Inno Setup 6, then run:

```bat
build-installer.cmd
```

CMake is an alternative: `cmake -S . -B build -A x64`, then
`cmake --build build --config Release`.

## Tests

```bat
tools\build-settings-tests.cmd
tools\build-rtsp-tests.cmd
tools\build-encoder-tests.cmd
tools\build-logging-tests.cmd
```

The [English deployment guide](DEPLOYMENT.en.md#developer-checks) lists UI,
decoder, network and dual-stream checks.

## Compatibility and security

Intel integrated graphics is a primary target. No NVIDIA-specific API is used.
A working D3D11/WDDM graphics driver and an interactive desktop session are required.
Windows N editions need the official Windows Media Feature Pack. A separate
Visual C++ Redistributable is not needed: the Release build uses the static runtime.

Test the specific GPU driver, NVR firmware and viewing client before deployment.
The 1.1.0 release fixes a malformed H.264 access-unit order; compatibility with
iVMS-4200 3.6.0.6 through the affected NVR still needs an on-device check.

The stream exposes your desktop. Use authentication and an IP allowlist on a
trusted LAN; RTSP and HTTP traffic are not encrypted. Do not publish
`%ProgramData%\Screen2NVR\config.ini`: it may contain the camera password.

## Attribution and license

The icon is based on Google Material Symbols; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
The repository owner has not yet specified a source-code license.

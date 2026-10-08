# Changelog

## 1.1.0

- English and Russian application UI, selectable on Device and applied without
  interrupting streaming. Settings, tray menus, notifications and dynamic status
  messages are localized; user-authored text is preserved.
- Bilingual installer with an initial language selector; the chosen language is
  applied to the application. Upgrades preserve other settings.
- English project overview and deployment guide, with the Russian documentation retained.
- Corrected H.264 RTP access-unit ordering: AUD is first; cached SPS/PPS are added
  only when omitted by the encoder. Main and secondary streams receive the fix.
- Added packetization, actual H.264 decoding, RTP timing and language-switch tests.

The RTP correction targets stricter/older clients. Compatibility with the reported
iVMS-4200 3.6.0.6/NVR combination has not yet been confirmed on that equipment.
See [COMPATIBILITY.md](COMPATIBILITY.md) for evidence and test limits.

## 1.0.0

Initial public release: integrated desktop capture, GPU H.264, main/sub RTSP,
ONVIF discovery, Hikvision Digest compatibility, tray settings, overlays and
privacy masks, startup support, watchdog and optional bounded logging.

# iVMS-4200 compatibility investigation

## Observations

The user reported gray live/remote-playback images in iVMS-4200 **3.6.0.6**, while
the same NVR's web interface showed the desktop. Updating iVMS to **3.13.0.5**
made the picture appear. Downloaded recordings also played correctly. Hik-Connect
remote archive playback was reported to run unusually fast.

These observations point toward a client decoder, demultiplexer or remote-stream
handling difference; they do not establish a specific cause. Exported files can
use a different container and timing path than remote playback.

## What the published changelogs establish

- [Hikvision iVMS 3.8.0 release notes](https://us-legacy.hikvision.com/en/system/files_force/manual/ivms-4200_client_software_release_notes_v3.8.0_external_20220706.pdf?download=1)
  identify Player 7.4.0.58. Resolved issues 9 and 10 concern unresponsive multi-window
  live view, including hardware decoding. Their documented solution is a
  `playctrl.dll` update; issue 8 also fixes a decoding channel left running after exit.
- [Hikvision iVMS 3.10.0 release notes](https://www.hikvision.com/content/dam/hikvision/en/support/download/vms/ivms4200-series/software-download/v3-10-0-5_e/Release-Notes-for-iVMS-4200.pdf)
  list a disappearing playback picture when dragging the timeline, accompanied
  by an incorrect-calling-order error (issue 5). This documents a playback-path
  correction, not a disclosed encoder/SPS change.
- [Hikvision-authored 3.11.0 notes, mirrored text](https://studylib.net/doc/27432833/ivms-4200-client-software-release-notes-v3.11.0-20240126)
  identify Player 7.4.1.67 and playback search/download improvements; their
  resolved-issues list does not specify the reported H.264 gray-screen problem.
- [Hikvision-authored 3.12.0 notes, mirrored document](https://fr.scribd.com/document/796000009/IVMS-4200-Client-Software-Release-Notes-V3-12-0-20240913)
  identify Player 7.4.2.58. Listed fixes concern visitor permissions and discovery
  lag, without details of a matching codec correction.
- [Hikvision-authored 3.13.0 release notes, mirrored PDF](https://smartsystemvideo.ru/Files/HiWatch/iVMS-4200-Client-Software-V3.13.0-Release-Notes_20250526-1.pdf)
  identify Player 7.4.2.62 and Network SDK 6.1.10.5. Their resolved-issues section
  does not disclose a matching H.264 gray-screen correction or the bitstream
  condition triggering it.

A release-note document specifically identifying build 3.6.0.6 was not located;
neither was a usable 3.9.0 note in this search. Some Hikvision-hosted documents
reject direct fetching; the 3.10 issue above was available in the indexed vendor PDF.
The available notes do not provide a complete codec-fix history for every build
between 3.6 and 3.13. It would therefore be incorrect to claim that a particular
vendor change explains the user's case.

## Confirmed Screen2NVR defect and correction

Before 1.1.0, an IDR sample containing its own AUD/SPS/PPS was transmitted as:

```text
SPS → PPS → AUD → SPS → PPS → IDR
```

The server unconditionally inserted cached parameter sets before the encoder's
original access unit. This duplicated headers and placed the access-unit delimiter
after other NALs. [H.264 section 7.4.1.2.3](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-H.264-201610-S%21%21PDF-E&lang=s&type=items)
requires an AUD, when present, to be first in its access unit.

Version 1.1.0 transmits an existing AUD first and only supplies cached SPS/PPS
when that kind is missing from the IDR. Existing distinct parameter sets and
coded picture bytes are preserved. The observed NVIDIA and software streams now use:

```text
AUD → SPS → PPS → IDR
```

RTP timestamps, the last-packet marker, authentication, encoder policy and
configured stream dimensions are not altered by this fix. There is no new codec
dependency or architectural change. This is a standards-conformance improvement,
not yet a demonstrated explanation of the old client's gray screen.

## Verification and remaining limits

Local tests cover missing/complete headers, Annex-B and length-prefixed samples,
preservation of distinct SPS records, both hardware NVIDIA and Microsoft software
encoding, main/sub RTP ordering, Digest-authenticated streaming, monotonic
timestamps and elapsed-time agreement. Independent Windows decoding of 48
synthetic pictures per encoder succeeded without gray/frozen output.

The exact iVMS 3.6.0.6 decoder and the affected NVR are not available in the test
environment. The desktop test in this session could not obtain its first DXGI
frame; synthetic GPU tests are not a substitute for real desktop/Intel checks.
The optional Windows SSPI curl Digest check also failed locally before a usable
authenticated exchange; the native protocol/security tests passed.

After installing 1.1.0, reconnect the existing NVR channel and test main/sub live
view and a newly recorded interval in old iVMS. Do not delete existing archives.
Stored footage is unaffected by this change. If the problem persists, a short
untranscoded export can still identify actual SPS/PPS and timestamps even when
the exported file itself plays normally; a client/NVR diagnostic comparison would
then be needed before changing unrelated encoder parameters.

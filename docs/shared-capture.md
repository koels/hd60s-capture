# Shared capture — version 0.4.1

## What works

HD60 S Capture and the **HD60 S Native** OBS source can receive the
same card simultaneously. Each receives raw YUY2 video and stereo PCM with the
same capture timestamps. Their display settings and gain are independent.
Pausing or closing one consumer does not stop another consumer's capture.

The native app now starts preview automatically. **Play capture audio (−6 dB)**
is enabled by default and remembers the user's choice. Audio goes to the Mac's
current default playback device. Keep OBS speaker monitoring off when using the
app for playback to avoid hearing the stream twice; OBS still receives audio
for recording/streaming.

This does not add a system camera or microphone. Discord, QuickTime and other
unmodified apps do not gain direct HD60 S device enumeration. They can capture
this app's window, or a separate integration can consume the shared client API.
Older direct-USB plugins and Elgato's original software do not participate in
this shared service and may still contend for USB ownership.

## Architecture

- `src/service.c` is the sole USB owner. It reuses `src/capture.c` for device
  initialization, transfer reception, parsing, status and reconnect handling.
- `src/shared-client.c` is linked into the app and OBS plugin. Either consumer
  can prepare and start the capture service without the other app installed.
  It copies its bundled helper to a private versioned cache and uses a per-user
  launchd job, then consumes local IPC.
- IPC lives in a private directory `/tmp/hd60s-<uid>` (0700), with a 0600
  memory-mapped file. File type, owner, permissions, version and size are checked.
  No network port is opened. Other processes under the same user account can
  participate; this is not a boundary against malicious code running as that user.
- A service lock prevents duplicate USB owners. Up to 32 consumer leases are
  supported; stale/dead leases are reclaimed. Readers keep independent cursors.
- Three video buffers avoid overwriting the current published/read-held buffer.
  Slow readers can skip frames; they do not block the USB thread.
- A ring of 128 bounded PCM blocks preserves audio ordering. Consumers that fall
  behind its capacity skip stale audio with a diagnostic message.
- The service starts USB capture for the first consumer, releases USB when the
  last consumer detaches, and exits after approximately 20 seconds without any
  consumers. The job is bootstrapped into the current GUI session only; it is not installed
  in Login Items or `~/Library/LaunchAgents` and has no KeepAlive setting.
- Service failures cause clients to reconnect/restart the helper automatically.
  This recovery path exists but a forced service-crash/restart was not part of
  the final hardware test.

The mapped file holds recent media buffers; it is temporary private IPC storage,
not an exported recording. A hard service termination can leave recent buffer
contents there until replaced or temporary-file cleanup. Diagnostic text is in
`/tmp/hd60s-<uid>/service.log`.

## Build

```sh
make service app plugin
make install
```

The app includes its helper in `Contents/MacOS`; the OBS plugin includes the same
helper in its own `Contents/MacOS`. Both use the same per-user IPC namespace. The running service executable is
  cached privately under `/tmp/hd60s-<uid>/service-0.4.1`, so quitting the
  preview app does not terminate an app-owned child or remove OBS's executable.
Run matching versions of the service and consumers. Restart both consumers when
upgrading an already-running service across incompatible IPC versions.

No system extension, restricted entitlement, paid signing certificate or system
security change is needed for this architecture. Current builds use local ad-hoc
signatures; public distribution/notarization remains separate.

## Validation on October 27, 2026

- `make test`: parser, audio-processing and status-rendering tests pass.
- `make test-shared`: synthetic IPC test with fast and slow readers plus a killed
  reader. Fast reader received 111 frames and 331 PCM blocks in four seconds;
  slow reader received 38 frames and 333 PCM blocks. Both had zero data/timestamp
  assertion failures. Slowing/killing one reader did not stall the fast reader.
- Hardware: native app and OBS simultaneously displayed live 720×480, 59.94 fps
  Wii video. OBS's audio meter was active and the app's playback checkbox enabled.
- IORegistry identified `hd60s-service` as the capture device/interface owner,
  rather than separate app and OBS USB clients.
- A third hardware consumer received 481 frames, 800 PCM blocks and 384000 stereo
  sample frames over eight seconds, with zero timestamp/format assertion failures.
- Audible playback confirmation is separate from these data/API checks.

Multi-consumer 1080p60 throughput, long-term clock drift and every external audio
output device are not established by the 480p test. The shared service treats every revision the same; only revision 4 has been tested.

## Full screen and flicker correction (0.4.1)

Use the **Full screen** button, **Control–Command–F**, or double-click the preview.
The button changes to Exit full screen while active; the standard green window
button also works. Live video preserves its aspect ratio.

A heartbeat-age race was reproduced in the IPC test: another process could
publish a newer heartbeat after a reader sampled its current time. Unsigned
subtraction made this fresh heartbeat appear extremely old, causing lease loss,
reconnection and possible frame-buffer reuse during copying. The elapsed-time
check now saturates at zero for a newer heartbeat. A regression assertion and
fast/slow multi-process test cover this case. Implicit CALayer contents/bounds
animations are disabled to prevent per-frame transitions in the preview.

OBS uses its own bundled shared client/helper. It does not launch the native app
or require that app to remain running. The per-user launchd service owns USB.
The native app and OBS can each start it independently.

The app receives each frame's source scan type (progressive or interlaced)
separately from the progressive pixels used for display and recording. The
metadata occupies the existing four trailing padding bytes in a v1 shared
frame, preserving offsets and map size for older OBS clients. A zero value
from an older service is shown as unknown, rather than guessed from resolution.

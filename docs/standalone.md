# HD60 S Capture native app

Open `/Applications/HD60 S Capture.app` or build it with `make app`.
Version 0.4.1 previews and plays the capture card without OBS. A floating control
bar over the picture holds pause, audio playback, an input level meter, capture
settings, and full screen. Audio playback starts enabled and remembers your
choice. It uses the current system playback device. Keep speaker volume
comfortable; source gain is separate from speaker volume.

The settings panel (the slider icon, or Command-comma) applies live:

- **Gain:** −30 to +6 dB, default −6 dB.
- **Mute monitoring:** silences playback without stopping capture or the meter.
- **Limiter:** −1 dBFS ceiling, immediate attenuation with a 100 ms release. It
  does not repair existing distortion or provide true-peak limiting.
- **Peak:** input peak in dBFS, measured before gain and limiting.
- **Fit / Fill / Stretch:** how the picture fills the preview. Fill crops and
  Stretch changes aspect ratio; neither adds detail.

Gain, mute and the limiter affect this app's playback and its recordings. Fit
only affects the preview. The OBS source keeps its own settings, and the shared
service sends both consumers the same untouched stream.

## Recording

The record button, or Command-R, writes a QuickTime movie to
`~/Movies/HD60 S Capture/` named `HD60 S <date> <time>.mov`. Elapsed time shows
next to the button while recording; **Show Recordings** (Shift-Command-R) opens
the folder.

Video is H.264 from the Apple silicon hardware encoder at the input's own
resolution and frame rate, tagged with the input's Rec. 601/709 matrix. The
bitrate is `width × height × fps × 0.2`, clamped to 4–50 Mbit/s. Audio is
48 kHz stereo AAC at 192 kbit/s, taken after gain and the limiter but before
**Mute monitoring**, so muting your speakers does not silence the recording.

Recording captures every delivered frame; the on-screen preview is throttled
separately and may skip frames under load without affecting the file.

A recording stops and the file is finalized when you press stop, pause capture,
quit the app, or the input resolution changes mid-recording. Losing the HDMI
signal does not stop a recording; it resumes when the signal returns, and the
gap is not padded, so the movie is shorter than wall-clock time. Recording needs
a live signal to start. Only one recording runs at a time, and this app's
recording is independent of anything OBS records.

The app and the current HD60 S Native OBS source can now run together using
[shared capture](shared-capture.md). Close/update older direct-capture versions
before testing. Pausing the app disconnects its consumer only; OBS can continue.

Status screens distinguish unplugged USB, no HDMI signal, connecting, a card
owned by older direct-capture software, and live frames. They clear stale video.
The same state is available to all shared consumers, including OBS.

Discord can share this app's window. Application-audio sharing depends on the
Discord/macOS version and has not been end-to-end verified. The app does not
join calls or start streams. It does not register a system camera/microphone.

The experimental camera extension and its signing targets have been deleted. No
camera extension was ever activated and no security setting was changed.

Local builds have an ad-hoc signature. A downloaded copy may trigger Gatekeeper;
there is no promise that every Mac will open an unnotarized development build
without warnings. Public signing/notarization is a separate release task.

Use the full-screen button, Control–Command–F, or double-click the picture.
In full screen the picture fills the display, the header is hidden, and the
control bar fades out after 2.5 seconds without pointer movement; move the
pointer to bring it back, and press Escape to leave. The Space bar pauses and
resumes. OBS works independently of this window through the capture service.

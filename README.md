# HD60 S Native for macOS

Native Apple silicon USB capture and an OBS 33 source plugin for the **Elgato
HD60 S**. Built and tested on revision 4 (`0fd9:0076`); revisions 1 to 3
(`0fd9:004f`, `0fd9:005e`, `0fd9:0074`) are tried with the same protocol but
nobody has tested one yet. If you have one, an issue saying whether it works
either way is the most useful thing you can send. Video and HDMI audio come from one USB stream.
No source from adam8113/hd60s-arm64 is used.

**Made with AI.** The app, the USB driver, the OBS source and this README were
written with an AI coding assistant (Claude), directed and tested by a person on
real hardware: an HD60 S revision 4 capturing a Wii, through this app and OBS 33.
The protocol comes from Doug Brown's GPL-2.0 Linux driver, credited in
`src/capture.c`. Read the code before trusting it with anything that matters, and
expect the rough edges that come with a driver that has had one tester.

## Native app with sound and shared capture (version 0.4)

Open `/Applications/HD60 S Capture.app`, or build with `make app`. Preview and
sound start automatically. A floating control bar over the picture
holds, in three groups, recording and its timer; pause, audio playback and an
input level meter; settings and full screen. In full screen it fades out while
the pointer is idle, unless you turn that off in Settings.

Settings is its own window (the gear in the bar, or Command-comma), with four
sections: **Audio** (play through this Mac, gain, mute, peak ceiling, live peak
readout), **Video** (picture fit, keep the control bar on screen in full
screen), **Recording** (folder, quality) and **Capture** (what the card and the
signal are doing right now, and a button that adds the source to OBS). Escape
closes it. Your choices are remembered.

Recording writes H.264 + AAC QuickTime movies to `~/Movies/HD60 S Capture/` at
the input's own resolution and frame rate, using the Apple silicon hardware
encoder. See [native app usage](docs/standalone.md).

**The native app and our OBS source can now use the card simultaneously.** A
single bundled capture service owns USB and sends video/audio to each consumer.
Pausing or closing one consumer does not interrupt another. See
[shared capture and validation](docs/shared-capture.md) and
[native app usage](docs/standalone.md).

Other applications need a compatible integration or can capture the preview
window. This does not provide direct system-camera enumeration in Discord.
The native camera extension experiment has been removed from the source tree;
the app needs no signing entitlements or security changes.

## Use in OBS

If OBS is installed, the app keeps the source up to date itself: on launch it
copies the bundled plugin into `~/Library/Application Support/obs-studio/plugins/`,
which is inside your own home folder, so no password is needed. It does this
only when the installed copy differs from the one the app ships, and nothing at
all when OBS is absent. See [Updating](#updating) for the OBS restart. Build that bundle with `make bundle`; a plain
`make app` leaves the plugin out and the app then never offers it.

To install by hand instead, use `make install`. In OBS:

1. Sources → Add → **HD60 S Native**.
2. Add one source for the card. Initialization and capture start automatically.
3. Video and audio are included together; no additional audio capture source is
   needed for this card.
4. Use OBS Transform → Fit to Screen if you want to enlarge the source.

Hiding the source with the eye icon releases the capture card, so another app
can use it without closing OBS; showing it takes the card back. Turn that off
per source with **Release the card while this source is hidden**.

The source is already added to the **Wii Test** scene on this Mac. Your existing
sources were left in place. No recording or streaming was started during setup.
Audio monitoring is controlled by OBS; receiving audio does not automatically
play it through your speakers.

## Updating

The service runs from a copy of itself in `/tmp/hd60s-<uid>/`, so a rebuilt
binary does not take effect until the running copy stops. Each build carries a number,
`HD_SHARED_BUILD`. The version lives once, in the `VERSION` file: the Makefile
packs it as major×10000 + minor×100 + patch (0.4.7 is 407) and passes it to
every compile, and fills the same dotted string into both bundles'
`CFBundleVersion`. Building outside the Makefile fails with an `#error` rather
than guessing a build number. A client that finds an older
service running stops it, raises a floor in `/tmp/hd60s-<uid>/min-build`, and
starts its own; a service below that floor stands down instead of taking the
card. Downgrading on purpose works: when nothing is serving, a lower build
lowers the floor to itself.

Released builds update themselves with [Sparkle](https://sparkle-project.org)
2.10.1 from this repository's
[GitHub Releases](https://github.com/koels/hd60s-capture/releases). The feed is
`releases/latest/download/appcast.xml`; each release is signed with an EdDSA
key whose private half lives only in the maintainer's login Keychain (account
`hd60s-capture`) and whose public half is `packaging/appcast-key.pub`.

The app checks once a day while open, and **Check for Updates** is in the app
menu. It also updates when it is never opened: when OBS starts, and every six
hours while it runs, the OBS source launches the installed app with
`--background-update`, which checks the same feed with the same key, installs
silently and exits without a window. The source then replaces its own file
with the one the new app ships, and hands the card to the new driver.

The app is ad-hoc signed, with its designated requirement set to its bundle ID
so one build accepts the next. That is what lets Sparkle update it without a
paid certificate; the EdDSA signature is what proves a release is genuine.
Gatekeeper still blocks an ad-hoc app downloaded in a browser on another Mac,
so distributing to others needs a Developer ID and notarization.

To publish: bump `VERSION`, commit, run `tools/release.sh`, which builds,
tests, signs and creates the GitHub release. `tools/check-versions.sh` then
confirms the installed app, both copies of the source, and the running driver
all agree.

Whichever way the app updates, OBS is never closed. The driver is the service,
not the OBS source: every client starts the newest `hd60s-service` installed
anywhere, its own or the app's, so when the updated app takes the card over,
OBS shows a short gap and carries on with the new driver. The app also copies
the new source into your plugins folder; that file is only the thin client,
and OBS reads it the next time it starts. This holds as long as the shared
map's layout (`HD_SHARED_VERSION`) is unchanged; a release that changes it
would need OBS restarted once.

Sparkle covers released builds. A local `make bundle` is a different case, and
the app covers it separately: it watches its own bundle on disk and shows a bar
over the picture when a different version is installed, with a Relaunch button. Relaunching is the only
way a running binary picks up a new one. If OBS is running, the button also
offers to quit OBS and reopen both, because OBS keeps the old copy of the
source until OBS itself restarts; quitting OBS stops anything it is streaming
or recording, so that option is never the default.

A service from before build 405 does not honour the floor, so an OBS still running
the old plugin will keep restarting its own old service. Install the new source
when the app offers it, then restart OBS.

The shared service owns USB once for all current app/OBS consumers. Close shared
consumers before using the older direct-USB command-line capture tool. The
service retries lost signals and device connections automatically. The current core waits for an HDMI signal until stopped. Revision 4 is the only
one tested. An older card is opened and initialised the same way; if it does not
answer like revision 4, the app and OBS say so instead of a generic error, and
nothing is written to the card beyond the same setup commands revision 4 takes.

## OBS status screens and Discord Devices

The OBS Virtual Camera workaround was declined and has not been enabled.

The native OBS source emits visible status frames inside the scene: **Starting
up**, **No capture card**, **No signal**, **Card already in use**, and **Capture
interrupted**. Each is drawn at the source's own resolution with a glyph, the
state, and what to do about it. It retains the last live output dimensions (or
selected scaling dimensions), clears stale video, and refreshes status frames
while automatically retrying capture. The standalone app is not required.

The HD60 S timing registers occasionally read back zero or half-updated while
its MCU refreshes them, even with a stable source. Acting on one such sample
used to tear down a healthy capture, which showed up as the picture blanking to
a status frame for about a second. Timing changes and signal loss are now
confirmed over three quick re-reads before capture restarts, and the OBS source
no longer polls USB presence itself as a second, conflicting source of truth.

To expose the OBS scene as a camera in Discord's **Devices** tab, use OBS's
**Start Virtual Camera** and select **OBS Virtual Camera** in Discord. That
uses OBS's signed system camera extension; it is not an independent camera
named HD60 S. On this Mac OBS reported that its extension must first be allowed
in System Settings → General → Login Items & Extensions → Camera Extensions.
No system-extension permission has been changed by this project.

A native HD60 S camera extension was previously implemented and compiled, but it
never installed: activation failed with a missing-entitlement error, and that
code has now been deleted. This build does not disable platform security to load
unsigned system extensions. OBS Virtual Camera is video-only; it does not by
itself expose the embedded HDMI audio as a microphone.

## Source properties

Double-click **HD60 S Native** in Sources to open Properties.
Changes apply live without restarting the USB stream:

- **Output resolution:** match input, 640×480, 720×480, 1280×720, 1920×1080.
  Bicubic software scaling resizes the received frame; it does not alter the
  console HDMI mode or add detail. Explicit dimensions can change aspect ratio.
- **Frame-rate limit:** match input or 23.976/24/25/29.97/30/50/59.94/60.
  Caps delivery by dropping frames; it never fabricates frames. The final
  recording/stream FPS remains an OBS Settings → Video setting.
- **Pixel format:** native YUY2, NV12, BGRA RGB. NV12 reduces chroma resolution;
  conversion/scaling uses the installed OBS scaler.
- **Color space:** auto, Rec. 601, Rec. 709. **Input range:** auto limited,
  explicit limited, or full interpretation override. Use auto unless a known
  test pattern establishes that a manual override is necessary.
- **Audio:** −30 to +6 dB gain, mute, stereo-linked overload protection at
  −1 dBFS. Initial gain is −6 dB. The block limiter has immediate attenuation
  and a 100 ms release; it does not repair existing distortion or provide
  intersample/true-peak limiting.
- **Refresh input / audio levels:** current HDMI mode, raw peak since the last
  refresh, and cumulative near-full-scale sample count before gain/limiting.
  The near-full-scale threshold is absolute PCM magnitude ≥32760; this is a
  diagnostic, not proof of audible clipping. Counters reset on capture restart.

Hardware encoding belongs to the OBS output, not to a raw capture source.
Choose **Apple VT H.264 Hardware** or **Apple VT HEVC Hardware** in
**OBS Settings → Output**. The source properties include this guidance rather
than a nonfunctional source encoder switch.

On the live Wii menu test, raw peak reached −3.4 dBFS with zero near-full-scale
samples in the measured interval. That does not establish the cause of the
reported distortion or exclude distortion elsewhere in the signal chain.

The updated plugin was loaded in OBS and its property controls inspected.
Live 720p software scaling and BGRA output were visually checked. A standalone
check against the installed libobs passed all four explicit resolutions in
YUY2, NV12, and BGRA and verified neutral-white preservation (`make test-video`).
PCM gain, signedness, limiter ceiling, mute, and fractional frame dropping passed
sanitizer-enabled tests (`make test`).

## Build

Requires Xcode command line tools and OBS 33 installed at `/Applications/OBS.app`.
The dependency headers are already prepared in `.deps` on this Mac. For a fresh
checkout:

```sh
./tools/fetch-deps.sh
make all test plugin
make install
```

Restart OBS after installation.

The capture service is cached per version at `/tmp/hd60s-<uid>/service-<version>`
and is only copied there when that file is missing, so **changing anything the
service links (`src/capture.c`, `src/service.c`) needs the version string in
`src/shared-client.c` bumped**, otherwise clients keep running the old binary.
The shared memory map is not versioned, so a service already running is reused
whatever its version; stop it as well:

```sh
launchctl bootout gui/$(id -u)/local.hd60s.capture-service.<old>
pkill -f "hd60s-$(id -u)/service-<old>"
```

Confirm the right binary is live with
`shasum /tmp/hd60s-$(id -u)/service-<new> build/hd60s-service`. The plugin uses an ad-hoc local signature; this
is a development build, not a notarized public installer. No kernel extension,
firmware installation, or system security setting change is required.

Override `OBS_APP`, `OBS_SRC`, or `SIMDE_SRC` when needed. OBS headers must match
the installed OBS major version. Dependencies: OBS 33.0.0, SIMDe v0.8.2.

## Diagnostics

```sh
make probe
./build/hd60s-capture --seconds 10 --frame build/first-frame.yuy2
```

Run direct USB tools from a normal Terminal; a restricted execution sandbox can block
IOKit. The probe only reads standard descriptors. Capture also sends volatile
initialization/configuration commands and selects the bulk streaming interface.
Capture logs in OBS are prefixed `[HD60 S Native]` under
`~/Library/Application Support/obs-studio/logs`.

Convert a captured frame using `tools/preview.py` (requires NumPy and Pillow):

```sh
python3 tools/preview.py build/first-frame.yuy2 build/first-frame.png --width 720 --height 480
```

Use the width and height printed by the capture tool. Add `--bt709` for a BT.709
input. Frames are packed YUY2 with limited-range YUV metadata passed to OBS.

## Verified and limitations

On October 27, 2026, on the connected revision-4 card:

- Native arm64 builds succeeded with `-Wall -Wextra`.
- Standard descriptor reads and user-space device/interface open succeeded.
- Ten-second hardware test: 599 complete 720×480 frames at 59.94 Hz, zero
  partial frames, embedded 48 kHz stereo PCM.
- Saved raw frame decoded to the Wii menu without an obvious color cast.
- OBS 33 loaded the plugin; the Wii menu appeared and its audio meter was active.
- Parser tests with AddressSanitizer/UndefinedBehaviorSanitizer passed for every
  chunk size in a synthetic stream, PCM batching, and partial-frame rejection.

The initial parser scan discards data before the first valid record; startup
bad-header counts are expected. Counts should stop increasing after alignment.
Current capture uses asynchronous bulk transfers, with 32 queued 32 KiB reads.
Only progressive inputs up to 1920×1080 are accepted. Higher modes, interlaced
capture, revisions 1–3 (attempted, untested), analog audio selection, multiple cards, long-term A/V
clock drift correction, and calibrated color accuracy are not implemented or
validated. 720p/1080p throughput and reconnect behavior still need hardware tests;
they are not established by the 480p test. HDCP-protected sources are rejected.
Source geometry changes cause a stop and reinitialization, rather than parsing
with an outdated frame size. HDMI/USB removal can take a control/read timeout to
be reflected. Removing the source may briefly block while USB requests drain.

## Sources and licensing

GPL-2.0-only; see LICENSE. Protocol/control-flow reference:
Doug Brown's [hd60s-linux-driver](https://github.com/dougg3/hd60s-linux-driver),
commit `34621f7184b602ae7797d206ab2c9627cc045d1a`, copyright 2026 Doug Brown.
The native transport, parser, diagnostic, and OBS integration are written here;
the GPL license conservatively preserves the Linux reference's obligations.
OBS API headers are GPL; SIMDe has its own license in `.deps/simde/COPYING`.

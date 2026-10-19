# USB audio: low-latency custom driver

Goal: replace the Android audio stack (Oboe/AAudio) for the USB interface
with a userspace USB audio driver, and take end-to-end monitoring latency
from the current Oboe path to the 4-10 ms band on the Samsung Galaxy Tab
S10 FE + Zoom AMS-22 rig. Plan: `t3k-usb-low-latency.md` (working copy,
deliberately uncommitted).

## Status (2026-10-19)

- Driver core is host-tested (77/77) and runs on-device: 48-frame callback,
  8 packets x 4 URBs, 128-frame ring, 48 kHz. Measured **5.52 ms
  digital-loop RTL**, zero URB xruns at that layout.
- The app integrates it as the "USB Direct" driver type; Oboe remains the
  default and the fallback. Settings shows the device's reported latency,
  xruns and (USB Direct only) drift; the log gets one `[AudioSettings]`
  line per device change.
- **Android Debug builds must compile the DSP at `-O3`.** Gradle defaults to
  `-O0`; at 48-frame deadlines that alone drops ~900 capture frames/s with a
  two-model NAM chain. `plugin/CMakeLists.txt` links an interface target that
  places `-O3` after JUCE's `-g -O0`; verify `build.ninja` has it before
  trusting any performance number from a debug APK.
- **The L2 end-to-end number is still unmeasured.** The development AMS-22
  now passes direct monitor but not USB playback at either rear line
  output (its headphone output works), on both hosts, with firmware 1.10.
  That is a unit fault; see `m3-report.md`. The driver is unaffected.

## Probe app (M1)

`android/probe` (package `com.TONE3000.USBCHECK`) is the spike vehicle:
enumerate, `dump` (descriptor tree + fixtures), `stream` (sustained duplex
with xrun/URB stats), `rtl` (self-measurement, section 4.1), `sweep` (rtl
across the URB layout matrix, plan M1.6). Driven entirely over adb:

```sh
./script/android-dev.sh probe build
./script/android-dev.sh probe install
adb shell am start -n com.TONE3000.USBCHECK/.MainActivity -e mode dump
adb logcat -s T3KUSB                    # structured one-line records
./script/android-dev.sh probe csv       # files/rtl.csv measurement rows
./script/usb-scenarios.sh doctor        # adb, USB topology, battery, power
./script/usb-scenarios.sh start         # shipping layout 8x4 + 128 ring
./script/usb-scenarios.sh status        # one-line counters
./script/usb-scenarios.sh soak 60 --screen-off
```

The probe session lives in `ProbeService` (foreground, partial wake lock), so
the stream survives the screen turning off. Commands are service intents;
the activity only owns the USB permission prompt.

The USB permission dialog needs an unlocked screen; the first launch of each
session asks it once. Claiming the AudioStreaming interfaces detaches the
kernel's `snd-usb-audio`, which is expected and required; the log records
what each interface's kernel driver was beforehand.

## Measurement method

Two complementary rigs (plan section 4):

- **L2 PC rig (ground truth).** This PC (Audient EVO8) is both source and
  recorder, so its own latency cancels. `script/rtl_pc.py` plays a 2 ms
  exponential sine sweep at -12 dBFS and records two channels:

  ```
  EVO8 Out 3 (PA out ch2) ──> AMS-22 IN      ──> [tablet path] ──> AMS-22 OUT ──> EVO8 In 4 (PA cap ch2)  path
  EVO8 Out 4 (PA out ch3) ────────────────────────────────────────────────────> EVO8 In 3 (PA cap ch3)  reference
  ```

  `rtl = (argmax xcorr(path) - argmax xcorr(ref)) / rate`. Per-trial CSV in
  the `results-*.csv` files. The peak/next-lobe ratio sanity metric must stay
  above 3.0 (measured values are > 100). Use
  `--out-channels 2,3 --ref-channel 3 --path-channel 2 --in-channels 4` with
  the EVO devices named explicitly. AMS-22 direct monitor OFF and digital
  loopback OFF for every run (the plan's D5 rules).

- **L1 device self-measurement** (RTL with the AMS-22's digital loopback ON)
  arrives with the probe app at M1; it measures our own buffering only.

**The AMS-22 feeds its input to its output at ~1 ms regardless of its
direct-monitor and loopback switches** (proven 2026-10-17: the +1.08 ms
correlation peak survives with the monitoring app force-stopped and both
switches off). That hardware peak is stronger than the app path and wins a
naive argmax, so `rtl_pc.py --window-ms 30,300` selects the app-path peak in
its expected window instead. The same day's baseline at "pretty low" input
gain picked the app peak only because the hardware feed was attenuated.

Rig gotchas learned the hard way (see `results-baseline-oboe-bypass.txt`
header): the EVO8's hardware matrix is a **mixer**, so the PipeWire channel
to physical-jack mapping depends on the card profile and on the evo-control
mixer rows. Measured 2026-10-19 with `analog-surround-51` and the second mix
carrying PC Out 3+4: PipeWire **out ch2 = Out 3, ch3 = Out 4**, and capture
**ch2 = In 4, ch3 = In 3** (the capture channels are swapped relative to the
jack numbers). Re-run the per-leg tone check below after any profile or
mixer change:

```sh
# each leg separately: play a tone on one output channel, record all inputs
./script/usb-scenarios.sh doctor   # or use the probe's dump/stream
```

The AMS-22 also feeds its input to its output at ~1 ms regardless of its
direct-monitor switch, so the path capture always contains that early peak;
`--window-ms 2,200` keeps it out of the app-path search.

## Latency budget: current Oboe path (M0 baseline)

Measured 2026-10-17 on the Samsung Galaxy Tab S10 FE (SM-X520, Android 16,
kernel 6.6.102-android15) + Zoom AMS-22 (input gain 5/10, output 10/10),
TONE3000 debug build, chain bypassed (no NAM model), monitoring live:

```
e2e (bypass)      =  88.2 ms  [PC rig, median of 20, spread 0.02 ms]
e2e (model)       = 74-137 ms [PC rig, 20 trials, median 81.6; UNSTABLE]
converters        =   ~1.0 ms [measured: the AMS-22's raw feed peak]
app buffer        =  40.0 ms  [samplesPerBlock=1920 at 48 kHz, from app log]
android in        =  ~20 ms   [MMAP capture ring 3072 frames = 64 ms;
                               JUCE polls input from the output callback]
android out       =  ~25 ms   [MMAP playback ring 3072 frames = 64 ms]
```

With the NAM chain active the pipeline latency jitters from trial to trial
(73-137 ms across runs; the bypass path is stable to 0.02 ms). The jitter is
audible as occasional digital artifacts and is a property of the Oboe
two-stream design under DSP load, exactly what the USB-direct driver's fixed
URB cadence replaces.

Attribution notes (from `results-baseline-oboe-dumpsys.txt`):

- The app streams on AAudio MMAP (`AudioMmapIn_236` capture +
  `AudioMmapOut_50` playback, both `MMAP_NOIRQ`, 48 kHz, USB device, one
  track each, pid 24183). Not deep-buffer; the Samsung USB route does grant
  MMAP.
- HAL frame count 3072 (64 ms) per direction is the ring size, not the
  steady-state delay; the dominant term is JUCE 9's Android design (input
  and output are two separate Oboe streams, input polled with blocking read
  from the output callback) plus the 1920-frame block.
- Input stream is mono PCM_16 on the USB capture; output is PCM_8_24
  stereo. No resampler on the app path.
- Samsung effects (`SoundBoosterPlus`, `DG-Action`) are attached to other
  mixer threads, not to the app's MMAP tracks.
- App-reported JUCE `getInput/OutputLatencyInSamples` is not surfaced yet
  (needs code; deferred to M3 per plan M0.5).

Raw evidence: `results-baseline-oboe-bypass.{txt,csv}`,
`results-baseline-oboe-dumpsys.txt`, `results-baseline-oboe-model.txt`.

## Results index

| File | What |
| --- | --- |
| `results-baseline-oboe-bypass.txt/.csv` | M0 baseline, chain bypassed, PC rig |
| `results-baseline-oboe-model.txt` | M0 baseline with a NAM model loaded |
| `results-m1-stream.txt` | M1.5 soak: 14.5 min, zero xruns, detach/replug |
| `results-m1-rtl.txt` | M1.6 rtl medians + URB layout sweep |
| `results-m2-feedback.txt` | M2.1 feedback pacing smoke test |
| `results-m2-ring.txt` | M2.2 ring policy + stress |
| `results-m2-layout.txt` | M2.3/M2.4 cadence, ring, RTL, recovery |
| `results-m2-soak-partial.txt` | M2.4 17-minute continuous soak (transport wedged; 60-min run pending) |
| `m2-report.md` | M2 driver-core report |
| `m3-report.md` | M3 USB Direct integration status + blocker |
| `results-baseline-oboe-dumpsys.txt` | audio_flinger / audio_policy / logcat during streaming |
| `ams22-descriptors.md` | AMS-22 descriptor dump and entity graph (M1.3) |
| `m1-report.md` | M1 probe spike go/no-go (M1.7) |
| `results-integration.txt` | TONE3000 through USB Direct vs baseline (M3.7) |

## Supported devices

| Device | USB class | Status |
| --- | --- | --- |
| Zoom AMS-22 | UAC2, explicit feedback | driver core complete (M2): 8x4/128, 5.52 ms digital RTL; USB Direct runs on-device at 48-frame blocks, zero xruns (M3); L2 e2e pending the EVO capture rig (`m3-report.md`) |

# USB audio: low-latency custom driver

Goal: replace the Android audio stack (Oboe/AAudio) for the USB interface
with a userspace USB audio driver, and take end-to-end monitoring latency
from the current Oboe path to the 4-10 ms band on the Samsung Galaxy Tab
S10 FE + Zoom AMS-22 rig. Plan: `~/.opencode/plan/t3k-usb-low-latency.md`.

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
```

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
  EVO8 Main L (PA out ch0) ──> AMS-22 IN      ──> [tablet path] ──> AMS-22 OUT ──> EVO8 in 2 (PA cap ch1)  path
  EVO8 Main R (PA out ch1) ───────────────────────────────────────────────────> EVO8 in 1 (PA cap ch0)  reference
  ```

  `rtl = (argmax xcorr(path) - argmax xcorr(ref)) / rate`. Per-trial CSV in
  the `results-*.csv` files. The peak/next-lobe ratio sanity metric must stay
  above 3.0 (measured values are > 100). AMS-22 direct monitor OFF and
  digital loopback OFF for every run (the plan's D5 rules).

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
header): the EVO8's hardware matrix routes only `DAW1/2` to the Main pair in
hardware; the "headphone" pair routes exist only in `evo-control`'s shadow
store and require its `evo-raw` kernel module to write. PA channel map is
5.1-reordered: PA out ch2/ch3 are the loopback inject pair (they mirror on
capture ch2/ch3, pre-volume), and the rig must use Main L/R (PA out ch0/1).

## Latency budget: current Oboe path (M0 baseline)

Measured 2026-10-17 on the Samsung Galaxy Tab S10 FE (SM-X520, Android 16,
kernel 6.6.102-android15) + Zoom AMS-22 (input gain 5/10, output 10/10),
TONE3000 debug build, chain bypassed (no NAM model), monitoring live:

```
e2e (bypass)      = 88.2 ms   [PC rig, median of 20, spread 0.02 ms]
e2e (model)       = (pending, see results-baseline-oboe-model.txt)
converters (est)  =  1-2 ms   [AMS-22 ADC+DAC, typical class-compliant]
app buffer        = 40.0 ms   [samplesPerBlock=1920 at 48 kHz, from app log]
android in        = ~20 ms    [MMAP capture ring 3072 frames = 64 ms;
                               JUCE polls input from the output callback]
android out       = ~25 ms    [MMAP playback ring 3072 frames = 64 ms]
```

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
| `results-baseline-oboe-dumpsys.txt` | audio_flinger / audio_policy / logcat during streaming |
| `ams22-descriptors.md` | AMS-22 descriptor dump and entity graph (M1.3) |
| `m1-report.md` | M1 probe spike go/no-go (M1.7) |
| `results-integration.txt` | TONE3000 through USB Direct vs baseline (M3.7) |

## Supported devices

| Device | USB class | Status |
| --- | --- | --- |
| Zoom AMS-22 | UAC2, explicit feedback | baseline measured; driver work in progress |

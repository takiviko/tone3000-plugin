# M3 status: USB Direct integration (implemented, on-device verification blocked)

Date: 2026-10-18. Rig: Samsung Galaxy Tab S10 FE (SM-X520, Android 16,
kernel 6.6.102-android15) + Zoom AMS-22 behind an Anker 332 hub.

## What is implemented

- `plugin/include/UsbDirectAudioDevice.h` and
  `plugin/src/UsbDirectAudioDevice.cpp`:
  - `UsbDirectAudioIODevice : juce::AudioIODevice` running the M2 driver core
    (8 packets x 4 URBs, 128-frame ring, 48 kHz; 96 kHz also exposed).
  - `UsbDirectAudioIODeviceType` registered as **"USB Direct"** next to Oboe
    in `StandaloneAudioSettings`' constructor (D4). Oboe stays the default.
  - The JUCE callback is invoked from the USB reaping thread (D3); the
    device interleaves/deinterleaves only the active channels.
  - Latency reporting from the URB layout plus a documented 1 ms converter
    estimate: 4x48 + 48 = 240 output samples, + ring target 64 for input.
  - `getXRunCount()` excludes the priming underruns; the app log gets a
    line every 10 s with xruns, ring over/under, drift and feedback.
- `plugin/usb/android/UsbfsTransport.{h,cpp}`: the shared raw usbfs
  transport (submit/reap/discard/control/SET_INTERFACE) used by the app.
- `android/app/.../UsbDirectBridge.java`: enumeration, USB permission from
  the activity, `openDevice` + `claimInterface(force=true)` for the audio
  interfaces (quirk registry leaves the AMS-22's extra AC block and MIDI
  function alone), fd and raw descriptors to native.
- `MainActivity`: registers the USB receiver and asks for permission on
  resume; a debuggable-build marker (`am start --ez usbDirect true`) selects
  USB Direct for automation.
- CMake: the USB core is defined before `plugin/`, the Android plugin links
  `t3k_usb_audio` and builds the transport/device sources. This is the
  M3.3 wiring.

## Build evidence

- `./script/android-dev.sh build` -> `BUILD SUCCESSFUL`, APK produced
  (`android/app/build/intermediates/apk/debug/app-debug.apk`).
- Desktop regression: `cmake -B /tmp/opencode/build-desktop
  -DCMAKE_BUILD_TYPE=Release` (83.6 s) and
  `cmake --build ... --target TONE3000_Standalone` exit 0; `UsbTests`
  builds and `ctest` is 77/77 (the CI step added in this change).
- Host suite: 77/77.

## On-device observations and the blocker

The app installs and runs. The USB permission dialog appears and the type
is registered: the app log records `USB Direct: select ok, current=USB
Direct`. The Oboe path is live at 1920-frame blocks before the switch.

**Blocker:** the switch from Oboe to USB Direct can freeze the app while it
claims the interfaces. This is the plan's known risk (risk table: "App
freezes when tapping the USB device row in settings"). The app log stops
before `USB Direct: config ...`, i.e. the freeze is inside the Java
`claimInterface(force=true)`/`openDevice` sequence (or the message thread's
1.5 s device-type transition), not in the URB machine. Once the claim has
detached `snd-usb-audio`, a subsequent app start cannot open Oboe, so the
standalone holder loops before the settings can select USB Direct again.
The probe's `USBDEVFS_RESET` recovers the kernel binding (verified twice:
`ACTION=bind ... DRIVER=snd-usb-audio` for all five interfaces), but it is
not a fix for the app's startup ordering.

Contributing factor: the standalone holder opens its default (Oboe) device
in `StandalonePluginHolder` before `StandaloneAudioSettings` exists, so a
remembered/marker USB Direct selection can only be applied *after* Oboe has
the AMS-22 open. The marker path (constructor-time switch) hits this worst;
the Settings UI path was not yet exercised after a clean boot.

The tablet was rebooted to clear the audio stack, and the reboot dropped the
wireless-debugging port (the old port refuses, mDNS does not advertise the
tablet). On-device testing is therefore blocked until wireless debugging is
re-enabled or the device is replugged. No claim is made that the L2 e2e
number was measured; it is not in this report.

## Next steps (morning checklist)

1. Re-enable Developer options -> Wireless debugging and `adb connect`
   (the port changes; `adb pair` if the pairing was reset). Or replug the
   AMS-22 and use wired adb before claiming anything.
2. Launch the app; confirm it starts on Oboe (the reboot restores
   `snd-usb-audio`).
3. Open Settings -> Audio Driver -> **USB Direct**. If the app freezes,
   force-stop it; use the probe workaround below to leave the kernel driver
   detached, relaunch (Oboe falls back to the tablet speaker), and switch
   again:
   ```sh
   adb shell am start -n com.TONE3000.USBCHECK/.MainActivity -e mode stream
   sleep 2
   adb shell am force-stop com.TONE3000.USBCHECK   # interfaces free, driver stays detached
   ```
4. Verify the app log for `USB Direct: config subslot=4 ...`,
   `USB Direct: open 48000 Hz, 48-frame blocks, 8x4 URBs, ring 128`,
   `USB Direct: start`, then the 10 s xrun/drift lines.
5. Run the L2 rig:
   ```sh
   /tmp/opencode/venv-audio/bin/python script/rtl_pc.py --trials 20 \
       --probe sweep --rate 48000 --out-channels 0,1 \
       --in-device "EVO8 Inputs" --out-device "EVO8 Outputs"
   ```
   The default PipeWire source is currently the webcam; pass the EVO8
   devices explicitly (or set the default source to
   `alsa_input.usb-Audient_EVO8-00.analog-surround-41` temporarily).
   The AMS-22 input gain must be usable for the analog path; the rig's
   reference channel (EVO8 in1) works regardless.
6. Fill in `results-integration.txt` and the budget table; restore Oboe,
   then replug the AMS-22 if the kernel driver needs to rebind.

**Likely fix for the freeze:** move the type registration and the
USB Direct selection to a point before `StandalonePluginHolder` initializes
its AudioDeviceManager (or make the switch close/stop Oboe and wait for the
kernel driver to release before claiming). Registering the type from a
startup hook and passing a preferred USB Direct setup to the holder would
avoid ever opening Oboe on the AMS-22. Do not merge a "works on my machine"
claim without the measured e2e number.

## M2.4 soak caveat

The 60-minute soak did not complete as a logged run. The stream ran
continuously for at least 17 minutes at 7,997 packets/s with zero URB xruns
and six ring overruns (feedback stepped to -442 ppm); the hose-side Wi-Fi
adb transport then wedged and the logger stopped at minute 12. See
`results-m2-soak-partial.txt`. The script now bounds every adb call and
reconnects. A clean 60-minute run is still owed.

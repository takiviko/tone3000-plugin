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

## On-device observations

The app now opens USB Direct successfully (commit e960468):

- `USB Direct: config subslot=4 play=0x2 cap=0x81 fb=0x82 clock=40`
- `USB Direct: open 48000 Hz, 48-frame blocks, 8x4 URBs, ring 128`
- `[Processor] prepareToPlay: sampleRate=48000, samplesPerBlock=48`
- `USB Direct: start`
- diagnostics over several minutes: `xruns=0 ringOver=0 ringUnder=5`,
  feedback 6.00000, drift settling within +/-170 ppm. The app input meter
  sits around -24 dB with the EVO feed, and monitoring is audible.

The earlier startup freeze was not the claim itself: JUCE's
`addAudioDeviceType()` does not scan the new type, so the marker switch saw
an empty device list and selected a device-less type; the app then stayed
on Oboe and, after the failed attempt, could not start Oboe with the
kernel driver detached. Scanning explicitly after adding, and registering
the Java bridge/marker before `super.onCreate`, fixes the flow. The first
switch no longer freezes.

## Remaining blocker: the L2 rig, not the app

The PC records the EVO8's analog capture at about -68 to -74 dBFS for both
the reference and the path, even with the EVO input gains up. On this
machine the EVO is normally reconfigured by
`evo-control-apply.service` ("Apply evo-control default preset") to a
2-output/DAW mode when Reaper is not in use; that preset mutes the analog
inputs in the mixer. Stopping and masking the service, setting input gains
0/50 dB, and forcing in1/in2 to all mixer outputs did not change the USB
capture: the capture stream remains in the 2-output/DAW mode. The service
was restored for normal use.

Consequence: the app path is audible on the EVO's direct monitor, but the
PC cannot record it at a usable level. **No L2 e2e number is claimed.** The
measured core number remains the probe's **5.52 ms digital-loop RTL** at
the shipping layout.

## Next steps

1. Put the EVO8 back into its normal input-capture (Reaper) mode, or stop
   the service and apply a preset that routes the analog inputs to the USB
   capture. Verify with a tone that the EVO USB capture is at a healthy
   level before measuring.
2. Run the L2 rig (20 trials, bypass chain) and write
   `results-integration.txt`; pass the EVO devices explicitly if the
   default PipeWire source is not the EVO:
   `rtl_pc.py --trials 20 --probe sweep --rate 48000 --out-channels 0,1
   --in-device "EVO8 Inputs" --out-device "EVO8 Outputs"`.
3. Complete a clean 60-minute screen-off soak (script is now
   timeout-hardened).
4. Keep the EVO service enabled in normal use; only disable it for a
   measurement if that is what the input-capture mode requires.

## M2.4 soak caveat

The 60-minute soak did not complete as a logged run. The stream ran
continuously for at least 17 minutes at 7,997 packets/s with zero URB xruns
and six ring overruns (feedback stepped to -442 ppm); the hose-side Wi-Fi
adb transport then wedged and the logger stopped at minute 12. See
`results-m2-soak-partial.txt`. The script now bounds every adb call and
reconnects. A clean 60-minute run is still owed.

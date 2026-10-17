#pragma once

// Device clock drift estimation and playback packet regulation (plan
// section 6.4). The interface clock and the host clock differ by up to
// ~+/-500 ppm; without compensation the ring drifts and xruns appear every
// few minutes. Two jobs:
//
//   1. Estimate the device's real rate against nominal from the observed
//      frames-per-time on the capture side, and report drift in ppm.
//   2. Regulate playback packet sizing on the ring fill level (PI on the
//      fill error, output in whole frames per packet) so the fill stays near
//      its target.
//
// Pure C++ with an injectable time base (nanoseconds as a plain integer), so
// the convergence tests run on synthetic clocks.

#include <cstddef>
#include <cstdint>

namespace t3k::usb {

class ClockModel {
 public:
  explicit ClockModel(double nominalRate, double kp = 0.05, double ki = 0.002);

  /** Feed observed capture progress: frames received over elapsedNs. */
  void addCaptureFrames(int64_t frames, uint64_t elapsedNs);

  /** Measured device rate from the observation window (Hz). */
  double measuredRate() const;
  /** (measured - nominal) / nominal * 1e6. Positive = device runs fast. */
  double driftPpm() const;
  void reset();

  /** Playback pacing: how many frames to put in the next packet given the
      ring fill. Returns a whole-frame adjustment of the nominal packet size,
      clamped to +/-1 frame (plan section 6.4: vary packet frame counts by
      +/-1 as needed). */
  int playbackFramesPerPacket(int nominalPerPacket, size_t fillFrames,
                              size_t targetFillFrames);

  /** Parse an explicit feedback endpoint value: UAC2 uses 16.16, UAC1
      (and some UAC2 devices) 10.14, of frames per (micro)frame. */
  static double parseFeedback(const uint8_t* data, bool format16_16);

 private:
  double nominalRate_;
  double kp_, ki_;
  int64_t windowFrames_ = 0;
  uint64_t windowNs_ = 0;
  double fillErrorSum_ = 0.0;
};

}  // namespace t3k::usb

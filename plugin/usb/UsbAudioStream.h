#pragma once

// The isochronous URB scheduling machine (plan section 6.3). Owns the URB
// pools for both directions, the capture ring between them, and the xrun
// bookkeeping; talks to the outside only through an UrbTransport (the raw
// usbfs ioctls on Android: USBDEVFS_SUBMITURB / REAPURB / DISCARDURB on the
// fd from UsbDeviceConnection.getFileDescriptor) and a Callback (the audio
// processor, invoked on the USB reaping thread per design D3).
//
// The transport is an interface so the whole machine runs on the host
// against a scripted fake: every ordering, xrun, and teardown case is
// exercised in tests without hardware.
//
// Machine per direction (section 6.3):
//   1. N URBs of P packets each; packet length holds the nominal frame count
//      (+/-1 frame tolerance on capture; the actual length decides).
//   2. Submit all N; playback URBs are filled before submission.
//   3. Reap on the USB thread. Capture packets land in the ring
//      (writeDropOldest); playback URBs are refilled from the callback and
//      resubmitted.
//   4. start_frame continuity is tracked; gaps count as xruns.
//   5. stop() discards outstanding URBs, waits for the reaps and reports the
//      stream as quiescent. Returning to altsetting 0 is the caller's job
//      (the control path owns SET_INTERFACE).
//
// Playback pacing is implicit-feedback style by default (plan section 6.4):
// a PI controller on the capture ring fill varies the frames per playback
// URB by +/-1. With an explicit feedback endpoint the shell can feed the
// device's real rate in via ClockModel::addCaptureFrames semantics or simply
// leave pacing at nominal.

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "AudioRing.h"
#include "ClockModel.h"
#include "UsbAudioTypes.h"

namespace t3k::usb {

/** One isochronous URB: a buffer plus per-packet descriptors. Mirrors
    usbdevfs_urb but stays portable; the transport maps it. */
struct Urb {
  uint8_t endpoint = 0;
  std::vector<uint8_t> buffer;
  /** Per-packet requested and actual lengths, and status (0 or -errno). */
  std::vector<uint32_t> packetLengths;
  std::vector<uint32_t> packetActual;
  std::vector<int32_t> packetStatus;
  int startFrame = 0;
  int status = 0;  // URB-level status (0 or -errno)
  bool inFlight = false;
  bool isCapture = true;
};

class UrbTransport {
 public:
  virtual ~UrbTransport() = default;
  /** Submit for async completion. False = rejected (device gone). */
  virtual bool submit(Urb* urb) = 0;
  /** Block up to timeoutMs for a completed URB; nullptr on timeout. */
  virtual Urb* reap(int timeoutMs) = 0;
  /** Ask the kernel to cancel an in-flight URB (it will still be reaped). */
  virtual void discard(Urb* urb) = 0;
};

class UsbAudioStream {
 public:
  struct Callback {
    virtual ~Callback() = default;
    /** Run on the USB thread once per playback URB. in/out interleaved
        floats, frames * channels. Input frames that never arrived are zero;
        the callback may write anything into out. */
    virtual void process(const float* in, float* out, int frames, int channels) = 0;
  };

  struct Options {
    int urbsInFlight = 3;   // plan M1.5 default
    int packetsPerUrb = 8;  // 1 ms per URB at high speed
    size_t ringFrames = 512;
    /** Monotonic nanoseconds for drift measurement; null disables drift
        tracking (the estimator then reports 0 ppm). */
    std::function<uint64_t()> nowNs;
  };

  UsbAudioStream(const StreamConfig& config, const Options& options,
                 UrbTransport& transport, Callback& callback);

  /** Allocate the URB pools and submit the first round. */
  bool start();
  /** Discard outstanding URBs, reap them back and go quiescent. */
  void stop();

  /** Handle one reap. False = nothing completed within timeoutMs. */
  bool pumpOnce(int timeoutMs);

  bool running() const { return running_; }
  /** Frames the callback sees per invocation at the nominal pacing. */
  int callbackFrames() const;
  const StreamStats& captureStats() const { return captureStats_; }
  const StreamStats& playbackStats() const { return playbackStats_; }
  ClockModel& clock() { return clock_; }

  int inFlightCount() const;

 private:
  void prepareCaptureUrb(Urb& urb);
  void preparePlaybackUrb(Urb& urb);
  void handleCapture(Urb& urb);
  void handlePlayback(Urb& urb);
  bool resubmit(Urb& urb);
  void noteXrun(StreamStats& stats) { ++stats.xruns; }

  StreamConfig config_;
  Options options_;
  UrbTransport& transport_;
  Callback& callback_;
  ClockModel clock_;
  int framesPerPacket_;
  int bytesPerFrame_;
  size_t targetFill_;

  std::vector<std::unique_ptr<Urb>> urbs_;
  AudioRing captureRing_;
  std::vector<float> inScratch_;
  std::vector<float> outScratch_;
  std::vector<uint8_t> wireScratch_;

  StreamStats captureStats_;
  StreamStats playbackStats_;
  bool running_ = false;
  int lastCaptureStartFrame_ = -1;
  int lastPlaybackStartFrame_ = -1;
  // Observed start_frame increment between consecutive URBs of one
  // direction. usbfs reports start_frame in units the HCD chooses (1 ms
  // frames on most, microframes on others), so continuity is judged against
  // the observed step, not an assumed one.
  int captureStartFrameDelta_ = 0;
  int playbackStartFrameDelta_ = 0;
  uint64_t lastCaptureNs_ = 0;
};

}  // namespace t3k::usb

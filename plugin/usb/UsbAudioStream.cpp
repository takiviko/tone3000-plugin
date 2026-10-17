#include "UsbAudioStream.h"

#include <algorithm>
#include <cstring>

#include "PcmConvert.h"

namespace t3k::usb {
namespace {

// URB-level statuses the kernel reports on reap (they are -errno).
constexpr int kErrConnReset = -104;  // after DISCARDURB: expected at stop()
constexpr int kErrShutdown = -108;   // device unplugged
constexpr int kErrNoDevice = -19;

bool isDeviceGone(int status) {
  return status == kErrShutdown || status == kErrNoDevice;
}

}  // namespace

UsbAudioStream::UsbAudioStream(const StreamConfig& config, const Options& options,
                               UrbTransport& transport, Callback& callback)
    : config_(config),
      options_(options),
      transport_(transport),
      callback_(callback),
      clock_(static_cast<double>(config.sampleRate)),
      framesPerPacket_(config.nominalFramesPerPacket(config.speed)),
      bytesPerFrame_(config.bytesPerFrame()),
      targetFill_(std::max<size_t>(options.ringFrames / 2, 1)),
      captureRing_(options.ringFrames, config.bytesPerFrame()),
      inScratch_(static_cast<size_t>(callbackFrames()) * config.channels, 0.0f),
      outScratch_(static_cast<size_t>(callbackFrames()) * config.channels, 0.0f) {}

int UsbAudioStream::callbackFrames() const {
  return options_.packetsPerUrb * framesPerPacket_;
}

int UsbAudioStream::inFlightCount() const {
  int n = 0;
  for (const auto& u : urbs_) {
    if (u->inFlight) ++n;
  }
  return n;
}

void UsbAudioStream::prepareCaptureUrb(Urb& urb) {
  urb.isCapture = true;
  urb.endpoint = config_.captureEndpoint;
  urb.packetLengths.assign(options_.packetsPerUrb, 0);
  urb.packetActual.assign(options_.packetsPerUrb, 0);
  urb.packetStatus.assign(options_.packetsPerUrb, 0);
  // Capture packets tolerate +/-1 frame (plan section 6.3): ask for the max
  // and believe actual_length on reap.
  const uint32_t packetMax = static_cast<uint32_t>((framesPerPacket_ + 1) * bytesPerFrame_);
  for (uint32_t& len : urb.packetLengths) len = packetMax;
  urb.buffer.assign(packetMax * options_.packetsPerUrb, 0);
}

void UsbAudioStream::preparePlaybackUrb(Urb& urb) {
  urb.isCapture = false;
  urb.endpoint = config_.playbackEndpoint;
  urb.packetLengths.assign(options_.packetsPerUrb, 0);
  urb.packetActual.assign(options_.packetsPerUrb, 0);
  urb.packetStatus.assign(options_.packetsPerUrb, 0);

  // Pacing: the PI controller on the capture ring fill adjusts the URB total
  // by +/-1 frame, applied to the last packet (plan section 6.4).
  const int nominalTotal = callbackFrames();
  const int total = std::max(
      1, clock_.playbackFramesPerPacket(nominalTotal, captureRing_.readAvailable(),
                                        targetFill_));
  const int adjust = total - nominalTotal;

  size_t bytes = 0;
  for (int p = 0; p < options_.packetsPerUrb; ++p) {
    const int frames = std::max(0, framesPerPacket_ + (p == options_.packetsPerUrb - 1
                                                           ? adjust
                                                           : 0));
    urb.packetLengths[p] = static_cast<uint32_t>(frames * bytesPerFrame_);
    bytes += urb.packetLengths[p];
  }
  urb.buffer.assign(bytes, 0);

  // Run the DSP directly into the URB: capture ring in, wire bytes out. The
  // only intermediate copy in the whole path is the capture ring (D3).
  const int frames = static_cast<int>(bytes / static_cast<size_t>(bytesPerFrame_));
  std::fill(inScratch_.begin(), inScratch_.end(), 0.0f);
  std::fill(outScratch_.begin(), outScratch_.end(), 0.0f);
  wireScratch_.resize(static_cast<size_t>(frames) * bytesPerFrame_);
  const size_t gotFrames = captureRing_.read(wireScratch_.data(),
                                             static_cast<size_t>(frames));
  if (gotFrames > 0) {
    pcm::wireToFloat(wireScratch_.data(), inScratch_.data(), gotFrames, config_.channels,
                     config_.subslotBytes);
  }
  callback_.process(inScratch_.data(), outScratch_.data(), frames, config_.channels);
  pcm::floatToWire(outScratch_.data(), urb.buffer.data(), static_cast<size_t>(frames),
                   config_.channels, config_.subslotBytes);
}

void UsbAudioStream::handleCapture(Urb& urb) {
  ++captureStats_.urbsReaped;
  if (lastCaptureStartFrame_ >= 0 &&
      urb.startFrame != lastCaptureStartFrame_ + options_.packetsPerUrb) {
    noteXrun(captureStats_);  // start_frame gap: something was missed
  }
  lastCaptureStartFrame_ = urb.startFrame;

  size_t offset = 0;
  int64_t framesInUrb = 0;
  for (int p = 0; p < options_.packetsPerUrb; ++p) {
    ++captureStats_.packets;
    // Xrun check against the nominal packet with the documented +/-1 frame
    // tolerance (the allocation is nominal+1; actual_length decides).
    const uint32_t nominal = static_cast<uint32_t>(framesPerPacket_ * bytesPerFrame_);
    const uint32_t actual = urb.packetActual[p];
    const uint32_t tolerance = static_cast<uint32_t>(bytesPerFrame_);
    if (urb.packetStatus[p] != 0 || actual + tolerance < nominal ||
        actual > nominal + tolerance) {
      noteXrun(captureStats_);
    }
    if (actual > 0 && urb.packetStatus[p] == 0) {
      captureRing_.writeDropOldest(urb.buffer.data() + offset,
                                   actual / static_cast<size_t>(bytesPerFrame_));
      captureStats_.bytesTransferred += actual;
      framesInUrb += actual / bytesPerFrame_;
    }
    offset += urb.packetLengths[p];
  }

  // Drift from observed capture progress (plan section 6.4).
  if (options_.nowNs) {
    const uint64_t now = options_.nowNs();
    if (lastCaptureNs_ != 0 && now > lastCaptureNs_ && framesInUrb > 0) {
      clock_.addCaptureFrames(framesInUrb, now - lastCaptureNs_);
    }
    lastCaptureNs_ = now;
    captureStats_.driftPpm = clock_.driftPpm();
    playbackStats_.driftPpm = clock_.driftPpm();
  }
}

void UsbAudioStream::handlePlayback(Urb& urb) {
  ++playbackStats_.urbsReaped;
  if (lastPlaybackStartFrame_ >= 0 &&
      urb.startFrame != lastPlaybackStartFrame_ + options_.packetsPerUrb) {
    noteXrun(playbackStats_);
  }
  lastPlaybackStartFrame_ = urb.startFrame;

  for (int p = 0; p < options_.packetsPerUrb; ++p) {
    ++playbackStats_.packets;
    const uint32_t expected = urb.packetLengths[p];
    const uint32_t actual = urb.packetActual[p];
    if (urb.packetStatus[p] != 0 || actual != expected) noteXrun(playbackStats_);
    playbackStats_.bytesTransferred += actual;
  }
}

bool UsbAudioStream::resubmit(Urb& urb) {
  if (!running_) return false;
  if (urb.isCapture) {
    prepareCaptureUrb(urb);
  } else {
    preparePlaybackUrb(urb);
  }
  urb.inFlight = true;
  ++(urb.isCapture ? captureStats_ : playbackStats_).urbsSubmitted;
  if (!transport_.submit(&urb)) {
    urb.inFlight = false;
    running_ = false;
    return false;
  }
  return true;
}

bool UsbAudioStream::start() {
  if (running_) return true;
  urbs_.clear();
  captureRing_.clear();
  lastCaptureStartFrame_ = -1;
  lastPlaybackStartFrame_ = -1;
  lastCaptureNs_ = 0;

  const int n = std::max(options_.urbsInFlight, 1);
  for (int i = 0; i < n; ++i) {
    if (config_.captureEndpoint != 0) {
      urbs_.push_back(std::make_unique<Urb>());
      prepareCaptureUrb(*urbs_.back());
    }
    if (config_.playbackEndpoint != 0) {
      urbs_.push_back(std::make_unique<Urb>());
      preparePlaybackUrb(*urbs_.back());
    }
  }
  running_ = true;
  for (auto& u : urbs_) {
    u->inFlight = true;
    ++(u->isCapture ? captureStats_ : playbackStats_).urbsSubmitted;
    if (!transport_.submit(u.get())) {
      u->inFlight = false;
      running_ = false;
      return false;
    }
  }
  return true;
}

void UsbAudioStream::stop() {
  if (!running_ && inFlightCount() == 0) return;
  running_ = false;
  for (auto& u : urbs_) {
    if (u->inFlight) transport_.discard(u.get());
  }
  // The kernel reaps discarded URBs with -ECONNRESET; collect them so no URB
  // object is freed while the driver still holds it (plan pitfall 4).
  for (int tries = 0; inFlightCount() > 0 && tries < 64; ++tries) {
    Urb* urb = transport_.reap(50);
    if (urb == nullptr) continue;
    urb->inFlight = false;
  }
}

bool UsbAudioStream::pumpOnce(int timeoutMs) {
  Urb* urb = transport_.reap(timeoutMs);
  if (urb == nullptr) {
    if (running_) noteXrun(captureStats_);  // late reap: deadlines slipped
    return false;
  }
  urb->inFlight = false;
  if (isDeviceGone(urb->status)) {
    running_ = false;  // unplug: stop resubmitting (plan pitfall 4)
    return true;
  }
  if (urb->isCapture) {
    handleCapture(*urb);
  } else {
    handlePlayback(*urb);
  }
  if (running_) resubmit(*urb);
  return true;
}

}  // namespace t3k::usb

#include "ClockModel.h"

#include <algorithm>
#include <cmath>

namespace t3k::usb {

ClockModel::ClockModel(double nominalRate, double kp, double ki)
    : nominalRate_(nominalRate), kp_(kp), ki_(ki) {}

void ClockModel::addCaptureFrames(int64_t frames, uint64_t elapsedNs) {
  windowFrames_ += frames;
  windowNs_ += elapsedNs;
  // Keep the window bounded (~10 s) so the estimate tracks slow temperature
  // drift instead of averaging the device's whole history.
  const uint64_t kMaxWindowNs = 10ull * 1000 * 1000 * 1000;
  if (windowNs_ > kMaxWindowNs) {
    const double keep = static_cast<double>(kMaxWindowNs) * 0.5 / windowNs_;
    windowFrames_ = static_cast<int64_t>(windowFrames_ * keep);
    windowNs_ = static_cast<uint64_t>(windowNs_ * keep);
  }
}

double ClockModel::measuredRate() const {
  if (windowNs_ == 0) return nominalRate_;
  return static_cast<double>(windowFrames_) * 1e9 / static_cast<double>(windowNs_);
}

double ClockModel::driftPpm() const {
  return (measuredRate() - nominalRate_) / nominalRate_ * 1e6;
}

void ClockModel::reset() {
  windowFrames_ = 0;
  windowNs_ = 0;
  fillErrorSum_ = 0.0;
}

int ClockModel::playbackFramesPerPacket(int nominalPerPacket, size_t fillFrames,
                                        size_t targetFillFrames) {
  const double error = static_cast<double>(fillFrames) - static_cast<double>(targetFillFrames);
  fillErrorSum_ += error;
  // Anti-windup: a bounded integrator, so a long outage cannot shove the
  // output around afterwards.
  fillErrorSum_ = std::clamp(fillErrorSum_, -1e6, 1e6);
  const double out = kp_ * error + ki_ * fillErrorSum_;
  return nominalPerPacket + static_cast<int>(std::clamp(std::lround(out), -1L, 1L));
}

double ClockModel::parseFeedback(const uint8_t* data, bool format16_16) {
  // Feedback values are frames per microframe (HS) or frame (FS).
  const uint32_t raw = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                       (static_cast<uint32_t>(data[2]) << 16) |
                       (static_cast<uint32_t>(data[3]) << 24);
  return format16_16 ? static_cast<double>(raw) / 65536.0
                     : static_cast<double>(raw) / 16384.0;
}

}  // namespace t3k::usb

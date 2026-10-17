// L0: ClockModel drift estimation and playback regulation (plan section 6.4:
// converge within 60 s under +/-500 ppm synthetic drift, stay within
// +/-2 ppm of the input estimate, step response damped).

#include <cmath>

#include "ClockModel.h"
#include "gtest/gtest.h"

namespace t3k::usb {
namespace {

// Feed one second of capture progress per call at the given true rate.
void feedSeconds(ClockModel& model, double trueRate, int seconds) {
  for (int i = 0; i < seconds; ++i) {
    model.addCaptureFrames(static_cast<int64_t>(std::llround(trueRate)),
                           1000ull * 1000 * 1000);
  }
}

TEST(ClockModel, ConvergesOnPlus500ppmWithinTwoPpm) {
  ClockModel model(48000.0);
  const double trueRate = 48000.0 * (1.0 + 500e-6);  // 48024 Hz
  feedSeconds(model, trueRate, 60);
  EXPECT_NEAR(model.driftPpm(), 500.0, 2.0);
  EXPECT_NEAR(model.measuredRate(), trueRate, 1.0);
}

TEST(ClockModel, ConvergesOnMinus500ppmWithinTwoPpm) {
  ClockModel model(48000.0);
  const double trueRate = 48000.0 * (1.0 - 500e-6);
  feedSeconds(model, trueRate, 60);
  EXPECT_NEAR(model.driftPpm(), -500.0, 2.0);
}

TEST(ClockModel, TracksA20ppmStepWithoutRinging) {
  ClockModel model(48000.0);
  feedSeconds(model, 48000.0 * (1.0 + 20e-6), 30);
  EXPECT_NEAR(model.driftPpm(), 20.0, 2.0);
  // The window forgets the old regime within its ~10 s bound.
  feedSeconds(model, 48000.0 * (1.0 - 20e-6), 20);
  EXPECT_NEAR(model.driftPpm(), -20.0, 2.0);
  // And stays there: no oscillation after the step.
  feedSeconds(model, 48000.0 * (1.0 - 20e-6), 10);
  EXPECT_NEAR(model.driftPpm(), -20.0, 2.0);
}

TEST(ClockModel, PlaybackRegulationMovesOneFrameAtATime) {
  ClockModel model(48000.0);
  // Ring running high: the device consumes slower than we produce, so the
  // packet grows to drain it (at most one frame per decision).
  EXPECT_EQ(model.playbackFramesPerPacket(6, 200, 100), 7);
  EXPECT_EQ(model.playbackFramesPerPacket(6, 200, 100), 7);
  // Ring running low: the packet shrinks to let it refill.
  ClockModel low(48000.0);
  EXPECT_EQ(low.playbackFramesPerPacket(6, 10, 100), 5);
  EXPECT_EQ(low.playbackFramesPerPacket(6, 10, 100), 5);
  // Exactly on target: unchanged.
  ClockModel onTarget(48000.0);
  EXPECT_EQ(onTarget.playbackFramesPerPacket(6, 100, 100), 6);
}

TEST(ClockModel, RegulationHoldsTheRingThroughA500ppmDrift) {
  // Simulated duplex loop: the device delivers 6 frames per packet period
  // scaled by +500 ppm, the controller chooses the playback packet size, and
  // the fill must stay in a bounded band without growing oscillation.
  ClockModel model(48000.0);
  const int nominal = 6;
  const size_t target = 120;
  double fill = static_cast<double>(target);
  double producedPerTick = nominal * (1.0 + 500e-6);
  double maxDeviationAfterWarmup = 0.0;
  for (int tick = 0; tick < 60 * 8000; ++tick) {  // 60 s of 125 us packets
    fill += producedPerTick;
    const int play = model.playbackFramesPerPacket(nominal, static_cast<size_t>(fill),
                                                   target);
    fill -= play;
    if (tick > 8000 && std::abs(fill - target) > maxDeviationAfterWarmup) {
      maxDeviationAfterWarmup = std::abs(fill - target);
    }
  }
  // A 500 ppm drift is 3 frames/s at 6/packet; the integrator must keep the
  // fill within a few packets of target, never let it run away.
  EXPECT_LT(maxDeviationAfterWarmup, 48.0);
  EXPECT_NEAR(fill, static_cast<double>(target), 48.0);
}

TEST(ClockModel, ParsesFeedbackFormats) {
  // raw 0x000C0000: 12.0 in 16.16, 48.0 in 10.14.
  const uint8_t raw[4] = {0x00, 0x00, 0x0C, 0x00};
  EXPECT_DOUBLE_EQ(ClockModel::parseFeedback(raw, /*format16_16=*/true), 12.0);
  EXPECT_DOUBLE_EQ(ClockModel::parseFeedback(raw, /*format16_16=*/false), 48.0);
}

}  // namespace
}  // namespace t3k::usb

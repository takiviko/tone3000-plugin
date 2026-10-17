// L0: the URB scheduling machine against a scripted fake transport (plan
// section 6.3: submission order, xrun accounting, unplug and teardown).

#include <cstdint>
#include <vector>

#include "PcmConvert.h"
#include "UsbAudioStream.h"
#include "gtest/gtest.h"

namespace t3k::usb {
namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kSubslot = 3;
constexpr int kBpf = kChannels * kSubslot;  // 6 bytes per frame

StreamConfig testConfig() {
  StreamConfig cfg;
  cfg.sampleRate = kRate;
  cfg.channels = kChannels;
  cfg.subslotBytes = kSubslot;
  cfg.bitResolution = 24;
  cfg.speed = UsbSpeed::high;
  cfg.playbackEndpoint = 0x02;
  cfg.captureEndpoint = 0x81;
  return cfg;
}

struct FakeTransport : UrbTransport {
  std::vector<Urb*> inFlight;
  std::vector<Urb*> completed;
  int discards = 0;
  bool rejectSubmits = false;

  bool submit(Urb* urb) override {
    if (rejectSubmits) return false;
    inFlight.push_back(urb);
    return true;
  }
  Urb* reap(int) override {
    if (completed.empty()) return nullptr;
    Urb* urb = completed.front();
    completed.erase(completed.begin());
    return urb;
  }
  void discard(Urb* urb) override {
    ++discards;
    // The kernel reaps discarded URBs with -ECONNRESET; mirror that.
    urb->status = -104;
    completed.push_back(urb);
    for (size_t i = 0; i < inFlight.size(); ++i) {
      if (inFlight[i] == urb) {
        inFlight.erase(inFlight.begin() + static_cast<long>(i));
        break;
      }
    }
  }
  /** Mark every packet as arriving full and error-free. */
  static void fillFull(Urb* urb) {
    for (size_t p = 0; p < urb->packetLengths.size(); ++p) {
      urb->packetActual[p] = urb->packetLengths[p];
      urb->packetStatus[p] = 0;
    }
    urb->status = 0;
  }
  /** Hand a completed URB to the stream; packet fields are the caller's. */
  void complete(Urb* urb, int startFrame) {
    urb->startFrame = startFrame;
    for (size_t i = 0; i < inFlight.size(); ++i) {
      if (inFlight[i] == urb) {
        inFlight.erase(inFlight.begin() + static_cast<long>(i));
        break;
      }
    }
    completed.push_back(urb);
  }
};

struct RecordingCallback : UsbAudioStream::Callback {
  int frames = 0;
  int channels = 0;
  int calls = 0;
  std::vector<float> lastIn;
  float outValue = 0.25f;
  void process(const float* in, float* out, int f, int ch) override {
    frames = f;
    channels = ch;
    ++calls;
    lastIn.assign(in, in + static_cast<size_t>(f) * ch);
    for (int i = 0; i < f * ch; ++i) out[i] = outValue;
  }
};

Urb* firstOf(FakeTransport& t, bool capture) {
  for (Urb* u : t.inFlight) {
    if (u->isCapture == capture) return u;
  }
  return nullptr;
}

TEST(UsbAudioStream, StartSubmitsBothDirections) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream stream(testConfig(), {}, transport, cb);
  ASSERT_TRUE(stream.start());
  EXPECT_TRUE(stream.running());
  EXPECT_EQ(transport.inFlight.size(), 6u);  // 3 capture + 3 playback
  EXPECT_EQ(stream.inFlightCount(), 6);
  EXPECT_EQ(stream.callbackFrames(), 48);  // 8 packets * 6 frames at 48 kHz HS
}

TEST(UsbAudioStream, CaptureReapFillsRingAndResubmits) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream stream(testConfig(), {}, transport, cb);
  ASSERT_TRUE(stream.start());

  Urb* urb = firstOf(transport, /*capture=*/true);
  ASSERT_NE(urb, nullptr);
  // Packets full of a recognizable wire pattern (frame n = value n).
  for (int p = 0; p < 8; ++p) {
    for (int f = 0; f < 6; ++f) {
      const int32_t v = p * 6 + f;
      uint8_t* at = urb->buffer.data() + (p * 7 + f) * kBpf;  // 7-frame packets
      at[0] = static_cast<uint8_t>(v & 0xff);
      at[1] = static_cast<uint8_t>((v >> 8) & 0xff);
      at[2] = static_cast<uint8_t>((v >> 16) & 0xff);
      at[3] = 0;
      at[4] = 0;
      at[5] = 0;
    }
    urb->packetActual[p] = urb->packetLengths[p];  // full packet (7 frames)
    urb->packetStatus[p] = 0;
  }
  transport.complete(urb, /*startFrame=*/1000);

  ASSERT_TRUE(stream.pumpOnce(0));
  // 8 packets * 7 frames arrived; nominal is 6, so the tolerance check must
  // accept this without an xrun.
  EXPECT_EQ(stream.captureStats().urbsReaped, 1u);
  EXPECT_EQ(stream.captureStats().packets, 8u);
  EXPECT_EQ(stream.captureStats().xruns, 0u);
  EXPECT_EQ(transport.inFlight.size(), 6u);  // resubmitted
}

TEST(UsbAudioStream, PlaybackReapRunsCallbackAndFillsWire) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream stream(testConfig(), {}, transport, cb);
  ASSERT_TRUE(stream.start());

  // start() primes every playback URB through the callback (the URBs must
  // leave the door with data), so the reap is one call past the priming.
  const int primed = cb.calls;
  EXPECT_EQ(primed, 3);

  Urb* urb = firstOf(transport, /*capture=*/false);
  ASSERT_NE(urb, nullptr);
  FakeTransport::fillFull(urb);
  transport.complete(urb, /*startFrame=*/2000);
  ASSERT_TRUE(stream.pumpOnce(0));
  EXPECT_EQ(cb.calls, primed + 1);
  EXPECT_EQ(cb.channels, kChannels);

  // The URB buffer must be the callback's output on the wire.
  const int frames = cb.frames;
  std::vector<float> back(static_cast<size_t>(frames) * kChannels);
  pcm::wireToFloat(urb->buffer.data(), back.data(), static_cast<size_t>(frames),
                   kChannels, kSubslot);
  for (float v : back) EXPECT_NEAR(v, cb.outValue, 1e-4);
  EXPECT_EQ(stream.playbackStats().urbsReaped, 1u);
}

TEST(UsbAudioStream, DuplexCallbackSeesCapturedAudio) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream stream(testConfig(), {}, transport, cb);
  ASSERT_TRUE(stream.start());

  // Capture first: 8 packets x 6 frames of a ramp in the left channel.
  Urb* cap = firstOf(transport, /*capture=*/true);
  ASSERT_NE(cap, nullptr);
  for (int p = 0; p < 8; ++p) {
    for (int f = 0; f < 6; ++f) {
      const int32_t v = p * 6 + f;
      uint8_t* at = cap->buffer.data() + (p * 7 + f) * kBpf;
      at[0] = static_cast<uint8_t>(v & 0xff);
      at[1] = static_cast<uint8_t>((v >> 8) & 0xff);
      at[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    }
    cap->packetActual[p] = cap->packetLengths[p];
    cap->packetStatus[p] = 0;
  }
  transport.complete(cap, 0);
  ASSERT_TRUE(stream.pumpOnce(0));

  // Then a playback reap: the callback must see those samples as input.
  Urb* play = firstOf(transport, /*capture=*/false);
  ASSERT_NE(play, nullptr);
  FakeTransport::fillFull(play);
  transport.complete(play, 8);
  const int primed = cb.calls;
  ASSERT_TRUE(stream.pumpOnce(0));
  ASSERT_EQ(cb.calls, primed + 1);
  // The PI controller trims the URB by one frame while the ring sits below
  // its 50% target: the callback sees 46..48 frames here.
  const int frames = cb.frames;
  ASSERT_GE(frames, 46);
  ASSERT_LE(frames, 48);
  ASSERT_EQ(cb.lastIn.size(), static_cast<size_t>(frames) * kChannels);
  for (int f = 0; f < frames; ++f) {
    EXPECT_NEAR(cb.lastIn[static_cast<size_t>(f) * kChannels],
                pcm::int24ToFloat(f), 1e-5)
        << "frame " << f;
    EXPECT_NEAR(cb.lastIn[static_cast<size_t>(f) * kChannels + 1], 0.0f, 1e-5);
  }
}

TEST(UsbAudioStream, ShortCapturePacketCountsXrun) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream stream(testConfig(), {}, transport, cb);
  ASSERT_TRUE(stream.start());
  Urb* urb = firstOf(transport, /*capture=*/true);
  FakeTransport::fillFull(urb);
  urb->packetActual[0] = 4 * kBpf;  // 4 frames: 2 short of nominal 6
  transport.complete(urb, 0);
  ASSERT_TRUE(stream.pumpOnce(0));
  EXPECT_EQ(stream.captureStats().xruns, 1u);
}

TEST(UsbAudioStream, PacketStatusErrorCountsXrun) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream stream(testConfig(), {}, transport, cb);
  ASSERT_TRUE(stream.start());
  Urb* urb = firstOf(transport, /*capture=*/true);
  FakeTransport::fillFull(urb);
  urb->packetStatus[3] = -71;  // -EPROTO
  transport.complete(urb, 0);
  ASSERT_TRUE(stream.pumpOnce(0));
  EXPECT_EQ(stream.captureStats().xruns, 1u);
}

TEST(UsbAudioStream, StartFrameGapCountsXrun) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream stream(testConfig(), {}, transport, cb);
  ASSERT_TRUE(stream.start());
  Urb* first = firstOf(transport, /*capture=*/true);
  FakeTransport::fillFull(first);
  transport.complete(first, 1000);
  ASSERT_TRUE(stream.pumpOnce(0));
  Urb* second = firstOf(transport, /*capture=*/true);
  FakeTransport::fillFull(second);
  transport.complete(second, 1008 + 20);  // a 20-microframe hole
  ASSERT_TRUE(stream.pumpOnce(0));
  EXPECT_EQ(stream.captureStats().xruns, 1u);
}

TEST(UsbAudioStream, UnplugStopsResubmission) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream stream(testConfig(), {}, transport, cb);
  ASSERT_TRUE(stream.start());
  Urb* urb = firstOf(transport, /*capture=*/true);
  FakeTransport::fillFull(urb);
  urb->status = -108;  // -ESHUTDOWN
  transport.complete(urb, 0);
  ASSERT_TRUE(stream.pumpOnce(0));
  EXPECT_FALSE(stream.running());
  EXPECT_EQ(transport.inFlight.size(), 5u);  // nothing resubmitted
}

TEST(UsbAudioStream, StopDiscardsAndCollectsEverything) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream stream(testConfig(), {}, transport, cb);
  ASSERT_TRUE(stream.start());
  stream.stop();
  EXPECT_EQ(transport.discards, 6);
  EXPECT_EQ(stream.inFlightCount(), 0);  // all reaped back
  EXPECT_FALSE(stream.running());
}

TEST(UsbAudioStream, LateReapTimeoutCountsXrun) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream stream(testConfig(), {}, transport, cb);
  ASSERT_TRUE(stream.start());
  EXPECT_FALSE(stream.pumpOnce(0));  // nothing completed in time
  EXPECT_EQ(stream.captureStats().xruns, 1u);
}

TEST(UsbAudioStream, DriftIsEstimatedFromCaptureProgress) {
  FakeTransport transport;
  RecordingCallback cb;
  UsbAudioStream::Options opts;
  // Synthetic time: each reap advances 1 ms + 500 ppm of clock, while the
  // device delivers the nominal 48 frames per reap => the device is slow by
  // 500 ppm.
  uint64_t now = 0;
  opts.nowNs = [&now] { return now; };
  UsbAudioStream stream(testConfig(), opts, transport, cb);
  ASSERT_TRUE(stream.start());

  for (int i = 0; i < 200; ++i) {
    now += 1000500;  // 1.0005 ms of wall time
    Urb* urb = firstOf(transport, /*capture=*/true);
    urb->packetActual.assign(8, static_cast<uint32_t>(6 * kBpf));
    urb->packetStatus.assign(8, 0);
    urb->packetLengths.assign(8, static_cast<uint32_t>(6 * kBpf));
    transport.complete(urb, i * 8);
    ASSERT_TRUE(stream.pumpOnce(0));
  }
  EXPECT_NEAR(stream.captureStats().driftPpm, -500.0, 20.0);
}

}  // namespace
}  // namespace t3k::usb

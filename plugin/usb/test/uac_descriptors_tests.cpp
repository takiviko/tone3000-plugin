// L0: UAC descriptor and entity-graph parsing against synthetic fixture
// blobs. Until the probe captures the AMS-22's real descriptors (plan M1.3
// saves them under fixtures/), these hand-built blobs model the AMS-22
// layout: UAC2, audio control + two streaming interfaces (playback with an
// explicit feedback endpoint, capture implicit), 24-bit subslot 3.

#include <cstdint>
#include <string>
#include <vector>

#include "UacDescriptors.h"
#include "gtest/gtest.h"

namespace t3k::usb {
namespace {

struct Blob {
  std::vector<uint8_t> bytes;
  void add(std::initializer_list<uint8_t> b) { bytes.insert(bytes.end(), b); }
  void u16(uint16_t v) {
    bytes.push_back(static_cast<uint8_t>(v & 0xff));
    bytes.push_back(static_cast<uint8_t>(v >> 8));
  }
};

// A UAC2 configuration shaped like the AMS-22 dump (4 interfaces: AC,
// AS playback, AS capture, MIDI). Descriptor layouts follow USB Audio 2.0
// chapter 4; lengths are what the parser is allowed to rely on.
Blob makeUac2Config() {
  Blob b;
  b.add({0x09, 0x02, 0x00, 0x00, 0x04, 0x01, 0x00, 0x80, 0xFA});  // config, 500 mA
  // Interface 0: AudioControl, UAC2 protocol 0x20.
  b.add({0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x20, 0x00});
  // AC header (bcdADC 2.00, wTotalLength patched below, category 0x04).
  b.add({0x09, 0x24, 0x01, 0x00, 0x02, 0x04, 0x00, 0x00, 0x00});
  // Clock source id 1: internal fixed, frequency control present.
  b.add({0x08, 0x24, 0x0A, 0x01, 0x01, 0x03, 0x00, 0x00});
  // Input terminal id 2 (USB streaming), 2 ch.
  b.add({0x10, 0x24, 0x02, 0x02, 0x01, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00});
  // Output terminal id 3 (speaker), source 6.
  b.add({0x0C, 0x24, 0x03, 0x03, 0x03, 0x02, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00});
  // Input terminal id 4 (microphone), 2 ch.
  b.add({0x10, 0x24, 0x02, 0x04, 0x02, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00});
  // Output terminal id 5 (USB streaming), source 4.
  b.add({0x0C, 0x24, 0x03, 0x05, 0x01, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00});
  // Feature unit id 6, source 2, master + 2 ch controls (4 bytes each).
  b.add({0x12, 0x24, 0x06, 0x06, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00});

  // Interface 1: AudioStreaming playback. alt0 = zero bandwidth.
  b.add({0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x20, 0x00});
  // alt1: two endpoints.
  b.add({0x09, 0x04, 0x01, 0x01, 0x02, 0x01, 0x02, 0x20, 0x00});
  b.add({0x07, 0x24, 0x01, 0x02, 0x01, 0x01, 0x00});  // AS general: terminal 2, PCM
  b.add({0x0C, 0x24, 0x02, 0x01, 0x02, 0x03, 0x18, 0x00, 0x00, 0x00, 0x00,
         0x00});  // Type I: 2 ch, subslot 3, 24 bit
  b.add({0x09, 0x05, 0x02, 0x05, 0x68, 0x00, 0x01, 0x00, 0x00});  // ep 0x02 isoc async
  b.add({0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00});              // CS endpoint
  b.add({0x09, 0x05, 0x82, 0x11, 0x04, 0x00, 0x04, 0x00, 0x00});  // ep 0x82 feedback

  // Interface 2: AudioStreaming capture. alt0 = zero bandwidth.
  b.add({0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x02, 0x20, 0x00});
  b.add({0x09, 0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x20, 0x00});
  b.add({0x07, 0x24, 0x01, 0x05, 0x01, 0x01, 0x00});  // AS general: terminal 5
  b.add({0x0C, 0x24, 0x02, 0x01, 0x02, 0x03, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00});
  b.add({0x09, 0x05, 0x81, 0x05, 0x68, 0x00, 0x01, 0x00, 0x00});  // ep 0x81 isoc data

  // Interface 3: MIDIStreaming (bulk). Out of scope; must be ignored.
  b.add({0x09, 0x04, 0x03, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00});
  b.add({0x09, 0x05, 0x03, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00});
  b.add({0x09, 0x05, 0x84, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00});

  // Patch wTotalLength of the configuration (bytes 2..3).
  const uint16_t total = static_cast<uint16_t>(b.bytes.size());
  b.bytes[2] = static_cast<uint8_t>(total & 0xff);
  b.bytes[3] = static_cast<uint8_t>(total >> 8);
  return b;
}

// A UAC1 configuration: one streaming interface with per-altsetting rates.
Blob makeUac1Config() {
  Blob b;
  b.add({0x09, 0x02, 0x00, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32});
  b.add({0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00});  // AC, protocol 0
  b.add({0x09, 0x24, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00});  // UAC1 AC header
  b.add({0x0C, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00,
         0x00});  // IT 1
  b.add({0x09, 0x24, 0x03, 0x02, 0x03, 0x02, 0x00, 0x01, 0x00});  // OT 2 <- IT 1

  // Interface 1: AudioStreaming, capture-ish direction for simplicity.
  b.add({0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00});  // alt0
  // alt1: 44.1/48 kHz.
  b.add({0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00});
  b.add({0x07, 0x24, 0x01, 0x02, 0x01, 0x01, 0x00});  // AS general: terminal 2
  b.add({0x0E, 0x24, 0x02, 0x01, 0x02, 0x03, 0x18, 0x02, 0x44, 0xAC, 0x00, 0x80,
         0xBB, 0x00});  // Type I: 2 rates
  b.add({0x09, 0x05, 0x81, 0x05, 0x68, 0x00, 0x01, 0x00, 0x00});

  const uint16_t total = static_cast<uint16_t>(b.bytes.size());
  b.bytes[2] = static_cast<uint8_t>(total & 0xff);
  b.bytes[3] = static_cast<uint8_t>(total >> 8);
  return b;
}

TEST(UacDescriptors, Uac2TreeStructure) {
  const Blob b = makeUac2Config();
  ConfigTree tree;
  ASSERT_TRUE(parseConfig(b.bytes.data(), b.bytes.size(), tree));
  EXPECT_EQ(tree.uacVersion(), UacVersion::uac2);
  EXPECT_EQ(tree.altSettings.size(), 6u);  // AC + 2x2 AS + MIDI
  EXPECT_EQ(tree.streamingAlts(true).size(), 1u);
  EXPECT_EQ(tree.streamingAlts(false).size(), 1u);
  EXPECT_TRUE(tree.busPowered);
  EXPECT_EQ(tree.maxPowerMa, 500u);
}

TEST(UacDescriptors, Uac2EntityGraph) {
  const Blob b = makeUac2Config();
  ConfigTree tree;
  ASSERT_TRUE(parseConfig(b.bytes.data(), b.bytes.size(), tree));
  const Entity* clock = tree.rateClockSource();
  ASSERT_NE(clock, nullptr);
  EXPECT_EQ(clock->id, 1);
  EXPECT_TRUE(clock->clockFrequencyControl);
  bool sawIn2 = false, sawOut3 = false, sawFeature6 = false;
  for (const Entity& e : tree.entities) {
    if (e.kind == Entity::Kind::inputTerminal && e.id == 2) {
      sawIn2 = true;
      EXPECT_EQ(e.terminalType, 0x0101);  // USB streaming
    }
    if (e.kind == Entity::Kind::outputTerminal && e.id == 3) {
      sawOut3 = true;
      EXPECT_EQ(e.sourceId, 6);
    }
    if (e.kind == Entity::Kind::featureUnit && e.id == 6) sawFeature6 = true;
  }
  EXPECT_TRUE(sawIn2);
  EXPECT_TRUE(sawOut3);
  EXPECT_TRUE(sawFeature6);
}

TEST(UacDescriptors, Uac2FormatAndEndpoints) {
  const Blob b = makeUac2Config();
  ConfigTree tree;
  ASSERT_TRUE(parseConfig(b.bytes.data(), b.bytes.size(), tree));
  const AltSetting* play = tree.streamingAlts(true)[0];
  EXPECT_EQ(play->altSetting, 1);
  EXPECT_EQ(play->format.channels, 2);
  EXPECT_EQ(play->format.subslotBytes, 3);
  EXPECT_EQ(play->format.bitResolution, 24);
  ASSERT_NE(play->dataEndpoint(), nullptr);
  EXPECT_EQ(play->dataEndpoint()->address, 0x02);
  EXPECT_EQ(play->dataEndpoint()->maxPacketSize, 104);
  EXPECT_TRUE(play->dataEndpoint()->isIsochronous());

  // Explicit feedback on playback (AMS-22 style), implicit on capture.
  const EndpointInfo* fb = tree.feedbackEndpointFor(true);
  ASSERT_NE(fb, nullptr);
  EXPECT_EQ(fb->address, 0x82);
  EXPECT_TRUE(fb->isFeedback());
  EXPECT_EQ(tree.feedbackEndpointFor(false), nullptr);
}

TEST(UacDescriptors, Uac2FindAltIsRateAgnostic) {
  const Blob b = makeUac2Config();
  ConfigTree tree;
  ASSERT_TRUE(parseConfig(b.bytes.data(), b.bytes.size(), tree));
  // UAC2 carries no rate list in the format descriptor: the altsetting is
  // selected by direction/channels and the rate is set on the clock source.
  EXPECT_EQ(tree.findAlt(true, 48000, 2)->altSetting, 1);
  EXPECT_EQ(tree.findAlt(true, 96000, 2)->altSetting, 1);
  EXPECT_EQ(tree.findAlt(false, 44100, 2)->altSetting, 1);
  EXPECT_EQ(tree.findAlt(false, 48000, 1), nullptr);  // channels do not match
}

TEST(UacDescriptors, Uac1RatesSelectAltsetting) {
  const Blob b = makeUac1Config();
  ConfigTree tree;
  ASSERT_TRUE(parseConfig(b.bytes.data(), b.bytes.size(), tree));
  EXPECT_EQ(tree.uacVersion(), UacVersion::uac1);
  const AltSetting* alt = tree.findAlt(false, 44100, 2);
  ASSERT_NE(alt, nullptr);
  EXPECT_EQ(alt->format.sampleRates.size(), 2u);
  EXPECT_EQ(alt->format.sampleRates[0], 44100u);
  EXPECT_EQ(alt->format.sampleRates[1], 48000u);
  EXPECT_NE(tree.findAlt(false, 48000, 2), nullptr);
  EXPECT_EQ(tree.findAlt(false, 96000, 2), nullptr);  // not in the list
  // No feedback endpoint: implicit/adaptive device.
  EXPECT_EQ(tree.feedbackEndpointFor(false), nullptr);
}

TEST(UacDescriptors, DescribeMentionsTheKeyFacts) {
  const Blob b = makeUac2Config();
  ConfigTree tree;
  ASSERT_TRUE(parseConfig(b.bytes.data(), b.bytes.size(), tree));
  const std::string text = tree.describe();
  EXPECT_NE(text.find("clock source"), std::string::npos);
  EXPECT_NE(text.find("[feedback]"), std::string::npos);
  EXPECT_NE(text.find("subslot=3"), std::string::npos);
  EXPECT_NE(text.find("uac=2.0"), std::string::npos);
}

TEST(UacDescriptors, ParseRateRangesWithSubRangeHeader) {
  // wNumSubRanges=2, then (44100,44100,0) and (48000,48000,0) as uint32 LE
  // min/max/resolution triplets.
  const uint8_t data[] = {
      0x02, 0x00,
      0x44, 0xAC, 0x00, 0x00, 0x44, 0xAC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x80, 0xBB, 0x00, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  std::vector<uint32_t> rates;
  ASSERT_TRUE(parseRateRanges(data, sizeof(data), rates));
  ASSERT_EQ(rates.size(), 2u);
  EXPECT_EQ(rates[0], 44100u);
  EXPECT_EQ(rates[1], 48000u);
}

TEST(UacDescriptors, TruncatedBlobsAreHandled) {
  ConfigTree tree;
  EXPECT_FALSE(parseConfig(nullptr, 0, tree));
  const uint8_t tiny[5] = {0x09, 0x02, 0x00, 0x00, 0x00};
  EXPECT_FALSE(parseConfig(tiny, sizeof(tiny), tree));
  // A blob whose tail is cut mid-descriptor parses the prefix without
  // crashing and reports success (the tree is partial but usable).
  const Blob b = makeUac2Config();
  ConfigTree partial;
  EXPECT_TRUE(parseConfig(b.bytes.data(), b.bytes.size() - 4, partial));
  EXPECT_FALSE(partial.altSettings.empty());
}

}  // namespace
}  // namespace t3k::usb

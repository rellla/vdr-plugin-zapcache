/**
 * @file test_zapbuffer.cpp
 * Unit tests for cZapBuffer
 *
 * @license{AGPL-3.0-or-later}
 */

#include <array>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <vdr/remux.h>

#include "../zapbuffer.h"

namespace {

/**
 * A TS packet with a pid and a counter at its end, so packets can be told apart
 *
 * With a pts, the packet starts a PES packet with this pts.
 */
std::array<uint8_t, TS_SIZE> Packet(int pid, int counter, int64_t pts = -1, bool scrambled = false)
{
	std::array<uint8_t, TS_SIZE> packet{};
	packet[0] = TS_SYNC_BYTE;
	packet[1] = ((pid >> 8) & 0x1F) | (pts >= 0 ? 0x40 : 0x00);
	packet[2] = pid & 0xFF;
	packet[3] = 0x10 | (scrambled ? 0x80 : 0x00) | (counter & 0x0F);
	if (pts >= 0) {
		uint8_t *pes = &packet[4];
		pes[2] = 0x01;
		pes[3] = 0xE0;
		pes[6] = 0x80;
		pes[7] = 0x80;          // PTS only
		pes[8] = 5;
		pes[9] = 0x21 | ((pts >> 29) & 0x0E);
		pes[10] = (pts >> 22) & 0xFF;
		pes[11] = 0x01 | ((pts >> 14) & 0xFE);
		pes[12] = (pts >> 7) & 0xFF;
		pes[13] = 0x01 | ((pts << 1) & 0xFE);
	}
	packet[TS_SIZE - 2] = counter & 0xFF;
	packet[TS_SIZE - 1] = (counter >> 8) & 0xFF;
	return packet;
}

/** Counter of the n-th packet in a buffer */
int CounterAt(const std::vector<uint8_t> &data, size_t n)
{
	return data[n * TS_SIZE + TS_SIZE - 2] | (data[n * TS_SIZE + TS_SIZE - 1] << 8);
}

/** no limit for the delay of a start with synchronous audio */
const int64_t NO_LIMIT = INT64_MAX;

/** Take all packets from the newest I-frame */
std::vector<uint8_t> TakeNewest(cZapBuffer &buffer, const uint8_t *next = nullptr)
{
	cZapBuffer::sTaken taken;
	return buffer.Take(next, cZapBuffer::NEWEST_I_FRAME, NO_LIMIT, &taken);
}

const int VPID = 0x100;
const int APID = 0x101;

const int V = cZapBuffer::VIDEO_PES_START;
const int F = cZapBuffer::FRAME_START;
const int I = cZapBuffer::INDEPENDENT_FRAME;
const int A = cZapBuffer::AUDIO_PES_START;

} // namespace

TEST_CASE("cZapBuffer - waits for an I-frame", "[zapbuffer]") {
	cZapBuffer buffer;

	buffer.Put(Packet(VPID, 1).data(), V | F);       // P-frame
	buffer.Put(Packet(APID, 2).data(), 0);
	REQUIRE_FALSE(buffer.Ready());
	REQUIRE(TakeNewest(buffer).empty());

	// an audio packet before a video PES start isn't kept
	buffer.Put(Packet(APID, 3).data(), 0);
	REQUIRE(buffer.Size() == 0);

	buffer.Put(Packet(VPID, 4).data(), V | F | I);   // I-frame
	buffer.Put(Packet(APID, 5).data(), 0);
	REQUIRE(buffer.Ready());
	REQUIRE(buffer.Frames() == 1);
	REQUIRE(buffer.Size() == 2 * TS_SIZE);
}

TEST_CASE("cZapBuffer - starts with the PES packet of the I-frame", "[zapbuffer]") {
	cZapBuffer buffer;

	// the frame detector detects the I-frame in the second packet of its PES packet
	buffer.Put(Packet(VPID, 1).data(), V | F);       // P-frame
	buffer.Put(Packet(VPID, 2).data(), V);           // PES start of the I-frame
	buffer.Put(Packet(APID, 3).data(), 0);
	REQUIRE_FALSE(buffer.Ready());
	buffer.Put(Packet(VPID, 4).data(), F | I);       // I-frame detected
	buffer.Put(Packet(VPID, 5).data(), V | F);       // P-frame
	REQUIRE(buffer.Ready());
	REQUIRE(buffer.Frames() == 2);

	std::vector<uint8_t> data = TakeNewest(buffer);
	REQUIRE(data.size() == 4 * TS_SIZE);
	REQUIRE(CounterAt(data, 0) == 2);
	REQUIRE(CounterAt(data, 3) == 5);

	// the same for an I-frame, while the buffer is ready
	buffer.Put(Packet(VPID, 6).data(), V | F | I);   // I-frame
	buffer.Put(Packet(VPID, 7).data(), V);           // PES start of the next I-frame
	buffer.Put(Packet(VPID, 8).data(), F | I);       // next I-frame detected
	data = TakeNewest(buffer);
	REQUIRE(data.size() == 2 * TS_SIZE);
	REQUIRE(CounterAt(data, 0) == 7);
}

TEST_CASE("cZapBuffer - an I-frame without PES start is ignored", "[zapbuffer]") {
	cZapBuffer buffer;

	buffer.Put(Packet(VPID, 1).data(), F | I);
	REQUIRE_FALSE(buffer.Ready());
	REQUIRE(buffer.Size() == 0);
}

TEST_CASE("cZapBuffer - starts with the newest I-frame", "[zapbuffer]") {
	cZapBuffer buffer;

	buffer.Put(Packet(VPID, 1).data(), V | F | I);   // I-frame
	buffer.Put(Packet(VPID, 2).data(), V | F);       // P-frame
	buffer.Put(Packet(APID, 3).data(), 0);
	REQUIRE(buffer.Frames() == 2);

	buffer.Put(Packet(VPID, 4).data(), V | F | I);   // next I-frame
	buffer.Put(Packet(VPID, 5).data(), 0);           // continuation
	buffer.Put(Packet(APID, 6).data(), 0);
	REQUIRE(buffer.IFrames() == 2);
	REQUIRE(buffer.Frames() == 3);

	cZapBuffer::sTaken taken;
	std::vector<uint8_t> data = buffer.Take(nullptr, cZapBuffer::NEWEST_I_FRAME, NO_LIMIT, &taken);
	REQUIRE(data.size() == 3 * TS_SIZE);
	REQUIRE(taken.frames == 1);
	REQUIRE_FALSE(taken.synced);
	REQUIRE(taken.delay == -1);    // no pts
	REQUIRE(CounterAt(data, 0) == 4);
	REQUIRE(CounterAt(data, 1) == 5);
	REQUIRE(CounterAt(data, 2) == 6);
}

TEST_CASE("cZapBuffer - starts with the newest I-frame, whose audio is buffered", "[zapbuffer]") {
	// the video is sent 1s ahead of the audio: the I-frames have the pts 0, 1s and 2s
	cZapBuffer buffer;
	buffer.Put(Packet(VPID, 1, 0).data(), V | F | I);
	buffer.Put(Packet(APID, 2, 0 - 90000).data(), A);
	buffer.Put(Packet(VPID, 3, 90000).data(), V | F | I);
	buffer.Put(Packet(APID, 4, 90000 - 90000).data(), A);
	buffer.Put(Packet(VPID, 5, 180000).data(), V | F | I);
	buffer.Put(Packet(APID, 6, 180000 - 90000 + 27000).data(), A);   // the audio reached 1.3s
	REQUIRE(buffer.AudioPts() == 117000);

	SECTION("the audio covers the I-frame with the start threshold") {
		cZapBuffer copy = buffer;
		cZapBuffer::sTaken taken;
		std::vector<uint8_t> data = copy.Take(nullptr, 400 * 90, NO_LIMIT, &taken);
		REQUIRE(taken.synced);
		REQUIRE(taken.frames == 3);
		REQUIRE(taken.delay == 180000);     // 2s to the newest video PES packet
		REQUIRE(CounterAt(data, 0) == 1);   // the I-frame at 0s (the audio covers it by 1.3s, the one at 1s by 0.3s)
	}

	SECTION("an I-frame with synchronous audio may delay the playback up to a limit") {
		cZapBuffer copy = buffer;
		cZapBuffer::sTaken taken;
		std::vector<uint8_t> data = copy.Take(nullptr, 400 * 90, 2000 * 90, &taken);
		REQUIRE(taken.synced);
		REQUIRE(CounterAt(data, 0) == 1);   // the delay of 2s is just allowed
	}

	SECTION("an I-frame, which delays the playback too much, isn't taken") {
		cZapBuffer copy = buffer;
		cZapBuffer::sTaken taken;
		std::vector<uint8_t> data = copy.Take(nullptr, 400 * 90, 1500 * 90, &taken);
		REQUIRE_FALSE(taken.synced);
		REQUIRE(taken.delay == 0);
		REQUIRE(CounterAt(data, 0) == 5);   // the newest I-frame
	}

	SECTION("a smaller threshold takes a newer I-frame") {
		cZapBuffer copy = buffer;
		cZapBuffer::sTaken taken;
		std::vector<uint8_t> data = copy.Take(nullptr, 300 * 90, NO_LIMIT, &taken);
		REQUIRE(taken.synced);
		REQUIRE(taken.frames == 2);
		REQUIRE(taken.delay == 90000);
		REQUIRE(CounterAt(data, 0) == 3);   // the I-frame at 1s (the audio covers it by 0.3s)
	}

	SECTION("no I-frame has enough audio") {
		cZapBuffer copy = buffer;
		cZapBuffer::sTaken taken;
		std::vector<uint8_t> data = copy.Take(nullptr, 2000 * 90, NO_LIMIT, &taken);
		REQUIRE_FALSE(taken.synced);
		REQUIRE(CounterAt(data, 0) == 5);   // the newest I-frame
	}
}

TEST_CASE("cZapBuffer - the pts difference wraps around", "[zapbuffer]") {
	REQUIRE(cZapBuffer::PtsDiff(1000, 4000) == 3000);
	REQUIRE(cZapBuffer::PtsDiff(4000, 1000) == -3000);
	REQUIRE(cZapBuffer::PtsDiff(MAX33BIT - 1000, 2000) == 3001);
	REQUIRE(cZapBuffer::PtsDiff(2000, MAX33BIT - 1000) == -3001);
}

TEST_CASE("cZapBuffer - Take() excludes the next packet", "[zapbuffer]") {
	SECTION("the buffer got the next packet already") {
		cZapBuffer buffer;
		buffer.Put(Packet(VPID, 1).data(), V | F | I);
		buffer.Put(Packet(APID, 2).data(), 0);
		auto next = Packet(VPID, 3);
		buffer.Put(next.data(), 0);

		std::vector<uint8_t> data = TakeNewest(buffer, next.data());
		REQUIRE(data.size() == 2 * TS_SIZE);
		REQUIRE(CounterAt(data, 1) == 2);
	}

	SECTION("the buffer didn't get the next packet yet") {
		cZapBuffer buffer;
		buffer.Put(Packet(VPID, 1).data(), V | F | I);
		buffer.Put(Packet(APID, 2).data(), 0);
		auto next = Packet(VPID, 3);

		std::vector<uint8_t> data = TakeNewest(buffer, next.data());
		REQUIRE(data.size() == 2 * TS_SIZE);
	}

	SECTION("the buffer is empty afterwards") {
		cZapBuffer buffer;
		buffer.Put(Packet(VPID, 1).data(), V | F | I);
		REQUIRE_FALSE(TakeNewest(buffer).empty());
		REQUIRE_FALSE(buffer.Ready());
		REQUIRE(buffer.Size() == 0);
		REQUIRE(TakeNewest(buffer).empty());
	}
}

TEST_CASE("cZapBuffer - a scrambled packet discards the buffer", "[zapbuffer]") {
	cZapBuffer buffer;

	buffer.Put(Packet(VPID, 1).data(), V | F | I);
	buffer.Put(Packet(APID, 2, -1, true).data(), 0);
	REQUIRE_FALSE(buffer.Ready());
	REQUIRE(buffer.Size() == 0);

	// the following packets of the frame are dropped, too
	buffer.Put(Packet(APID, 3).data(), 0);
	REQUIRE(buffer.Size() == 0);

	buffer.Put(Packet(VPID, 4).data(), V | F | I);
	REQUIRE(buffer.Ready());
}

TEST_CASE("cZapBuffer - the oldest I-frames are dropped", "[zapbuffer]") {
	cZapBuffer buffer;

	// GOPs of 50 frames
	for (int i = 0; i < cZapBuffer::MAX_FRAMES; i++)
		buffer.Put(Packet(VPID, i).data(), V | F | (i % 50 ? 0 : I));
	REQUIRE(buffer.IFrames() == cZapBuffer::MAX_FRAMES / 50);
	REQUIRE(buffer.Frames() == cZapBuffer::MAX_FRAMES);

	buffer.Put(Packet(VPID, cZapBuffer::MAX_FRAMES).data(), V | F | I);
	REQUIRE(buffer.IFrames() == cZapBuffer::MAX_FRAMES / 50);
	REQUIRE(buffer.Frames() == cZapBuffer::MAX_FRAMES - 50 + 1);
	REQUIRE(buffer.Size() == static_cast<size_t>(buffer.Frames()) * TS_SIZE);

	// the buffer starts with the oldest kept I-frame
	cZapBuffer::sTaken taken;
	std::vector<uint8_t> data = buffer.Take(nullptr, 0, NO_LIMIT, &taken);
	REQUIRE_FALSE(taken.synced);    // no audio
	REQUIRE(CounterAt(data, 0) == cZapBuffer::MAX_FRAMES);
}

TEST_CASE("cZapBuffer - a too long GOP discards the buffer", "[zapbuffer]") {
	cZapBuffer buffer;

	buffer.Put(Packet(VPID, 0).data(), V | F | I);
	for (int i = 1; i < cZapBuffer::MAX_FRAMES; i++)
		buffer.Put(Packet(VPID, i).data(), V | F);
	REQUIRE(buffer.Ready());
	REQUIRE(buffer.Frames() == cZapBuffer::MAX_FRAMES);

	buffer.Put(Packet(VPID, cZapBuffer::MAX_FRAMES).data(), V | F);
	REQUIRE_FALSE(buffer.Ready());
	REQUIRE(buffer.Size() == 0);

	// it waits for the next I-frame
	buffer.Put(Packet(VPID, 1000).data(), V | F);
	REQUIRE_FALSE(buffer.Ready());
	buffer.Put(Packet(VPID, 1001).data(), V | F | I);
	REQUIRE(buffer.Ready());
	REQUIRE(buffer.Frames() == 1);
}

TEST_CASE("cZapBuffer - the size is limited", "[zapbuffer]") {
	cZapBuffer buffer;
	const size_t maxPackets = cZapBuffer::MAX_BYTES / TS_SIZE;

	buffer.Put(Packet(VPID, 0).data(), V | F | I);
	for (size_t i = 1; i < maxPackets; i++)
		buffer.Put(Packet(APID, i).data(), 0);
	REQUIRE(buffer.Ready());
	REQUIRE(buffer.Size() == maxPackets * TS_SIZE);

	buffer.Put(Packet(APID, 0).data(), 0);
	REQUIRE_FALSE(buffer.Ready());
	REQUIRE(buffer.Size() == 0);
}

TEST_CASE("cZapBuffer - a radio channel starts with an audio PES packet", "[zapbuffer]") {
	cZapBuffer buffer;
	buffer.SetRadio(true);
	buffer.Put(Packet(APID, 1).data(), 0);              // the rest of a PES packet
	REQUIRE_FALSE(buffer.Ready());
	buffer.Put(Packet(APID, 2, 9000).data(), A);
	buffer.Put(Packet(APID, 3).data(), 0);
	REQUIRE(buffer.Ready());
	REQUIRE(buffer.Frames() == 1);

	std::vector<uint8_t> data = TakeNewest(buffer);
	REQUIRE(data.size() == 2 * TS_SIZE);
	REQUIRE(CounterAt(data, 0) == 2);
}

TEST_CASE("cZapBuffer - a radio channel starts with the audio up to the start threshold", "[zapbuffer]") {
	// audio PES packets every 200ms from 0 to 1s
	cZapBuffer buffer;
	buffer.SetRadio(true);
	for (int i = 0; i <= 5; i++) {
		buffer.Put(Packet(APID, 2 * i + 1, i * 18000).data(), A);
		buffer.Put(Packet(APID, 2 * i + 2).data(), 0);
	}
	REQUIRE(buffer.AudioMs() == 1000);

	SECTION("the newest PES packet with enough audio") {
		cZapBuffer::sTaken taken;
		std::vector<uint8_t> data = buffer.Take(nullptr, 500 * 90, NO_LIMIT, &taken);
		REQUIRE(taken.synced);
		REQUIRE(taken.frames == 4);
		REQUIRE(taken.delay == 600 * 90);
		REQUIRE(CounterAt(data, 0) == 5);   // the PES packet at 400ms (600ms audio up to the newest one)
	}

	SECTION("not enough audio: as much as possible") {
		cZapBuffer::sTaken taken;
		std::vector<uint8_t> data = buffer.Take(nullptr, 2000 * 90, NO_LIMIT, &taken);
		REQUIRE_FALSE(taken.synced);
		REQUIRE(taken.frames == 6);
		REQUIRE(CounterAt(data, 0) == 1);
	}

	SECTION("not older than the max. delay") {
		cZapBuffer::sTaken taken;
		std::vector<uint8_t> data = buffer.Take(nullptr, 2000 * 90, 700 * 90, &taken);
		REQUIRE_FALSE(taken.synced);
		REQUIRE(taken.delay == 600 * 90);
		REQUIRE(CounterAt(data, 0) == 5);   // the PES packet at 400ms, 600ms before the newest one
	}
}

TEST_CASE("cZapBuffer - a radio channel keeps the audio of the last seconds", "[zapbuffer]") {
	cZapBuffer buffer;
	buffer.SetRadio(true);
	for (int i = 0; i < 50; i++)
		buffer.Put(Packet(APID, i, i * 9000).data(), A);    // every 100ms, 4.9s

	// at least RADIO_MAX_AUDIO, the next PES packet would be less
	REQUIRE(buffer.AudioMs() == cZapBuffer::RADIO_MAX_AUDIO / 90);

	// all buffered audio from 1.9s up to 4.9s
	std::vector<uint8_t> data = TakeNewest(buffer);
	REQUIRE(data.size() == 31 * TS_SIZE);
	REQUIRE(CounterAt(data, 0) == 19);
}

// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file zapbuffer.cpp
 * Zap Buffer
 *
 * @copyright 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <algorithm>
#include <cstring>

#include <vdr/remux.h>

#include "zapbuffer.h"

/**
 * Get the pts of a TS packet, which starts a PES packet
 *
 * @param packet    TS packet
 *
 * @return          the pts (-1: no PES start or no pts)
 */
int64_t cZapBuffer::PacketPts(const uint8_t *packet)
{
	if (!TsPayloadStart(packet))
		return -1;

	const uchar *pes = packet;
	int length = TsGetPayload(&pes);
	if (length < 14 || !PesHasPts(pes))
		return -1;

	return PesGetPts(pes);
}

/**
 * Get the difference of two pts values (33 bit with wrap around)
 *
 * @return          pts2 - pts1
 */
int64_t cZapBuffer::PtsDiff(int64_t pts1, int64_t pts2)
{
	int64_t diff = pts2 - pts1;
	if (diff > MAX33BIT / 2)
		return diff - (MAX33BIT + 1);
	if (diff < -MAX33BIT / 2)
		return diff + (MAX33BIT + 1);

	return diff;
}

/**
 * Add a TS packet
 *
 * A scrambled packet discards the buffer until the next I-frame. The oldest
 * I-frames are dropped, if the buffer gets too large. If only one I-frame is
 * buffered (a too long GOP), the buffer is discarded until the next one.
 *
 * @param packet            TS packet (TS_SIZE bytes)
 * @param flags             flags of the packet (enum Flags)
 */
void cZapBuffer::Put(const uint8_t *packet, int flags)
{
	// a channel switch can't start with scrambled data
	if (TsIsScrambled(packet)) {
		Reset();
		return;
	}

	if (flags & VIDEO_PES_START) {
		// while waiting for an I-frame, only the PES packet is kept, which may contain it
		if (m_iFrames.empty())
			m_data.clear();
		m_pesStart = m_data.size();
		m_pesPts = PacketPts(packet);
	}

	if ((flags & FRAME_START) && (flags & INDEPENDENT_FRAME)) {
		if (m_pesStart == NO_PES_START) {
			Reset();
			return;
		}
		// the first I-frame starts the buffer with its PES packet
		if (m_iFrames.empty()) {
			m_data.erase(m_data.begin(), m_data.begin() + m_pesStart);
			m_pesStart = 0;
			m_frames = 0;
		}
		m_iFrames.push_back({ m_pesStart, m_pesPts, m_frames });
	}

	if (m_iFrames.empty()) {
		if (m_pesStart == NO_PES_START)
			return;
	} else {
		if (flags & FRAME_START)
			m_frames++;

		// the audio before the first I-frame isn't played
		if (flags & AUDIO_PES_START) {
			int64_t pts = PacketPts(packet);
			if (pts >= 0)
				m_audioPts = pts;
		}
	}

	while (m_frames > MAX_FRAMES || m_data.size() + TS_SIZE > MAX_BYTES) {
		if (m_iFrames.size() < 2) {
			Reset();
			return;
		}
		DropOldestIFrame();
	}

	m_data.insert(m_data.end(), packet, packet + TS_SIZE);
}

/**
 * Drop the packets before the second I-frame
 */
void cZapBuffer::DropOldestIFrame(void)
{
	m_iFrames.pop_front();

	size_t offset = m_iFrames.front().offset;
	int frame = m_iFrames.front().frame;

	m_data.erase(m_data.begin(), m_data.begin() + offset);
	for (sIFrame &iFrame : m_iFrames) {
		iFrame.offset -= offset;
		iFrame.frame -= frame;
	}
	if (m_pesStart != NO_PES_START)
		m_pesStart -= offset;
	m_frames -= frame;
}

/**
 * Take the buffered packets, which precede a given packet
 *
 * The given packet is played next. It is the last buffered one, if the buffer
 * got it before (the receivers of a device get the same packets one after
 * another), otherwise all buffered packets precede it. The buffer is empty
 * afterwards.
 *
 * @param next          TS packet, which is played next (may be nullptr)
 * @param audioAhead    start with the newest I-frame, whose pts is at least this
 *                      much (in 90kHz ticks) before the pts of the last audio PES
 *                      packet, so its synchronous audio is buffered (NEWEST_I_FRAME:
 *                      start with the newest I-frame). If no I-frame has enough
 *                      audio, the buffer starts with the newest one.
 * @param maxDelay      an I-frame with synchronous audio is only taken, if it is at
 *                      most this much (in 90kHz ticks) before the newest video PES
 *                      packet, otherwise the buffer starts with the newest I-frame
 * @param[out] taken    the frames, the delay and if the audio is synchronous
 *
 * @return              the packets from the I-frame up to the next packet
 *                      (empty, if no I-frame is buffered)
 */
std::vector<uint8_t> cZapBuffer::Take(const uint8_t *next, int64_t audioAhead, int64_t maxDelay, sTaken *taken)
{
	std::vector<uint8_t> data;
	*taken = sTaken();

	if (!m_iFrames.empty()) {
		const sIFrame *start = &m_iFrames.back();
		if (audioAhead != NEWEST_I_FRAME && m_audioPts >= 0) {
			for (auto it = m_iFrames.rbegin(); it != m_iFrames.rend(); ++it) {
				if (it->pts >= 0 && PtsDiff(it->pts, m_audioPts) >= audioAhead) {
					// an older I-frame would delay the playback even more
					if (m_pesPts >= 0 && PtsDiff(it->pts, m_pesPts) > maxDelay)
						break;
					start = &*it;
					taken->synced = true;
					break;
				}
			}
		}

		taken->frames = m_frames - start->frame;
		if (start->pts >= 0 && m_pesPts >= 0)
			taken->delay = std::max<int64_t>(0, PtsDiff(start->pts, m_pesPts));
		data.assign(m_data.begin() + start->offset, m_data.end());
		if (next && data.size() >= TS_SIZE && !memcmp(&data[data.size() - TS_SIZE], next, TS_SIZE))
			data.resize(data.size() - TS_SIZE);
	}

	Reset();

	return data;
}

/**
 * Discard the buffered packets and wait for the next I-frame
 */
void cZapBuffer::Reset(void)
{
	m_data.clear();
	m_iFrames.clear();
	m_pesStart = NO_PES_START;
	m_pesPts = -1;
	m_frames = 0;
	m_audioPts = -1;
}

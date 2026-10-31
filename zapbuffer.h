// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file zapbuffer.h
 * Zap Buffer Header File
 *
 * @copyright 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __ZAPCACHE_ZAPBUFFER_H
#define __ZAPCACHE_ZAPBUFFER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

/**
 * Buffer for the TS packets of a channel since an independent frame
 *
 * The packets of all pids are kept in their order, starting with an
 * independent frame (I-frame). A channel switch can start with an I-frame at
 * once instead of waiting for the next one. It doesn't depend on VDR, the
 * frames are detected by the caller.
 *
 * The buffer keeps the last I-frames within MAX_FRAMES frames. The video of a
 * channel is sent ahead of its audio, so the audio of the newest I-frame may
 * not be received yet. Take() can start with the newest I-frame, whose audio
 * is buffered already, so the playback starts with synchronous audio at once.
 * The older the I-frame, the more the playback is delayed, so the delay is
 * limited.
 *
 * The buffer starts with the video PES packet of an I-frame: a frame detector
 * may detect the start of a frame in a later TS packet of the PES packet (e.g.
 * VDR's H.265 parser, if the slice follows the parameter sets).
 *
 * The buffer of a radio channel (SetRadio()) keeps the audio of the last
 * RADIO_MAX_AUDIO ticks instead, it starts with an audio PES packet. A channel
 * switch starts with as much audio, as the output device needs to start the
 * playback.
 *
 * @ingroup zapcache
 */
class cZapBuffer {
public:
	/** flags of a TS packet for Put() */
	enum Flags {
		VIDEO_PES_START   = 1,          ///< the packet starts a video PES packet
		FRAME_START       = 2,          ///< a video frame starts in the packet
		INDEPENDENT_FRAME = 4,          ///< the started frame is an independent frame (I-frame)
		AUDIO_PES_START   = 8,          ///< the packet starts an audio PES packet
	};

	/** max. frames since the oldest I-frame (the device buffers them in addition to the live data, 3s at 50 fps) */
	static constexpr int MAX_FRAMES = 150;
	static constexpr size_t MAX_BYTES = 12 * 1024 * 1024; ///< max. size of the buffered packets
	static constexpr int64_t NEWEST_I_FRAME = -1;         ///< Take(): start with the newest I-frame
	static constexpr int64_t RADIO_MAX_AUDIO = 3 * 90000; ///< buffered audio of a radio channel (90kHz ticks)

	/** the taken packets (see Take()) */
	struct sTaken {
		int frames = 0;                 ///< number of the frames (radio: audio PES packets)
		bool synced = false;            ///< the packets start with an I-frame with synchronous audio
		int64_t delay = -1;             ///< time from the I-frame to the newest video PES packet (90kHz ticks, -1: unknown)
	};

	void SetRadio(bool radio) { m_radio = radio; }          ///< buffer a radio channel (only the audio)
	void Put(const uint8_t *, int);
	std::vector<uint8_t> Take(const uint8_t *, int64_t, int64_t, sTaken *);
	void Reset(void);

	bool Ready(void) const { return !m_iFrames.empty(); }    ///< true, if the buffer starts with an I-frame (radio: with audio)
	bool Radio(void) const { return m_radio; }               ///< true, if the buffer keeps a radio channel
	int Frames(void) const { return m_frames; }              ///< number of frames since the oldest I-frame (radio: audio PES packets)
	int AudioMs(void) const;
	int IFrames(void) const { return m_iFrames.size(); }     ///< number of the buffered I-frames
	size_t Size(void) const { return m_data.size(); }        ///< size of the buffered packets in bytes
	int64_t AudioPts(void) const { return m_audioPts; }      ///< pts of the last audio PES packet (-1: none)

	static int64_t PacketPts(const uint8_t *);
	static int64_t PtsDiff(int64_t, int64_t);

private:
	static constexpr size_t NO_PES_START = static_cast<size_t>(-1); ///< m_pesStart: no video PES packet started

	/** a buffered I-frame (radio: an audio PES packet) */
	struct sIFrame {
		size_t offset;              ///< offset of its video PES packet in m_data
		int64_t pts;                ///< pts of its video PES packet (-1: none, radio: pts of the audio PES packet)
		int frame;                  ///< number of the frames before it in the buffer
	};

	std::vector<uint8_t> m_data;      ///< TS packets since the oldest I-frame (or since the last video PES start, while not ready)
	std::deque<sIFrame> m_iFrames;    ///< buffered I-frames (the oldest one starts m_data)
	size_t m_pesStart = NO_PES_START; ///< offset of the last video PES start in m_data
	int64_t m_pesPts = -1;            ///< pts of the last video PES start
	int m_frames = 0;                 ///< number of frames since the oldest I-frame (including it)
	int64_t m_audioPts = -1;          ///< pts of the last audio PES start
	bool m_radio = false;             ///< a radio channel, only the audio is buffered

	void DropOldestIFrame(void);
	void PutRadio(const uint8_t *, int);
};

#endif

// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file zapcache.h
 * Zap Cache Header File
 *
 * @copyright 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __ZAPCACHE_ZAPCACHE_H
#define __ZAPCACHE_ZAPCACHE_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <vdr/receiver.h>
#include <vdr/remux.h>
#include <vdr/tools.h>

#include "zapbuffer.h"

struct sZapCacheSetup;

class cChannel;
class cChannels;
class cDevice;

/**
 * Zap cache
 * @defgroup zapcache Zap Cache
 */

/**
 * The data of a channel, which a zap receiver needs
 *
 * A copy, so a receiver can be created without the channels lock.
 *
 * @ingroup zapcache
 */
struct sZapChannel {
	int number = 0;                 ///< channel number (0: none)
	std::string name;               ///< channel name (for the log)
	int vpid = 0;                   ///< video pid
	int vtype = 0;                  ///< video stream type
	int ppid = 0;                   ///< pcr pid
	std::vector<int> apids;         ///< audio pids
	std::vector<int> dpids;         ///< dolby pids
	std::vector<int> spids;         ///< subtitle pids

	static sZapChannel FromChannel(const cChannel *);
};

/**
 * Receiver for the channel, which is expected to be switched to next
 *
 * It receives the pids of the channel like VDR's transfer receiver does, so
 * the tuner of the channel is locked and the pids are streamed, before the
 * channel is switched to. It has the lowest priority, so it never blocks a
 * recording or the live view.
 *
 * The receiver of an encrypted channel may have a CAM (zapcache.Cam):
 * it gets the channel and a priority above the lowest one, so VDR decrypts it
 * on the device (a receiver with the lowest priority isn't decrypted).
 *
 * The packets since the last I-frame are kept in a cZapBuffer, so a channel
 * switch can start with them (see cZapCache::Splice()). A radio channel keeps
 * the audio of the last seconds.
 *
 * @ingroup zapcache
 */
class cZapReceiver : public cReceiver {
public:
	static constexpr int CAM_PRIORITY = MINPRIORITY + 1;   ///< priority of a receiver with a CAM (VDR decrypts it)

	cZapReceiver(const sZapChannel &, cDevice *, const cChannel * = nullptr);
	virtual ~cZapReceiver(void);

	int ChannelNumber(void) const { return m_channelNumber; }                ///< number of the received channel
	cDevice *Device(void) const { return m_attached ? m_pDevice : nullptr; } ///< the receiving device (nullptr, if detached)
	bool HasPid(int) const;
	bool IsRadio(void) const { return !m_vpid; }                             ///< true, if a radio channel is received
	bool IsTaken(void);
	void Info(int *, bool *);
	std::vector<uint8_t> Take(const uint8_t *, int64_t, int64_t, cZapBuffer::sTaken *);

protected:
	virtual void Activate(bool);
	virtual void Receive(const uchar *, int);

private:
	int m_channelNumber;                    ///< number of the received channel
	cDevice *m_pDevice;                     ///< device, which receives the channel
	std::atomic<bool> m_attached = false;   ///< the receiver is attached (VDR may detach it, if it needs the device)
	std::vector<int> m_pids;                ///< received pids
	std::vector<int> m_audioPids;           ///< received audio pids
	int m_vpid;                             ///< video pid (0: radio channel, only the audio is buffered)

	std::mutex m_mutex;                     ///< protects the members below (receiver thread and Take())
	cFrameDetector m_frameDetector;         ///< detects the frames of the video pid
	std::vector<uint8_t> m_pending;         ///< packets, which the frame detector needs ahead
	cZapBuffer m_buffer;                    ///< packets since the last I-frame
	bool m_taken = false;                   ///< the buffer was taken, the receiver doesn't buffer anymore

	void AddChannelPid(int);
	int PesStartFlags(const uchar *) const;
};

/**
 * Tunes the neighbour channels of the live channel on free devices ahead
 *
 * After a channel switch the next channel in the direction of the last channel
 * switch is received by a cZapReceiver on a free device. After a switch to
 * another channel (or the first switch after VDR's start) the zap direction is
 * up. VDR prefers a device, which is tuned to the transponder of a channel, so
 * a switch to this channel doesn't have to tune and to wait for the tuner lock.
 *
 * The next channel doesn't take the device, which is tuned to the channel in the
 * opposite direction (usually the device of the last live channel), if another
 * device is free. So a switch back doesn't have to tune.
 *
 * In ZAP_AHEAD_BOTH mode (three tuners: live view, next and previous channel)
 * the channel in the opposite direction is received, too, preferably on the
 * device, which is tuned to it already. The live channel has a receiver on the
 * device of its transfer, too. VDR stops the transfer, before it reports a
 * channel switch, so this receiver keeps the stream and the buffer of the last
 * live channel, a switch back can start with it at once.
 *
 * The receivers of the channels, which are neighbours after a channel switch,
 * too, are kept with their buffers.
 *
 * The switch to this channel starts with the packets since a buffered I-frame,
 * before VDR's transfer receiver plays its first packet (Splice()), so it
 * doesn't have to wait for the next I-frame. It may start with an older I-frame,
 * whose synchronous audio is buffered, too, so the audio doesn't have to be
 * waited for.
 *
 * An encrypted channel is only tuned, its scrambled packets aren't buffered. With
 * zapcache.Cam its receiver gets a CAM like a recording with the lowest
 * priority, if a CAM can decrypt it on a device, which isn't used by the live
 * view or a recording (see CamDevice()).
 *
 * While a channel number is entered, the entered channel is received, too (on a
 * free device or on a device of the neighbour channels, which aren't neighbours
 * after the switch anymore), so the switch may start with its buffer, too.
 *
 * The receivers are released after a time without key press, so VDR's EPG scan
 * (which starts 60s after the last key press) can use the devices. The next key
 * press tunes the channels again, but not, while a menu is shown.
 *
 * The receiver is started and stopped by the main thread (MainThreadHook()),
 * like VDR's own channel switches. The channel switches are reported by
 * cPluginZapcache::ChannelSwitch() (cStatus), which may be called by another
 * thread (SVDRP). The output device starts a channel switch with the buffered
 * packets through the service interface (see zapcache_service.h).
 *
 * @ingroup zapcache
 */
class cZapCache {
public:
	/** statistics of the zap cache (see GetStats()) */
	struct sStats {
		/** a receiver */
		struct sReceiver {
			int channel;            ///< channel number
			int device;             ///< device number (starting with 1)
			int frames;             ///< buffered frames (radio: buffered audio in ms)
			bool ready;             ///< an I-frame (radio: audio) is buffered
			bool radio;             ///< a radio channel
		};
		std::vector<sReceiver> receivers;   ///< current receivers
		int switches = 0;                   ///< channel switches with the zap cache
		int cached = 0;                     ///< switches, which started with a buffer
		int synced = 0;                     ///< switches, which started with synchronous audio
		int pictureMs[2] = {};              ///< average time until the first picture [0: without buffer, 1: with buffer] (0: unknown)
		int startMs[2] = {};                ///< average time until the playback started [0: without buffer, 1: with buffer] (0: unknown)
		int delayMs = 0;                    ///< average delay of the switches with buffer (0: unknown, see cZapBuffer::sTaken::delay)
		double corrections[2] = { -1, -1 }; ///< average frame corrections of the A/V sync after a switch [0: without buffer, 1: with buffer] (-1: unknown)
	};

	/** the channels, which are tuned ahead (zapcache.Tuners) */
	enum Mode {
		ZAP_AHEAD_OFF,                     ///< no channel
		ZAP_AHEAD_NEXT,                    ///< the next channel in zap direction (two tuners)
		ZAP_AHEAD_BOTH                     ///< the next channel and the channel in the opposite direction (three tuners)
	};

	cZapCache(void);
	~cZapCache(void);

	static int NumTuners(void);
	static int ReleaseSeconds(int);
	static int MaxDelayMs(int);

	void ChannelSwitchStarted(void);
	void SetMode(int mode) { m_mode = mode; }                     ///< set the current mode (enum Mode)
	void ChannelSwitched(int);
	void ChannelEntered(int);
	void MainThreadHook(int, const sZapCacheSetup &);
	void Clear(void);

	void ReportSwitchEvent(int);
	void ReportFrameCorrection(int, int);
	void GetStats(sStats *);
	void ResetStats(void);

	bool SpliceArmed(void) const { return m_spliceArmed; }   ///< true, if a channel switch may start with the buffered packets
	int Splice(const uchar *, int, int (*)(void *, const uchar *, int), void *);

private:
	static constexpr int PREPARE_DELAY_MS = 500;   ///< delay after a channel switch (the live view gets the tuner first)
	static constexpr int SPLICE_TIMEOUT_MS = 3000; ///< max. time from a channel switch until the transfer plays its first packet
	static constexpr int PLAY_RETRIES = 20;        ///< max. number of retries for a buffered packet (like cTransfer)
	static constexpr int PLAY_RETRY_WAIT_MS = 5;   ///< time between two retries
	static constexpr int MAX_RECEIVERS = 2;        ///< zap direction and opposite direction
	static constexpr int ALL_RECEIVERS = MAX_RECEIVERS + 2; ///< with the receivers of the live channel and of the entered channel
	static constexpr int RELEASE_TIMES = 5;        ///< number of the release times (zapcache.Release)
	static constexpr int MAX_DELAYS = 5;           ///< number of the max. delays (zapcache.MaxDelay)

	std::mutex m_mutex;                      ///< protects the members below
	/** receivers of the next channel and of the channel in the opposite direction */
	cZapReceiver *m_pReceivers[MAX_RECEIVERS] = {};
	int m_liveChannel = 0;                   ///< current live channel
	int m_previousChannel = 0;               ///< live channel before the last channel switch
	bool m_preparePending = false;           ///< a channel was switched, the next channel has to be tuned
	cTimeMs m_prepareTimer;                  ///< delays the tuning of the next channel after a channel switch
	int m_direction = 1;                     ///< zap direction (1: up, -1: down)
	std::atomic<bool> m_spliceArmed = false; ///< a channel switch may start with the buffered packets
	int m_armedFromChannel = 0;              ///< live channel before the channel switch (its packets may still be played)
	cTimeMs m_spliceTimer;                   ///< keeps the receiver of the new live channel until the splice
	cZapReceiver *m_pLive = nullptr;         ///< receiver of the live channel (ZAP_AHEAD_BOTH), it keeps its buffer after a switch
	cZapReceiver *m_pEntered = nullptr;      ///< receiver of the channel, whose number is entered
	int m_enteredChannel = 0;                ///< number of the channel, which is entered (0: none)
	bool m_enterPending = false;             ///< the entered channel changed, it has to be tuned
	std::atomic<int> m_mode = ZAP_AHEAD_OFF; ///< current mode (enum Mode)
	bool m_cam = false;                      ///< encrypted channels are received with a CAM (main thread only)
	std::atomic<bool> m_syncAudio = true;    ///< start a switch with synchronous audio (zapcache.SyncAudio)
	std::atomic<int> m_maxDelayMs = 2000;    ///< max. delay of a start with synchronous audio (zapcache.MaxDelay)

	// statistics
	int m_switches = 0;                      ///< channel switches with the zap cache
	int m_cached = 0;                        ///< switches, which started with a buffer
	int m_synced = 0;                        ///< switches, which started with synchronous audio
	bool m_measuring = false;                ///< the times of the current switch are measured
	bool m_switchCached = false;             ///< the current switch started with a buffer
	bool m_pictureReported = false;          ///< the time until the first picture of the current switch was reported
	int64_t m_pictureSumMs[2] = {};          ///< sum of the times until the first picture [without buffer, with buffer]
	int m_pictureCount[2] = {};              ///< number of these times
	int64_t m_startSumMs[2] = {};            ///< sum of the times until the playback started [without buffer, with buffer]
	int m_startCount[2] = {};                ///< number of these times
	int64_t m_delaySumMs = 0;                ///< sum of the delays of the switches with buffer
	int m_delayCount = 0;                    ///< number of these delays
	std::chrono::steady_clock::time_point m_switchStart;   ///< start of the current switch
	bool m_countingCorrections = false;      ///< the frame corrections of the current switch are counted (after its playback start)
	std::atomic<bool> m_correctionWindow = false;          ///< the frame corrections are counted now (ReportFrameCorrection())
	std::chrono::steady_clock::time_point m_correctionEnd; ///< end of the time, in which the corrections are counted
	int m_corrections = 0;                   ///< frame corrections of the current switch
	int64_t m_correctionSum[2] = {};         ///< sum of the frame corrections after a switch [without buffer, with buffer]
	int m_correctionCount[2] = {};           ///< number of these switches
	time_t m_lastSwitch;                     ///< time of the last channel switch (also by SVDRP)
	bool m_released = false;                 ///< the receivers were released without key press (main thread only)

	void FinishCorrections(bool);
	bool HasReceivers(void) const;
	cZapReceiver *ReceiverFor(int) const;
	void Prepare(int, int, int);
	void TuneEntered(int);
	static const cChannel *Neighbour(const cChannels *, const cChannel *, int);
	cDevice *DeviceFor(const cChannel *, const cDevice *, bool *) const;
	static cDevice *FreeDevice(const cChannel *, const cDevice *);
	static cDevice *CamDevice(const cChannel *);
	static cDevice *TunedDevice(const cChannel *);
	static cZapReceiver *Attach(const cChannel *, cDevice *, bool, bool = false);
	static cZapReceiver *Keep(cZapReceiver **, int, const cChannel *);
};

#endif

// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file zapcache.cpp
 * Zap Cache
 *
 * @copyright 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <algorithm>
#include <ctime>

#include <vdr/channels.h>
#include <vdr/config.h>
#include <vdr/device.h>
#include <vdr/remote.h>
#include <vdr/skins.h>
#include <vdr/transfer.h>

#include "setup.h"
#include "zapcache.h"

/*****************************************************************************
 * cZapReceiver class
 ****************************************************************************/

/**
 * Create a receiver for the pids of a channel
 *
 * The receiver gets the same pids as VDR's transfer receiver. Without a CAM
 * channel it doesn't pass a channel to cReceiver and has the lowest priority,
 * so no CAM is started for it.
 *
 * @param channel    channel to receive
 * @param device     device, which receives the channel
 * @param camChannel the channel, if a CAM of the device decrypts it (the caller
 *                   holds the channels lock)
 */
cZapReceiver::cZapReceiver(const sZapChannel &channel, cDevice *device, const cChannel *camChannel)
	: cReceiver(camChannel, camChannel ? CAM_PRIORITY : MINPRIORITY),
	  m_channelNumber(channel.number),
	  m_pDevice(device),
	  m_vpid(channel.vpid)
{
	AddChannelPid(channel.vpid);
	AddChannelPid(channel.ppid);
	for (int pid : channel.apids) {
		AddChannelPid(pid);
		m_audioPids.push_back(pid);
	}
	for (int pid : channel.dpids) {
		AddChannelPid(pid);
		m_audioPids.push_back(pid);
	}
	for (int pid : channel.spids)
		AddChannelPid(pid);

	if (m_vpid)
		m_frameDetector.SetPid(m_vpid, channel.vtype);

	dsyslog("zapcache: receive channel %d (%s) on device %d%s", m_channelNumber, channel.name.c_str(), device->DeviceNumber() + 1, camChannel ? " with CAM" : "");
}

/**
 * Copy the data of a channel (the caller holds the channels lock)
 */
sZapChannel sZapChannel::FromChannel(const cChannel *channel)
{
	sZapChannel c;

	c.number = channel->Number();
	c.name = channel->Name();
	c.vpid = channel->Vpid();
	c.vtype = channel->Vtype();
	c.ppid = channel->Ppid();
	for (const int *pid = channel->Apids(); *pid; pid++)
		c.apids.push_back(*pid);
	for (const int *pid = channel->Dpids(); *pid; pid++)
		c.dpids.push_back(*pid);
	for (const int *pid = channel->Spids(); *pid; pid++)
		c.spids.push_back(*pid);

	return c;
}

/**
 * Detach the receiver
 */
cZapReceiver::~cZapReceiver(void)
{
	Detach();
}

/**
 * Add a pid of the channel, if it isn't added already
 *
 * @param pid       pid to receive (0: none)
 */
void cZapReceiver::AddChannelPid(int pid)
{
	if (!pid || HasPid(pid))
		return;

	AddPid(pid);
	m_pids.push_back(pid);
}

/**
 * Check, if a pid of the channel is received
 *
 * @param pid       pid of a TS packet
 */
bool cZapReceiver::HasPid(int pid) const
{
	return std::find(m_pids.begin(), m_pids.end(), pid) != m_pids.end();
}

/**
 * Get the state of the buffer for the statistics
 *
 * @param[out] frames   number of the buffered frames
 * @param[out] ready    true, if an I-frame is buffered
 */
void cZapReceiver::Info(int *frames, bool *ready)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	*frames = m_buffer.Frames();
	*ready = m_buffer.Ready();
}

/**
 * Check, if the buffer was taken by a channel switch (the receiver doesn't buffer anymore)
 */
bool cZapReceiver::IsTaken(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	return m_taken;
}

/**
 * Called, when the receiver is attached or detached
 *
 * VDR detaches the receiver (e.g. from the main thread in cDevice::GetDevice()),
 * if it needs the device for another channel.
 *
 * @param on       true: attached, false: detached
 */
void cZapReceiver::Activate(bool on)
{
	m_attached = on;
}

/**
 * Receive the data of the channel
 *
 * The frame detector needs some packets ahead to determine the type of a frame,
 * so the packets wait in m_pending, until they are analyzed. Then they are put
 * into the buffer, which starts with the last I-frame.
 *
 * @param data      TS packets
 * @param length    length of the data (a multiple of TS_SIZE)
 */
void cZapReceiver::Receive(const uchar *data, int length)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_taken || !m_vpid)
		return;

	m_pending.insert(m_pending.end(), data, data + length);

	size_t pos = 0;
	while (m_pending.size() - pos >= MIN_TS_PACKETS_FOR_FRAME_DETECTOR * TS_SIZE) {
#if APIVERSNUM >= 30011
		int n = m_frameDetector.Analyze(&m_pending[pos], m_pending.size() - pos, false);
		bool previousErrors;
		bool missingFrames;
		bool frameStart = m_frameDetector.NewFrame(previousErrors, missingFrames);
#else
		int n = m_frameDetector.Analyze(&m_pending[pos], m_pending.size() - pos);
		bool frameStart = m_frameDetector.NewFrame();
#endif
		if (n <= 0)
			break;

		// lost the TS packet borders, start again with the next I-frame
		if (n % TS_SIZE) {
			m_buffer.Reset();
			pos = m_pending.size();
			break;
		}

		// a new frame starts in the first analyzed packet
		bool independentFrame = frameStart && m_frameDetector.IndependentFrame();
		for (int i = 0; i < n; i += TS_SIZE) {
			const uchar *packet = &m_pending[pos + i];
			int flags = PesStartFlags(packet);
			if (!i && frameStart)
				flags |= cZapBuffer::FRAME_START;
			if (!i && independentFrame)
				flags |= cZapBuffer::INDEPENDENT_FRAME;
			m_buffer.Put(packet, flags);
		}

		pos += n;
	}

	m_pending.erase(m_pending.begin(), m_pending.begin() + pos);
}

/**
 * Get the PES start flags of a TS packet for the buffer
 *
 * @param packet    TS packet of the channel
 *
 * @return          cZapBuffer::VIDEO_PES_START, cZapBuffer::AUDIO_PES_START or 0
 */
int cZapReceiver::PesStartFlags(const uchar *packet) const
{
	if (!TsPayloadStart(packet))
		return 0;

	int pid = TsPid(packet);
	if (pid == m_vpid)
		return cZapBuffer::VIDEO_PES_START;
	if (std::find(m_audioPids.begin(), m_audioPids.end(), pid) != m_audioPids.end())
		return cZapBuffer::AUDIO_PES_START;

	return 0;
}

/**
 * Take the buffered packets, which precede a given packet, and stop buffering
 *
 * Must be called by the receiver thread of the device (see cZapCache::Splice()),
 * so the receiver doesn't get a packet meanwhile.
 *
 * @param next          TS packet, which is played next
 * @param audioAhead    see cZapBuffer::Take()
 * @param maxDelay      see cZapBuffer::Take()
 * @param[out] taken    see cZapBuffer::Take()
 *
 * @return              the packets from an I-frame up to the next packet
 */
std::vector<uint8_t> cZapReceiver::Take(const uint8_t *next, int64_t audioAhead, int64_t maxDelay, cZapBuffer::sTaken *taken)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	m_taken = true;

	// The packets, which wait for the frame detector, belong to the last frame. Their
	// frames aren't detected, but their PES starts update the pts of the newest video
	// and audio (for the I-frame with synchronous audio).
	for (size_t pos = 0; pos + TS_SIZE <= m_pending.size(); pos += TS_SIZE)
		m_buffer.Put(&m_pending[pos], PesStartFlags(&m_pending[pos]));
	m_pending.clear();

	return m_buffer.Take(next, audioAhead, maxDelay, taken);
}

/*****************************************************************************
 * cZapCache class
 ****************************************************************************/

cZapCache::cZapCache(void)
	: m_lastSwitch(time(NULL))
{
}

cZapCache::~cZapCache(void)
{
	Clear();
}

/**
 * Get the time without key press, after which the receivers are released
 *
 * @param release   release time (zapcache.Release): 0 = never, 1 = 20s,
 *                  2 = 50s (before VDR's EPG scan starts), 3 = 5min, 4 = 30min
 *
 * @return          the time in seconds (0: never)
 */
int cZapCache::ReleaseSeconds(int release)
{
	static const int seconds[RELEASE_TIMES] = { 0, 20, 50, 5 * 60, 30 * 60 };

	return seconds[std::clamp(release, 0, RELEASE_TIMES - 1)];
}

/**
 * Get the max. delay of a channel switch, which starts with synchronous audio
 *
 * An older I-frame, whose audio is buffered, delays the playback more. The
 * delay is the time from the I-frame to the newest buffered video packet.
 *
 * @param maxDelay  max. delay (zapcache.MaxDelay): 0 = 1s, 1 = 1.5s,
 *                  2 = 2s, 3 = 3s, 4 = 5s
 *
 * @return          the delay in ms
 */
int cZapCache::MaxDelayMs(int maxDelay)
{
	static const int ms[MAX_DELAYS] = { 1000, 1500, 2000, 3000, 5000 };

	return ms[std::clamp(maxDelay, 0, MAX_DELAYS - 1)];
}

/**
 * Report the start of a channel switch of the live view
 *
 * Arms the splice, if there is a receiver: the first packet of the new channel
 * decides, if the switch starts with its buffered packets.
 */
void cZapCache::ChannelSwitchStarted(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	m_armedFromChannel = cDevice::CurrentChannel();
	m_spliceArmed = HasReceivers();
	m_spliceTimer.Set(SPLICE_TIMEOUT_MS);

	// statistics
	m_measuring = m_mode != ZAP_AHEAD_OFF;
	m_countingCorrections = m_measuring;
	if (m_measuring)
		m_switches++;
	m_switchCached = false;
	m_pictureReported = false;
}

/**
 * Report a channel switch of the live view
 *
 * @param channelNum     number of the new live channel
 */
void cZapCache::ChannelSwitched(int channelNum)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	m_previousChannel = m_liveChannel;
	m_liveChannel = channelNum;
	m_lastSwitch = time(NULL);
	m_preparePending = true;
	m_prepareTimer.Set(PREPARE_DELAY_MS);

	// the channel entry is done
	m_enteredChannel = 0;
	m_enterPending = false;

	// a switch to the same channel: Splice() can't tell its packets from the ones before
	if (channelNum == m_armedFromChannel)
		m_spliceArmed = false;

	// the new channel isn't buffered
	if (!ReceiverFor(channelNum))
		m_spliceArmed = false;
}

/**
 * Report the channel number, which is entered
 *
 * While a channel number is entered, VDR's channel display shows the number
 * (cStatus::OsdChannel()). The channel with this number is tuned ahead. If the
 * entry is cancelled, the neighbour channels are tuned again.
 *
 * @param channelNum     the entered number (0: no number is entered)
 */
void cZapCache::ChannelEntered(int channelNum)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (channelNum == m_enteredChannel)
		return;

	m_enteredChannel = channelNum;
	m_enterPending = channelNum != 0;

	// the entry ended (the switch follows or it was cancelled)
	if (!channelNum) {
		m_preparePending = true;
		m_prepareTimer.Set(PREPARE_DELAY_MS);
	}
}

/**
 * Check, if a channel is received ahead (the caller holds the mutex)
 */
bool cZapCache::HasReceivers(void) const
{
	for (cZapReceiver *receiver : m_pReceivers) {
		if (receiver)
			return true;
	}

	return m_pLive != nullptr || m_pEntered != nullptr;
}

/**
 * Get the receiver of a channel (the caller holds the mutex)
 *
 * @param channelNum     number of the channel
 *
 * @return               the receiver (nullptr, if the channel isn't received ahead)
 */
cZapReceiver *cZapCache::ReceiverFor(int channelNum) const
{
	for (cZapReceiver *receiver : m_pReceivers) {
		if (receiver && receiver->ChannelNumber() == channelNum)
			return receiver;
	}

	if (m_pLive && m_pLive->ChannelNumber() == channelNum)
		return m_pLive;

	if (m_pEntered && m_pEntered->ChannelNumber() == channelNum)
		return m_pEntered;

	return nullptr;
}

/**
 * Stop the receivers of the neighbour channels
 */
void cZapCache::Clear(void)
{
	cZapReceiver *receivers[ALL_RECEIVERS];
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (int i = 0; i < MAX_RECEIVERS; i++) {
			receivers[i] = m_pReceivers[i];
			m_pReceivers[i] = nullptr;
		}
		receivers[MAX_RECEIVERS] = m_pLive;
		m_pLive = nullptr;
		receivers[MAX_RECEIVERS + 1] = m_pEntered;
		m_pEntered = nullptr;
	}

	// detached outside the mutex, cDevice::Detach() waits for the receiver thread of the device
	for (cZapReceiver *receiver : receivers)
		delete receiver;
}

/**
 * Tune the neighbour channels after a channel switch
 *
 * Must be called by the main thread (cPlugin::MainThreadHook()).
 *
 * @param mode          channels to tune (enum Mode, ZAP_AHEAD_OFF, if the plugin is
 *                      off, the live view isn't in transfer mode or the output device
 *                      can't use the zap cache)
 * @param setup         the setup of the plugin
 */
void cZapCache::MainThreadHook(int mode, const sZapCacheSetup &setup)
{
	int releaseSeconds = ReleaseSeconds(setup.release);
	bool cam = setup.cam;

	m_mode = mode;
	m_syncAudio = setup.syncAudio;
	m_maxDelayMs = MaxDelayMs(setup.maxDelay);

	// tune the channels again with or without CAM
	if (cam != m_cam) {
		m_cam = cam;
		Clear();
		std::lock_guard<std::mutex> lock(m_mutex);
		m_spliceArmed = false;
		m_preparePending = true;
		m_prepareTimer.Set(0);
	}

	if (mode == ZAP_AHEAD_OFF) {
		bool hasReceivers;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_preparePending = false;
			m_spliceArmed = false;
			m_enteredChannel = 0;
			m_enterPending = false;
			hasReceivers = HasReceivers();
		}
		if (hasReceivers)
			Clear();
		m_released = false;
		return;
	}

	// release the devices, so VDR's EPG scan can use them
	if (releaseSeconds) {
		time_t lastActivity;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			lastActivity = std::max(cRemote::LastActivity(), m_lastSwitch);
		}
		if (time(NULL) - lastActivity >= releaseSeconds) {
			if (!m_released) {
				bool hasReceivers;
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_preparePending = false;
					m_spliceArmed = false;
					m_enteredChannel = 0;
					m_enterPending = false;
					hasReceivers = HasReceivers();
				}
				if (hasReceivers) {
					Clear();
					dsyslog("zapcache: released the devices after %ds without key press", releaseSeconds);
				}
				m_released = true;
			}
			return;
		}
	}

	// a key was pressed after the release, tune the channels again, but not
	// for the keys of a menu (the next channel switch would tune them anyway)
	if (m_released) {
		if (dynamic_cast<cSkinDisplayMenu *>(cSkinDisplay::Current()))
			return;
		m_released = false;
		std::lock_guard<std::mutex> lock(m_mutex);
		m_preparePending = true;
		m_prepareTimer.Set(0);
	}

	// a channel number is entered: tune the entered channel, the neighbour
	// channels are tuned after the switch (or after a cancelled entry)
	int enteredChannel = 0;
	bool entering;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_enterPending)
			enteredChannel = m_enteredChannel;
		m_enterPending = false;
		entering = m_enteredChannel != 0;
	}
	if (enteredChannel)
		TuneEntered(enteredChannel);
	if (entering)
		return;

	int liveChannel;
	int previousChannel;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_preparePending || !m_prepareTimer.TimedOut())
			return;

		// the receiver of the new live channel is needed, until the transfer plays its first packet
		if (m_spliceArmed && !m_spliceTimer.TimedOut())
			return;

		m_preparePending = false;
		m_spliceArmed = false;
		liveChannel = m_liveChannel;
		previousChannel = m_previousChannel;
	}

	Prepare(liveChannel, previousChannel, mode);
}

/**
 * Get the next or previous channel in the channel list
 *
 * Group separators are skipped. The channel list wraps around, if this is enabled
 * in VDR's setup.
 *
 * @param channels      locked channel list
 * @param channel       channel to start from
 * @param direction     1: next channel, -1: previous channel
 *
 * @return              the neighbour channel (nullptr, if there is none)
 */
const cChannel *cZapCache::Neighbour(const cChannels *channels, const cChannel *channel, int direction)
{
	const cChannel *c = channel;

	while (true) {
		c = direction > 0 ? channels->Next(c) : channels->Prev(c);
		if (!c && Setup.ChannelsWrap)
			c = direction > 0 ? channels->First() : channels->Last();
		if (!c || c == channel)
			return nullptr;
		if (!c->GroupSep())
			return c;
	}
}

/**
 * Get a device for a channel, which is tuned to its transponder or free
 *
 * A free device has no receiver and may be tuned without disturbing the live
 * view (like the devices of VDR's EPG scan).
 *
 * @param channel       channel to receive
 * @param avoid         device, which is only taken, if no other device is free
 *                      (it is tuned to the channel in the opposite direction)
 *
 * @return              the device (nullptr, if there is none)
 */
cDevice *cZapCache::FreeDevice(const cChannel *channel, const cDevice *avoid)
{
	if (cDevice *device = TunedDevice(channel))
		return device;

	cDevice *fallback = nullptr;
	for (int i = 0; i < cDevice::NumDevices(); i++) {
		cDevice *device = cDevice::GetDevice(i);
		if (device && device->ProvidesTransponder(channel) && device->MaySwitchTransponder(channel)) {
			if (device != avoid)
				return device;
			fallback = device;
		}
	}

	return fallback;
}

/**
 * Get a device with a CAM, which decrypts an encrypted channel
 *
 * VDR selects the device and assigns a CAM slot, which can decrypt the channel,
 * like for a recording with the priority of the receiver (cDevice::GetDevice()).
 * A device, which is used by the live view or a recording, isn't taken: its CAM
 * would have to decrypt another channel in parallel (and may stop decrypting
 * the channel of the live view or the recording, if it can't).
 *
 * Must be called by the main thread.
 *
 * @param channel       encrypted channel
 *
 * @return              the device (nullptr, if there is none)
 */
cDevice *cZapCache::CamDevice(const cChannel *channel)
{
	// check the device first, the CAM slot is assigned by the second call
	cDevice *device = cDevice::GetDevice(channel, cZapReceiver::CAM_PRIORITY, false, true);
	if (!device || device->Priority() > cZapReceiver::CAM_PRIORITY)
		return nullptr;

	device = cDevice::GetDevice(channel, cZapReceiver::CAM_PRIORITY, false);
	if (!device || device->Priority() > cZapReceiver::CAM_PRIORITY)
		return nullptr;

	return device;
}

/**
 * Get a device for a channel
 *
 * An encrypted channel gets a device with a CAM (CamDevice()), if this is
 * enabled. Otherwise or without such a device, the channel is only tuned
 * (FreeDevice()), its scrambled packets aren't buffered.
 *
 * @param channel       channel to receive
 * @param avoid         see FreeDevice()
 * @param[out] cam      true, if a CAM of the device decrypts the channel
 *
 * @return              the device (nullptr, if there is none)
 */
cDevice *cZapCache::DeviceFor(const cChannel *channel, const cDevice *avoid, bool *cam) const
{
	*cam = false;

	if (m_cam && channel->Ca() >= CA_ENCRYPTED_MIN) {
		if (cDevice *device = CamDevice(channel)) {
			*cam = true;
			return device;
		}
		dsyslog("zapcache: no device with a CAM for channel %d (%s)", channel->Number(), channel->Name());
	}

	return FreeDevice(channel, avoid);
}

/**
 * Get a device, which is tuned to the transponder of a channel
 *
 * @param channel       channel to receive
 *
 * @return              the device (nullptr, if there is none)
 */
cDevice *cZapCache::TunedDevice(const cChannel *channel)
{
	for (int i = 0; i < cDevice::NumDevices(); i++) {
		cDevice *device = cDevice::GetDevice(i);
		if (device && device->IsTunedToTransponder(channel))
			return device;
	}

	return nullptr;
}

/**
 * Get the number of the devices, which can receive channels (tuners)
 *
 * The devices are counted with every call, a plugin may add devices at runtime.
 */
int cZapCache::NumTuners(void)
{
	int numTuners = 0;

	for (int i = 0; i < cDevice::NumDevices(); i++) {
		cDevice *device = cDevice::GetDevice(i);
		if (device && device->NumProvidedSystems() > 0)
			numTuners++;
	}

	return numTuners;
}

/**
 * Tune a channel on a device and attach a zap receiver for it
 *
 * A device with a CAM is switched to the channel like for a recording, even if
 * it is tuned to its transponder already: so the CAM slot gets the pids of the
 * channel.
 *
 * @param channel       channel to receive
 * @param device        device for the channel
 * @param cam           a CAM of the device decrypts the channel (see CamDevice())
 * @param camHasChannel the CAM slot of the device has the pids of the channel
 *                      already (it decrypts the live channel), the tuned device
 *                      isn't switched
 *
 * @return              the receiver (nullptr, if the channel can't be received)
 */
cZapReceiver *cZapCache::Attach(const cChannel *channel, cDevice *device, bool cam, bool camHasChannel)
{
	bool switchChannel = !device->IsTunedToTransponder(channel) || (cam && !camHasChannel);
	if (switchChannel && !device->SwitchChannel(channel, false)) {
		dsyslog("zapcache: can't tune channel %d (%s) on device %d", channel->Number(), channel->Name(), device->DeviceNumber() + 1);
		return nullptr;
	}

	cZapReceiver *receiver = new cZapReceiver(sZapChannel::FromChannel(channel), device, cam ? channel : nullptr);
	if (!device->AttachReceiver(receiver)) {
		dsyslog("zapcache: can't attach the receiver for channel %d to device %d", channel->Number(), device->DeviceNumber() + 1);
		delete receiver;
		return nullptr;
	}

	return receiver;
}

/**
 * Keep the receiver of a channel, if it still buffers it
 *
 * @param receivers     the old receivers, the kept one is removed from them
 * @param count         number of the old receivers
 * @param channel       channel, which needs a receiver (may be nullptr)
 *
 * @return              the kept receiver (nullptr, if there is none)
 */
cZapReceiver *cZapCache::Keep(cZapReceiver **receivers, int count, const cChannel *channel)
{
	if (!channel)
		return nullptr;

	for (int i = 0; i < count; i++) {
		cZapReceiver *receiver = receivers[i];
		if (receiver && receiver->ChannelNumber() == channel->Number() && receiver->Device() && !receiver->IsTaken()) {
			receivers[i] = nullptr;
			dsyslog("zapcache: keep the receiver of channel %d (%s)", channel->Number(), channel->Name());
			return receiver;
		}
	}

	return nullptr;
}

/**
 * Tune the neighbour channels of the live channel
 *
 * The receivers of channels, which are still neighbours, are kept with their
 * buffers. The other ones are stopped, before the missing channels are tuned,
 * so their devices are free again.
 *
 * In ZAP_AHEAD_BOTH mode the live channel gets a receiver, too, on the device of
 * its transfer. VDR stops the transfer, before it reports a channel switch, so
 * only this receiver keeps the stream of the last live channel and its buffer.
 * It becomes the receiver of the channel in the opposite direction.
 *
 * @param liveChannel       current live channel
 * @param previousChannel   live channel before the last switch
 * @param mode              channels to tune (enum Mode)
 */
void cZapCache::Prepare(int liveChannel, int previousChannel, int mode)
{
	cZapReceiver *old[ALL_RECEIVERS];
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (int i = 0; i < MAX_RECEIVERS; i++) {
			old[i] = m_pReceivers[i];
			m_pReceivers[i] = nullptr;
		}
		old[MAX_RECEIVERS] = m_pLive;
		m_pLive = nullptr;
		old[MAX_RECEIVERS + 1] = m_pEntered;
		m_pEntered = nullptr;
	}

	cZapReceiver *receivers[MAX_RECEIVERS] = {};
	cZapReceiver *liveReceiver = nullptr;

	{
		LOCK_CHANNELS_READ;
		const cChannel *live = Channels->GetByNumber(liveChannel);
		const cChannel *next = nullptr;
		const cChannel *opposite = nullptr;

		if (live) {
			// a switch to the next or previous channel sets the zap direction, a switch
			// to another channel and the first switch after VDR's start zap up
			const cChannel *previous = Channels->GetByNumber(previousChannel);
			if (previous && Neighbour(Channels, previous, -1) == live)
				m_direction = -1;
			else
				m_direction = 1;

			next = Neighbour(Channels, live, m_direction);
			if (next && !next->Vpid() && !next->Apid(0) && !next->Dpid(0))
				next = nullptr;

			opposite = mode == ZAP_AHEAD_BOTH ? Neighbour(Channels, live, -m_direction) : nullptr;
			if (opposite == next || (opposite && !opposite->Vpid() && !opposite->Apid(0) && !opposite->Dpid(0)))
				opposite = nullptr;

			receivers[0] = Keep(old, ALL_RECEIVERS, next);
			receivers[1] = Keep(old, ALL_RECEIVERS, opposite);
			if (mode == ZAP_AHEAD_BOTH)
				liveReceiver = Keep(old, ALL_RECEIVERS, live);
		}

		// the other receivers aren't needed anymore, their devices may be needed below
		for (cZapReceiver *receiver : old)
			delete receiver;

		// The device, which is tuned to the channel in the opposite direction (usually
		// the device of the last live channel), is only tuned to the next channel, if
		// no other device is free. So a switch back doesn't have to tune.
		if (next && !receivers[0]) {
			const cChannel *back = Neighbour(Channels, live, -m_direction);
			bool cam;
			if (cDevice *device = DeviceFor(next, back && back != next ? TunedDevice(back) : nullptr, &cam))
				receivers[0] = Attach(next, device, cam);
			else
				dsyslog("zapcache: no free device for channel %d (%s)", next->Number(), next->Name());
		}

		// the channel in the opposite direction preferably on a device, which is tuned to it already
		if (opposite && !receivers[1]) {
			bool cam;
			if (cDevice *device = DeviceFor(opposite, nullptr, &cam))
				receivers[1] = Attach(opposite, device, cam);
			else
				dsyslog("zapcache: no free device for channel %d (%s)", opposite->Number(), opposite->Name());
		}

		// the live channel on the device of its transfer (with its CAM, so the CAM
		// keeps decrypting it after the switch, until another channel needs the CAM)
		if (mode == ZAP_AHEAD_BOTH && live && !liveReceiver) {
			cDevice *device = cTransferControl::ReceiverDevice();
			if (device && device->IsTunedToTransponder(live)) {
				bool cam = m_cam && live->Ca() >= CA_ENCRYPTED_MIN && device->CamSlot();
				liveReceiver = Attach(live, device, cam, true);
			}
		}
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	for (int i = 0; i < MAX_RECEIVERS; i++)
		m_pReceivers[i] = receivers[i];
	m_pLive = liveReceiver;
}

/**
 * Tune the channel, whose number is entered
 *
 * If no device is free, the devices of the neighbour channels are taken: they
 * aren't neighbours after the switch to the entered channel anymore. The tuner
 * may not lock or no I-frame may be buffered, before the entry is done, but VDR
 * prefers the tuned device for the switch then.
 *
 * Must be called by the main thread (see MainThreadHook()).
 *
 * @param channelNum    number of the entered channel
 */
void cZapCache::TuneEntered(int channelNum)
{
	cZapReceiver *old;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		// the live channel or a channel, which is received already
		if (channelNum == m_liveChannel || ReceiverFor(channelNum))
			return;
		old = m_pEntered;
		m_pEntered = nullptr;
	}
	delete old;

	cZapReceiver *receiver = nullptr;
	{
		LOCK_CHANNELS_READ;
		const cChannel *channel = Channels->GetByNumber(channelNum);
		if (!channel || (!channel->Vpid() && !channel->Apid(0) && !channel->Dpid(0)))
			return;

		bool cam;
		cDevice *device = DeviceFor(channel, nullptr, &cam);
		for (int i = MAX_RECEIVERS - 1; !device && i >= 0; i--) {
			cZapReceiver *neighbour;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				neighbour = m_pReceivers[i];
				m_pReceivers[i] = nullptr;
			}
			// detached outside the mutex (see Clear())
			if (neighbour) {
				delete neighbour;
				device = DeviceFor(channel, nullptr, &cam);
			}
		}

		if (!device) {
			dsyslog("zapcache: no free device for the entered channel %d (%s)", channel->Number(), channel->Name());
			return;
		}

		receiver = Attach(channel, device, cam);
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	m_pEntered = receiver;
}

/**
 * Start a channel switch with the buffered packets of the new channel
 *
 * Called by the output device (through the service interface) with every packet,
 * which the transfer plays, while the splice is armed. The first packet of the new channel, which the zap receiver gets,
 * too, decides:
 *
 * - The transfer of the new channel runs on the device of the zap receiver:
 *   the packets since the last I-frame, which precede the packet, are played
 *   first. The transfer receiver and the zap receiver get the same packets
 *   from the receiver thread of the device one after another, so the played
 *   stream has no gap and no duplicate packet.
 * - Otherwise the switch starts normally.
 *
 * The PAT and PMT of the transfer are played before (cTransfer::Activate()), the
 * packets of the old channel may still be played after the start of the switch.
 *
 * With zapcache.SyncAudio the switch starts with an I-frame, whose synchronous
 * audio is buffered up to the playback start threshold of the output device,
 * if it isn't older than zapcache.MaxDelay (see cZapBuffer::Take()).
 *
 * @param data          TS packet, which the transfer plays next
 * @param audioAheadMs  playback start threshold of the audio of the output device
 * @param play          plays a TS packet (returns the played bytes, <= 0: try again)
 * @param context       passed to play
 */
void cZapCache::Splice(const uchar *data, int audioAheadMs, int (*play)(void *, const uchar *, int), void *context)
{
	int64_t audioAhead = m_syncAudio ? audioAheadMs * 90LL : cZapBuffer::NEWEST_I_FRAME;
	int64_t maxDelay = m_maxDelayMs * 90LL;
	std::vector<uint8_t> packets;
	cZapBuffer::sTaken taken;
	int channelNum;

	{
		std::lock_guard<std::mutex> lock(m_mutex);

		if (!m_spliceArmed)
			return;

		// a packet of the old channel
		int currentChannel = cDevice::CurrentChannel();
		if (currentChannel == m_armedFromChannel)
			return;

		cZapReceiver *receiver = ReceiverFor(currentChannel);
		if (!receiver) {
			m_spliceArmed = false;
			return;
		}

		// the PAT and PMT of the transfer
		if (!receiver->HasPid(TsPid(data)))
			return;

		m_spliceArmed = false;
		channelNum = currentChannel;

		cDevice *device = receiver->Device();
		if (!device || device != cTransferControl::ReceiverDevice()) {
			dsyslog("zapcache: the transfer of channel %d doesn't run on the device of the zap receiver", channelNum);
			return;
		}

		packets = receiver->Take(data, audioAhead, maxDelay, &taken);
	}

	if (packets.empty()) {
		dsyslog("zapcache: no I-frame of channel %d buffered", channelNum);
		return;
	}

	for (size_t pos = 0; pos < packets.size(); pos += TS_SIZE) {
		int retries = PLAY_RETRIES;
		while (play(context, &packets[pos], TS_SIZE) <= 0) {
			if (!--retries) {
				isyslog("zapcache: channel %d: buffered packet not accepted, %zu of %zu packets played", channelNum, pos / TS_SIZE, packets.size() / TS_SIZE);
				return;
			}
			cCondWait::SleepMs(PLAY_RETRY_WAIT_MS);
		}
	}

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_switchCached = true;
		m_cached++;
		if (taken.synced)
			m_synced++;
		if (taken.delay >= 0) {
			m_delaySumMs += taken.delay / 90;
			m_delayCount++;
		}
	}

	dsyslog("zapcache: channel %d started with %d buffered frames (%zu packets, %dms delay)%s", channelNum, taken.frames, packets.size() / TS_SIZE,
		taken.delay >= 0 ? static_cast<int>(taken.delay / 90) : 0,
		audioAhead == cZapBuffer::NEWEST_I_FRAME ? "" : taken.synced ? " and synchronous audio" : ", without synchronous audio");
}

/**
 * Check, if the current channel switch started with the buffered packets
 */
bool cZapCache::SwitchCached(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	return m_switchCached;
}

/**
 * Report the time of a channel switch for the statistics
 *
 * Called by the renderer for the first picture (in the fast channel switch modes)
 * and for the start of the playback. Without a fast channel switch mode, the
 * playback starts with the first picture.
 *
 * @param start         false: the first picture was shown, true: the playback started
 * @param ms            time since the start of the channel switch in ms
 */
void cZapCache::ReportSwitchTime(bool start, int ms)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_measuring)
		return;

	int cached = m_switchCached ? 1 : 0;

	if (!start || !m_pictureReported) {
		m_pictureSumMs[cached] += ms;
		m_pictureCount[cached]++;
		m_pictureReported = true;
	}

	if (start) {
		m_startSumMs[cached] += ms;
		m_startCount[cached]++;
		m_measuring = false;
	}
}

/**
 * Report the frame corrections of the A/V sync after a channel switch
 *
 * Called by the renderer, when the first frames after the playback start were
 * shown (see cVideoRender::SWITCH_CORRECTION_FRAMES). A switch, which starts
 * with audio, which isn't synchronous to the video, has to drop or to repeat
 * frames.
 *
 * @param corrections   number of the dropped and repeated frames
 */
void cZapCache::ReportSwitchCorrections(int corrections)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_countingCorrections)
		return;

	int cached = m_switchCached ? 1 : 0;
	m_correctionSum[cached] += corrections;
	m_correctionCount[cached]++;
	m_countingCorrections = false;
}

/**
 * Reset the statistics of the zap cache (the current switch isn't measured anymore)
 */
void cZapCache::ResetStats(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	m_switches = 0;
	m_cached = 0;
	m_synced = 0;
	m_measuring = false;
	for (int i = 0; i < 2; i++) {
		m_pictureSumMs[i] = 0;
		m_pictureCount[i] = 0;
		m_startSumMs[i] = 0;
		m_startCount[i] = 0;
		m_correctionSum[i] = 0;
		m_correctionCount[i] = 0;
	}
	m_delaySumMs = 0;
	m_delayCount = 0;
	m_countingCorrections = false;
}

/**
 * Get the statistics of the zap cache
 *
 * @param[out] stats    the statistics
 */
void cZapCache::GetStats(sStats *stats)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	stats->receivers.clear();
	for (cZapReceiver *receiver : { m_pReceivers[0], m_pReceivers[1], m_pEntered }) {
		if (!receiver)
			continue;
		cDevice *device = receiver->Device();
		sStats::sReceiver r = { receiver->ChannelNumber(), device ? device->DeviceNumber() + 1 : 0, 0, false };
		receiver->Info(&r.frames, &r.ready);
		stats->receivers.push_back(r);
	}

	stats->switches = m_switches;
	stats->cached = m_cached;
	stats->synced = m_synced;
	for (int i = 0; i < 2; i++) {
		stats->pictureMs[i] = m_pictureCount[i] ? m_pictureSumMs[i] / m_pictureCount[i] : 0;
		stats->startMs[i] = m_startCount[i] ? m_startSumMs[i] / m_startCount[i] : 0;
		stats->corrections[i] = m_correctionCount[i] ? static_cast<double>(m_correctionSum[i]) / m_correctionCount[i] : -1;
	}
	stats->delayMs = m_delayCount ? m_delaySumMs / m_delayCount : 0;
}

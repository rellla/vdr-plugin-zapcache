// SPDX-License-Identifier: MIT

/**
 * @file zapcache_service.h
 * Service interface of the zapcache plugin for output devices
 *
 * @copyright 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{MIT}
 *
 * An output plugin copies this header. It gets the interface once in its
 * cPlugin::Start(): VDR starts all plugins, before it switches to the first
 * live channel, so cDevice::PlayTs() doesn't run yet:
 *
 * @code
 * ZapCacheService_v1_0 zapCache = {};
 * bool hasZapCache = cPluginManager::CallFirstService(ZAPCACHE_SERVICE_V1_0, &zapCache);
 * @endcode
 *
 * Without the zapcache plugin, CallFirstService() returns false and the
 * structure stays empty. With the plugin, but without these calls, the plugin
 * still tunes the next channel ahead, a channel switch to it doesn't have to
 * tune, but it doesn't start with the buffered packets.
 *
 * The output device calls in cDevice::PlayTs() for every TS packet, before it
 * plays it:
 *
 * @code
 * if (zapCache.SpliceArmed && zapCache.SpliceArmed())
 *     int delayMs = zapCache.Splice(data, startThresholdMs, PlayBufferedTs, this);
 * @endcode
 *
 * PlayBufferedTs() plays a TS packet with cDevice::PlayTs() of the base class.
 *
 * Any plugin may query the channels, which are received ahead (e.g. a skin,
 * which marks them in the channel list):
 *
 * @code
 * ZapCacheChannels_v1_0 channels;
 * if (cPluginManager::CallFirstService(ZAPCACHE_CHANNELS_V1_0, &channels)) {
 *     for (int i = 0; i < channels.count; i++)
 *         ... channels.channels[i].number ...
 * }
 * @endcode
 *
 * The header can be included by C code, too (e.g. the decoder of an output
 * plugin).
 */

#ifndef __ZAPCACHE_SERVICE_H
#define __ZAPCACHE_SERVICE_H

#ifndef __cplusplus
#include <stdbool.h>
#endif

/** the id of the service (version 1.0 of the interface) */
#define ZAPCACHE_SERVICE_V1_0 "ZapCache-v1.0"

/** events of a channel switch (see ZapCacheService_v1_0::ReportSwitchEvent()) */
enum ZapCacheSwitchEvent {
	ZAPCACHE_FIRST_PICTURE = 0,     /**< the first picture of the new channel is shown */
	ZAPCACHE_PLAYBACK_STARTED = 1   /**< the playback of the new channel started (picture and sound) */
};

/** time after ZAPCACHE_PLAYBACK_STARTED, in which the frame corrections are counted */
#define ZAPCACHE_CORRECTION_WINDOW_MS 5000

/**
 * Service interface of the zapcache plugin (version 1.0)
 *
 * The functions may be called by any thread.
 */
struct ZapCacheService_v1_0 {
	/**
	 * Check, if a channel switch may start with buffered packets
	 *
	 * Cheap enough for every TS packet.
	 */
	bool (*SpliceArmed)(void);

	/**
	 * Start a channel switch with the buffered packets of the new channel
	 *
	 * Called by the output device in cDevice::PlayTs() with every TS packet,
	 * which the transfer plays, before it is played, while SpliceArmed()
	 * returns true. If the packet is the first one of the new channel, the
	 * buffered packets since an I-frame, which precede it, are played with
	 * play() first.
	 *
	 * @param data              TS packet, which is played next
	 * @param startThresholdMs  buffer fill level of the audio, at which the output
	 *                          device starts the playback (for a start with
	 *                          synchronous audio)
	 * @param play              plays a TS packet (the base class cDevice::PlayTs()),
	 *                          returns the played bytes (<= 0: try again)
	 * @param context           passed to play (e.g. the output device)
	 *
	 * @return                  -1: the switch doesn't start with buffered packets
	 *                          (yet), >= 0: it started with them, the delay of the
	 *                          playback to the live stream in ms (the time from the
	 *                          buffered I-frame to the newest buffered packet). An
	 *                          output device may reduce the delay (e.g. by playing
	 *                          a bit faster).
	 */
	int (*Splice)(const unsigned char *data, int startThresholdMs,
	              int (*play)(void *context, const unsigned char *packet, int size), void *context);

	/**
	 * Report an event of a channel switch for the statistics (optional)
	 *
	 * The zap cache measures the time since the start of the channel switch
	 * (VDR's cStatus::ChannelSwitch()), so all output devices use the same
	 * time base. Without ZAPCACHE_FIRST_PICTURE the playback start counts as
	 * the first picture, too. Only the first report of an event after a channel
	 * switch counts.
	 *
	 * @param event     ZAPCACHE_FIRST_PICTURE or ZAPCACHE_PLAYBACK_STARTED
	 */
	void (*ReportSwitchEvent)(int event);

	/**
	 * Report frame corrections of the A/V sync for the statistics (optional)
	 *
	 * Called, whenever the output device drops or repeats frames to keep the
	 * video in sync with the audio, e.g. ReportFrameCorrection(1, 0) for a
	 * dropped frame. Cheap enough for every frame. The zap cache counts the
	 * corrections within ZAPCACHE_CORRECTION_WINDOW_MS after
	 * ZAPCACHE_PLAYBACK_STARTED of a channel switch, so the output device has to
	 * report this event, too.
	 *
	 * @param dropped   number of the dropped frames
	 * @param repeated  number of the repeated frames
	 */
	void (*ReportFrameCorrection)(int dropped, int repeated);

	/**
	 * Report, if the output device can show the live view with the zap cache
	 *
	 * E.g. false, while it is detached or shows a picture in picture (pip uses
	 * the free devices, too). The zap cache releases its devices meanwhile.
	 */
	void (*SetOutputAvailable)(bool available);
};

/** the id of the query of the channels, which are received ahead (version 1.0) */
#define ZAPCACHE_CHANNELS_V1_0 "ZapCache-Channels-v1.0"

/** max. number of the channels, which are received ahead */
#define ZAPCACHE_MAX_CHANNELS 4

/** a channel, which is received ahead */
struct ZapCacheChannel_v1_0 {
	int number;     /**< channel number */
	int device;     /**< number of the receiving device (starting with 1, 0: detached) */
	bool ready;     /**< a switch to it can start with the buffer (an I-frame or audio is buffered) */
	bool radio;     /**< a radio channel */
};

/** the channels, which are received ahead (ZAPCACHE_CHANNELS_V1_0) */
struct ZapCacheChannels_v1_0 {
	int count;      /**< number of the channels */
	struct ZapCacheChannel_v1_0 channels[ZAPCACHE_MAX_CHANNELS]; /**< the channels */
};

#endif

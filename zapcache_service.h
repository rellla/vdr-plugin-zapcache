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
 *     zapCache.Splice(data, startThresholdMs, PlayBufferedTs, this);
 * @endcode
 *
 * PlayBufferedTs() plays a TS packet with cDevice::PlayTs() of the base class.
 */

#ifndef __ZAPCACHE_SERVICE_H
#define __ZAPCACHE_SERVICE_H

/** the id of the service (version 1.0 of the interface) */
#define ZAPCACHE_SERVICE_V1_0 "ZapCache-v1.0"

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
	 */
	void (*Splice)(const unsigned char *data, int startThresholdMs,
	               int (*play)(void *context, const unsigned char *packet, int size), void *context);

	/** true, if the current channel switch started with buffered packets */
	bool (*SwitchCached)(void);

	/**
	 * Report the time of a channel switch for the statistics
	 *
	 * @param start     false: the first picture was shown, true: the playback started
	 * @param ms        time since the start of the channel switch in ms
	 */
	void (*ReportSwitchTime)(bool start, int ms);

	/**
	 * Report the frame corrections of the A/V sync after a channel switch
	 *
	 * @param corrections   dropped and repeated frames within the first frames
	 *                      after the playback start (e.g. 250)
	 */
	void (*ReportSwitchCorrections)(int corrections);

	/**
	 * Report, if the output device can show the live view with the zap cache
	 *
	 * E.g. false, while it is detached or shows a picture in picture (pip uses
	 * the free devices, too). The zap cache releases its devices meanwhile.
	 */
	void (*SetOutputAvailable)(bool available);
};

#endif

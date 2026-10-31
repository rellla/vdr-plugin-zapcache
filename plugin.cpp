// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file plugin.cpp
 * The zapcache plugin
 *
 * @copyright 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <atomic>
#include <cstdio>
#include <cstring>

#include <string>

#include <vdr/config.h>
#include <vdr/device.h>
#include <vdr/plugin.h>
#include <vdr/status.h>

#include "setup.h"
#include "zapcache.h"
#include "zapcache_service.h"

static const char *VERSION        = "0.1.0";
static const char *DESCRIPTION    = trNOOP("Tunes the neighbour channels ahead and buffers them");

/**
 * The zap cache of the plugin for the service functions (nullptr: the plugin is destroyed)
 *
 * The service functions are plain functions, so an output plugin can call them
 * without knowing the classes of this plugin.
 */
static std::atomic<cZapCache *> s_pZapCache = nullptr;

/** the output device can use the zap cache (see ZapCacheService_v1_0::SetOutputAvailable) */
static std::atomic<bool> s_outputAvailable = true;

/**
 * The zapcache plugin
 *
 * It monitors the channel switches and the channel display of VDR (cStatus) and
 * tunes the neighbour channels in VDR's main thread (MainThreadHook()). An output
 * device starts a channel switch with the buffered packets through the service
 * interface (zapcache_service.h).
 *
 * @ingroup zapcache
 */
class cPluginZapcache : public cPlugin, public cStatus {
public:
	cPluginZapcache(void);
	virtual ~cPluginZapcache(void);
	virtual const char *Version(void) { return VERSION; }
	virtual const char *Description(void) { return tr(DESCRIPTION); }
	virtual void Stop(void);
	virtual void MainThreadHook(void);
	virtual cMenuSetupPage *SetupMenu(void);
	virtual bool SetupParse(const char *, const char *);
	virtual bool Service(const char *, void * = nullptr);
	virtual const char **SVDRPHelpPages(void);
	virtual cString SVDRPCommand(const char *, const char *, int &);

protected:
	virtual void ChannelSwitch(const cDevice *, int, bool);
	virtual void OsdChannel(const char *);
	virtual void OsdClear(void);

private:
	cZapCache m_zapCache;           ///< the zap cache
	sZapCacheSetup m_setup;         ///< the setup values (main thread only)
	std::atomic<int> m_enabled = 1; ///< zapcache.Enable for SVDRP (the SVDRP thread doesn't read m_setup)
	std::atomic<int> m_svdrpEnable = -1; ///< zapcache.Enable set by SVDRP (-1: none), stored by the main thread

	cString Status(void);
};

cPluginZapcache::cPluginZapcache(void)
{
	s_pZapCache = &m_zapCache;
}

cPluginZapcache::~cPluginZapcache(void)
{
	s_pZapCache = nullptr;
}

/**
 * Release the devices, before VDR shuts them down
 */
void cPluginZapcache::Stop(void)
{
	m_zapCache.Clear();
}

/**
 * Tune the neighbour channels (VDR's main thread)
 *
 * The zap cache works, while the live view is in transfer mode and the output
 * device can use it.
 */
void cPluginZapcache::MainThreadHook(void)
{
	// SVDRP ENAB
	int enable = m_svdrpEnable.exchange(-1);
	if (enable >= 0 && enable != m_setup.enable) {
		SetupStore("Enable", m_setup.enable = enable);
		Setup.Save();
	}
	m_enabled = m_setup.enable;

	int mode = cZapCache::ZAP_AHEAD_OFF;
	cDevice *primary = cDevice::PrimaryDevice();

	if (m_setup.enable && s_outputAvailable && primary && primary->Transferring())
		mode = m_setup.tuners >= 3 && cZapCache::NumTuners() >= 3 ? cZapCache::ZAP_AHEAD_BOTH : cZapCache::ZAP_AHEAD_NEXT;

	m_zapCache.MainThreadHook(mode, m_setup);
}

cMenuSetupPage *cPluginZapcache::SetupMenu(void)
{
	return new cMenuSetupZapcache(&m_setup, &m_zapCache);
}

bool cPluginZapcache::SetupParse(const char *name, const char *value)
{
	return m_setup.Parse(name, value);
}

/*****************************************************************************
 * VDR cStatus interface
 ****************************************************************************/

/**
 * Monitor a channel switch of the live view
 *
 * The start of the switch (channelNum == 0) arms the splice, after the switch
 * the neighbour channels are tuned.
 */
void cPluginZapcache::ChannelSwitch(const cDevice *device, int channelNum, bool liveView)
{
	if (device != cDevice::PrimaryDevice() || !liveView)
		return;

	if (channelNum)
		m_zapCache.ChannelSwitched(channelNum);
	else
		m_zapCache.ChannelSwitchStarted();
}

/**
 * Monitor the channel display
 *
 * While a channel number is entered, VDR's channel display shows the number
 * followed by a "-" (see VDR's ChannelString()), e.g. "27-  Das Erste HD".
 * The channel with this number is tuned ahead.
 *
 * @param text      text of the channel display
 */
void cPluginZapcache::OsdChannel(const char *text)
{
	int number = 0;
	char dash = 0;

	if (!text || sscanf(text, "%d%c", &number, &dash) != 2 || dash != '-' || number <= 0)
		number = 0;

	m_zapCache.ChannelEntered(number);
}

/**
 * Monitor the closing of the OSD
 *
 * The channel display may be closed, while a channel number is entered (e.g.
 * by another key or after the channel info time), so the entry ends.
 */
void cPluginZapcache::OsdClear(void)
{
	m_zapCache.ChannelEntered(0);
}

/*****************************************************************************
 * Service interface (zapcache_service.h)
 ****************************************************************************/

static bool ServiceSpliceArmed(void)
{
	cZapCache *zapCache = s_pZapCache;
	return zapCache && zapCache->SpliceArmed();
}

static int ServiceSplice(const unsigned char *data, int startThresholdMs, int (*play)(void *, const unsigned char *, int), void *context)
{
	cZapCache *zapCache = s_pZapCache;
	return zapCache ? zapCache->Splice(data, startThresholdMs, play, context) : -1;
}

static void ServiceReportSwitchEvent(int event)
{
	if (cZapCache *zapCache = s_pZapCache)
		zapCache->ReportSwitchEvent(event);
}

static void ServiceReportFrameCorrection(int dropped, int repeated)
{
	if (cZapCache *zapCache = s_pZapCache)
		zapCache->ReportFrameCorrection(dropped, repeated);
}

static void ServiceSetOutputAvailable(bool available)
{
	s_outputAvailable = available;
}

/**
 * Provide the service interface for an output device and the channels, which are received ahead
 *
 * @param id        ZAPCACHE_SERVICE_V1_0 or ZAPCACHE_CHANNELS_V1_0
 * @param data      ZapCacheService_v1_0, which gets the functions, or
 *                  ZapCacheChannels_v1_0, which gets the channels (nullptr: query only)
 *
 * @return          true, if the service is known
 */
bool cPluginZapcache::Service(const char *id, void *data)
{
	if (!id)
		return false;

	if (!strcmp(id, ZAPCACHE_CHANNELS_V1_0)) {
		if (data) {
			ZapCacheChannels_v1_0 *channels = static_cast<ZapCacheChannels_v1_0 *>(data);
			cZapCache::sStats stats;
			m_zapCache.GetStats(&stats);
			channels->count = 0;
			for (const auto &r : stats.receivers) {
				if (channels->count == ZAPCACHE_MAX_CHANNELS)
					break;
				channels->channels[channels->count++] = { r.channel, r.device, r.ready, r.radio };
			}
		}
		return true;
	}

	if (strcmp(id, ZAPCACHE_SERVICE_V1_0))
		return false;

	if (data) {
		ZapCacheService_v1_0 *service = static_cast<ZapCacheService_v1_0 *>(data);
		service->SpliceArmed = ServiceSpliceArmed;
		service->Splice = ServiceSplice;
		service->ReportSwitchEvent = ServiceReportSwitchEvent;
		service->ReportFrameCorrection = ServiceReportFrameCorrection;
		service->SetOutputAvailable = ServiceSetOutputAvailable;
	}

	return true;
}

/*****************************************************************************
 * SVDRP
 ****************************************************************************/

/**
 * Format an average channel switch time for the status
 */
static std::string SvdrpTime(int ms)
{
	return ms ? std::to_string(ms) + "ms" : "-";
}

/**
 * Format the average frame corrections for the status
 */
static std::string SvdrpCorrections(double corrections)
{
	return corrections >= 0 ? *cString::sprintf("%.1f", corrections) : "-";
}

/**
 * Create the status and the statistics of the zap cache (SVDRP STAT)
 */
cString cPluginZapcache::Status(void)
{
	static const char *modes[] = { "off", "next channel", "next and previous channel" };
	int mode = m_zapCache.Mode();
	cZapCache::sStats stats;
	m_zapCache.GetStats(&stats);

	std::string channels;
	for (const auto &r : stats.receivers) {
		if (!channels.empty())
			channels += ", ";
		if (r.radio)
			channels += *cString::sprintf("%d (device %d, %dms audio)", r.channel, r.device, r.ready ? r.frames : 0);
		else
			channels += *cString::sprintf("%d (device %d, %d frames)", r.channel, r.device, r.ready ? r.frames : 0);
	}

	return cString::sprintf("enabled: %s\n"
	                        "mode: %s\n"
	                        "channels: %s\n"
	                        "switches: %d, buffered: %d, synchronous audio: %d\n"
	                        "switch time buffered: picture %s, playback %s, delay %s\n"
	                        "switch time without buffer: picture %s, playback %s\n"
	                        "frame corrections: buffered %s, without buffer %s",
	                        m_enabled ? "yes" : "no",
	                        modes[mode >= 0 && mode <= cZapCache::ZAP_AHEAD_BOTH ? mode : 0],
	                        channels.empty() ? "-" : channels.c_str(),
	                        stats.switches, stats.cached, stats.synced,
	                        SvdrpTime(stats.pictureMs[1]).c_str(), SvdrpTime(stats.startMs[1]).c_str(), SvdrpTime(stats.delayMs).c_str(),
	                        SvdrpTime(stats.pictureMs[0]).c_str(), SvdrpTime(stats.startMs[0]).c_str(),
	                        SvdrpCorrections(stats.corrections[1]).c_str(), SvdrpCorrections(stats.corrections[0]).c_str());
}

const char **cPluginZapcache::SVDRPHelpPages(void)
{
	static const char *helpPages[] = {
		"STAT\n"
		"    Show the status and the statistics of the zap cache.",
		"RSTS\n"
		"    Reset the statistics.",
		"ENAB [ on | off ]\n"
		"    Switch the zap cache on or off (zapcache.Enable, stored in\n"
		"    setup.conf). Without option, show if it is on.",
		nullptr
	};

	return helpPages;
}

/**
 * Handle an SVDRP command (SVDRP thread)
 *
 * @param command       the command
 * @param option        the option of the command
 * @param[out] replyCode    reply code (900: ok, 501: error)
 *
 * @return              the reply (nullptr: unknown command)
 */
cString cPluginZapcache::SVDRPCommand(const char *command, const char *option, int &replyCode)
{
	if (!strcasecmp(command, "STAT"))
		return Status();

	if (!strcasecmp(command, "RSTS")) {
		m_zapCache.ResetStats();
		return "statistics reset";
	}

	if (!strcasecmp(command, "ENAB")) {
		if (option && *option) {
			int enable;
			if (!strcasecmp(option, "on") || !strcmp(option, "1"))
				enable = 1;
			else if (!strcasecmp(option, "off") || !strcmp(option, "0"))
				enable = 0;
			else {
				replyCode = 501;
				return cString::sprintf("invalid option \"%s\"", option);
			}
			m_svdrpEnable = enable;
			m_enabled = enable;
		}
		return m_enabled ? "zap cache is on" : "zap cache is off";
	}

	return nullptr;
}

VDRPLUGINCREATOR(cPluginZapcache); // Don't touch this!

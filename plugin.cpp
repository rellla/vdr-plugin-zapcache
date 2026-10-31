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

protected:
	virtual void ChannelSwitch(const cDevice *, int, bool);
	virtual void OsdChannel(const char *);
	virtual void OsdClear(void);

private:
	cZapCache m_zapCache;           ///< the zap cache
	sZapCacheSetup m_setup;         ///< the setup values
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

static void ServiceSplice(const unsigned char *data, int startThresholdMs, int (*play)(void *, const unsigned char *, int), void *context)
{
	if (cZapCache *zapCache = s_pZapCache)
		zapCache->Splice(data, startThresholdMs, play, context);
}

static bool ServiceSwitchCached(void)
{
	cZapCache *zapCache = s_pZapCache;
	return zapCache && zapCache->SwitchCached();
}

static void ServiceReportSwitchTime(bool start, int ms)
{
	if (cZapCache *zapCache = s_pZapCache)
		zapCache->ReportSwitchTime(start, ms);
}

static void ServiceReportSwitchCorrections(int corrections)
{
	if (cZapCache *zapCache = s_pZapCache)
		zapCache->ReportSwitchCorrections(corrections);
}

static void ServiceSetOutputAvailable(bool available)
{
	s_outputAvailable = available;
}

/**
 * Provide the service interface for an output device
 *
 * @param id        ZAPCACHE_SERVICE_V1_0
 * @param data      ZapCacheService_v1_0, which gets the functions (nullptr: query only)
 *
 * @return          true, if the service is known
 */
bool cPluginZapcache::Service(const char *id, void *data)
{
	if (!id || strcmp(id, ZAPCACHE_SERVICE_V1_0))
		return false;

	if (data) {
		ZapCacheService_v1_0 *service = static_cast<ZapCacheService_v1_0 *>(data);
		service->SpliceArmed = ServiceSpliceArmed;
		service->Splice = ServiceSplice;
		service->SwitchCached = ServiceSwitchCached;
		service->ReportSwitchTime = ServiceReportSwitchTime;
		service->ReportSwitchCorrections = ServiceReportSwitchCorrections;
		service->SetOutputAvailable = ServiceSetOutputAvailable;
	}

	return true;
}

VDRPLUGINCREATOR(cPluginZapcache); // Don't touch this!

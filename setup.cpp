// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file setup.cpp
 * Setup of the zapcache plugin
 *
 * @copyright 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <algorithm>
#include <cstring>
#include <string>

#include <vdr/i18n.h>

#include "setup.h"
#include "zapcache.h"

/*****************************************************************************
 * sZapCacheSetup
 ****************************************************************************/

/**
 * Parse a setup value (setup.conf)
 *
 * @param name      name of the value (without "zapcache.")
 * @param value     the value
 *
 * @return          true, if the name is known
 */
bool sZapCacheSetup::Parse(const char *name, const char *value)
{
	if (!strcasecmp(name, "Enable"))
		enable = atoi(value);
	else if (!strcasecmp(name, "Tuners"))
		tuners = std::clamp(atoi(value), 2, 3);
	else if (!strcasecmp(name, "Release"))
		release = std::clamp(atoi(value), 0, 4);
	else if (!strcasecmp(name, "SyncAudio"))
		syncAudio = atoi(value);
	else if (!strcasecmp(name, "MaxDelay"))
		maxDelay = std::clamp(atoi(value), 0, 4);
	else if (!strcasecmp(name, "Cam"))
		cam = atoi(value);
	else
		return false;

	return true;
}

/*****************************************************************************
 * cMenuSetupZapcache
 ****************************************************************************/

/**
 * Format an average channel switch time for the statistics
 *
 * @param ms        time in ms (0: unknown)
 * @return          e.g. "420ms" or "-"
 */
static cString SwitchTime(int ms)
{
	return ms ? cString::sprintf("%dms", ms) : cString("-");
}

/**
 * Format the average frame corrections after a channel switch for the statistics
 *
 * @param corrections   average number of the dropped and repeated frames (-1: unknown)
 * @return              e.g. "0.4" or "-"
 */
static cString Corrections(double corrections)
{
	return corrections >= 0 ? cString::sprintf("%.1f", corrections) : cString("-");
}

/**
 * Create the setup menu
 *
 * @param setup     the setup of the plugin
 * @param zapCache  the zap cache (statistics)
 */
cMenuSetupZapcache::cMenuSetupZapcache(sZapCacheSetup *setup, cZapCache *zapCache)
	: m_pSetup(setup),
	  m_pZapCache(zapCache),
	  m_data(*setup)
{
	m_tuners[0] = tr("2 (next channel)");
	m_tuners[1] = tr("3 (next and previous channel)");
	m_releases[0] = tr("never");
	m_releases[1] = tr("after 20s");
	m_releases[2] = tr("after 50s");
	m_releases[3] = tr("after 5min");
	m_releases[4] = tr("after 30min");
	m_maxDelays[0] = "1s";
	m_maxDelays[1] = "1.5s";
	m_maxDelays[2] = "2s";
	m_maxDelays[3] = "3s";
	m_maxDelays[4] = "5s";

	// the selection starts with 2 tuners
	m_data.tuners -= 2;

	Create();
}

/**
 * Add a statistics item, whose text is updated, while the menu is shown
 *
 * @param text      creates the current text of the item
 */
void cMenuSetupZapcache::AddLiveItem(std::function<cString(void)> text)
{
	cOsdItem *item = new cOsdItem(text(), osUnknown, false);
	Add(item);
	m_liveItems.push_back({ item, std::move(text) });
}

/**
 * Update the texts of the statistics items and redraw the menu, if one changed
 *
 * The whole menu is drawn again, because not every skin draws a single item,
 * which is changed with cOsdMenu::DisplayItem().
 */
void cMenuSetupZapcache::UpdateLiveItems(void)
{
	bool changed = false;

	for (auto &live : m_liveItems) {
		cString text = live.text();
		if (strcmp(text, live.item->Text())) {
			live.item->SetText(text);
			changed = true;
		}
	}

	if (changed)
		Display();
}

/**
 * Build the menu (the shown items depend on the edited values)
 */
void cMenuSetupZapcache::Create(void)
{
	int current = Current();

	Clear();
	m_liveItems.clear();

	Add(new cMenuEditBoolItem(tr("Tune channels ahead"), &m_data.enable, trVDR("no"), trVDR("yes")));
	if (m_data.enable) {
		if (cZapCache::NumTuners() >= 3)
			Add(new cMenuEditStraItem(tr(" Tuners for live view and cache"), &m_data.tuners, 2, m_tuners));
		Add(new cMenuEditStraItem(tr(" Release the tuners without key press"), &m_data.release, 5, m_releases));
		Add(new cMenuEditBoolItem(tr(" Start with synchronous audio"), &m_data.syncAudio, trVDR("no"), trVDR("yes")));
		if (m_data.syncAudio)
			Add(new cMenuEditStraItem(tr("  Max. delay"), &m_data.maxDelay, 5, m_maxDelays));
		Add(new cMenuEditBoolItem(tr(" Buffer encrypted channels (CAM)"), &m_data.cam, trVDR("no"), trVDR("yes")));
	}

	// statistics
	Add(new cOsdItem(tr("Statistics"), osUnknown, false));
	AddLiveItem([this]() {
		cZapCache::sStats stats;
		m_pZapCache->GetStats(&stats);
		std::string channels;
		for (const auto &r : stats.receivers) {
			if (!channels.empty())
				channels += ", ";
			channels += *cString::sprintf(tr("%d (device %d, %d frames)"), r.channel, r.device, r.ready ? r.frames : 0);
		}
		return cString::sprintf(tr("Zap cache: %s"), channels.empty() ? "-" : channels.c_str());
	});
	AddLiveItem([this]() {
		cZapCache::sStats stats;
		m_pZapCache->GetStats(&stats);
		return cString::sprintf(tr("Zap cache: %d of %d switches buffered, %d with synchronous audio"), stats.cached, stats.switches, stats.synced);
	});
	AddLiveItem([this]() {
		cZapCache::sStats stats;
		m_pZapCache->GetStats(&stats);
		return cString::sprintf(tr("Switch time buffered: picture %s, playback %s, delay %s"), *SwitchTime(stats.pictureMs[1]), *SwitchTime(stats.startMs[1]), *SwitchTime(stats.delayMs));
	});
	AddLiveItem([this]() {
		cZapCache::sStats stats;
		m_pZapCache->GetStats(&stats);
		return cString::sprintf(tr("Switch time without buffer: picture %s, playback %s"), *SwitchTime(stats.pictureMs[0]), *SwitchTime(stats.startMs[0]));
	});
	AddLiveItem([this]() {
		cZapCache::sStats stats;
		m_pZapCache->GetStats(&stats);
		return cString::sprintf(tr("Frame corrections after a switch: buffered %s, without buffer %s"), *Corrections(stats.corrections[1]), *Corrections(stats.corrections[0]));
	});

	SetHelp(tr("Button$Reset statistics"));
	SetCurrent(Get(current));
	Display();
	m_liveUpdateTimer.Set(LIVE_UPDATE_INTERVAL_MS);
}

/**
 * Handle a key
 *
 * The red key resets the statistics. The statistics are updated every second.
 *
 * @param key       pressed key
 */
eOSState cMenuSetupZapcache::ProcessKey(eKeys key)
{
	int oldEnable = m_data.enable;
	int oldSyncAudio = m_data.syncAudio;

	eOSState state = cMenuSetupPage::ProcessKey(key);

	if (state == osUnknown && key == kRed) {
		m_pZapCache->ResetStats();
		UpdateLiveItems();
		return osContinue;
	}

	// update the statistics, while the menu is shown
	if (key == kNone && m_liveUpdateTimer.TimedOut()) {
		UpdateLiveItems();
		m_liveUpdateTimer.Set(LIVE_UPDATE_INTERVAL_MS);
	}

	// the shown items depend on these values
	if (key != kNone && (oldEnable != m_data.enable || oldSyncAudio != m_data.syncAudio))
		Create();

	return state;
}

/**
 * Store the setup (OK), only the changed values are stored
 */
void cMenuSetupZapcache::Store(void)
{
	auto store = [this](const char *name, int &value, int newValue) {
		if (value != newValue)
			SetupStore(name, value = newValue);
	};

	store("Enable", m_pSetup->enable, m_data.enable);
	store("Tuners", m_pSetup->tuners, m_data.tuners + 2);
	store("Release", m_pSetup->release, m_data.release);
	store("SyncAudio", m_pSetup->syncAudio, m_data.syncAudio);
	store("MaxDelay", m_pSetup->maxDelay, m_data.maxDelay);
	store("Cam", m_pSetup->cam, m_data.cam);
}

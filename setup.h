// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file setup.h
 * Setup of the zapcache plugin
 *
 * @copyright 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __ZAPCACHE_SETUP_H
#define __ZAPCACHE_SETUP_H

#include <functional>
#include <vector>

#include <vdr/menuitems.h>
#include <vdr/plugin.h>
#include <vdr/tools.h>

class cZapCache;

/**
 * The setup values of the plugin (setup.conf: zapcache.*)
 *
 * @ingroup zapcache
 */
struct sZapCacheSetup {
	int enable = 1;         ///< tune the neighbour channels ahead
	int tuners = 2;         ///< tuners for the live view and the zap cache (2: next channel, 3: next and previous channel)
	int release = 2;        ///< release the devices without key press (see cZapCache::ReleaseSeconds())
	int syncAudio = 1;      ///< start a channel switch with synchronous audio
	int maxDelay = 2;       ///< max. delay of a start with synchronous audio (see cZapCache::MaxDelayMs())
	int cam = 0;            ///< receive encrypted channels with a CAM

	bool Parse(const char *, const char *);
};

/**
 * Setup menu of the plugin with the statistics of the zap cache
 *
 * @ingroup zapcache
 */
class cMenuSetupZapcache : public cMenuSetupPage {
public:
	cMenuSetupZapcache(sZapCacheSetup *, cZapCache *);
	virtual eOSState ProcessKey(eKeys);

protected:
	virtual void Store(void);

private:
	static constexpr int LIVE_UPDATE_INTERVAL_MS = 1000; ///< update interval of the statistics

	/** a statistics item, whose text is updated, while the menu is shown */
	struct sLiveItem {
		cOsdItem *item;                     ///< the item
		std::function<cString(void)> text;  ///< creates the current text
	};

	sZapCacheSetup *m_pSetup;               ///< the setup of the plugin
	cZapCache *m_pZapCache;                 ///< the zap cache (statistics)
	sZapCacheSetup m_data;                  ///< the edited values
	const char *m_tuners[2];                ///< texts of the tuners selection
	const char *m_releases[5];              ///< texts of the release time selection
	const char *m_maxDelays[5];             ///< texts of the max. delay selection
	std::vector<sLiveItem> m_liveItems;     ///< statistics items
	cTimeMs m_liveUpdateTimer;              ///< timer for the next update of the statistics

	void Create(void);
	void AddLiveItem(std::function<cString(void)>);
	void UpdateLiveItems(void);
};

#endif

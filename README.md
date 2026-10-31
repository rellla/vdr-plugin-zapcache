# vdr-plugin-zapcache

A [VDR](https://www.tvdr.de) plugin, which tunes the neighbour channels of the
live channel ahead on free devices and buffers them, so a channel switch to
them is faster.

Written by: Andreas Baierl <ichgeh@imkreisrum.de>

License: AGPL-3.0-or-later (see LICENSE.md), the service interface
`zapcache_service.h` is MIT licensed, so any output plugin can use it.

## How it works

After a channel switch, the next channel in the direction of the last switch
(channel up or down) is received on a free device with the lowest priority. A
switch to this channel doesn't have to tune and to wait for the tuner lock,
because VDR prefers a device, which is tuned to the transponder already. After
a switch to another channel and after VDR's start, the next higher channel is
received. The device of the last live channel is only taken for it, if no other
device is free, so a switch back doesn't have to tune.

The packets of the channel since its last I-frames are buffered. With an output
plugin, which supports the service interface (e.g. drmdevice), the switch
starts with a buffered I-frame instead of waiting for the next one. With any
other output plugin the channel is only tuned ahead.

A radio channel keeps the audio of the last 3s. A switch to it starts with as
much buffered audio, as the output device needs to start the playback (up to
zapcache.MaxDelay), so the sound starts at once.

The zap cache only works, while the live view is in transfer mode (the primary
device is an output device without tuner), and only on devices, which are free
(or tuned to the transponder of the channel). A recording or the live view gets
the device at any time. The output device can switch the zap cache off (e.g.
while pip is shown, because pip uses the free devices, too).

While a channel number is entered with the number keys, the entered channel is
tuned ahead, too (on a free device or on a device of the neighbour channels).
So the switch may start with its buffer, when the entry is done (by OK or by
VDR's channel entry timeout).

## Requirements

- VDR 2.4 or later
- for the start with the buffered packets: an output plugin with the service
  interface (see below)

## Setup (setup.conf)

	zapcache.Enable = 1
		0 = off
		1 = tune the neighbour channels ahead

	zapcache.Tuners = 2
		Tuners for the live view and the zap cache (only, if VDR has at least
		three tuners, selectable in the setup menu)
		2 = the next channel in zap direction is received
		3 = the channel in the opposite direction (usually the last live channel)
		    is received, too, preferably on the device, which is tuned to it
		    already. So a switch back starts with its buffer, too. The live
		    channel has a receiver on its device, too, so its buffer continues
		    after a switch and a quick switch back starts with it at once.
		The receivers of channels, which are neighbours after a channel switch,
		too, are kept with their buffers.

	zapcache.Release = 2
		Release the devices after a time without key press (and without channel
		switch), so VDR's EPG scan can use them. The EPG scan starts 60s after
		the last key press. The next key press tunes the channels again, but not,
		while a menu is shown (only after it is closed).
		0 = never
		1 = after 20s
		2 = after 50s
		3 = after 5min
		4 = after 30min

	zapcache.SyncAudio = 1
		0 = a channel switch starts with the newest buffered I-frame. Its audio
		    is sent later by the broadcaster (the video is sent ahead of the
		    audio, often about 1s), so the playback waits for it.
		1 = the switch starts with the newest buffered I-frame, whose synchronous
		    audio is buffered, too (up to the playback start threshold of the
		    output device), so the audio doesn't have to be waited for. The buffer
		    keeps the last I-frames within 150 frames for it. The picture is up to
		    one GOP later than with 0 then. If no I-frame has enough audio yet
		    (e.g. right after the channel was tuned), the switch starts with the
		    newest one. The delay is limited by zapcache.MaxDelay.

	zapcache.MaxDelay = 2
		Max. delay of a channel switch with synchronous audio (the time from the
		I-frame with synchronous audio to the newest buffered video packet). If
		the I-frame is older, the switch starts with the newest I-frame and waits
		for its audio.
		0 = 1s
		1 = 1.5s
		2 = 2s
		3 = 3s
		4 = 5s

	zapcache.Cam = 0
		0 = an encrypted channel is only tuned, not buffered
		1 = an encrypted channel is received with a CAM (experimental): VDR
		    selects the device and a CAM, which can decrypt the channel, like for
		    a recording with the lowest priority (-98). A device, which is used
		    by the live view or a recording, isn't taken, so their decryption
		    isn't disturbed. A switch to the channel starts with its buffer and
		    doesn't have to wait for the keys of the CAM.
		    It only works, if a CAM is free for the channel: a CAM, which
		    decrypts one channel only (most CI+ modules), is used by the live
		    view, if the live channel is encrypted, too. A CAM with MTD (multi
		    transponder decryption) or several CAMs can decrypt several channels.
		    A recording or the live view takes the CAM back at any time. With
		    three tuners the receiver of the live channel keeps its CAM after a
		    switch, until another channel needs it.
		    With several CAMs VDR detaches a receiver, whose channel isn't
		    decrypted within 3s, and doesn't try this CAM for the channel for
		    15s. Some CAMs don't like the frequent channel changes, so it is off
		    by default.

## Statistics

The setup menu shows the received channels, how many switches started with a
buffer and the average switch times (until the first picture and until the
playback started) with and without a buffer, the average delay of the switches
with a buffer and the average frame corrections of the A/V sync (dropped and
repeated frames) within 5s after the playback start. The output device reports
the first picture, the playback start and the frame corrections, the zap cache
measures the times since the start of the switch. Without these reports the
values are shown as "-". The red key resets the statistics.

## SVDRP commands

	STAT              status and statistics of the zap cache
	RSTS              reset the statistics
	ENAB [on|off]     switch the zap cache on or off (zapcache.Enable, stored in
	                  setup.conf), without option: show, if it is on

e.g. `svdrpsend plug zapcache STAT`

## Service interface for output plugins

An output plugin copies `zapcache_service.h` and gets the functions once in its
cPlugin::Start() (VDR starts all plugins, before it switches to the first live
channel):

	ZapCacheService_v1_0 zapCache = {};
	cPluginManager::CallFirstService(ZAPCACHE_SERVICE_V1_0, &zapCache);

Its cDevice::PlayTs() calls for every TS packet, before it plays it:

	if (zapCache.SpliceArmed && zapCache.SpliceArmed())
		zapCache.Splice(data, startThresholdMs, PlayBufferedTs, this);

The transfer receiver and the zap receiver of the new channel get the same
packets from the receiver thread of the device one after another. So the
buffered packets, which precede the first packet of the new channel, are
played without a gap and without a duplicate packet. Only the output device
can do this, because every packet of the transfer passes cDevice::PlayTs().

Optional calls:
- SetOutputAvailable(false), while the output device can't use the zap cache
  (e.g. detached or pip shown),
- the return value of Splice(), to know, if a switch started with the buffer
  and its delay to the live stream,
- ReportSwitchEvent() for the first picture and the playback start and
  ReportFrameCorrection() for every dropped or repeated frame (statistics).

See `zapcache_service.h` for the details.

## Service for other plugins

Any plugin can query the channels, which are received ahead, with the service
`ZapCache-Channels-v1.0` (e.g. a skin, which marks them in the channel list):
their numbers, the receiving devices and if a switch to them can start with
the buffer. See `zapcache_service.h`.

## Tests

The unit tests of the buffer use Catch2 v3:

	make -C tests test
	make -C tests BUILD=sanitize test

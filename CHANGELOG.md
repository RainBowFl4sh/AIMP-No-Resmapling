# Changelog

## 2.5.4

New:
- Updates: after AIMP has installed a downloaded update, the plugin notices the new files and restarts AIMP by itself (AIMP only loads new plugin versions at start-up). The current track continues at the same position and the sample rates stay as they are
- Downloads for every platform: the .aimppack contains Windows 32-bit, Windows 64-bit and Linux; releases also offer separate win32, win64 and linux-x64 ZIPs for manual installation (plugin file directly in the PreventResampling folder - AIMP does not load it from an x64 sub-folder). The package follows the layout documented in the AIMP SDK (Linux library in x64)
- Built against the AIMP SDK of AIMP 6.00 Beta 7, now part of the repository (sdk/)
- Linux: the Voicemeeter tab shows all options greyed out with a "Linux detected - this tab is disabled" note
- Install and update only with the .aimppack - copying the DLL over an installed version fails while AIMP is running, because Windows does not allow overwriting a loaded DLL. The README explains this
- Official support for AIMP 4 (4.70 or newer) besides AIMP 5 and 6. Tested in real AIMP builds: 6.00 Beta 7 (32/64 bit, Windows and native Linux), 5.40 (32/64 bit) and 4.70, each installed through the .aimppack and with Windows 7, 8, 8.1, 10 and 11 emulated by Wine - switching, Voicemeeter, restoring on exit, the AIMP restart and the settings page all work in every version. AIMP 3 is not supported (no settings pages and no thread service in its plugin interface); the log now says which AIMP service is missing

Fixed:
- ASIO / WASAPI exclusive / DirectSound with the restart option: when the plugin closed AIMP to apply a new rate, "Restore the original sample rates when AIMP closes" also ran - Windows and Voicemeeter went back to the old rate and the restarted AIMP had to switch everything again. A restart by the plugin now keeps the new rates. The rates from before the session are handed to the restarted AIMP (settings file, section [Session]), so closing AIMP yourself still restores them
- Linux: AIMP for Linux resamples every track to its own fixed rate (Sound Output -> Parameters), just like on Windows. The plugin set PipeWire to the track's rate, so a 96 kHz track was resampled twice (96 -> 44.1 kHz in AIMP, 44.1 -> 96 kHz in PipeWire). PipeWire now follows AIMP's rate instead; following each track will need an AIMP restart on Linux (planned). Found while testing the native AIMP 6.00 Beta 7 for Linux, which installs the .aimppack correctly
- AIMP 4: the plugin did not know AIMP's output device (AIMP 4 does not offer it to plugins), so ASIO / exclusive / DirectSound and Voicemeeter were not detected. It is now read from AIMP's settings; the output list on the Output tab shows it greyed out
- AIMP 4: after switching or after a restart by the plugin the track started from the beginning instead of continuing at the same position (AIMP 4 cannot start a track at a position). The plugin now seeks to the position as soon as AIMP has opened the track
- Update check: a failed check (e.g. GitHub not reachable) was shown as "You have the latest version" after the next start and only repeated after the full interval. The About tab now says that the last check failed, and the check is repeated at the next AIMP start (at most once an hour). Error messages from AIMP are logged on one line

## 2.5.3

Fixed:
- The track AIMP restores at start-up was played at the wrong sample rate until another track (or the same one again) was opened. AIMP loads it before the plugin receives any events; the plugin now checks the playing track whenever playback starts and, if the rate differs, switches and continues at the same position
- Enabling the plugin while music is playing now switches to the current track's rate right away

## 2.5.2

Fixed:
- Voicemeeter was not set back to its previous sample rate when AIMP closed ("Restore the original sample rates when AIMP closes"). The rate before the session is now taken from the running engine instead of Option.sr (which can report stale values), and the plugin waits until Voicemeeter really runs at that rate before it logs out
- An output on WDM (e.g. A2) could turn red in Voicemeeter when AIMP closed: after restoring the Windows formats the plugin now restarts the Voicemeeter engine itself

New:
- Output tab: "Bit-perfect playback" notes - what has to be switched off (crossfade, equalizer / DSP, normalisation / ReplayGain, volume below 100 %), that gapless only works between tracks with the same rate, and that with effects on the plugin only prevents resampling
- Voicemeeter tab: note that it is only relevant for Voicemeeter users (Windows only), and that "Auto restart audio engine (all devices)" has to be ticked in Voicemeeter - the Voicemeeter API offers no way to set it from the plugin
- Linux: a Voicemeeter tab that explains why it has no function there (support for Linux mixers such as Pulsemeeter may follow)

## 2.5.1

Fixed:
- About tab: controls were shifted or cut off. All tabs now use the same fixed grid as the Discord Rich Presence plugin (465 x 420, anchored top-left), so nothing drifts or gets cut off when the dialog is resized
- Changelog and statistics are wrapped to the width of their box - no more horizontal scrolling

Changed:
- Settings pages streamlined in the style of the Discord Rich Presence plugin: "label: control" rows, compact info texts; the header line with name and author on the General tab is gone (it is on the About tab)
- About tab: bigger title, sharper avatar, "Install <version>" button and a "What's new?" link when an update is available
- General tab: "Open folder" link to the settings file and log

## 2.5.0

New:
- About tab like the Discord Rich Presence plugin: author, links to GitHub (page, releases, report a problem) and the complete changelog - the installed version always on top
- Update check: looks for a new release on GitHub - at every AIMP start, once a day, once a week or once a month (or switched off). New versions are downloaded, checked (SHA-256) and opened in AIMP, which installs them; a new version is opened automatically only once. "Check now" and "Install" buttons on the About tab
- Settings live in their own file `%APPDATA%\AIMP\PreventResampling.ini` (Linux: `~/.config/AIMP/PreventResampling.ini`), written with all defaults on the first start

Fixed:
- The plugin was enabled after installation if settings of an older version were left in AIMP's configuration. A new installation now always starts disabled; the old entries are removed
- Music started playing by itself when AIMP started with a paused or stopped track. The plugin now only switches while AIMP is really playing; a track that is loaded without playing is handled when you press play

## 2.4.0

- Everything in English: plugin UI, log, README
- Disabled after installation, with a warning to use the plugin only while listening to music exclusively
- AIMP restart is opt-in and limited to the outputs that need it (ASIO, WASAPI exclusive event/push, DirectSound) - never for WASAPI shared. New "ASIO / Exclusive" tab with a clear disclaimer
- Seamless restart: a still image of AIMP covers the restart, so AIMP seems to stay open
- Voicemeeter: all hardware outputs A1-A5 (option) get the right Windows format, not only A1; the engine is only restarted when something actually changed
- Faster switching with hardware-based timing (Fast / Normal / Conservative profile); Voicemeeter is polled instead of waited for
- DirectSound is a separate output type; tabs cleaned up and scrollable

## 2.3.0

- WASAPI exclusive and ASIO: AIMP always plays with its own rate (AIMPSoundOut\DeviceFreq) and reads it only at start-up. New option to restart AIMP with the new rate via a small helper; the track continues at the same position

## 2.2.0

- Voicemeeter hardware outputs are detected even if Voicemeeter names them differently than Windows; their Windows default format is switched too
- WASAPI fix: a wrong byte in IID_IMMDeviceEnumerator prevented the plugin from finding any audio device
- "Test format" button on the Statistics tab

## 2.1.0

- Renamed to Prevent Resampling, version and author on the settings page, gear button in the plugin list
- Voicemeeter switched only once (stale Option.sr read-back) - fixed
- The Windows default format is set with the correct float mix format and verified afterwards
- The AIMP output device on the plugin page no longer overrides AIMP's own selection

## 2.0.0

- Native settings page in AIMP with statistics; Windows 32/64 bit and Linux builds
- WASAPI shared/exclusive, ASIO, Voicemeeter (engine rate and A1 device) and PipeWire support

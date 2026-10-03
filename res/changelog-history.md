# Versions before 2.0 - shown on the plugin's About tab only (appended to CHANGELOG.md when building)

## 1.0 (alpha, never released)

First test version, then still called "AIMP AutoRate": Windows only, configured through a config.ini file, no settings page.

Approach:
- Only the Windows side was switched: the default format of the device AIMP plays into and of Voicemeeter's hardware outputs, then playback was started again. In players that open the device with the format Windows is set to, or that follow the track's rate on their own, that is all it takes

Why it did not work with AIMP:
- AIMP does not simply follow the device. On ASIO, WASAPI exclusive and DirectSound it plays with its own fixed output rate (Preferences -> Sound Output -> Parameters), reads that rate only at start-up and resamples every track to it - switching the device changed nothing
- Voicemeeter's engine was only restarted, its sample rate (Option.sr) stayed the same - so Voicemeeter resampled instead
- Every switch started the track from the beginning, and the plugin was active right after installation

That is why version 2.0 was rebuilt around a different approach: the plugin finds out which output AIMP really uses and which rate AIMP itself plays with, switches Voicemeeter's engine rate, and gets its own settings page - from 2.3 on it also restarts AIMP where AIMP's own rate has to change. Version 1.0 was never released.

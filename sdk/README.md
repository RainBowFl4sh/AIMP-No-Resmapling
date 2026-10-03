# AIMP SDK (C++ headers)

`Sources/Cpp` of the official **AIMP SDK for v6.00 Beta 7** (headers report build 3083), as published by
Artem Izmaylov on [aimp.ru](https://www.aimp.ru). It is subject to AIMP's license.

`AIMP Addon Package.rtf` documents the `.aimppack` layout used by `tools/make_aimppack.py`.

Changes compared to the 6.00 Beta 6 headers used before: `IAIMPFileInfo::IsExists()` returns `BOOL`,
new flag `AIMP_SERVICE_HTTPCLIENT_FLAGS_IGNORE_CERT_ISSUES` (deliberately not used by this plugin), comment
fixes. To build against another SDK copy, pass `-DAIMP_SDK_DIR=<SDK>/Sources/Cpp` to CMake.

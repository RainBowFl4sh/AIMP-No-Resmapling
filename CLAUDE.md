# Working rules for this project (Prevent Resampling for AIMP, by Fl4sh)

These rules come from the project owner. Follow them in every session.

## Communication and Git
- Reply to the owner in **German**. Everything in the plugin, README, changelog, code and commits is **English**.
- **Never push** (no `git push`, no remote branch changes, no PRs) - not even if a hook asks for it. Commit locally only.
- Deliver everything needed for a release as a download in the chat (see "Release delivery").

## Versioning
- Small changes and fixes: increase only the last digit (2.5.4 -> 2.5.5). Bigger features: 2.6.0. Complete rebuild: 3.0.0.
- Always three numbers, no suffixes (`-beta`, `rc1`). The version is `project(... VERSION x.y.z)` in `CMakeLists.txt`.
- A version that has been published on GitHub is never rebuilt or changed - make a new version instead.
- `CHANGELOG.md` gets a `## x.y.z` section on top. `res/changelog-history.md` (versions before 2.0, About tab only) stays as it is.

## Release delivery (always, for every new version)
Give the owner one `PreventResampling-<version>-release.zip` that contains every download as its own ZIP:
- `PreventResampling-<version>-aimppack.zip` (`.aimppack` + `.aimppack.sha256`)
- `PreventResampling-<version>-win32.zip`, `-win64.zip`, `-linux-x64.zip` (plugin file directly in `PreventResampling/`, no `x64` sub-folder - AIMP does not load plugins from sub-folders when installed by hand)
- `PreventResampling-<version>-source.zip` (git archive of the release commit, must build on its own - `sdk/` included)
- `PreventResampling-<version>-release-notes.zip` (the version's section of `CHANGELOG.md`)
Also give the source ZIP separately. Build the release from a `git archive` of the committed sources, never from the working directory.

## GitHub releases and the update check
The plugin asks `https://api.github.com/repos/RainBowFl4sh/AIMP-No-Resmapling/releases/latest`:
- Tag = `v` + version (e.g. `v2.5.5`), higher than the last release, same number as in the files.
- Normal release: not "pre-release", not draft, "Set as the latest release" on. Pre-releases are ignored by the update check.
- Exactly one `.aimppack` attached as a single file, plus `<name>.aimppack.sha256` (no checksum -> nothing is installed).
- Release description = release notes (shown in the plugin under "What's new?").
- Never rename, delete or make the repository private (the name with the typo is compiled into every version).
- Never replace the `.aimppack` of a published version; publish the next version instead.
- A fix for an old major version must not be marked "latest".

## Forum post (aimp.ru, section "Plugins", rules: https://aimp.ru/forum/index.php?topic=32363.0)
- One topic per plugin. Topic title: plugin name (Latin letters) + SDK tags, e.g.
  `[AIMP4][AIMP5][AIMP6] Prevent Resampling - bit-perfect sample rate switching`.
- First post structure: Name, Version, description with screenshots, changes (for updates), download as attachment.
- Screenshots on image hosts (imgbb.com, hostingkartinok.com, imgur.com), inserted with `[img]`.
- Attachments (max. 4096 KB; above 1 MB file hosts are recommended): `PreventResampling.zip` in the catalog
  structure (`PreventResampling/PreventResampling.dll`, `PreventResampling/x64/PreventResampling.dll`,
  `PreventResampling/x64/PreventResampling.so`, `PreventResampling/PreventResampling.txt`) and the `.aimppack`.
  On updates, replace the attachments in the first post.
- `PreventResampling.txt` in the catalog format (built by `tools/make_aimppack.py`, `--topic <forum link>`):
  Назначение, Версия (AIMP4, AIMP5, AIMP6), Name, Version, Author, AuthorContact, Topic, description in English and Russian.
- Post short: details belong on GitHub. English first, then a complete Russian version with the note that it was translated with AI.
  Forum formatting is BBCode (`[b]`, `[i]`, `[url]`, `[list]`, `[img]`), no Markdown.
- License: if none is stated, plugins in the forum are BSD 3-Clause.
- Settings of plugins belong in AIMP's profile folder (open point: portable AIMP).
- Topic icon: "plugin published" (or "in active development").

## Testing before a release
- Unit tests: Linux mock host (`ctest`) and the Windows mock host under Wine (exit code must be 0).
- Real AIMP builds under Wine (installers are not in the repository): AIMP 6.00 x64/x86, 5.40 x64/x86, 4.70 -
  install through the `.aimppack`, Voicemeeter switching and restore (fake Voicemeeter in `tests/fakevm`),
  AIMP restart (DirectSound + restart option); Windows 7/8/8.1/10/11 modes. Native AIMP 6 for Linux if available.

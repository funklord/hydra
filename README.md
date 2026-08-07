# Hydra — a tree-shaped browser

Working name (rename freely — it's `TARGET` in `hydra.pro` and `@target` in
`src/main.cpp`). A Linux/X11 desktop browser on **Qt 6 Widgets** (no QML)
and **Qt WebEngine**: a side-tree of tabs and links over its own embedded
Chromium, with a per-site security policy engine, kiosk mode, an AI tree
reorganizer and ad-filter loop, a media detector with external-player handoff,
downloads including BitTorrent, and a KeePassXC-backed password manager.
**Android is a first-class target** and builds an APK against the system
WebView.

All seven build-order steps of the architecture doc are implemented. See
`project.md` for what currently *is* — including what has been measured versus
what is merely assumed — and `docs/architecture.md` for the full design;
`tests/README.md` before running anything.

## What works

**The tree and the shell**

- **Shell** — classic desktop furniture in a plain `QWidget` (not a
  `QMainWindow` — `docs/architecture.md` §6): menu bar, toolbar, a splitter with
  the tab tree on the left and chrome-less web views on the right, status bar.
- **Tree model** — `tab_tree_model` loads the canonical outline file, exposes
  custom sort roles, and keeps an id index for O(1) lookup. Nesting is bounded
  at 64 and the invariants are checked.
- **Sorting and search** — tree order / title / created / last-seen, folders
  first, plus a live filter that keeps ancestors of matches visible.
- **Tab lifecycle** — unopened → open → suspended, with an LRU cap on live
  views; a suspended tab's navigation history is a state blob keyed by node id.
- **Drag and drop** — tabs move like files, with folders opening on hover and
  Ctrl-drag copying.
- **Importers** — Firefox's open tabs and Chromium's session log can be read
  into a mirror folder of their own, off unless asked for.
- **Chrome** — find-in-page (Ctrl+F), per-tab zoom, a stop control, link targets
  on hover, the page in the window title, and a loading indicator. A page's
  new-window request becomes a child tab rather than a second window.

**Security and filtering**

- **policy_engine** — per-site rules as packed 2-bit tri-states, precedence
  (exact host > `*.domain` > global default), persisted as INI. Covers
  JavaScript, cookies, third-party cookies, ads, popups, images, autoplay,
  location, camera, mic, notifications and referer.
- **Interceptor** — one request filter on the shared profile blocks ads, scripts
  and images per policy and strips Referer; the same filter serves Android.
- **Shield editor** — a per-site tri-state popup, scoped this-host / this-domain
  / global.
- **Filter evolution** — passive signals, an element picker, dry-run validation,
  and a diff/accept dialog; rules can be exchanged as files with provenance.
- **Consent banners and anti-adblock** — answers "accept cookies?" dialogs from
  a shared rule store, and says so when a page is checking for a blocker.
- **Certificates, authentication and crashes** are reported rather than silent:
  a rejected certificate says so, an HTTP authentication challenge asks, and a
  dead renderer names itself.

**Media, downloads and AI**

- **Media detector** — URL-shaped classification plus a Media Source tap that
  reports what a page is actually playing, with capture to disk.
- **Player handoff** — PATH probe and capability routing to an external player;
  yt-dlp (vendored or on PATH) resolves a page to real stream URLs and headers.
- **Site extractors** — generated, sandboxed scripts with a gate, a review loop,
  and an optional per-site helper tier that may fetch but not touch the DOM.
- **Downloads** — one queue behind a transport seam, HTTP and BitTorrent
  (libtorrent-rasterbar) sources, resume, and a single list for both.
- **AI reorganizer** — a metadata-only payload to a local-first provider
  (Ollama, or Claude as an explicit external opt-in), an invariant check, and a
  non-destructive diff/accept with undo.
- **Password manager** — the KeePassXC-Browser protocol with the pairing key in
  the session keyring; autofill behind an origin gate and per-site policy.
- **Kiosk mode** and a **settings window** laid out the way browsers that
  outgrew a tab strip do, with export/import of every setting as one INI file.

## Requirements

- **Qt 6.8 or newer.** The floor is 6.8 because `QWebEnginePermission` and
  `Qt::ColorScheme` are used unguarded; 6.8.2 and 6.11 are the versions actually
  built against, and `qmake` refuses anything older with a message rather than a
  wall of template errors. `Qml` is wanted only for `QJSEngine`, the extractor
  sandbox — there is no QML in the UI.
- `make`, `qmake` (Debian: `qmake6`) and a C++17 compiler.
- **Optional, each buying one feature and nothing else.** A missing one is a
  smaller build rather than a failure, which is worth knowing: without
  `libsodium` there is no KeePassXC bridge, without `libtorrent-rasterbar` no
  BitTorrent, without `libsecret-1` the KeePassXC pairing does not survive a
  restart, and without `liblz4` Firefox session files go through this project's
  own decoder instead.
- **X11 / XWayland** on Linux — `main.cpp` forces `QT_QPA_PLATFORM=xcb` there
  unless the environment already set it, matching the X11-only design decision.
  The forcing is guarded to desktop Linux.

### Every dependency, and the package that carries it

Debian and Ubuntu names. The Qt column is what `hydra.pro` asks for; the
package column is what `dpkg -S` says provides it, rather than what looked
likely.

| what for | Qt module / library | Debian package |
|---|---|---|
| the shell, networking, DBus, and the test harness | `widgets` `network` `dbus` `Test` | `qt6-base-dev` |
| moc, rcc, uic | — | `qt6-base-dev-tools` |
| the page bridge | `webchannel` | `qt6-webchannel-dev` |
| the extractor sandbox (`QJSEngine`) | `qml` | `qt6-declarative-dev` |
| the engine (not on Android) | `webenginewidgets` | `qt6-webengine-dev` |
| KeePassXC bridge — *optional* | libsodium | `libsodium-dev` |
| pairing that survives a restart — *optional* | libsecret-1 | `libsecret-1-dev` |
| BitTorrent — *optional* | libtorrent-rasterbar | `libtorrent-rasterbar-dev` |
| Firefox session decoding — *optional* | liblz4 | `liblz4-dev` |

**`qt6-webengine-dev` happens to pull in the declarative and webchannel
packages**, so a shorter list builds too. They are named anyway because this
project uses them directly and an Android build asks for no WebEngine at all,
which is exactly where a transitively-satisfied dependency stops being
satisfied.

```sh
sudo apt install build-essential pkgconf \
  qt6-base-dev qt6-base-dev-tools qt6-declarative-dev \
  qt6-webchannel-dev qt6-webengine-dev \
  libsodium-dev libsecret-1-dev liblz4-dev libtorrent-rasterbar-dev
```

Arch: `qt6-base qt6-declarative qt6-webchannel qt6-webengine libsodium
libsecret liblz4 libtorrent-rasterbar`.

`debian/control` carries the same list as `Build-Depends`, and CI installs it
explicitly; all three are meant to agree, and the two that did not were found by
writing this table.

## Build & run

```sh
make                          # build
make run                      # build and run against a scratch copy of the tree
make test                     # every suite that needs nothing but a build
make style                    # the indentation gate and the document gate
make help                     # the rest: android, install, deb, clean, DEBUG=1 …
```

`./build/hydra my-tree.txt` points it at your own outline file. Builds are
`-Os`; `DEBUG=1` switches to `-Og -g`.

**Do not run `make -j` or `fmake` without a job count.** Each of the 34
live drivers links Qt WebEngine, and unlimited parallelism has taken this
machine's desktop session down twice — the OOM killer takes the whole user
session, not just the build. The Makefile defaults to `-j2` and takes `JOBS=`.

**Two build systems are maintained, and the Makefile is the interface to
both.** Underneath it, `hydra.pro` (qmake) builds the app and the APK, and
`tests/Makefile` builds the test tree. The second is **fmake**, which builds the
same sources from no build file at all:

```sh
fmake -C src -j2              # name a number; see the JOBS warning above
```

It works the whole build out by itself -- every `Q_OBJECT`, the moc runs, the
platform-specific sources, and a link set closed over symbols rather than
guessed. The six things it cannot know are annotations in the sources: `@target`
in `main.cpp`, and one `@pkg_optional` beside each optional dependency's
include. `project.md` carries the measurements and the reasoning.

## Android

```sh
make android                  # the APK (make apk is the same target)
```

Needs `QT_ANDROID_ROOT` pointing at a Qt for Android kit, plus
`ANDROID_SDK_ROOT`, `ANDROID_NDK_ROOT` and `JAVA_HOME` — each checked before
anything runs, because Gradle will not run on a JRE and the failure a long way
in says something else. Installing and running are `adb` by hand for now; the
`android-install` / `android-run` / `android-log` vocabulary is agreed across
these projects but not implemented here. There is
no Qt WebEngine for Android at all, which is why the `web_view_backend` seam
exists: the port runs on the system WebView, with the request filter, content
scripts, file picker, external links, player handoff and downloads all carried
over. The remaining gap is the platform's own autofill service, which needs a
device to verify.

## File format (canonical tree)

Two spaces per depth level; fields separated by ` | `:

```
- [f0] folder | Work
  - [a1] open | Qt Documentation | https://doc.qt.io | created=… | seen=…
  - [a2] unopened | Wikipedia | https://www.wikipedia.org
```

First field is the node type (`folder` / `open` / `unopened` / `suspended`);
trailing `created=` / `seen=` are optional ISO-8601 timestamps. This file is the
authority for structure and manual order — it is plain text on purpose, so it is
diffable and hand-editable, and it is exactly what gets serialized to a model.

`policy.ini`, the tree file and `state/<id>.blob` all sit beside the outline file
named on the command line. The one thing that does not is the KeePassXC
association key, which lives in the session keyring instead.

## Licence

GPL-3.0-or-later — see `LICENSE`.

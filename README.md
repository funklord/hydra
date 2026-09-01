# Hydra — a tree-shaped browser

Working name (rename freely — it's `TARGET` in `hydra.pro` and `@target` in
`src/main.cpp`). A Linux/X11 desktop browser on **Qt 6 Widgets** (no QML) over
its own embedded Chromium, with **Android as a first-class target** running on
the system WebView behind the same seam.

The shape of it: your tabs are a **tree in a plain text file**, one request
filter and one policy engine serve both engines, and the parts that would
normally be extensions — ad filtering, consent banners, media capture, a
password manager, a torrent client — are in the browser and answer to the same
per-site rules.

`project.md` is the working record: what *is*, what was measured, and what is
merely assumed. `doc/architecture.md` is the design. `test/README.md` before
running anything.

---

## What it does that other browsers don't

The list the rest of this file exists to support. Some of these are large and
some are a single line of code; they are here because a mainstream browser does
not do them, or does something adjacent that is not the same thing.

### Your tabs are a file

- **The tab tree is a plain-text outline you can edit, diff and commit.** Not a
  cache of a database — *the* authority for structure and manual order. Open it
  in an editor, fix a title, move a subtree, `git diff` it. Tree-tab extensions
  elsewhere keep this in browser-internal storage you cannot reach.
- **`unopened` is a real state.** A node with a url that has never been loaded,
  kept for ever at no cost. It fills the role a bookmark does, which is why
  there is no separate bookmark store to keep in step with the tabs.
- **Suspended tabs keep their history.** Not just the url: the back/forward
  stack is a state blob keyed by node id, so waking a tab lands where you left
  it.
- **Tabs move like files.** Drag between folders, folders spring open on hover,
  Ctrl-drag copies instead of moves.
- **A popup becomes a child tab**, nested under the page that opened it, rather
  than a second window you have to re-find.
- **Sub-tabs and locked tabs** — a tab can be pinned to its node so a
  navigation opens a child instead of replacing it.
- **Another browser's tabs import into a mirror folder of their own** — Firefox's
  open tabs and Chromium's session log — kept separate from yours rather than
  merged into them.
- **Sort by tree order, title, created, or last-seen**, with a live filter that
  keeps the *ancestors* of matches visible so a hit never appears rootless.
- **Nesting is bounded and checked.** 64 deep, with tree invariants asserted
  rather than hoped for.

### Blocking, and admitting when it fails

- **"Something got through here."** One toolbar button that reports the page you
  are on as one where blocking failed. Every other browser makes you find the
  filter list's issue tracker.
- **It tells you when a page is checking whether you block ads**, instead of
  silently losing the arms race.
- **Cookie consent banners are answered for you**, taking the least permissive
  option the site actually offers — not dismissed, not accepted, *answered*.
- **Rules are files with provenance.** Learned filter rules and consent rules
  can be exported, handed to somebody else, and imported with a record of where
  they came from.
- **An element picker** for building your own cosmetic rule by pointing at the
  thing.
- **A filter-evolution loop**: passive signals from pages you visited become
  proposed rules, dry-run validated against real requests, then shown as a diff
  you accept or reject.
- **Nineteen per-site capabilities**, not the usual handful — javascript,
  cookies, third-party cookies, ads, popups, images, autoplay, location, camera,
  microphone, notifications, referer, autofill, extractor-fetch, cookie notices,
  clipboard reading, pointer lock, screen sharing and media auto-detection.
- **Packed two bits per feature into one integer**, with precedence: exact host
  beats `*.domain` beats the global default.
- **The same request filter serves both engines.** The policy you set applies
  identically to desktop Chromium and to Android's system WebView, because it is
  one filter with two adapters rather than two implementations.

### Media it can actually see

- **A Media Source tap.** Most "download this video" tools guess from urls. This
  watches what the page hands the decoder, so it reports what is *actually
  playing* — including streams whose urls say nothing.
- **Capture to disk** from that tap, with HLS playlist assembly and remux.
- **yt-dlp handoff**, vendored or from PATH, resolving a page to real stream urls
  *and the headers they need*.
- **External player routing** with a PATH probe and capability matching — it
  checks what your player can actually open before sending it there.
- **Site extractors**: generated, sandboxed JavaScript that learns how a
  particular site hides its manifest, behind a gate and a review loop.
- **A helper tier with two separate powers** — an extractor may be allowed to
  *fetch* a manifest the page already asked for without being allowed to touch
  the DOM. One switch for both would quietly grant the second to get the first.

### Downloads that are not just HTTP

- **BitTorrent is a first-class source in the same queue.** Not a separate app,
  not a handoff — magnet links and torrents download beside HTTP transfers in
  one list, behind one transport seam.
- **Watch a torrent while it downloads**, through a local proxy that serves the
  incomplete file to your player.
- **In-page magnet links work**, rather than dead-ending on a scheme the browser
  does not own.

### An AI that is not a chat box

- **Reorganize the tree.** The model is handed *metadata only* — titles, urls,
  timestamps — proposes a structure, and the result is checked against the tree's
  invariants before you ever see it. Then shown as a diff to accept or reject,
  with undo.
- **Local-first and explicit.** Ollama by default; Claude is an opt-in you have
  to make, not a default you have to find and disable.
- **Nothing is destructive.** Every AI output in this browser arrives as a
  proposal with a diff.

### Passwords without an extension

- **The KeePassXC-Browser protocol, implemented directly.** No browser
  extension, no native-messaging host — the browser speaks the protocol, with
  libsodium doing the crypto and the association key living in the session
  keyring rather than on disk.
- **Autofill behind two gates**: a strict origin match *and* the per-site policy,
  and it declines on plain HTTP unless you say otherwise, because filling a
  password over HTTP puts it on the wire.

### Permissions that say what is happening

- **Screen sharing asks twice, on purpose.** "May this site present" is a
  decision about a site and can be remembered; "share *that* window, now" is a
  decision about this moment and is asked every single time. A meeting allowed
  to present last week has not been allowed to present whatever is open today.
- **The picker says what is at stake** — everything on what you pick is sent,
  including whatever appears on it later.
- **A refusal is visible.** A blocked capability used to reach the page as an
  error and reach you as nothing at all; the shield's decisions are now
  inspectable rather than silent.

### The rest

- **Kiosk mode** in the browser, with an unattended-running page in settings.
- **Every setting exports and imports as one INI file** — the whole
  configuration, portable, diffable, and readable by a person.
- **One instance per profile**, so a second launch reaches the running window
  instead of fighting it over the same files.
- **A certificate refusal, an authentication challenge, a blocked popup and a
  dead renderer each say so.** Every one of those was silent at some point in
  this project's history, and each silence is recorded in `project.md` as the bug
  it was.

---

## Rare, or easy to get wrong

Not unique — other browsers do these — but they are the parts that take real
work, and most of them were got wrong here first and are written up in
`project.md`.

- **Two rendering engines behind one seam.** `web_view_backend` is implemented
  by Qt WebEngine on the desktop and by Android's system WebView through JNI.
  There is no Qt WebEngine for Android at all, which is why the seam exists
  rather than being tidiness.
- **The JNI boundary is a threading problem, not a plumbing one.** Android's UI
  thread and Qt's thread can each block waiting for the other — that deadlock
  happened here, in ordinary navigation, and the ANR trace is in `project.md`.
  The fix is to answer questions that do not need the other thread on the thread
  that asked, and to put a deadline on the ones that do.
- **A native WebView is composited above everything Qt draws.** So every dialog,
  menu, drop-down and tooltip has to hide the page while it is up, or it opens
  behind it and cannot be seen. One predicate, used by both the guard and the
  counting loop, because two copies of "what counts" is exactly how menus came
  to be missed.
- **An asynchronous permission decider.** A `bool` cannot say "I am going to ask
  a person and tell you afterwards", so the answer is a callback that may arrive
  long after the question — and every call site captures by value, because the
  tab it was asked about may be gone.
- **Nothing is written non-atomically.** Every persisted file goes through
  `QSaveFile`; the process saves on SIGTERM/SIGINT/SIGHUP through a self-pipe;
  and the debounced writers were each verified against a real `SIGKILL`.
- **Notifications are actually delivered.** Chromium treats a missing
  notification presenter as *success*: the page's promise resolves and nothing
  appears. There is a real presenter over `org.freedesktop.Notifications`, the
  call is asynchronous so a slow daemon cannot hold the GUI thread, and a page
  closing its own notification reaches the service.
- **The user agent is corrected**, because a bank turned this browser away over a
  token no real browser sends.
- **`Accept-Language` is sent at all** — Qt sets no default and nothing asked for
  one, so every request went out without it until that was measured.
- **Light and dark follow the desktop**, read from the portal rather than
  guessed, and applied before the first window is built.
- **The Android launcher icon is an adaptive icon done properly** — 108 dp
  canvas, 72 dp visible, a real plate colour, and sized by measuring the other
  browsers on the phone rather than by reading the spec.
- **Two build systems, and one of them has no build file.** `hydra.pro` is
  qmake; `fmake` derives the entire build from the sources — every `Q_OBJECT`,
  the moc runs, the platform-specific files, and a link set closed over symbols.
  Six annotations in the source are the only things it cannot infer.
- **The tests drive the real thing.** Forty-three test suites — most needing
  nothing but a build, a handful wanting a network or a device — and forty
  live drivers that stand up the actual shell against a local server and check
  what a page sees. Every dialog is measured at phone geometry — width, cut
  labels, focus, Tab coverage, and paragraphs absorbing spare height — and
  renders a picture, because the last two dialog defects were found by looking
  at one.

---

## What is not done

Kept here rather than left for you to discover.

- **Android geolocation cannot work.** No location permission is declared or
  requested, and the WebView's geolocation prompt is not handled — so the
  setting promises something the platform will not deliver.
- **Android has no screen sharing**, and the desktop's has never met a real
  `getDisplayMedia` — everything up to the engine call is tested against fakes.
- **The desktop has no application-level camera gate.** Android's OS permission
  stands in front of the camera; on Linux there is no equivalent, so a page that
  asks gets it. Set `camera=ask` in `policy.ini` for a prompt.
- **Media detection and the extractor loop are measured, not finished.** The loop
  returns a usable extractor roughly three times in five on a clean media host
  and once in five on a noisy one. `project.md` has the numbers and the failures.
- **The platform's own autofill service on Android** is unimplemented.
- **A saved `policy.ini` pins the defaults that were current when it was
  written**, so a default changed later does not reach an existing profile.

---

## How it fits together

One `QWidget` shell (not a `QMainWindow` — `doc/architecture.md` §6): menu bar,
toolbar, a splitter with the tab tree on the left and chrome-less web views on
the right, a status bar. On a narrow window the tree becomes a drawer behind the
leftmost toolbar button.

| the part | what it is |
|---|---|
| `tab_tree_model` / `tree_outline` / `tree_serializer` | the outline file, its model, and the invariants between them |
| `web_view_backend` | the seam: `qtwebengine_view` on the desktop, `android_view` over JNI |
| `policy_engine` / `request_filter` | per-site rules, and the one filter both engines consult |
| `filter_list` / `cosmetic_filters` / `element_picker` | blocking rules, learned and hand-made |
| `consent_blocker` / `antiadblock_watch` / `annoyance_log` | banners, blocker-detection, and "something got through here" |
| `media_detector` / `mse_tap` / `hls_assembler` / `media_remux` | what is playing, and getting it to disk |
| `download_manager` + `http_` / `torrent_download_source` | one queue, two transports |
| `site_extractor` / `extractor_helpers` / `ollama_provider` / `claude_provider` | the learning loop and its providers |
| `keepass_bridge` / `keepass_protocol` / `box_crypto` / `credential_store` | passwords, and the crypto under them |
| `permission_dialog` / `screen_picker` / `site_policy_dialog` | the three places a capability is decided |

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
  restart, without `liblz4` Firefox session files go through this project's own
  decoder instead, and without `Qt6DBus` there is no desktop colour scheme and
  no notifications.
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

**Do not run `make -j` or `fmake` without a job count.** Each of the live
drivers links Qt WebEngine, and unlimited parallelism has taken this machine's
desktop session down twice — the OOM killer takes the whole user session, not
just the build. The Makefile defaults to `-j2` and takes `JOBS=`.

**Two build systems are maintained, and the Makefile is the interface to
both.** Underneath it, `hydra.pro` (qmake) builds the app and the APK, and
`test/Makefile` builds the test tree. The second is **fmake**, which builds the
same sources from no build file at all:

```sh
fmake -C src -j2              # name a number; see the JOBS warning above
```

It works the whole build out by itself — every `Q_OBJECT`, the moc runs, the
platform-specific sources, and a link set closed over symbols rather than
guessed. The six things it cannot know are annotations in the sources: `@target`
in `main.cpp`, and one `@pkg_optional` beside each optional dependency's
include. `project.md` carries the measurements and the reasoning.

## Testing

```sh
make test                     # the offline suites
make test-one T=test_settings # one of them
test/live/sweep.sh            # every live driver, offscreen
```

The offline suites need nothing but a build. The live drivers stand the real
shell up against a local server and assert on what a page actually sees — they
are how the permission callbacks, the interceptor, the media tap and the
dialogs are checked, because a unit test of any of those would be testing Qt.
`try_phone` opens every window at 360×640 and measures it. `test/README.md` says
what each one needs.

## Android

```sh
make android                       # arm64-v8a, which is a phone
make android ANDROID_ABI=x86_64    # which is what an emulator usually is
```

`ANDROID_ABI` selects the architecture and the Qt kit follows it — the kit is
found under `QT_ROOT` (`~/Qt`), newest Qt first, and is asked what it actually
builds before anything compiles. It used to select nothing but the output
filename, so that second line produced an arm64 apk called x86_64; a mismatch
now fails, and `make apk` reads the ABI back out of the finished zip rather
than trusting the name. Naming a kit by hand still works and is checked the
same way:

```sh
make android ANDROID_ABI=x86_64 QT_ANDROID_ROOT=$HOME/Qt/6.10.0/android_x86_64
```

Also needs `ANDROID_SDK_ROOT`, `ANDROID_NDK_ROOT` and `JAVA_HOME` — each
checked before anything runs, because Gradle will not run on a JRE and the
failure a long way in says something else. Installing and running are `adb` by
hand for now.

**There is no Qt WebEngine for Android at all**, which is why the
`web_view_backend` seam exists. The port runs on the system WebView, and what
crosses over is most of the browser: the request filter and policy engine,
content scripts, the file picker, external links, player handoff, downloads,
cookies, the permission prompt, and camera and microphone capture with the
platform's own permission asked in front of them. `make jni` checks every native
method resolves against its Java declaration.

What does not cross over is listed under *What is not done* above.

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

Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>

**None, deliberately.** This browser is being developed unlicensed until it is
complete, and the terms will be settled then. No rights are granted in the
meantime, which is what an absent licence means rather than an oversight in it.

The tree carried GPL-3.0-or-later until 2026-08-31. Do not restore it and do
not add another: a licence is the copyright holder's to choose, an absent one
leaves every option open, and a published grant cannot be taken back.

Vendored and linked components keep their own terms — yt-dlp is public domain
(Unlicense), and Qt is used under its LGPL-3.0-only option, linked dynamically.

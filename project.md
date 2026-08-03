# project.md — Hydra

Working notes and conventions for this repo: what exists, what is next, and the
rules to follow while working in it. The full design lives in
`docs/architecture.md` — read it before making changes; where this file and the
architecture doc disagree about intent, the architecture doc wins, and where
they disagree about *current state*, this file wins.

## What this is

Hydra (working name) is a Linux/X11 desktop browser built on **Qt 6 Widgets**
(no QML) and **Qt WebEngine**. It presents a side-tree of tabs/links over its
own embedded Chromium, with a per-site security policy engine, and (planned)
kiosk mode, AI tree-sorting + ad-filter evolution, a media detector with
external-player handoff, and a KeePassXC-based password manager. **Android is a
planned first-class target, deferred until desktop is complete** (see
`docs/architecture.md` §19).

## Resuming work here

Read `docs/architecture.md` for what the design *intends*, this file for what
currently *is*, and `tests/README.md` before running anything. Where the two
docs disagree about intent the architecture doc wins; about current state, this
file wins.

This file is a running log and is long. The fastest orientation is: **What is
implemented** (the table below), then **What is next**, then the section for
whatever you are touching.

### On this machine

| thing | state |
|---|---|
| `libsodium` | installed — KeePassXC bridge builds |
| `libtorrent-rasterbar` 2.0.11 | installed — BitTorrent builds |
| `Qt6::Qml` | required, and only for `QJSEngine` (the extractor sandbox). No QML in the UI |
| `third_party/yt-dlp` | vendored submodule. Clone with `--recurse-submodules` |
| `yt-dlp` on PATH | **not** installed, so the vendored copy is used via `python3` |
| Ollama | installed user-local at `~/.local/ollama` (release tarball, no root). **Not running** — start with `~/.local/ollama/bin/ollama serve` |
| models | `~/.ollama/models`, ~13.7 GB: `qwen2.5-coder:7b` and `:14b` |
| inference | **CPU-only.** Ollama drops the integrated Intel GPU, so it is 12 cores / ~29 GB. A 14B proposal takes a minute or two; 32B is not viable |
| `~/Qt/6.11.1` | Qt online-installer kits: `gcc_64` (desktop, **with** WebEngine) and `android_arm64_v8a` / `armv7` / `x86` / `x86_64` |
| `~/android-ndk-r29` | NDK r29 (29.0.14206865) |

**The Android prerequisites arrived, and they confirm §19.2 rather than change
it.** Checked rather than assumed: the Android kits ship **no WebEngine at all**
— `libQt6WebEngine*` is absent from every ABI, while the desktop kit has twenty
of them. So the `web_view_backend` seam is not a nicety for a hypothetical
future; it is the only way this builds for Android, exactly as designed in step
3.5.

Two things the desktop `gcc_64` kit makes newly checkable, neither yet done:

- **Whether the app still builds against 6.11.** The system Qt here is 6.8.2, so
  everything measured in this file is 6.8.2. The deprecated permissions path
  (§"Build-verification state") is still present in 6.11.1 — verified in the
  header, behind `QT_DEPRECATED_SINCE(6, 8)` — so it should compile with
  warnings rather than break, and `permissionRequested` / `QWebEnginePermission`
  is the named replacement. Worth an actual build before believing it.
- **Whether the geolocation gap is Qt's or the build's.** `try_permissions`
  records that a *granted* geolocation request still ends as PERMISSION_DENIED
  here because this build has no location provider. A second Qt build is the
  cheapest way to find out whether that is universal or packaging.

### ⚠️ Do not build with unbounded `-j`

The live drivers under `tests/live/` each compile ~40 app sources and link Qt
WebEngine, and there are a dozen of them. `cmake --build … -j` with no number
has exhausted memory and taken the desktop session down on this machine, twice
— worst when a model is loaded, since a 14B holds ~10 GB before the compiler
starts. Use `-j2`, or name a single target. Stop Ollama first if it is running.

The mechanism, since it is easy to underestimate: this repo configures the
**Unix Makefiles** generator, and `make -j` with no number is *unlimited*, not
one job per core. Every ready translation unit starts at once. What follows is
not a failed build — the kernel OOM killer takes the whole user session slice.
The 2026-07-30 23:08 event killed `dbus`, `pipewire`, `wireplumber`, both
`xdg-desktop-portal`s, `plasma-kactivitymanagerd`, and the running browsers;
31 GB of RAM and 31 GB of swap were not enough. `systemd-oomd` is inactive
here, so nothing intervenes earlier or more gently.

### What is actually proven, and what is not

The project's habit is to measure rather than assert, so the distinction is
kept explicit:

**Measured.** The interceptor's mutations; the kiosk geometric-scale spike; a
real torrent moved over loopback byte-identically; watch-while-downloading
against a throttled swarm; capture byte-identical across 519 segments with flat
descriptor use; in-page magnet handling with real clicks; the yt-dlp handoff
end to end; the extractor loop against `qwen2.5-coder` at two sizes, and one
prompt change measured at ten runs an arm and reverted for making things worse;
the content-type tier against the real CDN, where the manifest declares
`application/vnd.apple.mpegurl` once it is asked with the page's context and
403s without it; the §11.5.1 helper tier against that same CDN, following a
master playlist to a variant the page never requested in four calls; and — at
last — **the extractor loop finding the real manifest on real evidence, 2 runs
in 5**, having been 0 in 5 every time it was asked before.

**Not measured, and known.**
- Capture under *live network* conditions — the mechanism is proven locally,
  the timing against a real site is not.
- ~~The MSE tap on a page whose player is an iframe.~~ **Answered badly, then
  fixed:** it reported nothing there — which is most real watch pages — because
  an iframe has no relay in it. A subframe hands its report to the top frame
  now. Every earlier verification had used a player page loaded as its own
  document, where no subframe is involved.
- ~~The permission callbacks.~~ **Answered.** Every one of the four is refused
  by default and grantable per host, driven through the real shell; geolocation's
  page-visible outcome is the engine's to decide and is checked at our boundary
  instead.
- ~~The cookie filter.~~ **Answered, both halves.** First-party measured through
  the real profile in both directions; third-party measured against a local TLS
  origin, where allowing it stores the cookie and blocking it does not.
- ~~The ad-host list at runtime.~~ **Partly answered, and not the way it was
  meant to be:** it demonstrably blocks, because a real player refuses to start
  while its anti-adblock script can see it. That proves the list acts on live
  traffic; it still does not prove the *matching predicate* is right, which was
  the original question. See the second-mirror section.
- The KeePassXC bridge above the crypto layer — `keepassxc` is not installed.
- ~~Whether the extractor prompt generalises past one synthetic evidence set.~~
  **Answered, badly:** it does not. Against evidence captured from the real
  site the loop returned prose and no parser in five runs out of five, so every
  hit-rate in this file describes the synthetic fixture and nothing else.
- ~~Whether it does better now that the payload carries what each address
  actually serves.~~ **Answered: not on its own.** Carrying the content types
  changed nothing — 0 of 5 — until the payload's *tail* said to use them. What
  the tier supplies and what the model reads are two different questions, and
  only the second one moved the number. See the four arms below.
- ~~Whether any of this transfers to a second site.~~ **Answered: it does not,
  yet.** A second site was captured and the loop scored 0 of 5 there — but the
  cause is measured and is upstream of the model: the probe budget never reached
  the media host, so the payload carried no content types at all. See the
  second-evidence-set section.
- The DOM half of the helper tier (§11.5.1). The fetch half is proven against
  a live CDN; the DOM half is designed and unbuilt, and nothing has needed it.

**A fourth, from this session, and it is about construction order.** An observer
was registered on the interceptor seam one line *before* the object was
constructed, so a null went into the list. That was harmless for as long as the
function it reached touched no members — and it did not, while the judgement it
made was a static one over a hard-coded array. Moving those patterns into the
shared rule store made the function read a member, and every live driver
segfaulted at once, in a feature unrelated to the one at fault. The registration
now refuses a null outright, so the next one is a mistake where it is made
rather than undefined behaviour somewhere later.

**A caution learned repeatedly.** Six separate defects this project has hit were
wiring that existed but was never exercised — a signal never connected, a
message written into a label something else overwrote, a store that was saved
and never read. Treat "wired but untested" as "probably broken", and prefer a
test that drives the real widget over one that calls the function underneath it.

**A third, and the most expensive of the three: the apparatus lies.** Six times
in one session a driver or a script reported something that was not true, and
in every case the bug was in the measuring rather than in the measured:

- A harness found the proposal pane by *content* — "whichever one mentions a
  function" — so the moment a reply was prose it reported `(none)` and threw
  away the only artefact worth reading.
- Rewritten to find it by *position*, it then stood one pane away from breaking
  silently, and would have reported the wrong text rather than failing.
- A driver clicked Send the instant the dialog opened. A later change disabled
  Send while probing, and **a disabled button ignores a click without saying
  so**, so five runs "timed out" having never sent anything at all.
- A test called a blocking fetch from the thread its own fake origin was
  listening on. **A blocked thread serves nothing**, so every request timed out
  and the fetcher looked broken.
- A scoring script counted `extract = function` and missed `function extract`,
  so two runs that produced perfectly good code were tallied as failures.
- A background job's script was overwritten while bash was still executing it.

The habit that catches these: make the harness report what it *saw*, not
whether it matched — print the payload size, the number of annotated addresses,
the elapsed time, the reply verbatim. Every one of the above was found by a
diagnostic line rather than by a check, because a check can only fail in the
ways its author imagined.

**And its mirror image, learned once and worth the same weight.** The extractor
gate had 27 checks and still accepted the page's own url as a stream, because
every check asked whether a *wrong* answer was refused and none asked what the
obvious lazy answer would be. A suite can be thorough about the failures its
author imagined and blind to the one the model finds in ten tries. When a real
model is available, running it is a cheaper source of adversarial inputs than
inventing them.

## The icon

`icons/` holds the app icon and `icons/build_icons.py` regenerates every size
from `hydra-master.png`. One drawing, downscaled, with the small sizes
retouched afterwards.

**The master is the artwork cropped hard to the ink.** Its white plate and
cream halo are flood-filled away — flood, not "delete white", so a highlight
inside an eye survives while the halo does not — and then it is trimmed. Trim
on a *threshold*, not on `getbbox()`: the feathered edge carries alpha 2 out of
255, which `getbbox()` counts as content, so the obvious crop was four columns
and two rows looser than it looked. The source was 1024x1024 and only 774x853
of it was drawing, so a quarter of every icon would otherwise have been spent
on background. It is taller than it is wide and an icon slot is square, so the
last 9% is squashed rather than letterboxed: invisible at icon sizes, and it
buys back the margin that cutting just recovered.

**32px and below are retouched after the shrink**, in three passes and in this
order. The alpha curve is steepened first, because a shrink leaves a skirt of
part-transparent pixels that reads as a two-pixel grey fringe on a dark ground;
pushing the nearly-there pixels to solid and the nearly-gone ones to nothing
gives the silhouette an edge. Then colour and contrast go back up, since
averaging thousands of source pixels into one pulls it toward grey. Then
sharpening, but only where it helps.

**How hard depends on the size, and uniform settings are wrong.** At 32 each
output pixel averages a few hundred inputs and takes a firm hand well. At 16 it
averages a few thousand, and the same unsharp pass rings badly enough to invent
cyan and magenta that are nowhere in the drawing — so 16 gets colour and
contrast only. The numbers are in `TOUCH`, and they were set by looking at
magnified renders rather than by taste.

**A hand-drawn 16 was tried and thrown away**, and the reason generalises:
redrawing at that size loses the artwork rather than compressing it. The
attempt read as a strawberry — a serrated green band across the top is a hull,
and a warm body tapering to a point below it is the berry — and it took someone
else saying so to see it. A retouched downscale keeps the palette, the
proportions and the silhouette that were already approved at full size.

All seven sizes are compiled in through `icons/hydra.qrc` and added to a single
`QIcon` in `main.cpp`, so Qt chooses per use instead of rescaling one image;
adding only the large one would quietly discard the tuned small ones.
`packaging/install-icons.sh` lays the same files into a `hicolor` theme with
the desktop entry.

**Judge small icons at their real size.** `tests/` has nothing for this because
it is not a testable property, but the working method is: render at 16 and 32,
magnify with nearest-neighbour on both a light and a dark ground, and look at
the pixels. Every mistake in this section — the fringe, the ringing, the
strawberry, the loose crop — was invisible at 256px and obvious at 16.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2       # a number, always: see the warning above
./build/hydra                 # loads ./sample-tree.txt
./build/hydra my-tree.txt     # or a custom outline file
```

Requires Qt 6 with **Widgets** and **WebEngineWidgets** (Arch: `qt6-base
qt6-webengine`; Debian/Ubuntu: `qt6-base-dev qt6-webengine-dev`), CMake ≥ 3.19,
C++17. Clone with `--recurse-submodules`, or run `git submodule update --init
--depth 1` — `third_party/yt-dlp` is vendored for the site-extractor work
(arch §11.5) and is source and tooling, not something the build compiles.
On Linux this is an X11 / XWayland app: `main.cpp` forces
`QT_QPA_PLATFORM=xcb` there unless the environment already set it. That forcing
is guarded to desktop Linux, so other platforms keep Qt's own default plugin.

Two optional dependencies, both on the same pattern — found, and the feature is
on; absent, and it reports itself unavailable with no degraded mode:
`libsodium` (KeePassXC bridge) and `libtorrent-rasterbar` (BitTorrent
downloads). On Debian/Ubuntu: `libsodium-dev libtorrent-rasterbar-dev`.

**Watch the library name.** Both rasterbar's and rakshasa's unrelated libraries
install a pkg-config file called `libtorrent`, and the bare name resolves to
rakshasa's. `CMakeLists.txt` asks for `find_package(LibtorrentRasterbar)` first
and falls back only to the *qualified* `libtorrent-rasterbar` pkg-config name.

### Tests

`tests/` holds the harnesses, built separately from the app — the app's
`CMakeLists.txt` never references them:

```sh
cmake -S tests -B tests/build
cmake --build tests/build -j2          # a job limit, always: see the warning above
QT_QPA_PLATFORM=offscreen ./tests/build/test_seam
```

`tests/README.md` says which suites need a helper server, libtorrent, or a
model, and records the traps that cost time — screenshots going black when the
screen blanks, `import` hanging against a modal grab, and libtorrent exempting
loopback peers from rate limits so a "throttled" local transfer finishes
instantly.

## Build-verification state

**Builds and runs.** Verified on Debian 13 with Qt 6.8.2 and
`qt6-webengine-dev` 6.8.2: a clean `cmake --build` produces `build/hydra` with
no errors, and the binary starts both on X11 and headless
(`QT_QPA_PLATFORM=offscreen`), loads `sample-tree.txt`, and renders the shell —
menu bar, toolbar, populated tree with its bold/italic/muted state cues, status
bar.

Two of the three long-standing trouble spots turned out fine: the
`QWebEngineHistory` `QDataStream` operators and the
`QWebEngineCookieStore::FilterRequest` field names both compile clean. The
third is real but not urgent — the whole `featurePermissionRequested` /
`setFeaturePermission` permissions path is deprecated as of 6.8 and produces
eleven warnings. It still works. Migrating to `QWebEnginePermission` would
raise the Qt floor from 6.4 to 6.8, so it is a deliberate decision rather than
a cleanup; see "Qt version floor" below.

### The interceptor is confirmed working

Opening a tab drives a real navigation end to end: the page loads, subresources
are fetched, and both of the interceptor's mutations demonstrably take effect.
Tested against a local HTTP server that logs every request it receives, with
the page served from `127.0.0.1` and its subresources from `127.0.0.2` — both
loopback, but *different hosts*, which is what makes the result attributable:

| Case | Result |
|---|---|
| No policy file | page, image and script all fetched; `Referer` present |
| `javascript=block` for `127.0.0.2` only | the script never reaches the server; the image **from the same host** still does |
| `referer=block` for `127.0.0.1` | all three fetched, `Referer` stripped from both subresources |

The middle row is the one that isolates the interceptor. Per-page
`JavascriptEnabled` keys off the **top-level** host (`127.0.0.1`, untouched
here), so only `interceptRequest`'s per-origin `ResourceTypeScript` rule can
explain a blocked script alongside a loaded image from that same origin. The
third row is likewise interceptor-only — nothing else in the app touches
`Referer`.

**Not yet exercised:** the ad-host list specifically. It runs through the same
`info.block(true)` path proven above, but pointing a name like `doubleclick.net`
at a local server needs either `/etc/hosts` (root) or Chromium's
`--host-resolver-rules`, which Qt's `QTWEBENGINE_CHROMIUM_FLAGS` mangles because
it splits the variable on spaces and the `MAP host ip` syntax contains one. So
the *mechanism* is verified and only the host-matching predicate is untested at
runtime.

### The cookie filter, half proven

`tests/live/try_cookies` drives it the way the interceptor was driven, and for
the same reason: the page comes from `127.0.0.1` and one of its images from
`127.0.0.2`, both loopback but *different hosts*, so first-party and third-party
are told apart by something the test controls. The page carries a second image
from its own host whose only job is to report the `Cookie` header it was sent,
so the cookie's return journey is observed rather than assumed.

| case | result |
|---|---|
| defaults (cookies allow, third-party block) | `first=1` stored **and** sent back on the next same-host request; the third party's not stored |
| `cookies=block` for the host | nothing stored, and nothing sent on the next request either |
| third-party cookies **allowed** | still not stored — see below |

**The middle row is the one that proves anything.** Same page, same server, only
the policy differs, and the cookie goes from stored-and-returned to absent on
both channels. That is `request_filter::allow_cookie` being consulted and obeyed
through the real profile.

**The third-party half needed TLS, and now has it.** Over plain HTTP the cookie
is refused whatever the policy says — a `SameSite=None` cookie requires
`Secure`, `Secure` requires HTTPS, and anything else defaults to `Lax` and is
not set in a third-party context — so those rows measure Chromium and not us.
The driver reported them as inconclusive rather than as a pass, which was the
easiest mistake available: the observed behaviour is exactly what a working
filter produces.

It now also stands up a TLS origin, with a self-signed certificate made fresh
each run by `openssl` (not committed — a private key in a repository is a thing
someone eventually trusts by accident) and
`QTWEBENGINE_CHROMIUM_FLAGS=--ignore-certificate-errors` set by the driver
before the engine starts. One token, deliberately: Qt splits that variable on
spaces, so a flag containing one arrives mangled.

| case, over TLS | result |
|---|---|
| third-party blocked | `first` stored, `third` not |
| third-party allowed | **both stored** |
| cookies blocked for the host | neither |

The middle row is what was missing. Same page, same server, one policy bit
different, and the third-party cookie appears — so the row above it is our
filter refusing it rather than the engine declining to store it. The
third-party branch is measured.

**And the apparatus lied again, in the usual direction.** The first version
asked the store what it held after the page had loaded, via `loadAllCookies()`
plus `cookieAdded` — which emits nothing, because an already-loaded store does
not re-announce what it already has. It reported "(none)" for a cookie the
server could see arriving on the very next request. Two channels disagreeing is
what caught it; the driver now watches cookies as they arrive and prints the
server's view beside them, so a future disagreement is visible instead of
casting a vote.

### The permission callbacks, exercised

`tests/live/try_permissions` drives geolocation, camera, microphone and
notifications through the whole chain — `main_window` installs the decider, the
backend maps Qt's feature enum onto ours, the policy engine answers — and the
page reports what it got by fetching `/report?…`, so what is measured is what a
site would experience. Served from `127.0.0.1` because Chromium requires a
secure context for these, and loopback counts.

| case | result |
|---|---|
| the §7.2 defaults | all four refused, and the page sees `PERMISSION_DENIED`, `NotAllowedError`, `denied` |
| geolocation allowed for the host | our decider grants it; camera and microphone still refused |
| notifications allowed for the host | the page really is told `granted` |

**The enum mapping is confirmed by construction**, which was the part most
likely to be quietly wrong: with only the microphone blocked, Qt's feature 2 is
what gets refused and 3 is not, so `MediaAudioCapture`→`microphone` and
`MediaVideoCapture`→`camera` are the right way round rather than plausibly
swapped.

**Two things this cost, both worth keeping.**

- **Chromium remembers a permission answer per origin, and an origin includes
  the port.** The first version asked the same port three times, so the second
  and third cases were answered from that memory and never reached our decider —
  a granted feature still came back denied, which reads exactly like a broken
  callback. Each case gets its own port now; same host, so the per-host policy
  still applies to all three.
- **Geolocation cannot be measured from the page on this build.** Granted, it
  still ends as `PERMISSION_DENIED` — the engine has no location provider and
  the API has no other word for that, so the page-visible outcome is identical
  to a refusal. That is not our behaviour and asserting on it would be asserting
  on the engine's build options. The assertion moved to the boundary we control:
  `qtwebengine_view` logs what was asked and what was answered under
  `HYDRA_PERM_DEBUG`, and the driver captures its own log. Notifications remain
  the one feature here whose end-to-end outcome the engine can actually deliver,
  and it is checked that way.

### Qt version floor

`CMakeLists.txt` requires **Qt 6.4**, and that number is derived, not guessed:
the menu bar uses the `addAction(text, shortcut, receiver, member)` argument
order that 6.4 introduced (the older order is deprecated from 6.4 onward).
Everything else in the tree is 6.0-era API. Developed and tested against 6.8.2.

Qt WebEngine is a **system dependency, not a vendored one** — it bundles
Chromium, must be ABI-matched to the rest of Qt, and is LGPLv3/GPL/commercial
(arch §2), so linking the platform's build is far simpler than carrying it. How
it arrives differs per platform, but the CMake side is identical everywhere
(`find_package(Qt6 6.4 ...)`, with `CMAKE_PREFIX_PATH` pointed at the Qt
install where needed):

- **Linux** — distro packages (`qt6-webengine-dev` / `qt6-webengine`).
- **Windows / macOS** — the official Qt online installer, `aqtinstall`, or
  vcpkg/Conan. There is no system Qt to inherit, so the version is whatever CI
  and developers provision; pin it there.
- **Android** — Qt WebEngine does not exist at all (arch §19.2). That platform
  goes through the System WebView behind the `WebViewBackend` seam, which is
  why that seam is step 3.5 rather than an afterthought.

Note that going beyond Linux needs one code change first: `main.cpp`
unconditionally forces `QT_QPA_PLATFORM=xcb`, which has to become
Linux-conditional before a Windows or macOS build is meaningful.

## What is implemented (build-order steps 1–7)

| Area | Files | Notes |
|---|---|---|
| Shell / window | `main_window.{h,cpp}`, `main.cpp` | plain `QWidget`: menu bar, toolbar, splitter, status bar; tree + stacked chrome-less web views |
| Tree model | `tab_tree_model.{h,cpp}`, `node.h` | `QAbstractItemModel`, sort roles, id index |
| Canonical file | `tree_outline.{h,cpp}`, `sample-tree.txt` | id-tagged indented outline parser/serializer |
| Sorting / search | `tree_sort_proxy.{h,cpp}` | tree-order / title / created / last-seen + live search |
| Lifecycle | `main_window.cpp`, `state_store.{h,cpp}` | open⇄suspended, history blobs, LRU live cap (4) |
| Policy model | `policy.{h,cpp}`, `policy_engine.{h,cpp}` | packed 2-bit tri-states, precedence, JSON |
| Enforcement | `request_filter.{h,cpp}`, `qtwebengine_interceptor.{h,cpp}` | ad/script/image blocking, referer strip, cookie filter |
| Site editor | `site_policy_dialog.{h,cpp}` | shield popup, scope this-host/domain/global |
| WebView seam | `web_view_backend.h`, `web_view_factory.h`, `request_filter.{h,cpp}` | platform-neutral interfaces + shared block/cookie decisions |
| Desktop backend | `qtwebengine_{view,factory,interceptor}.{h,cpp}` | the only files that name Qt WebEngine |
| Kiosk | `kiosk_controller.{h,cpp}` | fullscreen stage, 3 scale paths, fit modes, idle-reset, watchdog |
| AI provider | `ai_provider.h`, `ollama_provider.{h,cpp}`, `claude_provider.{h,cpp}` | local-first seam; Ollama local, Claude external |
| AI reorganizer | `tree_serializer.{h,cpp}`, `tree_diff.{h,cpp}`, `reorganize_dialog.{h,cpp}` | metadata-only payload, invariant check, diff/accept |
| Media detector | `media_detector.{h,cpp}`, `media_dialog.{h,cpp}` | URL-shaped classification, segment attribution, Watch/Download list |
| Player handoff | `player_launcher.{h,cpp}` | PATH probe, capability routing, URL-not-pipe launch |
| yt-dlp handoff | `ytdlp_resolver.{h,cpp}` | resolve a page to real stream URLs + headers; PATH or vendored |
| MSE tap | `mse_tap.{h,cpp}` | main-world hook + isolated relay; reports what a page is actually playing |
| Downloads | `download_manager.{h,cpp}`, `download_source.h`, `http_download_source.{h,cpp}` | transport seam: manager owns queue/consent, source owns bytes |
| BitTorrent | `torrent_download_source.{h,cpp}` | libtorrent-rasterbar; magnet + .torrent, seeding, info-hash resume (optional dep) |
| Downloads UI | `downloads_dialog.{h,cpp}` | one list for every source, progress bars, public-transfer marking (Ctrl+J) |
| Settings | `settings_dialog.{h,cpp}` | player radio group, download folder, BitTorrent caps/ratio/interface; QSettings |
| Filter evolution | `filter_list.{h,cpp}`, `filter_signals.{h,cpp}`, `filter_dialog.{h,cpp}` | passive signals, dry-run validation, diff/accept |
| Password manager | `keepass_protocol.{h,cpp}`, `keepass_bridge.{h,cpp}`, `crypto_box.{h,cpp}` | KeePassXC-Browser client; no vault, no master password |
| Autofill | `autofill_controller.{h,cpp}`, `autofill_script.h` | QWebChannel bridge, origin gate, policy-governed |
| Consent banners | `consent_blocker.{h,cpp}`, `consent_rules.{h,cpp}`, `consent_dialog.{h,cpp}` | answers "accept cookies?" dialogs; rules as data, shareable later |
| Anti-adblock notice | `antiadblock_watch.{h,cpp}` | says so when a page is checking for a blocker, and names the lever |
| Shared rule store | `site_rules.{h,cpp}` | consent-banner and detector rules as one file, with provenance; the unit a future exchange would move |

Persistence: `policy.json`, `state/<id>.blob`, and the tree file all sit next to
the outline file passed on the command line.

## The WebView seam (step 3.5, done)

The shell no longer uses Qt WebEngine anywhere — `grep QWebEngine src/` hits
only the four `qtwebengine_*` files plus two *comments* in the seam headers
explaining what the desktop side does, and `main_window.{h,cpp}` is clean. (The
comments are worth mentioning because the grep is offered here as a check, and
someone running it and seeing six files would reasonably doubt the claim.) The
shape:

- `web_view_backend` — one rendered page: load, back/forward/reload,
  `apply_settings(view_settings)`, `save_state`/`restore_state`, a permission
  decider, and a `url_changed` signal. `view_settings` is plain bools, so
  a reduced Android backend applies what it supports and ignores the rest.
- `web_view_factory` — makes views and owns whatever profile-wide machinery
  sits behind them.
- `request_filter` — the platform-neutral half of interception. Deciding what
  to block is just policy plus a host list and is identical everywhere, so it
  is shared; only the plumbing that delivers requests is per-platform. Android's
  `shouldInterceptRequest` reuses this verbatim (arch §19.5).
- `qtwebengine_view` / `qtwebengine_factory` / `qtwebengine_interceptor` — the
  desktop implementation, and the only Qt-WebEngine-aware code in the tree.

`main()` is the single place that names a concrete backend: it builds the
policy engine, the filter, and the factory, then injects the factory and policy
into `main_window`. Adding Android is meant to be one new backend pair plus a
different two lines there.

A side benefit worth noting: the deprecated permissions API is now confined to
`qtwebengine_view.cpp`, so the eventual `QWebEnginePermission` migration is a
one-file change that cannot touch the shell.

## Kiosk mode (step 4, done)

`kiosk_controller` + `kiosk_config`, reached from **View → Kiosk Mode (F11)**,
Esc to leave. It borrows the current view's widget into a frameless fullscreen
stage of its own for the duration — which is what makes "strip all chrome" free,
since the stage has none — and hands it back on exit.

Three scale paths (arch §8.1), because they fail differently:

- **reflow** (default) — one zoom factor, the page re-lays out. Always works.
- **none** — crop via clip: native size, positioned by alignment, overflow
  clipped by the stage. No transform anywhere, so nothing can go wrong.
- **geometric** — `QGraphicsProxyWidget` transform. Exact layout, no reflow,
  and the only path that can do genuine per-axis `stretch`.

`fit_mode` (contain/cover/stretch/actual) composes with those; the header
documents which pairs are meaningful. Reflow cannot do `stretch` — one zoom
factor cannot scale axes independently — so it approximates with cover rather
than pretending. Also implemented: idle-reset to the home URL, a watchdog that
reloads on render-process death, cursor auto-hide, scrollbars and context menu
off, and an `allow_escape` flag for lockdown.

**The geometric spike is answered: it renders correctly here** (Qt 6.8.2 / X11,
1280×720 design scaled 1.5× to 1920×1080 — real content, not black). One data
point on one GPU, so re-test on any new deployment target. Two traps found
while getting there, both now handled:

- `QGraphicsProxyWidget::setWidget()` refuses a widget that still has a parent,
  warns to the console, and leaves the scene empty — which looks exactly like
  the black-render failure it is supposed to detect. Detach first, and fall
  back to reflow if the embed returns null.
- Tearing the shell down while a session is live was a use-after-free: the
  controller hands the borrowed widget back to a stack that has already been
  destroyed. Fixed with `QPointer` throughout plus a `~main_window` that exits
  kiosk while its children still exist.

**Not done from §8:** the off-the-record profile (a factory-level concern),
`disableInput`, an escape *gesture* as opposed to the flag, and any settings UI
— `kiosk_config` is currently code-level defaults with no way to edit it from
the app.

## AI reorganizer (step 5, done)

**Tools → Reorganize Tree with AI…**  The pipeline is arch §9.2 verbatim:
review payload → send → receive → check invariants → diff → cherry-pick →
apply. The live tree is untouched until Accept; the proposal is parsed into a
shadow tree the dialog owns and deletes.

**Provider seam.** `ai_provider` is one text request, one text reply — so the
reorganizer, the later filter-evolution loop, and the diff/accept pipeline are
all provider-agnostic. Resolution is local-first: `ollama_provider` if a local
server answers, else `claude_provider` if `ANTHROPIC_API_KEY` is set, else the
feature says so and does nothing. There is no official Anthropic SDK for C++,
so the Claude adapter speaks the REST endpoint directly (`anthropic-version:
2023-06-01`, model `claude-opus-5`, no `temperature`/`top_p` — that model
rejects them). A refusal arrives as a normal 200, so it is checked before the
response body is read.

**What travels** (§9.3): per node, id, parent/depth, title, URL, type, tags —
and nothing else. `tree_serializer` is deliberately not the canonical outline:
it drops the `created=` / `seen=` timestamps the on-disk file keeps, because
browsing history is not on §9.3's list and should not travel just because it
shares a file. State blobs and live views never come near it. The
review-before-send page shows the exact payload, and nothing is sent until the
user presses Send.

**The safety core** (§9.4) is `tree_diff::check_and_repair`, and it runs before
any diff is shown. The invariant: every original *leaf* id appears exactly once
in the proposal. Folders are the model's to invent, rename and drop; tabs are
not. So dropped leaves are re-attached and duplicates collapsed rather than
surfaced as a diff that could lose a tab — while an **invented leaf id fails the
whole proposal**, since there is no safe repair for a fabricated tab and
accepting one would put it in the user's tree. A leaf silently converted to a
folder counts as invented for the same reason.

Verified offline against 19 cases, all passing: timestamps absent from the
payload; a dropped tab restored; duplicates collapsed to one; an invented leaf
rejected by id; a leaf→folder conversion rejected; new folders allowed and
counted; moves computed and applied with the leaf count preserved; a prose-only
reply parsing to nothing rather than an empty tree (an empty proposal is a
provider failure, not a request to delete everything); a fenced/prose-wrapped
reply still parsing; and a move into a node's own subtree refused.

**Not done from §9:** the undo snapshot (§9.4's one-keystroke revert — the
cherry-pick UI covers accept-time control, but there is no post-apply undo), the
web-session provider (§9.1 rates it least preferred), and merging on the
duplicate-URL changes — those are detected and listed, never pre-selected, and
applying one is currently a no-op.

**Neither provider has been exercised against a live endpoint** — no Ollama and
no API key in the development environment. The request/response shapes follow
the documented contracts, but the first real call is unverified.

## Interceptor consumers (step 6, done)

Both features consume the one request stream the blocker already rides (§10)
rather than adding a second sensor: `request_filter` gained a
`request_observer` seam and the interceptor notifies it for every request,
blocked or not — the media detector wants what loaded, filter evolution wants
what slipped through. Observers are called off the UI thread and are
mutex-guarded accordingly.

**Media (§11).** `media_detector` classifies by URL shape, which is all a
request-only interceptor can do; real Content-Types and manifest bodies need
the optional local proxy (§10). Segments (`.ts`, `.m4s`) are tracked but never
offered for saving — they exist to answer *which stream is playing*, and each
is credited to the manifest sharing the longest directory prefix, so the one
being fetched sorts first. That is §11.3's primary-stream heuristic, made
answerable without response bodies.

`player_launcher` probes `PATH` and resolves the default from what is present —
it never assumes mpv exists. Launch always hands over a **URL, never a stdin
pipe**, because a pipe has no random access and the player could not seek.
Routing is capability-aware: handing a manifest to classic mplayer warns
explicitly rather than stumbling silently. `download_manager` is one queue with
`Range`-based resume and jobs tagged with the node they came from; it refuses
manifests rather than saving playlist text and calling it a video.

**Filter evolution (§12).** `filter_signals` logs third-party ad-shaped
requests that were *not* blocked — a blocked one is the system working, not a
gap — and never flags a first-party request, since proposing a rule against one
is how a filter list breaks the page it was meant to fix.
`filter_list::evaluate` is the safety core and the analogue of the
reorganizer's invariant check: nothing reaches the accept UI until a static
breadth check and a simulation against the page's real observed requests have
both run. A bare TLD, the page's own origin, an undomained cosmetic rule and a
generic-tag selector are rejected outright; survivors are shown with exactly
what they would block, and one matching nothing observed is left unticked as
unproven rather than wrong. Accepted rules merge into `filters-ai.txt`, apart
from any imported EasyList (§12.5).

Verified offline across 23 cases, all passing — classification and the
saveable/segment split, segment attribution picking the right primary, blocked
media not offered, all four dangerous-rule rejections, the dry run reporting
exactly what it matches, first-party and already-blocked requests excluded from
signals, and `||host^` matching subdomains but not a suffix-only lookalike.

**Not done from §11–§12:** segment assembly and the ffmpeg remux, tee-to-disk
for scrubbing a live stream, the local proxy that would inject Referer/cookies
for CDNs that 403 a naked stream URL, a downloads window, the per-site
auto-detect toggle, and regression re-runs over a known-clean page set.

## Password manager (step 7, done)

**Tools → Connect to KeePassXC…**  The design choice §13 leads with is what
this does *not* do: no `.kdbx` parsing, no master password, no vault of our
own. KeePassXC is already an unlocked local daemon in the session, so Hydra
becomes a first-class client of it. Transport is the Unix socket directly —
`keepassxc-proxy` exists only to bridge stdio for sandboxed extensions and a
native app skips it.

**The protocol is split from the crypto and the socket**, the same way
`request_filter` is split from the interceptor. `keepass_protocol` is pure
functions: message shapes, reply parsing, and the per-message nonce discipline.
That is where protocol bugs live, and keeping it free of libsodium and sockets
means it is testable with neither installed.

**libsodium is optional at build time.** Without it `crypto_box` reports
`available() == false`, the bridge refuses to start, and the menu item is
disabled — the protocol is end-to-end encrypted (X25519 + XSalsa20-Poly1305),
so there is no degraded mode worth offering, but an optional dependency should
not break the build. Install `libsodium-dev` to enable it.

**Autofill** injects a content script at document creation into an isolated
world, talking to `autofill_controller` over `QWebChannel`. The script does the
field detection every password manager has to do — `type=password`,
`autocomplete` values, name/id/aria heuristics — assigns through the native
value setter so framework-managed inputs see the change, and **never
auto-submits**.

The security rules are enforced in C++, not trusted to the script, because a
page can rewrite anything in its own document:

- **The page does not choose the origin it asks about.** The shell sets it from
  the view's real URL on navigation; a request naming anything else is refused.
  That is what stops a cross-origin iframe asking for the top page's
  credentials, and a lookalike asking for the real site's.
- **Autofill is a policy_engine feature**, so the shield's tri-state and the
  global default govern it exactly like JavaScript or cookies.
- **HTTPS-only by default** — filling a password over plain HTTP puts it on the
  wire.
- Credentials are held only for the fill that asked, and a navigation
  invalidates any request in flight.

Verified offline across 20 cases, all passing: nonce increment including the
carry into byte 1 and the full all-`0xFF` wrap (a nonce reuse is the one
failure this protocol cannot tolerate); message shapes including that the
envelope carries only ciphertext and leaks no plaintext URL; both error reply
shapes, and that an error reply yields no entries; and the full origin gate —
a different origin, an empty one, a suffix lookalike (`bank.test.evil.test`),
plain HTTP, and a policy-blocked site each refused, with navigation
re-pointing the gate.

**libsodium is now installed here, so the crypto path is verified**: X25519
keypairs, a seal/open round-trip, and rejection of a tampered ciphertext, a
wrong nonce and a wrong key all pass. What remains unexercised is everything
above the crypto — the socket handshake, association, and `get-logins` have
never run, because KeePassXC itself is not installed. Also not done from §13: the key icon and entry-picker UI (a single
match fills automatically; multiple matches are deliberately left alone rather
than guessed at), `set-login` on new-credential submit, `generate-password`,
storing the association key encrypted at rest via Secret Service — it is in
memory only, so pairing does not survive a restart — and the optional
direct-`.kdbx` fallback, which §13.4 recommends against anyway.

## When our own blocking breaks the page (partly done)

The measured case, from the second mirror: the player never started, the play
button stayed put, every click was answered by the ad network, and nothing in
the request log looked wrong. What the log *contained* was
`cdnjs.cloudflare.com/ajax/libs/fuckadblock/3.2.1/fuckadblock.min.js`. The page
was watching for a blocker and refusing to play, and allowing ads for that site
alone started it.

**The failure mode is what makes this worth building.** A page broken this way
looks like a broken site, or like a broken browser, and gives the user nothing
to act on. Our blocking is the cause and only we can say so.

`antiadblock_watch` is a fourth rider on the interceptor's observer seam, beside
the media detector, filter signals and extractor signals — the request stream is
already being watched and a second sensor for the same facts would be a second
thing to keep true (§10). When a page fetches a script whose only job is to
detect a blocker, the status bar says so once and names the lever: the shield's
per-site ads setting.

**It fixes the page and says so** — decided, not assumed. Reporting alone was
built first and is worse than it sounds: the message is easy to miss, the page
stays broken, and a broken page reads as a broken browser rather than a blocked
one. So the site's ads setting is relaxed, the page is reloaded, and the status
bar says what happened and where to undo it.

Three things keep that from being presumptuous, and each is checked:

- **A choice the user made is never overridden.** An explicit per-site rule means
  someone blocked ads there knowing it might break, and the thing that noticed it
  broke does not get to second-guess them.
- **It is a per-site rule**, so the global default is untouched — this cannot
  quietly stop blocking everywhere.
- **Once per site per session.** A page that keeps checking cannot put the
  browser in a reload loop.

The reload is not decoration: the allowance only affects requests from then on,
and the page has already been assembled without them, so without loading it
again the fix is invisible. Seven checks (`tests/live/try_adblock_fix`), driven
against a page that pulls in a detector.

**Nothing matches on the word "ad", deliberately.** This message tells someone to
lower their protection, so a false positive is not cosmetic. It matches the file
*name* against a short list of known detectors — not the query string, where a
page's own search terms live. Ten checks cover both directions: the script that
broke the real page and its better-known sibling recognised; a filter list, an
ordinary script, and a page merely mentioning one in a query all left alone; the
report fired once per page however many are fetched; and the finding scoped to
one page rather than to the browser.

**The patterns live in the shared rule store**, not in an array in C++. A
detector renames itself, so a list compiled into the binary is a release behind
for everyone; the consent rules already carried provenance and lived in a file
meant to be exchanged, and these are the same kind of perishable fact about how
sites behave. So `consent_rules` became **`site_rules`** and grew a `detector`
kind, which the rule model already supported — `kind` was always a free string.
A detector name learned rather than shipped is generic for exactly the reason a
button label is (it describes a script, not a site), so it is flagged for the
built-ins and travels in the same file. Checked: a detector added to the rule
file is honoured with no rebuild, and is flagged.

One store rather than one per feature is the whole point. Two files would be two
provenance models to keep honest and two things to send, and the direction here
is that these eventually go to other people.

## Cookie consent banners (§7.1, done)

**"Do you want to accept cookies?" — answered for you.** A blocking option
beside ads and popups rather than a setting under cookies, because it is a
different question: not what a site may *store*, but what it may put in front of
the page before you are allowed to read it. `block` — the default — means the
banner is answered and dismissed; `allow` means it is left alone and you answer
it yourself. It appears in the shield like every other tri-state, because the
dialog builds its rows from the feature enum.

**What it answers with, and why "reject" is not always the answer.** The point
of the option is to be able to use the page, so it takes the least permissive
option the banner actually offers: reject-all where there is one, otherwise
necessary-only, otherwise accept. A banner whose only exit is "OK" is a banner
whose only dismissal is acceptance, and refusing to click it on principle leaves
the page unreadable — which is the state the option exists to end.

**It answers first and hides second, and the order is the design.** Hiding a
banner without answering it leaves the site believing it has not been asked: the
overlay returns next load, the scroll lock usually stays, and some players will
not start until their consent object is set. Clicking is what ends the
conversation. The hide pass only cleans up what a click left behind — including
releasing `overflow: hidden` on the document, which is what makes a page
unreadable even when the overlay itself is small.

**Answering has to stick, which is why this touches cookie policy.** A consent
choice is itself recorded in a cookie. With cookies blocked for the site there
is nowhere to record it, so the banner returns on every load and the option
looks broken. So dismissing one relaxes **first-party** cookies for that host —
never third-party, which is precisely what these dialogs are bargaining for —
and writes it as an ordinary per-site rule, visible in the shield and revertible
there. The status bar says so when it happens: a policy change the user did not
make should be visible at the time, not merely findable afterwards.

### The rules are data, because they are going to be shared

Vendor markup changes on its own schedule, so anything pinned to
`#onetrust-banner-sdk` is a release away from useless — the argument §11.5 makes
for extractors and §12 for filters, with the shortest half-life of the three.
`consent_rules` is therefore a file (`consent-rules.json`, beside the tree and
the filter list), not a table in the binary, and the built-in set is
deliberately thin: a long vendor list looks like thoroughness and is really
maintenance debt.

### Sharing, as far as it can go without a transport decision

Proceeding under a stated assumption, since the direction was given and the
transport was not: **sharing means a file someone sends**, exported and imported
explicitly. No network anything. That defers the transport deliberately, because
the part that had to be right first is what a received rule must *prove*, and
that does not change when the bytes eventually arrive some other way.

**The threat is specific and worth naming plainly.** A consent rule is a licence
to click buttons on pages the user is signed into. An `accept` pattern of `^.*$`
presses the first button on every banner-shaped thing on every site — "Delete
account" included. So an imported rule is not trusted for being well-formed. It
faces `why_unsafe`, which is §12.4's argument applied to a different corpus:
decide what a rule *would* do before letting it do anything.

| refused | because |
|---|---|
| `^.*$`, or anything matching an empty label | it matches everything |
| `^(accept\|delete account)$` | it would also press "Delete account" |
| an uncompilable pattern | it cannot be reasoned about at all |
| a detector name under five characters | the message it drives tells someone to lower their protection |
| `*`, `body`, `html` as a container | it matches the whole page |

The decoy list it is checked against is deliberately full of destructive,
expensive and irrelevant buttons, because those are what a hostile or careless
rule would reach.

**A sender does not describe their own rule's standing.** `builtin` and
`promote` are taken from the document and thrown away; an imported rule is
marked `imported`, and an imported rule is **never** proposed for our binary. It
has been vouched for by nobody here — which is exactly the distinction the
provenance field was added for, now earning its keep.

**Nothing is added by opening a file.** What survived the check is shown, and
adding it is a second, deliberate act. A rule set from elsewhere should not be
something acquired by browsing to a filename.

**Where a rule came from is the importer's label, never the document's.** A file
describing itself as "Trusted community rules" is describing itself, which is
worth nothing; what the importer knows is the file it opened, so that is what is
recorded and shown in the list beside "learned here". The distinction is the
point of the column: a rule you vouched for and a rule somebody sent you should
not look the same.

**Undoing an import is one action.** *Forget imported* drops everything that came
from elsewhere and keeps the built-ins and what was learned here — which is the
reason `imported` is a stored field rather than something inferred. If a rule set
turns out careless or hostile, the remedy cannot be a hunt through a list, and it
must not cost the user their own rules.

**And the same check now guards the local path**, which was the more interesting
consequence. A learned rule carries no host, so it applies to *every* site: a
banner whose button reads "Yes" would teach a rule that presses "Yes"
everywhere, confirmation dialogs included. Being offered by the page is not the
same as being safe to generalise, and the dialog says so with the reason instead
of quietly learning it.

**17 checks** on the judgement alone, plus two on the local path.

**Sharing is not built and the shape for it is.** The stated direction is that
these rule sets travel between users, peer-to-peer or otherwise; everything here
is local. What that costs today is one field — every rule carries provenance —
and the reason to pay it now rather than later is that retrofitting provenance
onto a corpus people have already traded is how you end up unable to tell a rule
you shipped from a rule a stranger sent.

**A generic rule learned locally is flagged for the binary.** A rule with no
host is a description of a *shape* banners take rather than of a site, so it
belongs to everyone: `consent_rules::add` marks any learned generic rule
`promote`, and `promotable()` lists them for folding into `defaults()` at the
next release. Setting the flag at the point of insertion rather than at each
call site is deliberate — whichever path learns the next rule cannot forget to.

**And a banner it cannot answer is where rules come from.** The signal is the
one §12 uses for filters: record where the system fell short rather than guess.
A container that is consent-shaped, on screen, and offers nothing any pattern
matches is reported with the labels it *did* offer — which is most of the rule
already. The normal case for this is not exotic: it is any language the built-in
patterns do not cover. `rule_from_label` turns one into a rule, and the scope
falls out of what a label *is*: "Avvis alle" is Norwegian for reject-all and
works on every Norwegian site, so the rule carries no host, which makes it
generic, which flags it for the binary. `#accept-btn-42` would describe one page
and is not something this produces.

The label is escaped before it becomes a pattern. A banner reading `(.*)` must
not compile into a rule that matches every button on every site afterwards —
least of all one built to be shared.

**Tools → Cookie Banners We Missed…** is where one becomes a rule, and it is the
simplest of the three review loops in this project because there is no model in
it: the evidence *is* the proposal. The banner was recorded with the labels it
offered; all that is missing is which of them means refuse and which means
accept, and for a language nobody here reads only a person can say. So the
dialog asks exactly that. **Nothing is typed** — a rule is built from a label the
page really offered, escaped, because this is a corpus meant to be shared and a
free-text field is a `.*` waiting to happen. And it says in front of the person
accepting that the rule is generic and flagged for the built-ins: they are not
fixing one site, they are proposing something everyone would carry.

### A CMP shipped as an iframe

The first version was top-frame only, and a banner inside a cross-origin iframe
was simply left standing — which is not a corner case, because delivering the
dialog *as* an iframe is how CMP vendors ship them. Found by asking, straight
after the same question was answered badly for the MSE tap: the shape of the
mistake was already known, so this time it took one fixture rather than an
afternoon.

The script runs on subframes now, and a frame has no bridge to talk to — Qt puts
the channel transport in the main frame alone. So the two halves speak to each
other. The top frame is the only one that asks C++ anything; a child asks *it*
for the rules and hands its dismissals back up. Two properties hold that
together:

- **The top frame decides whether to act, once, for the page.** It only answers a
  child after C++ has said this page is one to act on, so a frame cannot talk it
  into working on a site where the user turned the option off.
- **A frame cannot report on behalf of a site it is not on**, because the host
  C++ files under is the shell's and never the message's — the same rule that
  governs autofill and the tap.

This is also what earns `inject_script` its `subframes` flag. It was written
once during the tap work, had no consumer that could use it, and was reverted
rather than left as dead API; this is a consumer, so it is back.

**One cost, stated:** the rules are posted into the child frame, so page script
in that frame can read them. They are a public corpus by intent — the direction
here is that they are shared — but it is worth knowing they are readable rather
than discovering it later.

**The rules are fetched per page rather than substituted at injection**, which is
what makes a rule learned a minute ago work now. A script with the rules baked
in carries whatever was true when its tab was built, and re-injecting to update
it would leave two copies racing a guard flag. Measured, not assumed: the last
check drives the same banner again after accepting and it is answered.

**What carries unknown banners is the generic pass, not the vendor list.** A
banner is a thing pinned over the page that talks about cookies and offers a way
out, so that is what is looked for: fixed or sticky position, consent-shaped
text, a small number of buttons. The vendor selectors are a shortcut to the same
answer. A fixed bar with buttons on a page that merely *mentions* cookies is
left alone, which is checked.

**Verified through the real shell, 34 checks** (`tests/live/try_consent`),
against fixture banners rather than a live CMP — pinning a test to one vendor's
current markup measures that vendor, as §11.5 already learned. The fixtures are
the shapes that recur and each is a different decision: one offering reject
(taken over the accept that comes first in the DOM), one whose only exit is
accept (taken rather than leaving the page unusable), one that is not a consent
banner at all (nothing clicked), and one that locks scrolling (released). Then
the policy side, starting from cookies actually blocked, and the option turned
off for the site leaving the banner alone. Which button was clicked is reported
by the *page*, so the claim is about what happened rather than what we intended.
Then the discovery half: a Norwegian banner nothing matches is left alone and
recorded with its labels, a label becomes a generic rule, the rule is flagged for
the built-ins, and the flag survives a save and load — the round trip being the
part that matters, since a flag that does not persist is a flag nobody will ever
act on. Then the dialog itself, clicked rather than admired — the banner listed,
the site row offering nothing because only a button can be a rule, a label
chosen, and the rule on disk afterwards. A review UI that is correct and never
clicked is this project's most common defect.

**A fourth, in the test itself, and the same shape as two before it.** The
round-trip phase saved a rule file into the directory the shell loads rules
from, so the *next* run started already knowing Norwegian and the "a banner
nothing matches" case quietly stopped testing anything. It is removed at startup
and the round trip writes elsewhere now, and the run is checked twice in a row.
An artefact of the last run is indistinguishable from a real result — which is
exactly what the tree and state contamination in `try_extract` was.

**Three defects it found in the feature, all ours.** The button visibility test applied the
*container's* size threshold to buttons, so the one banner whose only exit was a
small "OK" went unanswered — exactly the case the feature exists for. The
activation check asked C++ once at channel-connect time and raced the shell's
navigation signal, getting "no host yet, so no" and never asking again. And the
first version of the policy check passed vacuously because cookies were already
allowed globally, so the relaxation had nothing to do.

### One page, one channel (a latent defect this surfaced)

Every content script — autofill, the picker, the MSE relay, and now this —
constructed its **own** `QWebChannel` over the same `qt.webChannelTransport`. A
QWebChannel takes that transport's `onmessage` when it is built, so the last one
constructed received every reply and the others waited forever for a handshake
that had already been answered to somebody else. It has been visible all along
as `channel.execCallbacks[message.id] is not a function` in the console of
almost every live driver, and as a bridge that sometimes simply never arrived.
Adding a fourth script turned a race that usually went the right way into one
that never did.

There is one channel per page now, built by a bootstrap injected once beside
`qwebchannel.js`, handing out bridge objects through `window.hydraChannel(cb)`.
Every script waits on that instead. The four live drivers report **zero** of
those console errors afterwards, where before they appeared in nearly every run.

## The element picker (§12.1, done)

**Tools → Zap an Element…**  This is the half of filter-evolution signal
collection deferred from step 6 for a structural reason — capturing a leaked
ad's selector needs script injection and a channel into the page, which is the
plumbing the password manager introduced. Same seam, second purpose.

The page-side overlay highlights on hover, captures on click, and cancels on
Escape; it listens in the **capture phase** so a page that swallows its own
clicks cannot block it. What comes back is the element's shape — selector, tag,
id, classes, and an outerHTML with text nodes stripped. Dropping the prose is
the cheap, reliable version of §12.2's "personal data stripped from the
snippet", and loses nothing the model needs to write a selector. The derived
selector skips hashed-looking class names, since a rule built on `css-1a2b3c`
is dead at the next deploy.

It also makes the **cosmetic dry run real**. Before, a cosmetic rule showed "no
request-level preview" because there was nothing to check it against.
`filter_list::cosmetic_matches` now answers the question the user actually has:
*will this hide the thing I zapped?* Approximate by design — a full selector
match needs a live DOM — so it checks the selector's rightmost compound, the
part naming the element itself, and never claims more than it checked. A
cosmetic rule that misses the picked element is reported as a miss and left
unticked.

Everything arriving from the page is treated as hostile: the page URL comes
from the shell rather than the payload, a malformed or tag-less payload aborts
instead of yielding an element, and the snippet is capped.

Verified across 27 cases: matching on tag/class/id and combinations, the
universal selector, class-order independence; misses on wrong tag, wrong id and
an absent class; descendant and child combinators; a pseudo-class ignored
rather than mis-parsed; the generic-tag rejection still winning over a rule
that would have hit; and the picker aborting on garbage, on a tag-less payload,
and capping an oversized snippet.

Wiring it up found a bug in step 7's plumbing: `set_script_bridge` injected
`qwebchannel.js` once per registered object, so a second bridge would have run
the transport setup twice in the same page. It is now injected once.

## Local proxy (§10, player-facing half)

`local_proxy` is a loopback HTTP relay that solves the §11.3 problem: a naked
stream URL frequently 403s, because the CDN expects the same Referer, cookies
and User-Agent the page carried. Rather than depend on whichever header flags a
given player supports — they vary, and mplayer's are limited — "Watch in
player" now publishes the stream to the proxy and hands over a localhost URL,
and the proxy replays the context upstream.

Two properties carry the feature:

- **Range transparency.** A player's `Range: bytes=X-Y` is forwarded verbatim
  and the `206` comes back with `Content-Range` and `Accept-Ranges` intact.
  Without this the player cannot seek, and seekability is most of the point.
- **Verbatim relay.** Manifests and segments pass through unmodified, so an HLS
  playlist keeps its full segment list and its `#EXTINF`,
  `#EXT-X-MEDIA-SEQUENCE` and `#EXT-X-BYTERANGE` tags. Rewriting a manifest is
  how seeking silently breaks.

Security: loopback-only, and it serves nothing but URLs explicitly published to
it, each behind a 128-bit token. It is a context-injecting relay for streams the
user chose, not a general forward proxy — something that guessed the port still
cannot make it fetch anything. It is also optional: if it cannot listen, Watch
falls back to the raw URL.

Verified against a real origin server across 15 proxy cases, all passing:
full-body relay; a range request coming back as `206` with exactly the requested
bytes and an intact `Content-Range`; Referer, User-Agent and Cookie each
observed arriving upstream; an HLS manifest relayed byte-for-byte with its
timing tags; and an unpublished token refused with 404.

### The content-type tier (§10, §11.1) — and it settles the measured site

`stream_probe` fetches the opening 2 KB of one address with the page's own
`stream_context` and says what it is. Not the browser-through-the-proxy half of
§10: inspecting every response would mean terminating TLS with a certificate
the browser must trust, which the design does not address. This fetches one
address the user is already being asked about, which needs none of that.

**Measured against the real site, and it answers the question outright:**

| fetch | result |
|---|---|
| the manifest, no context | **403** from Cloudflare |
| the manifest, with Referer + User-Agent | **200**, `application/vnd.apple.mpegurl` |
| a `seg-N-f1-v1-a1.woff2` "web font" | **206**, `video/mp4` |

Two things follow, and the second corrects an assumption this file was carrying.

First, the §11.3 request-context argument is confirmed again from the field: the
same address is 403 naked and 200 with the page's Referer, so the context
injection is what makes the tier possible at all.

Second — **the disguise is in the URL only. The Content-Type is honest.** The
manifest admits to being a playlist and the counterfeit fonts admit to being
`video/mp4`. Everything upstream of this had assumed a server lying in both
places. It is not lying; nobody had asked it. That is why four rounds of prompt
work could not find the stream: the evidence handed to the model was the one
channel where the site *does* disguise itself, and the channel where it tells
the truth was never consulted.

**The body still outranks the header, by design.** Not because this site lies,
but because the failure mode of believing a header is silent and the cost of
sniffing is 2 KB. `#EXTM3U` in the opening bytes settles the question whatever
the header says, and a disagreement is reported rather than smoothed over.
`<MPD`, an ISO-BMFF `ftyp` box, Matroska and Ogg are recognised the same way.

**What it will not do.** A 403 is a CDN refusing the context, not a statement
about content, so it reports "could not be established" rather than "not a
stream" — as does an unreachable address. The tier is optional, so none of
those may block an accept. Only a body that positively is not a stream, an HTML
page, disables one.

**Wired into the extractor review**, which is §11.5's last clause: the gate
proves an address was *observed*, which is not the same as proving it is a
stream. The review now fetches what was picked and appends what it really
serves, advisory unless it contradicts.

**25 checks**, split between classification with no network — where the
subtlety is — and a fake origin answering the way the real one does, including
that Referer, User-Agent, cookies and an extractor's own headers all arrive,
and that only a range is asked for.

### Sending the probed types to the model (built; effect not yet measured)

The evidence sent to the model was URLs, types and order — the channel this
site disguises. The review now asks the server about the plausible candidates
*before* anyone asks the model, and writes what came back beside each address:

```
  42 | other  | https://…/cf-master.1785377837.txt?k=…   -> application/vnd.apple.mpegurl (HLS)
```

It probes on open rather than on Send, for two reasons: the payload pane is a
promise about what will be sent, so it has to be complete before Send is
available; and these requests go to the site the page already talked to, not to
the provider, so "nothing leaves until you press Send" is untouched.

**Choosing what to ask is the whole problem, and the obvious rule is wrong.**
Ten questions is the budget. Ordering by "fetched once" — a manifest is fetched
once, its segments are not — sounds right and fails completely: beacons and
stylesheets are fetched once too, and they arrive *first*. Measured on a real
capture, all ten questions went to Google Analytics, Yandex and a CSS file, and
the manifest was never asked about.

The rule that works is already in the evidence. **The host that served a flood
of near-identical requests is the media host**, so a request fetched once *on
that host* is the manifest, while a one-off on a host that never repeated
anything is a beacon. Segments rank next, since knowing one is `video/mp4`
tells the model what the flood is. Verified on a live capture:
`cf-master….txt → HLS` and `index-f1-v1-a1.txt → HLS`, with the trackers
correctly annotating nothing.

**What is measured, and what is not.** That the right addresses get asked and
the answers reach the payload is measured, at both unit and live-capture level.
Whether it makes the model find the stream is **not**: each run needs a fresh
capture, because the CDN tokens expire and stale evidence probes as 403, and a
14B on this payload exceeded a 900-second ceiling per run with the model
resident alongside a WebEngine capture. Do not record a hit rate for this until
it has actually been run — the temptation is to assume it works because the
mechanism plainly does.

**Two defects it turned up on the way**, both ours:

- `stream_probe` had a use-after-free. `readyRead`, `finished` and the timeout
  can all arrive; the first deleted the guard flag and the next read the freed
  memory to decide whether it had already run, so some replies were delivered
  twice. A `shared_ptr` now, with no manual delete.
- Every automated driver that clicks Send the instant the dialog opens broke
  silently, because probing disables Send and **a disabled button ignores a
  click without saying so**. It presented as the model never answering, with no
  request reaching Ollama at all. `test_live_model` waits for the button now,
  and prints how many addresses answered so a run cannot quietly measure the
  un-annotated case.

### Segment assembly (§11.2/§11.3)

`hls_playlist` parses manifests — master vs media, variants, segments, relative
URI resolution, `#EXT-X-BYTERANGE`, and VOD vs live by the presence of
`#EXT-X-ENDLIST`. It is pure parsing with no network, which is where the fiddly
cases are, so it is tested on its own.

`hls_assembler` follows a master playlist to its highest-bandwidth variant and
appends that variant's segments, in order, into one growing file — with the same
Referer/User-Agent/cookie injection the proxy does, since a CDN that 403s a
naked stream URL will 403 the segment fetches too. `local_proxy::publish_file`
then serves that file with full `Range` support against whatever has landed so
far.

That combination is what makes the §11.3 story real. A player that cannot take a
manifest — classic mplayer — now gets a progressive `.ts` it can seek around in,
and playback starts as soon as the first segment lands rather than waiting for
the whole stream. It is also the tee-to-disk trick: because segments are written
as they arrive and ranges are served against the current size, a live stream
becomes a locally seekable capture, and the same mechanism serves both "watch
this properly" and "save this" — HLS is now saveable rather than refused.

Verified across 22 cases, all passing: VOD/live detection, relative and nested
URI resolution, a quoted `CODECS="avc1,mp4a"` parsed as one attribute rather
than split on its comma, `BYTERANGE` `len@offset`, highest-bandwidth variant
selection; then master → variant → segments assembled in order into one file of
the right size; then serving it with a mid-file range, a suffix range, and a
range past EOF correctly returning 416.

**Still missing from §10–§11:** DASH assembly (HLS only), the ffmpeg remux —
concatenated MPEG-TS is directly playable, which is why this works without one,
but fMP4 segments would need their init segment and a real remux — re-polling a
live playlist as it grows (what is captured is what was in the list when it was
read), and cookie capture, since the context carries Referer and User-Agent but
reading cookies back needs cookie-store integration.

**Routing the browser through it is deliberately not attempted.** §10's other
use — response inspection for real Content-Types and manifest bodies — means
intercepting HTTPS, which means terminating TLS with a generated certificate the
browser must be made to trust. That is a different problem with its own risks,
and the design does not currently address it.

## Reorganizer undo (§9.4, done)

**Tools → Undo Reorganize (Ctrl+Shift+Z)**, enabled only after an accepted
reorganization. §9.4 asks for "a single undo snapshot [that] makes any accepted
change one keystroke to revert", and until now accepting was irreversible except
by rearranging the tree by hand — an odd gap in a feature whose entire design is
about not losing tabs.

The snapshot records **structure only**: id, parent, order, and folder titles.
That is sufficient because structure is the only thing a reorganization changes
— live views and state blobs are keyed by id and were never stored on the node,
so putting the structure back restores every tab without touching its payload,
for exactly the reason applying the change could not disturb it either.

Restore detaches everything and re-attaches in snapshot order, so sibling order
is reproduced rather than approximated. Folders the AI invented are not in the
snapshot; they are deleted **after** their children have been re-attached
elsewhere, so removing one cannot take a tab with it. The shortcut is
Ctrl+Shift+Z rather than Ctrl+Z so it never steals undo from a focused text
field in the page or the address bar.

Verified across 15 cases: a full apply→undo round trip compared against a
structural fingerprint (ids, parents, sibling indices, titles — not just a leaf
count), the invented folder gone afterwards, every tab surviving, a lone folder
rename reverted, an empty snapshot doing nothing, and a restore whose snapshot
references a tab that has since been closed skipping it rather than resurrecting
it or crashing.

One level only, as §9.4 specifies — the snapshot is cleared once used.

## Download transport seam (arch §11.4)

`download_manager` no longer contains a transport. It owns the queue, the
destination directory, consent and the job records; a `download_source` owns
the bytes. `http_download_source` is the first implementation and is a
straight move of the code that was inline before — same Range resume, same
refusal messages, now living where the knowledge does.

What the seam carries that HTTP never needed, all present and tested:

- **`resolving`** — a magnet link that is working but has no size, name or file
  list yet. **`seeding`** — every byte has arrived and the file is usable, but
  the source has not let go. `complete()` is true for both `seeding` and `done`;
  `terminal()` only for `done`/`failed`/`cancelled`.
- **Multi-file jobs** — `files` alongside a primary `path`.
- **Per-source concurrency** — `max_concurrent` comes from the source, because
  "one at a time" is right for HTTP and wrong for torrents. A seeding job still
  holds its slot.
- **The consent gate** — a source declaring `public_participation` cannot start
  until `set_consent()` is given; the job waits and `consent_required` fires
  with the source's own note. This is the §11.4 privacy decision made
  structural rather than left to whoever writes the UI: since torrents are
  deliberately made to look like every other download, the fact that this one
  announces you to strangers must not be forgettable.

The rule that keeps it honest: **nothing above this interface names a
transport.** If the manager or the UI has to ask "is this a torrent?", the
answer belongs in `source_capabilities` instead.

**Verified: 77 checks passing** (offline harness, `scratchpad/dlseam`). Routing
and refusal messages, the consent gate including revocation, the full
torrent-shaped lifecycle via a fake source with no libtorrent present,
per-source concurrency, cancel from both queued and running, failure paths,
pump re-entrancy, and the HTTP source against a live Range-capable server —
including a resume that the server log confirms issued `Range: bytes=30000-`
and that stitched byte-exactly.

Found and fixed while testing: `~http_download_source` aborted replies while
iterating `m_transfers`, and `abort()` delivers `finished()` synchronously,
which re-entered `teardown()` and mutated the map mid-iteration. The map is
now emptied first.

## The settings window, and what other browsers do with theirs

The layout was settled by looking rather than by taste. Firefox and Chromium are
both installed here, so their preference structures were read out of the
shipping builds — Firefox's panes and group ids come straight out of
`omni.ja`'s `preferences.xhtml`.

**Three things came back worth copying.**

- **A category list, not tabs.** Firefox, Chrome and Vivaldi all present
  preferences as a vertical list beside a stack, and all three moved that way as
  the count grew. The reason is mechanical: a tab strip runs out of width and
  starts eliding or wrapping, while a list has room for a name that says what is
  inside. Hydra had three tabs and passed four the moment site defaults arrived,
  so it moved too.
- **Privacy is one page, and its order is deliberate.** Firefox's privacy pane
  runs tracking → site data → **cookie banners** → passwords → history →
  permissions. Cookie-banner handling sitting directly after site data and
  before permissions is the right adjacency and was adopted: a banner is a
  question *about cookies*, and permissions are a different subject.
- **Firefox ships the banner blocker too**, which is a useful check on the
  design rather than a coincidence. Theirs is a single checkbox — "Automatically
  refuse cookie banners", private browsing only, "Only on supported sites". Two
  differences are deliberate here and worth stating. Hydra will **accept** where
  that is the only exit a banner offers, because refusing on principle leaves
  the page unreadable and unreadable was the problem. And it is a per-site
  tri-state like every other blocking option rather than one global switch,
  because the shield already governs everything else that way.

**The gap the survey exposed was not layout.** Every per-site feature had a
global default that only the shield could reach — so "what does this browser
allow by default" was answerable only through a popup attached to whichever page
happened to be open. There is a Privacy & security page now, built by walking
the feature enum, with a hand-written table deciding grouping and order.

**The table is not the mechanism, and that is the point.** A feature the table
does not mention still appears, under "Other". A settings screen that silently
omits a switch because nobody updated a list is worse than one with an untidy
last section — and four features were added to that enum in a week. Checked by
asserting that *every* feature has a control, so the check fails when the next
one is added and nothing places it.

**A defect it found on the way.** Saving was wired to the OK button's signal
rather than to acceptance, so any other route — code calling `accept()`, a
changed default button, a shortcut — would have closed the window and discarded
everything typed into it. `accept()` is overridden now. Found by a test doing
the obvious programmatic thing, which is the whole argument for driving a dialog
rather than reading it.

**15 checks** (`tests/live/try_settings_ui`): the list and stack exist and are
connected both ways, privacy comes first, all sixteen features have a control
including the banner blocker, a global default offers allow or block and never
"default" — that is what a *site* says when it falls through to here, so
offering it at this level would be a setting pointing at itself — accepting
writes through to the engine and leaves untouched features alone, and cancelling
writes nothing. Changing a default also re-applies policy to every live view, or
the setting appears not to have taken until the page is reloaded.

### Kiosk, which nobody could configure

§8 has had a settings gap since step 4: `kiosk_config` was code-level defaults
with no way to reach them from the app, so a presentation mode meant for
unattended displays could only be set up by editing the source and rebuilding.
It has a page now — home page and idle-return, scaling path and fit and design
size, cursor hiding, the render-process watchdog, and the escape flag.

**The page carries §8.1's warning where the choice is made**, not only in the
header file: reflow cannot stretch, so it approximates with cover; geometric is
the only genuine per-axis path and the one that has rendered black on some GPUs,
so it says to try it on the hardware being deployed on.

**Turning Esc off is a lockdown, and it says so plainly.** Unattended displays
need it, but switching it on can leave no way out of fullscreen except ending
the process, so it defaults to on, is worded as a warning rather than a feature,
and the default is asserted — a lockdown that arrived by accident would be the
worst bug this window could have.

**The shell reads the saved config now**, which is the difference between the
page existing and mattering: entering kiosk used the controller's compiled-in
defaults before. A configured home wins, and with none set the tab you were on
is still what is shown.

**Six more checks**, including the round trip that is the whole point — a value
set, accepted, and read back by a *freshly opened* window rather than from the
one that wrote it.

### Filters, and taking one back

The last of the pages, and it closed a gap rather than only presenting one: a
filter rule could be **accepted and never revoked** except by hand-editing
`filters-ai.txt`. That is a poor answer for a list built by accepting AI
proposals one at a time, because the whole design assumes some of them will turn
out wrong — §12.4's dry run exists precisely because a rule can look right and
be wrong.

`filter_list::remove` takes one back by its exact text, and the page lists what
has been accepted with its scope and the note saying why it was proposed. A rule
with no scope reads "every site" rather than showing an empty cell, since that
is the difference between a rule affecting one page and all of them.

**Removal is written through immediately, not at OK.** A rule that is blocking
something now should stop blocking it now, and the alternative is a window where
pressing Cancel silently restores rules the user watched disappear. Checked both
ways: the list stops matching the URL at once, and the file on disk agrees
without waiting for the dialog to close.

**Eight checks**, and the one that matters most is that `blocks()` goes false
immediately — a removal that only edits a file would look identical in the UI
and keep blocking until restart.

### And the flagged rules, where a maintainer can find them

The provenance field was carrying a flag nobody could see. A generic rule
learned locally is marked for the shipped defaults, which is worth nothing if
the only way to find the marked ones is reading a JSON file by hand — so the
Filters page lists the learned site rules beside the filter list, with the flag
spelled out in words (`→ ship as built-in`) rather than shown as a tick nobody
can interpret. Built-ins are not listed: they come from the program and cannot
be edited here.

**Copy the flagged ones** puts them on the clipboard already written as
`builtin("kind", "value", "note");` lines — the form `site_rules::defaults()` is
written in, so promoting them is a paste rather than a transcription. That is
the whole mechanism the "flag it for the next release" requirement asked for,
and it is now end to end: learn, flag, find, promote.

Learned rules can also be removed, written through immediately for the same
reason filter removal is.

**Ten checks**, including the two that keep the scope decision honest: a generic
rule says in words that it should be shipped, a rule tied to one host does not,
and the clipboard contains the first and not the second.

### Searching the settings

What makes a category list scale, and both Firefox and Chrome have it: six pages
is already more than anyone will read through to find one switch.

**The index is built by walking the pages after they are constructed**, not
declared beside each control. A registry someone has to remember to add to is a
registry that goes stale, and the whole point of a search box is finding the
setting whose name nobody could remember. Labels, group titles, checkbox text
and the labels attached to form rows are between them every word worth typing.

Each result says which page it is on, because that is the question being asked,
and choosing one **reveals the control** rather than dropping the reader at the
top of a long page — a form row's label points at its control, not at itself.

The settings window is six pages: Privacy & security, Media & players,
Downloads, Filters, Kiosk, AI.

## Settings (arch §11.3, §11.4)

Tools → Settings…. Two pages, both of which existed only as API before —
a setting nobody can change is a setting that does not exist, and that mattered
most for the BitTorrent caps: the whole argument for rasterbar was that the
ceiling can be raised, and that claim is only true if someone can raise it.

- **Player** — §11.3's radio group, finally built as specified: every supported
  player listed, installed ones selectable, missing ones greyed with
  "not installed" so you can see what to install, default resolved from what is
  actually present. Plus the **Custom…** entry §11.3 asked for, which needed
  new support in `player_launcher`: a command template where `%U` is the stream
  URL, appended if the placeholder is absent. Split on whitespace, **not** a
  shell — quoting and pipes do not work, and the hint text says so rather than
  letting someone discover it.
- **Downloads** — download folder, and BitTorrent connection caps, seed ratio,
  sequential-by-default and `listen_interfaces`. The caps carry the §11.4
  reasoning inline, where the person changing the number can read it. The
  interface field says plainly that Hydra does not tunnel and that this only
  makes a *system-level* VPN reliable — it is not a VPN feature.

**AI backend is a global setting** (§9.1), on its own page, and both consumers
— the tree reorganizer and the filter-evolution loop — resolve it through a
single `main_window::choose_ai()` instead of each repeating the local-first
rule:

- **Automatic** — local-first, as the design describes: a reachable Ollama
  handles everything and nothing leaves the machine; Claude is the fallback.
- **Local only** — never uses an external service. This exists because §1's
  "data stays on the machine" should be something you can *hold the app to*,
  not a default that quietly lapses the first time Ollama is not running.
  Under it, no local model means no AI features rather than a silent switch.
- **Claude** — explicit external opt-in, still gated by review-before-send.

Endpoint, both model names and the mode persist. **The API key does not**, and
that is deliberate: the settings file is plain INI, `claude_provider` states the
key is memory-only, and the same reasoning that kept the KeePassXC association
key out of plaintext applies here. The field is offered for the session and
labelled as not saved, with `ANTHROPIC_API_KEY` named as the way to persist it.
A test asserts the key never appears **anywhere in the file**, not merely that
no `setValue` call was written — that is the property that matters and it is
the one that could break silently.

**Ollama is re-probed rather than cached.** It is started and stopped like any
other local service, so an answer from earlier in the session says nothing
about now. `choose_ai()` re-probes on every use and the settings page re-probes
on open.

This turned out to be fixing a live bug, not just staleness. `probe()` is
asynchronous, and the old code probed then read `available()` immediately — so
the answer was always the *previous* one, and on the first use of a session
that is `false`. A running local model was therefore invisible on first use and
the payload went to Claude instead. `probe_now()` waits for the answer with a
bounded timeout; it blocks, deliberately, because the error is not symmetric:
treating a running local model as absent sends data off the machine that never
had to leave it, and a user-initiated action can afford a second to avoid that.
The settings page stays asynchronous — a window that freezes on open would be a
poor trade for a label — and refreshes the status when `probe_finished` lands.

**Probing in settings is a button, never a side effect of opening the window.**
Opening a config dialog must not reach out to the network by itself — the
endpoint may be remote, wrong, or mid-edit. So the AI page has **Check now**
(async, disables itself while in flight, tests what is *in the field* rather
than what was saved), and the Player page has **Rescan for players** for the
PATH probe. Until Check now is pressed the status says "not checked yet"
rather than claiming the model is unavailable — that would be an assertion
about the user's machine that nothing had verified.

The probe timeout is configurable (`ai/probe_timeout_ms`, default 2500,
clamped at 100). It only matters once the endpoint stops being local: loopback
answers in about a millisecond, but a host that *drops* packets rather than
refusing never answers at all, and then the full timeout is paid every time a
backend is needed. Tested against a stub that accepts and stays silent, which
is the only case that actually costs the timeout — a refused port returns
instantly and proves nothing.

Settings pages scroll. The explanatory text made a fixed dialog height a guess
that clipped on some font sizes, which is worse than a scrollbar.

**Capability routing is now accurate.** `player_launcher`'s header still said
the local proxy "does not exist yet" and that mplayer's manifest weakness was
merely *reported*; both stopped being true once `hls_assembler` landed. HLS is
assembled into one progressive file before it reaches a non-native player, so
only DASH — which has no assembly step — is still reported. A **Custom…**
player is treated as *not* handling manifests: nothing here knows what the
command is, assembling first works whatever it turns out to be, and assuming a
capability that is missing fails at playback with nothing to point at. The DASH
warning for a custom command says the limitation is ours rather than claiming
the player "cannot" do something we have no way to know.

Persistence is `QSettings` (INI, user scope, explicit `hydra/hydra` path so it
does not move if the app name is edited). **Saved values are applied by
`settings_store::load_into()` at startup, not by the dialog** — otherwise
"settings persist" would quietly mean "settings persist if you open the
dialog". A saved player that has since been uninstalled falls back to one that
is present rather than failing to launch anything.

**20 checks:** defaults, full round-trip of every field, fallback for a
now-missing player, `%U` substitution in place, append-when-absent, and a
custom command resolving to nothing correctly reporting itself uninstalled.
The tests redirect `QSettings` to a temp path, so they cannot touch the real
user config.

## Watch a torrent while it downloads (arch §11.3, §11.4)

**Watch** in the downloads window plays a job before it finishes, through the
existing local proxy and external player — the same path §11.3 already uses for
a growing HLS capture, now fed by a torrent.

Expressed neutrally, because it is not torrent-specific: an HTTP download is
written front-to-back and is streamable for the same reason. The seam gained
`source_capabilities::streamable`, `prioritize_streaming(id, file, on)` and
`contiguous_bytes(id, file)`; the UI's only transport-specific knowledge is
which *file extensions* are worth playing, which is a media judgement rather
than a transport one.

**The trap this is built around: a file's size is not a statement about what is
in it.** libtorrent allocates files **sparse and full-size from the outset**, so
`QFile::size()` returns the final size while the content is mostly holes that
read as zeros. The proxy previously trusted that size — which for a torrent
means confidently serving a player megabytes of silence, indistinguishable from
a corrupt stream. `publish_file()` now takes an optional readable-prefix
callback; the torrent source answers it by walking pieces from the file's first
piece to the first hole, mapping through `file_storage` because the file may
start mid-piece and may not be first in the torrent.

**Which file gets played.** The largest playable one, not the first. Releases
routinely ship a short sample clip that sorts ahead of the feature, and picking
by order would play the sample. That needed per-file sizes on the job, so
`download_progress::files` is a `QList<download_file>` (path + size) rather than
a `QStringList` — parallel arrays would have been fragile for no gain. Unknown
sizes and ties fall back to order, which is the best available answer rather
than a wrong one. The downloads window shows each file's size and marks the one
Watch would play, so the choice is inspectable instead of mysterious.

Sequential order alone is not enough either: it gets the front of the file first
but promises nothing about *when*. `prioritize_streaming` also sets
`set_piece_deadline` on the opening pieces, which is the difference between
"downloads in order" and "starts playing promptly" on a slow swarm.

**Verified against a throttled real swarm, 16 checks.** With the transfer held
mid-flight the file is 6 MiB on disk while only 32 KiB is readable; the proxy
serves exactly the readable prefix, **every byte matching the original**, and
refuses a range past it with 416 rather than answering with zeros. The prefix
never goes backwards, and after completion the whole file is served
byte-identically.

Two bugs this found:

- **Pad files were being reported as content.** libtorrent inserts zero-length
  padding files to align pieces in hybrid torrents, and all three reporting
  paths listed them — so the downloads window would have shown entries like
  `.pad/98304` beside the real files. Filtered at the source.
- `find_playable` originally returned `QString()` on both branches for
  single-file jobs, so Watch could never enable for them. The empty string is a
  *valid* answer there (the job's own path is the file), which is why the check
  is now a bool with an out-parameter.

Note for testing: libtorrent puts loopback peers in `local_peer_class`, which is
exempt from rate limits, so a "throttled" local seeder saturates the link and
finishes instantly. The test clears that class for 127/8 via
`set_peer_class_filter`.

## In-page magnet links (arch §11.4, §19.2)

Clicking a magnet link in a page routes it to the download manager. The seam is
`web_view_factory::set_external_url_handler` — "a URL the engine will not render
as a page" — and the shell's rule names no transport: **anything not renderable
that some download source will take becomes a download.**

**The obvious implementation does not work, and this was measured rather than
assumed.** Intercepting the navigation is the natural guess, but Chromium
classifies unregistered schemes as *external protocols* and disposes of them
before any per-navigation callback runs. Against Qt 6.8,
`QWebEnginePage::navigationRequested` is **never** invoked for `magnet:` while
it fires normally for `http` — the click goes to the desktop's registered
handler instead (during testing it launched kmail for `mailto:`, which is the
same mechanism doing its job).

What works is `QWebEngineUrlScheme::registerScheme()` before the engine
initialises, plus a `QWebEngineUrlSchemeHandler` on the profile. That catches
both a clicked link and a scripted `window.location`. Consequences:

- Registration lives in `main()`, because Qt requires it before QApplication.
- Only schemes a source can actually take are registered —
  `torrent_download_source::url_schemes()` returns empty without libtorrent, on
  purpose. A registered scheme with no handler swallows the click silently,
  which is worse than the error page.
- The hook is on the *factory*, not a view: the desktop mechanism is
  profile-wide. Android's `shouldOverrideUrlLoading` satisfies the same
  interface per view.

**Verified end to end with real clicks, 13 checks:** a clicked magnet creates
exactly one job on the torrent source, carrying the clicked URL and the tab it
came from; the page does not navigate away; ordinary links still navigate and
create no download; `mailto:` is left entirely to the engine. And the one that
matters most — **a page cannot start a torrent silently**: the job is `queued`
and `consent_required` fires, so page-initiated participation still goes
through the §11.4 gate.

## Downloads window (arch §11.2)

`downloads_dialog`, opened from Tools → Downloads… or **Ctrl+J**, and raised
automatically when a download starts. One list for every source — a torrent
appears beside an HTTP file with the same columns, progress bar and controls,
which is the whole point of §11.4's first-class decision.

Two rules it is built around:

- **It never asks what transport a row is.** Whether Pause is offered, whether
  children are shown, whether the row carries a warning — all of it comes from
  `source_capabilities` and the job's own fields. The word "torrent" appears
  only in a source's `display_name`.
- **Publicly-observable rows are visibly different**: an `⇅ public` marker in
  the Source column, coloured, with the source's participation note as a
  tooltip, plus a footer that appears only while such a transfer exists. The
  consent dialog says it once before the first one; this says it permanently,
  because the transfer keeps announcing for as long as it lives.

Indeterminate jobs (a magnet with no metadata) draw a busy bar rather than a
false 0%. Multi-file jobs list their files as children once known. Open Folder
opens the *containing directory*, never the file — the standing rule is that a
download is written to disk and not opened by us.

Rows are reconciled in place rather than rebuilt, and `changed()` is coalesced
on a 200 ms timer: it fires on every chunk of every transfer, and clearing the
tree at that rate would throw away the selection and scroll position several
times a second.

**Verified by rendering it**, which caught four defects that all looked fine in
the source: the `⇅ public` marker was being truncated away by column sizing
(defeating its entire purpose), progress text was clipped top and bottom by the
style's internal text layout (now drawn by hand), the Size column was elided,
and torrent rows were titled `magnet:` because stripping the query from a magnet
link leaves nothing. The last one is fixed in the *source* rather than the
dialog — it reports the `dn` name at `resolving` time, so the window never has
to know what a magnet link looks like.

## BitTorrent downloads (implemented)

Written up in **arch §11.4** rather than built, pending a decision. The short
version: BitTorrent fits as a third *source* for the existing download queue
rather than a new subsystem, and the trigger surfaces (`magnet:` navigation,
`.torrent` link, `application/x-bittorrent` response) are all things the current
spines already see.

**Implemented on desktop** in `torrent_download_source.{h,cpp}`, on
libtorrent-rasterbar, behind the `download_source` seam. Magnet links and
`.torrent` files (local, or fetched over HTTP first — libtorrent 2.0 does not
fetch them itself), multi-file jobs, seeding with a ratio policy, resume data,
and the connection caps raised off their defaults.

**Verified against a real swarm**, not a mock: a libtorrent seeder session and
the source exchanging a torrent over loopback with no tracker and no DHT, files
byte-identical, plus magnet metadata fetched from a peer over the wire. 35
checks. libtorrent is optional on the libsodium pattern — without it the source
reports `available() == false` and the shell does not add it, since a torrent
engine cannot be faked.

Three real bugs were found by running it rather than by reading it:

- Completion emitted `finished()` with no final byte count, so a torrent that
  finished between two status polls showed `received = 0` forever.
- The file list was only reported from `metadata_received_alert`, which fires
  when metadata arrives **from a peer** — so a `.torrent`, which has metadata
  from the start, reported no files at all.
- **Resume data was keyed by job id.** Job ids restart at 1 every session, so
  after a restart a new torrent would load an unrelated one's piece state. It
  is keyed by info-hash now, and a resume file whose hash does not match is
  ignored rather than trusted.

The decisions behind it:

- **First class, so embed.** Torrents are a peer of HTTP downloads — same queue,
  progress, pause/resume, and tab-tree association — not a handoff. A magnet
  link behaves like any other download link. Handoff is rejected on desktop
  (the association is lost the moment it starts) but stays the likely Android
  shape, so the `download_source` seam is still worth having.
- **First class means embed everywhere, including Android.** The side-loaded
  companion APK in arch §19.6 is a documented **fallback if store policy forces
  it**, not the plan — two installs is exactly the thing that would stop a
  torrent behaving like every other download.
- **rasterbar, chosen from the shipped headers.** rakshasa's 64 headers are a
  peer-scaling core — `choke_queue.h`, `connection_list.h`, `throttle.h`,
  `poll_epoll.h` — which confirms the throughput reputation, but it ships **no**
  magnet (BEP 9), µTP, UPnP/NAT-PMP, LSD, or streaming piece order. All of those
  are what "works when you click the link" is made of. µTP decides it: without
  LEDBAT backoff a torrent saturates the uplink and makes its own browser
  unusable. `set_piece_deadline` is the bonus — sequential pieces through the
  existing local proxy means **Watch** works on a torrent like it does on HLS.
  The scaling insight is honoured by raising the connection caps, not by
  library choice. Verified: both packages install a pkg-config named
  `libtorrent` — bare `libtorrent` resolves to rakshasa's; use
  `find_package(LibtorrentRasterbar)`.
- **If it is ever benchmarked, benchmark the right axis.** Swarm throughput
  comes from holding many mostly-poor peers, not from peak rate over a few good
  ones — measure throughput against peer count and find the ceiling, and check
  the default connection caps first, since a desktop-tuned default can hide that
  ceiling entirely. The ~2010 ranking is unverified today; the criterion is not.
- **No VPN feature, but visibility is required.** VPN/proxy tunnelling is a
  system-level concern — the OS and router do it properly, and a browser-local
  version would be a leakier reimplementation of something configured once for
  every app. What replaces it: torrent rows are visibly distinct in the download
  list, a plain-language explanation on the first magnet link, never starting a
  torrent on a page's initiative, and a `listen_interfaces` setting so a
  system-level VPN can actually be bound to (a settings field, not a VPN).

Seeding policy, ports/UPnP, multi-file jobs, the Android shape, and a suggested
implementation order are in arch §11.4.

## Wishlist (raised, not yet done)

Recorded verbatim-in-substance so they are not lost; none of these are started.


## Site extractors: the loop (implemented; measured against two model sizes)

`site_extractor` runs a generated parser script against the requests a page
made, and decides whether the answer may be shown to the user at all. This is
the core of arch §11.5, and the piece built first is deliberately the piece
where being wrong is expensive: the gate.

**The rule is the one §9.4 already uses on the tree.** A reorganization that
invents a tab id is rejected outright because no safe repair exists; an
extractor that returns a URL the page never requested is rejected for exactly
the same reason. A proposal is *choosing among addresses the page actually
fetched*, not authoring one. Tested both ways: an obviously foreign URL, and
the subtler case of a plausible-looking path on a host that really was
contacted — both refused as invented.

**The sandbox is a `QJSEngine` with nothing in it.** Verified rather than
assumed: `fetch`, `XMLHttpRequest`, `document`, `window`, `require` and
`process` all resolve to `undefined`. A script that never returns is
interrupted from a watchdog thread — a timer on this thread would never fire,
because a tight JS loop does not yield — so a proposal cannot hang the browser.
Not everything web-shaped is absent, though: `URL` *is* there, as a live model
demonstrated by parsing with it and getting no exception. It is inert — a
string parser with no I/O — so it is left alone, but "nothing in it" is a
statement about reach, not about the ECMAScript surface.

**All three ways of spelling the function now actually work.** The wrapper's
comment had claimed a bare expression, a function declaration and an assignment
were equivalent, and one of them was not: the wrapper opened with
`var extract = null;`, a function *declaration* hoists above that, and the
initialiser then ran and overwrote it — so `function extract(page, requests)
{ … }` was refused with "the script defines no extract() function" while
plainly defining one. `var extract;` with no initialiser leaves the hoisted
binding alone and all three forms arrive intact. Four checks, one per form plus
the guard still firing on a script that defines nothing.

Worth noting how it surfaced, because no amount of reading found it in months:
a real model, given real evidence, wrote the declaration form on its first
well-formatted answer. The synthetic fixture never produced that spelling, so
the suite never tried it, and the comment asserting it worked is presumably why
nobody checked.

**The page is not the stream.** A third rejection rule, and the model found it
rather than a test: asked for the stream inside a page, a run returned the
page's own address with `kind: 'direct'`. The document is the most
certainly-observed request there is, so the invented rule waved it through, and
it is fetched once, so the segment rule did not fire either. The media list
would have offered the HTML as though it were a video. Compared after the same
normalisation the invented rule uses, so a fragment or a trailing slash is not
a way around it, and the rule stays narrow — another request on the page's own
host is still a perfectly good answer.

Extractors are stored as plain JSON per host, so they can be read, diffed and
shared like the filter list. **84 checks** cover the accept path, both invented
cases, the segment rule, the page-url rule and its two edges, the furniture
rule and its mixed-type edge, the piece-of-the-stream rule with its
no-tier/playlist-passes/elsewhere-on-the-host edges, the `type`/`kind` split,
which addresses are worth
probing and what the answers do to the payload, all three ways of spelling the
function, the truncation trap, loops, throws, unparseable source, a script defining no
`extract()`, an unknown stream kind, the empty sandbox, folding, fenced
replies, and the store round-trip.

Qt6::Qml is a new dependency, and only for `QJSEngine`. No QML is used in the UI.

**The loop around it is wired**: Tools → Learn This Site…. `extractor_signals`
is a third rider on the interceptor's observer seam, keeping URL, kind and
order per page, bounded at 400 so a long-lived stream cannot grow it without
limit. `extractor_dialog` is the same review shape as the reorganizer and the
filter loop — see exactly what will be sent, send nothing until asked, judge
what comes back, accept or refuse — and accepted scripts are stored as JSON per
host under the app data directory.

**Evidence is folded before it is shown.** A player fetches hundreds of
segments differing only by a number; sending all of them buries the few
requests that matter. Requests are grouped by shape (runs of digits collapsed),
one line each, with `(+249 more like this)` appended rather than the repeats
being silently dropped — a reader should be able to tell a page that fetched
something once from one that fetched it four hundred times. Measured: 252
requests fold to 3 lines with the disguised manifest intact.

**Folding was measuring nothing on real evidence, for two years' worth of
reasons in one function.** `shape_of` collapsed runs of *two or more* digits and
kept query strings whole. On the measured site the segments are indexed
`seg-1 … seg-9` — single digits, never collapsed — and each carries a `k=`
token and a `kx=` timestamp that rotate per request. Fourteen segment requests
therefore produced **eleven distinct shapes**, and three things silently stopped
working:

- the `(+N more like this)` count, so the prompt's rules 4 and 5 (a manifest is
  fetched once, segments repeat) had nothing to bite on;
- the probe ranking's "which host is serving the flood" signal;
- **the gate's segment rule**, which meant a real segment was *accepted*. The
  synthetic fixture used `seg-00000.ts` — five digits, no token — which folds
  perfectly, so every test passed while the thing they tested did not work.

Now shared rather than copied (it was defined twice, once for the fold and once
for the gate, and they had to agree), keys kept but query *values* dropped, and
digit runs of any length collapsed. Ten real-shaped requests fold to two lines,
and a segment is refused as one.

**But folding also truncates, and that is a trap with a checked edge.** Each
line caps the url at 300 characters, while the gate compares against the full
address — so a request longer than the cap is *unreturnable by construction*:
the model can only return what it was shown, and what it was shown will be
judged invented. Real evidence already contains such requests, since the
analytics calls on the measured site run past the cap. No stream has yet been
long enough for it to bite, which is why it is a check rather than a comment,
and why the fix is deferred rather than guessed at — raising the cap lengthens
a payload that is already the suspected cause of the format failures above, and
eliding the middle leaves an address the model still cannot return. The honest
options are to send the full url for anything that could plausibly be a stream,
or to give proposals a way to name a request by its index rather than by
retyping its address.

**A learned extractor is actually used, and re-judged every time.** Opening the
media list runs the stored script for that host against the *current* evidence,
and the result is filed beside whatever detection found on its own. The gate is
applied on every run, not only when the script was accepted — so a site that
changes shape stops producing a result rather than producing a wrong one. That
was a real gap: the store was written and loaded but never read, so learning a
site did nothing and the status message promised otherwise.

**The review loop is exercised with a stub provider** (19 checks): the payload
is sent folded, a valid proposal becomes acceptable and stores with its fence
stripped, an invented URL leaves the accept button disabled and stores nothing,
and a stored script stops matching when the evidence changes. Five of those run
against a loopback CDN shaped like the measured site, so the probe → confirmed
manifests → gate → button chain is driven rather than assumed.

**Its headers are actually sent.** A learned extractor is asked for the headers
its CDN checks, and those were being dropped on the floor — built into the
verdict and then discarded when the media item was made, which is precisely how
you get the 403 measured earlier. `media_item` now carries them, and Watch
overlays them onto the page's own context before publishing through the proxy.
`stream_context` grew an `extra` map so headers beyond Referer / User-Agent /
Cookie are passed rather than silently dropped. Verified against a server that
reports what it received: all five arrive, including two the extractor invented
for itself.

**Download carries them too.** `download_request` gained a `headers` map, so
they travel to whichever source takes the job, and `http_download_source`
applies them. `enqueue()` takes them as a defaulted argument, which left every
existing call site alone.

**Range stays the source's own.** A caller-supplied `Range` is dropped rather
than merged: the resume offset is derived from what is on disk, and letting a
header decide it would mean a stale or hostile value silently corrupting a
resumed file. Tested with a partial file and a caller asking for
`bytes=999999-`: the request went out as `bytes=500-`, the caller's value never
appeared, and its other headers still did.

**Measured against a real model.** Ollama installed user-local at
`~/.local/ollama` (no root: the release tarball, extracted), running
`qwen2.5-coder:7b`. The loop works end to end — the model reads the folded
evidence, writes a parser, and the gate judges it.

**Model size dominates the hit rate.** Same evidence, same gate, five runs each:

| model | usable | how it failed |
|---|---|---|
| `qwen2.5-coder:7b` | ~1 in 3 | matched `.m3u8`/`.mpd` and found nothing; once returned a segment |
| `qwen2.5-coder:14b` | **4 of 5** | once used `endsWith('.txt')`, which misses a query string |

The 14B failure is the interesting one: it had the right idea — the manifest is
disguised as `.txt` — and then tripped on the query string, since
`…cf-master.1774687168.txt?k=…` does not *end* with `.txt`. A clean failure, and
the kind a prompt line about query strings might fix; worth trying before
tuning anything else.

### The query-string prompt line: tried, measured, reverted

It was tried. One sentence was added to rule 3 saying the url still carries its
query string, that `endsWith('.ext')` fails on `…/name.ext?k=1`, and to cut at
`?` or match with `includes()` or a regex. Ten runs per arm on the same
evidence, same gate, 14B — not five, because the target failure turned out not
to recur in the control at all:

| arm | found the manifest |
|---|---|
| original prompt | **8 of 10** |
| with the query-string line | **3 of 10** |

Worse, not better. Fisher's exact gives p ≈ 0.07 two-tailed, so on the numbers
alone this is suggestive rather than settled — but the mechanism is not
statistical. **The line does not do what it was written to do.** One run found
the disguise correctly, with `includes('/cf-master.')`, and then wrote
`&& endsWith('.txt')` anyway, with the new sentence sitting in its context. It
was reverted.

The likely reason it hurt: every failing run reasoned about *extensions*, and
the line is entirely about extensions, so it draws attention to the losing
strategy while rule 2's "prefer a stable path fragment" — what every clean pass
actually did — stays a single clause. The hypothesis worth testing next is
therefore the opposite one: strengthen the fragment rule rather than the
extension rule.

**Do not test it on this evidence set.** Every one of these twenty runs is
against the one synthetic fixture, and a prompt tuned until that fixture passes
is measuring how well `cf-master` has been described to the model, not whether
the loop can learn an unfamiliar site. That is the same over-fitting the ledger
at the top of this file already lists as unmeasured. The fragment-first line
should be tried against a second evidence set — which is what "point the
extractor loop at a real site" is for.

**A trap this exposed, worth keeping.** The first tally counted `gate:
ACCEPTED` as success, and it was wrong: one accepted run had returned the page's
own url. Gate-acceptance means *not provably bad*, not *correct* — the gate is a
safety rule, not an oracle. Score these runs on what was picked, never on
whether the button lit up.

Inference here is **CPU-only** (12 cores, 19 GiB usable; ollama drops the
integrated Intel GPU), so 14B takes a minute or two per proposal and 32B is not
viable — a ~20 GB model against 19 GB free would thrash.

**Roughly one run in three produces a usable extractor** with the 7B model, on
this evidence. The successful ones find the disguised manifest
(`cf-master.1774687168.txt?k=…`) by matching `master.` as a *shape*, which is
exactly what URL detection cannot do. Two failure modes were seen, repeatedly:

- **Extension matching.** The script looks for `.m3u8`/`.mpd`, finds nothing,
  returns null. A clean failure — the gate reports "found nothing".
- **Picking a segment.** The script returns `seg-00000.ts`, which the page
  genuinely requested. **The gate accepted this**, because observed is not the
  same as correct, and that was a real hole: it would have played ten seconds
  of video. Closed — a URL that is one of many near-identical requests is a
  segment, since a manifest is fetched once. The rejection now names the count.

**A prompt change made on three samples made things worse and was reverted.**
Adding a hint about which host the segments came from pushed the model toward
segments. Tuning a prompt against one model's quirks is not the durable fix;
tightening the gate is, because it holds whatever model is behind it.

That is the whole argument for this design: a model that is right one time in
three is *usable* precisely because the wrong answers are refused rather than
acted on. No proposal in any run returned an invented URL.

## Media Source tap (implemented, with capture)

`mse_tap` reports what a page is actually feeding its `<video>`, for the sites
where watching request URLs finds nothing. Verified on the site from the
section below, through the app's own seams: URL detection found **0 items**
while the tap reported
`video/mp4;codecs=mp4a.40.2,avc1.64001E, 5,168,080 bytes, pos 13.1s`, and the
media badge read **"Media (playing)"** instead of staying empty on a page that
was plainly playing video.

**Two scripts, and the split is the security design.** The hook must run in the
page's own world, because an isolated world cannot wrap the page's
`MediaSource` — verified, not assumed. So the main-world half holds nothing and
grants nothing: it wraps two methods, counts bytes, and dispatches a DOM
`CustomEvent`. The privileged half — the QWebChannel bridge — stays in the
isolated world where autofill already lives and only listens for those events.
The page can forge them, so everything arriving is treated as a claim: the site
key and mime are length-capped and a site is bounded to 8 mime entries, so a
page calling `report()` in a loop cannot grow the map.

The seam gained `inject_main_world_script()` as a **separate call** rather than
a flag on `inject_script()`, so the escalation is greppable. It also runs on
subframes, which is not optional: on real sites the player is a third-party
iframe, and a tap confined to the top frame sees nothing.

**And that last sentence described the hook only. Until now the tap did not work
in an iframe at all — which is the shape most watch pages have.**
`tests/live/try_subframe` is the reproduction and the proof, offline and
deterministic: a page on `127.0.0.1` embedding a player from `127.0.0.2` that
feeds a MediaSource, with the same player as its own document for a control and
a same-origin iframe to separate the two candidate causes.

**It was never about cross-origin.** An iframe of *any* origin has no relay in
it: the relay lives in the isolated world, and Qt installs
`qt.webChannelTransport` in the main frame alone, so one injected into a
subframe would have nothing to connect a QWebChannel to. The hook ran there
perfectly well and dispatched its DOM event into a document where nothing was
listening.

**The fix is that a subframe hands its report up.** `window.top.postMessage`
works cross-origin and the top frame does have a relay. What the top frame will
*not* take from the message is the name to file under: that stays its own
`location.hostname`, so a frame cannot speak for a site it is not, and the key
is the one the shell already looks up. The same rule §13.2 applies to
credentials, for the same reason. Everything else in the message keeps the
treatment it always had — a claim, length-capped and bounded per site.

**The price, stated rather than buried:** a report posted to the top frame can
be read by the embedding page's own scripts, so that page learns the mime, byte
count and playback position of media inside an iframe it embedded. It chose to
embed that iframe, the figures are coarse, and the alternative is a tap that
does not work — but it is a disclosure this instrumentation creates, and it
should be weighed again if the tap ever carries more than these numbers.

**A wrong diagnosis was committed before the right one, and it is worth more
than the fix.** This exact change was tried once, appeared to fail, and the
instrumentation said the top frame's relay had never connected its QWebChannel —
so it went into the file as an unexplained Qt behaviour and the change was
reverted. It was neither unexplained nor true. The channel connects on those
pages perfectly well, only later than the driver's fixed six-second window, and
the report arrived after the measurement had already been taken. **A fixed wait
is a measuring instrument, and this one manufactured a mechanism that did not
exist.** The driver now waits until a report arrives or a generous deadline
expires, which cannot fail in that direction.

Why it went unseen for so long: every earlier verification of the tap used a
player page loaded as its own document, where there is no subframe at all.

**Capture works, and is verified end to end.** Tools → Capture Playing Video
opens a file, arms the hook and reloads (the recorder has to be in place before
the player builds its MediaSource, or the init segment is missed and the result
decodes as nothing). Segments are POSTed to a `local_proxy` capture endpoint;
stopping offers the file as a normal media item, so Watch and the download list
need to know nothing about capture. Against a local page feeding a fragmented
MP4 through MSE, the captured file came out **byte-identical to the source**
(911,975 bytes) and passed a full `ffmpeg` decode with no errors.

**On the token that lands in the page.** The capture URL is main-world, so the
page can read it — and that is acceptable rather than tolerated: it grants
exactly one thing, appending to a file the user just asked to create, and the
bytes in that file are page-supplied by definition. A page abusing its own
token can only corrupt its own capture. It cannot read, cannot name a path,
cannot reach another capture, and only loopback URLs are ever emitted.

Proxy plumbing this needed: the connection handler used to assume a request
arrived in one read, which is true of a GET and false of a POST body, so each
connection now accumulates until its headers and declared `Content-Length` are
both in hand. A single chunk is capped at 32 MiB — a media segment is not a
file upload, and an absurd declared length must not be buffered.

**The media list agrees with the badge.** A page whose video only the tap can
see used to show `Media (playing)` and then open an empty list — the badge and
the thing it points at contradicting each other. Such streams now appear as
`Playing` rows carrying the mime and how much is buffered, and the one action
offered is **Capture**, because there is no URL to Watch or Download: the bytes
exist only inside the player. Reproduced locally rather than against a live
site by serving the media under an extension the detector ignores, which gives
the same condition deterministically: `detector=0`, `tap_active=1`, one row.

**A capture is a job.** `download_manager::adopt()` registers a job whose
transport is already running, which is the honest shape here: `enqueue()`
schedules — picks a source, waits for a slot, calls `start()` — and a capture
has nothing to schedule because the page drives it. Everything after that point
is ordinary, so `capture_source` reports through `progressed()`/`finished()`
like any other and the downloads window cannot tell the difference. Verified
live: the row reads `Media capture · 16.2 MiB · Downloading — 8.10 MiB/s` with a
busy bar (a recording's length is unknown until it stops, and a percentage of an
invented total would be a lie), Watch enabled because the file is written
front-to-back, Pause and Resume greyed because a recording cannot be resumed.

**Cancel from the window works, and testing it found a real bug.** The path is
Cancel → `download_manager::cancel()` → `capture_source::cancel()` →
`stop_requested` → the shell stops the recording, since a source cannot stop a
page by itself. It worked, but the job then showed as **Complete**:
`on_finished()` let a success overwrite a status already set to cancelled. HTTP
never exposed it because aborting a reply reports failure, while a capture
reports success whenever any bytes were written. A cancellation is the user's
decision and now survives a source finishing afterwards — that source is
describing what it managed before stopping, not undoing the cancel. Two seam
checks cover it, and the live path now ends `cancelled` with the partial
recording kept.

**It reports as it goes.** The action reads `Capture Playing Video — 16.20 MiB`
and the status bar shows a running rate, both updated on a 500 ms poll of the
proxy's byte count. If nothing arrives for twelve seconds it says so, and says
the likely reason — the commonest way to end up with an empty file is arming
capture and forgetting to press play, which previously produced silence for the
whole recording and an empty file at the end. A feed that stops mid-way is
reported as paused rather than left looking like work.

**Load-tested, since one HTTP connection per segment is the obvious way this
falls over.** A feeder appending 32 KiB at a time — 519 segments, 17 MB, 90 s
of 720p — came out byte-identical and decoded cleanly, while the process held
flat at 130 descriptors and 52 sockets for the whole run with no growth in
system-wide TIME-WAIT. Deliberately a local feeder rather than a real site: it
makes the segment rate the only variable and keeps the test repeatable. What
remains unverified against a live site is network variability, not the
mechanism — the hook is already known to fire there.
Observed while testing: the page reported `duration ≈ 7469s` for a short
episode, which is a placeholder an unbounded MediaSource commonly carries, and
a good reminder that these numbers are the page's claims rather than facts.

## yt-dlp handoff (implemented)

`ytdlp_resolver` asks yt-dlp what the video on a page actually is — the first
thing to try per arch §11.5, because a real URL is the best possible outcome:
everything downstream already works with one (external player, Range resume,
the proxy's context injection). Reachable from **Tools → Find Media on This
Page…**; results land in `media_detector` through a new `add_item()` and show
up in the existing media dialog with Watch and Download.

Found in one of two ways, in this order: `yt-dlp` on PATH, which is the copy
the user's package manager keeps current and therefore the point of the whole
exercise; else the vendored submodule under `python3`. Neither means
`available()` is false and the action reports why — nothing else degrades.

**Format preference is deliberate**: progressive HTTP beats a taller manifest.
480p progressive is chosen over 720p HLS and 1080p video-only, because it needs
no assembly, resumes with a Range request and plays in anything.
`http_headers` are carried through, which is what keeps the CDN from answering
403 (§11.3).

**20 offline checks** on the parsing and the choice — playlists falling back to
their first entry, `filesize_approx` standing in for `filesize`, `acodec:
"none"` meaning no audio rather than unknown, non-JSON refused, empty formats
treated as failure rather than empty success. Plus a live end-to-end run: the
subprocess resolves a real page, and the action populates the media dialog.

Note on that live run: the page used was a direct `.mp4`, which the detector
already sees from the request stream, so `add_item()` correctly deduplicated
by URL and the row shown came from the detector. The handoff is proven, but a
page where yt-dlp finds something the detector cannot is the case still to be
exercised.

## Media detection against a real site (tested; it does not work there)

Pointed the media path at `dramafren.org` in the running app, logging every
request through the interceptor's observer seam. The answer to "is URL-shaped
detection enough" is **no**, and the reasons are worth keeping.

**Nothing is requested until play is pressed.** On the watch page, 92 requests
across 22 hosts and not one media URL. §11.3 predicted exactly this ("many
sites only request the manifest when their player initializes or the user
presses play"), and it is now measured rather than assumed. A synthesized Qt
mouse click on the view — which the engine treats as a real user gesture — is
what made the stream appear.

**The manifest is disguised.** After the click the player fetched
`…/v4/db/<id>/cf-master.<digits>.txt?k=…`. The name says master playlist; the
extension says `.txt`. `media_detector::classify()` works from the URL, so it
sees nothing, and the badge stays empty on a page that is plainly playing
video. This is the limitation §11.1 already documents — "reliable Content-Type
and manifest-body classification uses the optional local proxy (§10)" — and it
is not a corner case but the normal state of affairs on a site that expects to
be filtered.

**Fetching that URL directly returns 403** from Cloudflare, which is the §11.3
request-context argument holding up in the field: the CDN wants the page's
Referer, cookies and User-Agent, so a naked fetch is refused. The body could
not be confirmed for that reason; the reading of it as an HLS master playlist
rests on the name, not on inspection.

**Some of the delivery is peer-to-peer.** `tracker.webtorrent.dev` and three
bare-IP hosts were contacted only after playback started. A design that watches
HTTP requests for media URLs cannot see video arriving over WebRTC data
channels at all, and no amount of Content-Type sniffing changes that.

**The conclusion is architectural, not a patch.** Site-specific knowledge has a
half-life of weeks, so it cannot be C++ — that would mean a rebuild and a
release per site, with the working set frozen at ship time. It belongs where
the filter list already lives: **produced, reviewed and stored as data**, by
the AI diff/accept pipeline that already does this twice (tree §9, filters
§12). Extraction is the third consumer, and its output is a **parser script**
validated by having to pick a URL that was actually observed — the same
"cannot invent" rule that governs reorganization. See arch §11.5.

**yt-dlp is vendored** at `third_party/yt-dlp` (Unlicense, so no friction with
GPL-3-or-later; shallow submodule, ~15 MB). Three jobs: try it first and skip
the model entirely where it supports a site, since it is free and maintained
by people tracking site changes; hand its nearest extractor to the model as
worked reference when it does not; and use it as ground truth where it does.
Not a build dependency — nothing in `CMakeLists.txt` refers to it.

**And there is a mechanism that works today, measured.** A main-world script
wrapping `MediaSource.addSourceBuffer` / `SourceBuffer.appendBuffer` captured
the real video on this exact site: a `video/mp4;codecs=mp4a.40.2,avc1.64001E`
source buffer, append totals past 5.1 MB, and the element reporting real
playback positions — while URL detection saw nothing, yt-dlp refused the site,
and part of the delivery was peer-to-peer. Whatever the transport, the page
ends up pushing segments into MSE, so a tap there is transport-agnostic by
construction. Design in arch §11.6.

**Multiple sources per page is what settles it.** A watch page offers a mirror
list (`ul.mirror`); one episode of the measured site carries two mirrors from
two unrelated vendors (`dramafrenvip.upns.pro`, `abyssplayer.com`), held as
base64 iframe snippets in `data-em` and absent from the DOM until clicked —
only one literal `<iframe>` is in the initial HTML. Extraction costs one recipe per player per
mirror, maintained forever; the tap costs none, because it captures whichever
mirror the user actually started. The price is that it must run in the **main
world** — an isolated world cannot see the page's `MediaSource`, which was
verified rather than assumed — so it is a read-only byte observer injected on
an explicit action, never the standing, privileged kind of script that
autofill and the element picker are.

Measured caveat: **yt-dlp does not support this site either**, dedicated or
generic. That is the case a generated extractor exists for, and a reminder
that vendoring is not by itself a solution. The remaining implications stand:
the local-proxy content-type tier is what makes detection work on real sites
rather than being a refinement; the media affordance should wait for a play
gesture; and P2P-delivered video is outside the model entirely.

## The extractor loop against a real site (measured; it fails there)

The step every prompt number in this file was waiting on. `tests/live/
try_extract` drives the real shell to a real watch page, scrolls the player
into view, clicks it, and writes what the interceptor saw to JSON;
`test_live_model <model> <evidence.json>` replays that file through the loop.
Capture and propose are deliberately separate: evidence captured once is
repeatable, costs the site nothing to re-measure against, and is the second
evidence set this file has been asking for.

**The disguise goes much further than the manifest.** The section above
recorded the manifest as `…/cf-master.<digits>.txt?k=…`, and that holds. What
had never been looked at is everything around it:

| what | how it arrives |
|---|---|
| manifest | `ssu5.stellarpathventures.space/v4/9ow/<id>/cf-master.<digits>.txt?k=…&kx=…` |
| variant index | `index-f1-v1-a1.txt?k=…` |
| init segment | `init-f1-v1-a1.**woff**?k=…` |
| media segments | `seg-N-f1-v1-a1.**woff2**?k=…` |

The segments are dressed as **web fonts**. Every extractor this project has
measured used `.ts` as its segment test, because the synthetic fixture used
`.ts` — so all of them would fall through to nothing here, and the two
"segment" heuristics in the prompt (rule 4's repeat-counting, rule 5's prefer
the manifest) are the only parts that still bite. The stream host is also
nowhere in the page or the player origin; it is reachable only by watching.

**The loop produces no parser at all on this evidence. Five runs, zero
usable.** Not a wrong parser — no parser. The 14B replies with *prose*: a tidy
summary of the request log, correctly naming the manifest and identifying the
`seg-…woff2` addresses as video, and then never writes the function. The gate
refuses all five, four as a syntax error and one for defining no `extract()`.
So the honest reading of the 8-of-10 recorded above is that it measures a short
synthetic payload, not the task: the real payload is longer and noisier, and
the reply format collapses under it before the extraction logic is ever
exercised. Fix the format adherence before drawing any further conclusion from
prompt experiments.

**The player fails when embedded and works when loaded directly.** On the watch
page it printed "Failed to setup player, please try again later" and never
issued its first API call (`/api/v1/info?id=…`); loaded on its own it issued it
immediately and streamed. Blocked requests still reach the observers, so this
is not the interceptor — the call is absent, not refused. Note this contradicts
the earlier session recorded above, which did get playback on the watch page
after a synthesized click, so it is either a site change or a difference in
where the click lands; the mirror list is base64 in `data-em` and only one
iframe is in the initial HTML, which is exactly the fragile part. Unresolved,
and worth console capture before guessing further.

**The harness was hiding this.** `test_live_model` picked its result pane by
looking for one containing "function" or "extract", which was fine while every
reply was JavaScript and reported a bare "(none)" the moment one was not —
discarding the only interesting artefact. It now takes the proposal pane by
position. A test that can only describe the outcomes it expected is the same
defect class as the gate that had 27 checks and no page-url rule.

**The capture is not committed.** It carries live CDN tokens and analytics
identifiers from a real session, so it lives outside the repo; regenerate it
with `try_extract` rather than reusing a stale one, since the `k=`/`kx=` tokens
expire anyway.

### Four prompt iterations against it, and what each one bought

Five runs per iteration, same captured evidence, `qwen2.5-coder:14b`. Every
change is in the payload's tail — the contract restated *after* the evidence
rather than only in the system prompt before it.

| iteration | produced code | right signature | found the stream |
|---|---|---|---|
| system prompt only | 0/5 — all prose | — | 0/5 |
| + "reply in JavaScript" | 4/5 | 0/5 | 0/5 |
| + the signature, as a skeleton | 4/5 | 4/5 | 0/5 |
| + the contract in prose, `kind` disambiguated | 5/5 | 5/5 | **0/5** |

Each step moved the failure one stage further down and none of them reached a
working extractor. That is the headline: **prompt iteration alone did not solve
this site**, and the last iteration is where the model finally attempts the
real task and still fails it.

**Three of the failures were ours, not the model's.** Worth separating,
because they inflated every "the model cannot do this" reading:

- `wrap()` rejected `function extract(…)` — the declaration hoisted above the
  wrapper's own `var extract = null;`, which then nulled it. Fixed.
- `test_live_model` picked its result pane by searching for the word
  "function", so a prose reply reported "(none)" and threw away the artefact.
  Fixed.
- The skeleton in the tail was copied out verbatim, placeholders and all:
  `url: <one of those urls>` came back as a syntax error. The shape is now
  described in prose, with nothing that can be pasted.

**`kind` meant two different things, and the model noticed before we did —
now fixed.** On a request it was what the browser fetched the thing as
(`script`, `image`, `other`); in the return value it is the stream type (`hls`,
`dash`, `direct`). Two vocabularies, one field name, and two runs in five
returned `request.kind === 'hls'` — never true of anything, so they returned
null and read as a model failure rather than a naming one.

A request now carries **`type`**; only the returned object has a `kind`, which
is the one the proposal decides rather than observes. Three checks: that `type`
is there, that `kind` on a request is `undefined` so the collision is gone
rather than merely discouraged, and that the return value still declares its
own. The payload also names its columns now (`order | type | url`), because
unlabelled the middle one reads as whatever the reader assumes.

Nothing had to be migrated: no `extractors.json` existed on this machine. A
stored extractor written against `r.kind` would break, and that is the moment
to have made this change rather than after a library of them accumulates.

The prompt gained a seventh rule at the same time, so it matches what the gate
now enforces: anything fetched as a `script` or an `image` is furniture and
will be rejected, so it is not a useful fallback. A gate rule the prompt does
not mention is a rule the model discovers by having its work thrown away.

**A third gate hole, same shape as the first two — now closed.** One run was
*accepted* having picked `mc.yandex.com/sync_cookie_image_check?…` — a tracking
pixel, `kind='image'`, which the script returned as a `direct` stream after
failing to find `.m3u8`. It is genuinely requested, it is not one of a flood,
and it is not the page, so nothing refused it, and the media list would have
offered a tracking pixel as a video. The interceptor knew it was an image the
whole time and the gate was discarding that.

The rule: **what the browser fetched as an image or a script is page
furniture, not a stream.** Judged on every sighting rather than the first, so
an address seen both as an image and as something else is not decided by
whichever came first — only an address that was never anything but furniture is
refused. Six checks, including that the manifest (fetched as `other`) still
passes and that the mixed case does.

It also caught a stale test. The page-url section proved its rule "stays
narrow" by returning `app.js` from the page's own host, which the furniture
rule now refuses on its own grounds — so the check passed for a reason that had
nothing to do with what it claimed to test. It uses a same-host request that is
not furniture now. A rule that breaks a test by being right is worth more than
the test was.

**The conclusion is architectural, and it echoes §11.1.** What a proposal gets
is url, resource kind and order. On this site the manifest is `.txt`, the
segments are `.woff2`, the init segment is `.woff`, and the stream host appears
nowhere else — so there is no extension signal, no host signal, and the only
real discriminator left is *how many times each shape was fetched*, which the
folding already computes and the prompt mentions in one clause. Every failing
run reached for an extension. It is worth asking whether URL-shaped evidence is
simply too thin here, exactly as `media_detector::classify()` was found to be
too thin on the same site: the answer there was the local proxy's content-type
tier (§10), and the same tier would tell an extractor that one `.txt` is
`application/vnd.apple.mpegurl` and the `.woff2` files are video segments.
Prompt work should probably wait behind that.

### Four more arms, with the content types in hand — and it works, sometimes

The measurement the section above was waiting on. Fresh capture, five runs an
arm, `qwen2.5-coder:14b`, one change per arm, scored on **what was picked**
rather than on whether the button lit up:

| arm | the change | found the manifest |
|---|---|---|
| the payload as it stood | content types annotated on each line, tail silent about them | 0/5 |
| + the instruction in the tail | "use the notes to work out which line is the stream" | 0/5 |
| + the field model | "the note is not part of the data your function gets" | 0/5 |
| + manifest precedence | "a line noted HLS or DASH **is** the answer" | **2/5** |

**Annotating the evidence did nothing until the tail said to read it.** The
first arm is the one the previous session predicted would work: the manifest sat
there annotated `-> application/vnd.apple.mpegurl (HLS)`, correct and in place,
and five runs in five wrote `endsWith('.m3u8')` and found nothing. The note was
present, right, and ignored. Every gain this project has had from prompt work
has come from moving a sentence into the paragraph *after* the evidence, and
this is the fourth time — the mid-payload sentence explaining the `-> …` syntax
had been there all along.

**Then it was read, and read as part of the url.** Arm two put the instruction in
the tail and three runs in five wrote
`request.url.includes('-> application/vnd.apple.mpegurl')`. That is our defect,
not the model's, and the same one as the pasted skeleton: the fold prints the
note on the same line as the address, so the text a reader sees and the object a
script receives were two different things and nothing said so. Arm three names
the fields a request actually has and says the url ends where the note begins.

**Arm three is where it starts working, and it picked the wrong piece.** Four
runs in five stopped reaching for extensions and matched the stream host by a
stable path fragment — the behaviour rule 2 has asked for since the beginning
and never got. Three of them returned `init-f1-v1-a1.woff`, the initialisation
segment, because both it and the manifest carry a note saying "this is a
stream" and nothing said they were not alternatives. Arm four says so, and two
runs then returned `/cf-master.` matched as a fragment, `kind: 'hls'`.

**What the gate did, and one correction to make.** Scored against the shipping
path rather than `site_extractor::check()` alone, the two beacon picks were
**refused by the content-type tier**, not by the static gate: `confirm_by_
fetching()` fetched `region1.google-analytics.com/g/collect?…`, found no stream
in it and disabled Accept. The harness's own `check()` call has no probe and
reports those as `usable=1`, which reads like a gate hole and is not one. This
is the tier doing the exact job it was built for, on an address nobody had
thought to write a rule about.

**The init segment was a real gap, and it is now the fifth gate rule.** It is
fetched once, so the segment rule does not fire; it is not the page and not
furniture; and when the tier fetches it the body genuinely is an ISO-BMFF
stream, so the probe confirms it too. Everything we had said yes and the answer
is wrong — an init segment on its own decodes to nothing. Written up below.

### A piece of the stream is not the stream (the fifth gate rule)

Same shape as the other four, and found the same way: by a real model, on real
evidence, returning something no test had thought to forbid.

**What settles it is what the tier already established.** The probe had
identified `cf-master….txt` as a playlist. The parts a playlist names sit beside
it, so a pick from the manifest's own directory that is not itself a confirmed
manifest is one of those parts rather than an alternative to it. `check()` takes
the confirmed manifests as an optional set; the dialog keeps them from the same
probe replies that annotate the payload, as urls rather than as the sentence it
prints, so the two spellings of one fact cannot drift.

**Same directory, not same host, and the narrowness is the point.** A
progressive mp4 served elsewhere on that host is a *better* answer than the
manifest, not a piece of it, and a rule that refused it would be doing harm.
Missing a part kept in a subdirectory is the failure this prefers to make.

**It cannot fire without the tier, by construction.** No network, a CDN that
refuses the context, a 403 — the set is empty and nothing changes. Only a
*positive* identification is ever acted on, which is the same rule §10 already
follows: an absent tier may not cost a proposal its accept.

**The consequence to know:** this is an accept-time rule. `main_window` re-judges
a stored extractor on every run with no probe results in hand, so the set is
empty there and the rule is inert. That is sound as far as it goes — a script
that picks a part never gets stored in the first place — but it is not the
"holds whatever model is behind it" property the other four rules have, and it
is worth knowing before relying on it.

**Verified through the real dialog, not just the function.** Seven checks on the
rule itself, and five more driving `extractor_dialog` against a loopback CDN
shaped like the measured one — a playlist wearing `.txt`, parts wearing `.woff2`
whose bodies really are ISO-BMFF. The dialog probes, keeps what came back, hands
it to the judge on another thread, and refuses an init pick while still
accepting the playlist. Written that way because the rule being right and the
path to it being dead is precisely this project's defect history.

**Then measured live, which is the only claim worth making.** Five more runs on
the same captured evidence with the rule in place:

| runs | picked | outcome |
|---|---|---|
| 3 of 5 | the manifest | accepted |
| 1 of 5 | a media segment | refused, segment rule |
| 1 of 5 | the init segment | refused, this rule, naming the playlist |

**No wrong answer was accepted.** That is the property worth having — not the
3-of-5, which is one model on one capture and will move. Every wrong pick the
model has produced against this evidence across fifteen runs is now refused by
some rule, and each of those rules exists because a run produced the answer it
refuses.

One thing the live run showed that the unit tests could not: the sentence named
the *variant index* rather than the master, because it was iterating a `QSet` and
taking whatever the hash yielded. Named in evidence order now — a player fetches
the master and then the variant it chose, so the earlier sighting is the one to
point someone at, and a user-facing sentence should not change between runs for
no reason.

**And the harness was corrected while it was here.** `test_live_model` printed
its own `check()` result as `verdict:`, with no probe and no fetch of the pick —
which is how an analytics beacon came to read as `usable=1` in these notes while
the dialog beside it had already refused it. That line is `check-only:` now, and
the shipping path's own words are printed as `dialog:`. The apparatus lies; this
is the seventh time.

**Two operational notes for the next capture.** The watch page still fails the
way the section above records — "Failed to setup player", screenshotted again —
so `try_extract` against it captures 63 requests with no stream in them. The
mirror loaded directly (`dramafrenvip.upns.pro/#<id>`) initialises immediately
and streams, and that is where this evidence came from. And the stream host has
rotated since the last capture: `sil5.luminarstrategyhub.site` now, where
project.md recorded `ssu5.stellarpathventures.space`. The path shape under it is
unchanged, which is the argument for matching a fragment rather than a host.

### The second evidence set: attempted, and blocked on input

The same watch page lists a second mirror from an unrelated vendor — Abyss,
`abyssplayer.com/<id>`, a JW Player build from `iamcdn.net`. It is the cheapest
honest second site available: different vendor, different player, same page.

**Three things were learned, and only the third is about the site.**

- **Its address cannot be loaded directly.** `abyssplayer.com/<id>` on its own
  bounces to `abyss.to/`, the vendor's marketing page. It wants the embedding
  page's referer, so the only way in is the way a user takes: click the mirror
  in the chooser. `try_extract` takes a CSS selector to click first now, which
  is a general need — a watch page that puts its player behind a chooser is
  normal, and only one iframe is ever in the initial HTML.
- **The captures had been quietly contaminated.** The driver opened a tab on the
  tree's first node and *then* typed the address, and that first page's slower
  subresources were still arriving after the navigation committed — at which
  point Chromium reports the new address as their first party, so they landed in
  the capture under the capture's host. It presented as twelve `doc.qt.io`
  requests in a dramafren capture. The driver now writes its own one-node tree
  pointing at `about:blank`, in the output directory, which also gives every run
  a fresh state directory and stops the driver rewriting `sample-tree.txt`.
  Worth noting the underlying behaviour is not the driver's: a slow subresource
  of the previous page can be attributed to the next one in the app too.
- **Hydra's own ad blocking is what stops that player, and this is measured.**
  The clicks were landing all along — every one was answered by the ad network,
  `fleraprt.com/push` and fresh creatives from `aichouphaugn.com`, while the play
  triangle stayed put. Eight clicks changed nothing, so more of them was not the
  answer. What the request log showed was: the player pulls in
  `cdnjs.cloudflare.com/ajax/libs/fuckadblock/3.2.1/fuckadblock.min.js`. It is
  watching for a blocker before it will play. Allowing ads for the capture — and
  *only* that, with popups still blocked, so the two are not confounded — makes
  the play button disappear and the player begin loading.

  This is the first thing the ad-host list has done at runtime, and it is worth
  more than the capture it was chasing: **on this mirror the blocker is why the
  video does not play**, which a user would experience as the browser being
  broken. It also has to be weighed against the other mirror's "Failed to setup
  player" on the same page, which now has a candidate cause.

**And it still does not stream.** With ads allowed and popups allowed, four
clicks and nearly three minutes of watching, the player spins and never contacts
a stream host at all — 75 requests, none of them media.

**The tap says it is stalled, not peer-to-peer.** `try_extract` reports what the
§11.6 tap holds beside what the request log saw, because those answer different
questions: a page feeding a MediaSource while fetching no media is delivering
over something a request log cannot see, and a page doing neither has not
started. The tap saw nothing here. It was calibrated first against the mirror
that does play, where it reports `YES` — a "no" from an instrument that has not
been shown to say "yes" is worth nothing.

Two caveats on that reading, and the second is a defect in its own right. The
tap is not a complete answer for WebRTC in general — it sees `MediaSource`
feeding, and a player could in principle route bytes elsewhere. And on *this*
page the tap could not have reported anyway, because the player is a
cross-origin iframe and the tap does not work in one — see the §11.6 section.
The calibration run is what carries the conclusion: it used the mirror as its
own document, and the mirror that does play reports there.

So there is still no second evidence set, and this mirror is not going to
provide one.

**What this does not establish.** One site, one model, one capture, five runs an
arm. The working runs match `/cf-master.`, which is this site's fragment; a
second evidence set is what would tell us whether the *method* transfers or
whether four arms of prompt work have described one CDN very well. That is the
same over-fitting warning the arms above were meant to escape, and it still
applies.

## The second evidence set, at last — and what it broke

A different site, chosen and captured: a drama aggregator whose player is JW
Player served from `kisscloud.online`. It is the second real capture this
project has had, and it earned its keep three times before a single model run
finished.

**The disguise is genuinely different**, which is what makes it worth having:

| | dramafren | the second site |
|---|---|---|
| player | vidstack | JW Player |
| manifest | `/v4/db/<id>/cf-master.<digits>.txt?k=…&kx=…` | `/cdn/hls/<hash>/master.txt` |
| variants | `index-f1-v1-a1.txt` | `/m3/<base64 blob>` |
| segments | `seg-N-…woff2`, dressed as web fonts | not reached before the capture ended |
| tokens | rotating query string | none — in the path |

Both hide a manifest behind `.txt` and **nothing else is shared**. In
particular `/cf-master.`, the fragment both winning dramafren runs matched, does
not occur here at all. The over-fitting this file kept warning about was real
and is now demonstrated rather than suspected.

**1. It contradicts a finding, and vindicates a decision.** dramafren's
Content-Type was *honest* — the disguise was in the url only — and this file
recorded that. This site's is not: the manifest is served as `text/plain`, and
only `#EXTM3U` in the opening bytes identifies it. The rule that the body
outranks the header was written "not because this site lies, but because the
failure mode of believing a header is silent". One site later, a site lies. A
decision made on principle rather than on evidence turns out to have been the
right one, which is the strongest argument for making them that way.

It also does **not** require the page's context: the manifest is 200 naked,
where dramafren's is 403. So neither is the general case.

**2. It broke the probe ranking completely.** The rule this file records — *the
host that served a flood of near-identical requests is the media host* — was
derived from one capture, and on this one it is simply false. The ad networks
flood: a dozen near-identical long-token requests across three rotating hosts.
The media host served fourteen requests of fourteen *different* shapes, so it
never looked like a flood. Measured: **all ten questions went to beacons, fonts
and a favicon, and the host serving the video was never asked about at all.**
The payload therefore carried no content types, which is exactly the blind
condition that produced 0 of 5 on dramafren before annotations existed.

**3. And the model scored 0 of 5, as that predicts.** Three runs returned the
page's own address, one found nothing, and one picked an *ad network* url — which
the content-type tier caught at accept time, the second time that tier has
refused something no static rule would have. The runs measure the blindness, not
the model.

### What was fixed, and what was left alone

**The budget is dealt round-robin across hosts** rather than spent down one
ranked list. Every host's best candidate before any host's second, so a host
that is wrong costs one question instead of all ten. Verified against *both*
captures, which is the only way a change like this can be trusted: the second
site now reaches its media host, and the first still gets both of its manifests
— `cf-master` actually moves up the list.

Also dropped: addresses that cannot be fetched at all. Two of dramafren's ten
questions were going to `wss://` urls, which can never answer what they serve.

**And then a third capture made the rest answerable.** Round-robin reached the
media host and asked it the wrong question — the embed page rather than
`master.txt` — and the two signals available for fixing that each helped one
capture and hurt the other. Rather than tune until two passed, which is how the
previous rule came to be wrong, a third site was captured: kisskh, an Angular
app whose stream comes from its own API.

**Site three is the undisguised control**, and valuable for exactly that: an
honest `…_index.m3u8` on one host and honest `.ts` segments flooding from
another. Any extractor that cannot manage that one is broken. It also breaks the
flood rule a third way — here the *manifest* host served a single request while
the *segment* host flooded, and they are different hosts.

With three, a signal that is not one site's vocabulary becomes visible: the
manifests are `cf-master.…txt`, `master.txt` and `…_index.m3u8`. The words are
HLS's own — master playlist, index playlist, `.m3u8` — so within a host the
budget now asks about whatever reads most like a playlist first, in two tiers
(`master`/`m3u8`/`playlist`/`manifest`/`mpd` above `index`/`hls`/`m3`/`stream`).
The tiers matter: on site two `index.php?…do=getVideo` is an API and
`master.txt` is the manifest, and one question is all that host gets.

**This is a lexical guess, and where it sits is the point.** It decides which
addresses are worth a 2 KB question — not what the model may answer, and not
what the gate will accept. Being wrong costs one request. That is a very
different risk from putting the same guess in the extractor itself.

Verified on all three captures rather than tuned until two passed: **each site's
manifest is now the first thing asked about on its host.** Two of the three
answer HLS.

**The third did not, and it was our apparatus rather than the site — now
fixed.** On a fresh capture the probe reached `master.txt` and was told
`text/html`, opening bytes not a stream, while `curl` fetching the same url at
the same moment got `text/plain` and `#EXTM3U`. It got it with the page's
referer, with the player's own, with none, with a Chrome user-agent, with none,
and with the same 2 KB `Range`: six variants right, and the one wrong was ours.

Reasoning about which header it might be had failed twice, so the two requests
were put on the wire and read. **We were sending no `Accept` header at all**,
where curl sends `Accept: */*`. That CDN treats a request without one as a bot
and answers with an HTML interstitial — 200, `text/html`, no `#EXTM3U`, which
looks exactly like a manifest that has expired.

```
ours:  GET … range: … referer: … accept-encoding: zstd, br, gzip, deflate
curl:  GET … Range: … Accept: */*
```

Both `stream_probe` and the §11.5.1 fetcher send `Accept: */*` now, before any
caller-supplied headers so an extractor that sets its own still wins. The
manifest answers **HLS** on that site immediately afterwards.

The lesson is the one this file keeps relearning in new clothes. The probe was
not misreporting what it received; it was *receiving something different*, and
the cause was not in any of the four inputs anyone thinks to check — it was in
what we failed to send. Two rounds of hypothesis cost more than one round of
looking would have.

## First-load flicker (fixed)

The whole browser window vanished for about a third of a second on the first
tab open, showing the desktop through. Diagnosed by recording the screen at
30 fps rather than reasoning about it, which is what made it findable: the
content pane went grey → **wallpaper for ~12 frames** → white → page, and a
full-screen capture showed no Hydra window anywhere during those frames.

It was a genuine window recreation, not a paint artefact — polling
`xwininfo` across the transition showed the X window id change
(`0x8800029` → `0x8800038`) with a gap of nothing in between.

Two plausible fixes failed before the right one. A warm `QWebEngineView`
created in the factory did nothing, because the window being rebuilt was
`main_window`'s, not that view's. Creating a warm view *inside* the window
before `show()` also did nothing. Instrumenting `open_node` step by step found
why: the surface is destroyed and recreated inside **`view->load()`**, not on
construction — the engine instantiates its render widget on the first load,
and that is what forces the rebuild. A warm view that never loads never
triggers it.

So the constructor now builds a view, loads `about:blank` into it, forces the
native window, and drops it again — paying the rebuild before anything is on
screen. After: grey → white → page, with no dark frame at any point. It also
moves Chromium's start-up (~165 ms of blocked UI, measured) off the first tab
open.

## Sidebar filter controls (done)

Search and Sort now sit above the tree rather than in the page toolbar. They
filter the *sidebar*, and a control placed next to something it does not
control is a small lie about the layout — it costs a beat every time to work
out which pane the search box searches. Search takes the full sidebar width,
Sort sits under it, and the toolbar is left with the things that act on the
page.

## Android: it builds, and there is an APK

Not a port — a **placeholder behind the seam**, which is a smaller thing that
proves a larger one. The seam has claimed since step 3.5 that adding a platform
is "one new backend pair plus a different two lines in `main()`". That claim is
now measured.

**The core is genuinely platform-neutral.** Fifty-one translation units compiled
for `arm64-v8a` with **no errors** and no changes. The only link failure was the
three `qtwebengine_factory` symbols `main()` names — exactly the file the design
says should be the only one that knows.

`android_view` is honest about what it is: it renders a message saying the web
view is not written yet and shows the address it was asked to open. A stub that
displayed a blank page would be indistinguishable from a real backend that is
broken, and this project has lost enough time to things that look like they
work. `set_script_bridge` does nothing on purpose — on Android that becomes
`addJavascriptInterface`, and pretending to register a bridge that cannot
deliver would make every script that waits for one hang rather than fail.

### What it took, since none of it was obvious

```sh
~/Qt/6.11.1/android_arm64_v8a/bin/qt-cmake -S . -B build-android \
    -DQT_HOST_PATH=$HOME/Qt/6.11.1/gcc_64 \
    -DANDROID_NDK_ROOT=$HOME/android-ndk-r29 \
    -DANDROID_SDK_ROOT=$HOME/Android/Sdk
JAVA_HOME=$HOME/android-studio/jbr cmake --build build-android --target apk
```

- **`qt_add_executable`, not `add_executable`.** On Android an app is a *shared
  library* the Java launcher loads (`libhydra_arm64-v8a.so`); with a plain
  executable target `androiddeployqt` has nothing to package. The failure mode is
  the memorable part: the `apk` target reported **success and produced no APK**,
  because there was nothing to put in one.
- **`JAVA_HOME` must point at a JDK.** The system `java-21-openjdk` here is a
  runtime with no `javac`, and Gradle says so in the language of toolchain
  capabilities rather than in English. Android Studio's bundled `jbr` has one.
- **Optional dependencies must not be asked of the host.** The first configure
  that got this far announced *libsodium found* and *libtorrent found* — for an
  arm64 target, from `/usr/lib/x86_64-linux-gnu`. It would have failed at link
  with something that reads like a toolchain fault. They are now skipped for
  Android, and the guard has to be on the **query**: Qt's own toolchain files
  find pkg-config first, so guarding the `find_package` changed nothing.
- Still warned, harmlessly so far: `Android platform 'android-37' does not exist
  in SDK`. The SDK here has `platforms/android-37.0`, with the dot, which is not
  the name Gradle looks for. The APK builds anyway; worth remembering when
  something stranger happens.

**Desktop is untouched** — same executable, same tests, and that was checked
after every one of these changes rather than at the end.

### And it runs on a phone

Installed on a running x86_64 emulator (API 30) and launched. **The whole shell
comes up**: menu bar, toolbar with back/forward/reload, address bar, the Shield
button, search and sort, the tree, and the status bar reading `Ready` and
`0 / 4 live`. Displayed in 927 ms, and `logcat` has no Qt warning or error in
it — a clean start, not a survived one.

The x86_64 kit was needed for that: the APK built first was arm64, and a desktop
emulator is x86_64, so it would not have installed. Both kits build from the
same tree with the same flags.

**Four things the screenshot says, and three of them are work:**

- ~~The tree is empty~~ **— fixed.** `main()` loaded `./sample-tree.txt`
  relative to the working directory, and on Android that is `/`, where nothing
  exists and nothing is writable. It now uses `AppDataLocation`, and because
  everything this program keeps lives *beside* the tree file — `policy.json`,
  `state/`, the filter list, the site rules — moving the tree moves the whole
  set at once. First run seeds it from a copy of `sample-tree.txt` compiled into
  the binary, so the app opens with something in it rather than an empty pane
  that reads as a failure. Verified on the device: the tree renders with its
  bold/italic/muted state cues, and `files/tree.txt` and `files/state/` exist in
  app storage afterwards. User-chosen files still want SAF (§19.4).
- ~~The layout is desktop-shaped~~ **— fixed, and it is §19.3's drawer.** A
  horizontal splitter on a portrait phone left the page a strip too narrow to
  read. Below 620 logical pixels the sidebar leaves the splitter, becomes an
  overlay, and slides in and out over the content; above it, the splitter is
  exactly what it always was. **Driven by the window's width, not by the
  platform** — a desktop window dragged narrow has the same problem, and a rule
  that said "Android" would have been a rule about the wrong thing.

  Verified on the device: the page now gets the whole screen, the toolbar grows
  a toggle that only exists in drawer mode, tapping it slides the tree in over
  the content, and **choosing a tab closes it again** — it was opened to pick
  something, so it gets out of the way of what was picked.
- ~~A toolbar glyph is missing~~ **— fixed.** Reload was the character `↻` and
  the emulator's font had no glyph, so it drew an empty box while back and
  forward happened to survive. All three now take their icon from the style
  (`SP_ArrowBack`, `SP_ArrowForward`, `SP_BrowserReload`), which is both better
  looking and not a bet on what fonts a platform ships. Verified on the device.

### The System WebView: it renders

`HydraWebView.java` creates a real `android.webkit.WebView`, adds it to the
Activity, and takes geometry, load, back/forward/reload and visibility from C++
over JNI; `android_view` syncs it to the widget's rect on every move, resize,
show and hide, in device pixels (Qt reports logical ones and Android's layout
wants physical — a factor of three on a phone). A JNI callback carries the
page's url back onto the Qt thread. **A page renders, in the page area, under
the toolbar.**

**Getting there took three wrong guesses and one act of reading**, and the
reading is the part worth keeping.

1. The renderer process died on the first attempt. Isolated rather than assumed:
   the stock *WebView Browser Tester* loaded the same page on the same emulator
   without complaint, so the fault was ours. A **software layer** fixed it — a
   hardware-accelerated WebView composited over Qt's own GL surface is what it
   objected to.
2. Then a blank rectangle. Guessed at Z-order twice (`bringToFront`, `setZ`) and
   changed nothing.
3. So: read Qt's Android sources instead. `QtSurface extends SurfaceView` with
   `setZOrderMediaOverlay`, and `QtActivityDelegate.insertNativeView` adds
   foreign views to `QtLayout` — which suggested the parent was wrong. **But the
   same reading killed the Z-order theory outright:** the placeholder text was
   *hidden* behind the blank area, so our view was already compositing above
   Qt's surface. Z-order had never been the problem.

   The bug was ordering. `create()` posts to the UI thread; `load()` looked the
   view up on the *calling* thread, found nothing because create had not run
   yet, and returned silently. Every call now goes through the same
   `runOnUiThread` queue, which is FIFO, so a load posted after a create runs
   after it.

A silent early return that leaves a white rectangle looks exactly like a
rendering failure, and it sent two rounds of investigation at compositing. The
thing that broke the deadlock was reading the sources rather than trying a
fourth variation.

**It is on by default**, because plain HTTP works too — and getting there
involved being wrong twice about the same attribute.

Android refuses cleartext by default, which is right for an app talking to its
own backend and wrong for one whose whole job is loading addresses a user typed;
without `android:usesCleartextTraffic="true"` every `http://` page fails with
`ERR_CLEARTEXT_NOT_PERMITTED`. I added it, dumped the APK's manifest, saw it was
absent, and concluded androiddeployqt had dropped it during the manifest merge.
**Both halves of that were wrong.** The APK I dumped was built *ninety seconds
before* I edited the manifest — a stale artifact. And once I rebuilt, the build
failed outright: `Expected '>', but got ' '`, because the comment I had written
above the attribute contained a `--`, which XML does not allow inside comments.
The attribute had never reached a build at all. With the comment fixed, it is in
the APK and `http://` pages load.

Two lessons, and the second is the one that generalises. Checking an artifact
proves nothing unless it is newer than the change — `stat` on both files would
have cost seconds and saved a wrong entry in this document. And a *silent*
absence had a loud explanation waiting one build away: the error message named
the file, the expected character and the actual one, and I never saw it because I
inspected output instead of running the thing that produces it.

Verified end to end on a phone-shaped emulator, against a plain HTTP server on
the host: the page renders, a link navigates, Back returns, and the address bar
and status bar both follow the WebView through the JNI url callback. The
placeholder is still one env var away — `HYDRA_ANDROID_WEBVIEW=0` — which is
worth keeping for bisecting a WebView bug against the rest of the shell.

### The filter reaches Android

`shouldInterceptRequest` now asks the same `request_filter` the desktop asks, so
ad hosts, per-origin script rules and per-site image rules apply on a phone with
no Android-specific policy code. Two things do not carry over, and both are
limits of the hook rather than choices:

* **Referer cannot be stripped.** The hook may replace a response but not edit
  an outgoing request; honouring it would mean re-issuing every request from
  Java. `request_decision` is flags rather than an action precisely so a backend
  can honour the parts it supports, and this was the case that shape was for.
* **The resource type is not reported.** Qt WebEngine states it outright;
  `WebResourceRequest` gives headers and a url. So `kind_from_hints()` infers it
  — shared and tested rather than buried in a platform file, because a wrong
  guess quietly turns a per-origin script rule into no rule at all.

That inference is **deliberately cautious**. A script request sends `Accept:
*/*`, but so does every `fetch()` and XHR, so `*/*` alone is not taken as
evidence: only an explicit javascript media type or a `.js`/`.mjs` path counts.
Guessing low means some scripts load on a site whose scripts are blocked;
guessing high would block a page's data requests under a rule the user set for
scripts, which reads as the site being broken. The suite pins both directions,
including the cases it declines to guess.

**Measured as a controlled pair**, because the obvious test proves nothing. A
first run showed the image simply absent from the server log — but the document
had come back `304`, so a cache hit would have looked identical. The real test
used two image urls that had never been requested before, one per run, with only
the policy file differing: with images blocked for the site, the document was
fetched and the image never was; with the rule removed, both were. Same page,
same server, distinct uncacheable urls.

The main document is deliberately exempt: a WebView that blocks its own page
shows an empty frame with no way back, and these rules are about what a page
loads, not which pages may be visited.

### The content scripts run, and can call back

There is no QWebChannel on Android, so `addJavascriptInterface` carries a
two-method native object and a shim rebuilds the same `window.hydraChannel(cb)`
the desktop scripts are written against. **They run unmodified** — autofill, the
picker, consent and the MSE relay all registered and all reachable from a page.

The marshalling is `bridge_invoker`, and it is shared and tested rather than
living in the Android file, because this is the one place a page names what runs
inside the shell. What it refuses is the interesting half:

* **`deleteLater()` is not callable.** `QObject` publishes it as a slot, and a
  page that could call it would delete the shell's bridge out from under the
  browser. Methods below `QObject::staticMetaObject.methodCount()` are excluded
  wholesale; QWebChannel draws the same line in the same place.
* **Signals are not invokable**, so a page cannot forge a report the shell
  believes.
* **Arguments are type-checked, not coerced.** A string where an int belongs is
  refused and the call does not happen. Overloads are matched on name *and*
  arity, because picking the first name match calls whichever one moc emitted
  first.

Verified on the device, not just in the suite: a page asked for the bridge list
and got all four, then called `rules_json()` and received 392 characters of the
shell's real rules, then `active_now()` and got `false` — the correct answer for
that host.

**Two platform limits, stated rather than papered over.** Android has one
javascript world, so an injected script shares globals with the page and a
hostile page can read or replace it; the bridges were already written on the
assumption that every argument is hostile, and here that assumption is
load-bearing. And injection happens at `onPageStarted`, which is early but not
before a page's own inline script — `addDocumentStartJavaScript` from
androidx.webkit is the real answer and is a dependency decision, not a line of
code. `inject_script`'s `subframes` flag is likewise not honoured, so the consent
script sees less here than on the desktop, where CMPs ship as iframes.

One Qt detail worth keeping, and I got its cause wrong at first. There are two
`QMetaMethod::invoke` overload families — a templated one taking real values and
an older one taking `QGenericArgument` — and **they cannot be mixed**. Passing
`QGenericArgument` parameters with a `Q_RETURN_ARG` return fails with *"cannot
convert formal parameter 0"*, which reads like an argument bug and is really
overload resolution picking the templated form. I first wrote this down as a
change in a newer Qt; it is not. Both families are present in the 6.8.2 this
builds against, and the rule is that one call uses one of them.

The version that *does* matter is Qt 7: the `QGenericArgument` family is declared
under `QT_VERSION <= QT_VERSION_CHECK(7, 0, 0)`, so it goes away, and this
function will have to be rewritten against the templated one — which takes values
rather than type-erased pointers, so it means dispatching on arity and type
instead of looping. Better to know now than as a build error.

### Links that are not pages

`shouldOverrideUrlLoading` is asked about every navigation and takes silence as
consent, so a WebView told nothing will try to load `magnet:` itself and show its
own error. It now asks the shell, and the shell answers with the same rule the
desktop uses — `renders_as_page()`, which moved out of `main_window`'s anonymous
namespace into `scheme_rules` so both backends read one list. A scheme on one
list and not the other is a link that works on the desktop and dead-ends on a
phone, and that is not a difference anybody would go looking for.

Measured on the device from both ends. A page navigated itself to a `magnet:`
url and then reported where it was: still on its own page, so the WebView never
attempted it and never showed an error. And the shell's status bar read *"Nothing
here can open magnet:"* — its own message, which means the url travelled the
whole way through JNI to the download manager, which has no torrent source in the
Android build and said so.

That completes §19.5's list except the file picker: requests, scripts, bridges
and external links all cross the seam now, and the Android backend is a set of
platform hooks onto shared code rather than a port of the shell.

### The file picker, which is Qt's

A page's `<input type=file>` opens the system document picker, and the file
comes back as a `content:` url the WebView can read — no storage permission
asked for and none needed, which is the whole point of the Storage Access
Framework.

**The interesting part is how little of it is ours.** `WebChromeClient`'s
`onShowFileChooser` calls into C++, which shows Qt's own `QFileDialog` — and on
Android that *is* the document picker, through
`qandroidplatformfiledialoghelper`. So the activity-result plumbing, the intent,
the permission grant and the content-url handling are all Qt's, already written
and already tested, and the Android file is the two hops on either side of them.
The alternative was registering an activity-result listener against a private
Qt header to reimplement what that helper does.

One threading decision: the call from Java is **posted, not waited on**. Every
other JNI entry point here blocks on the Qt thread for an answer, but the picker
Qt is about to show needs Android's UI thread — the same thread the chooser
callback arrives on — so blocking would deadlock the two against each other.

Measured on the device: a page's file input opened the picker, a 26-byte file was
chosen, and the page reported `count=1, name=hydra-upload.txt, size=26` — the
right name and the exact byte count, so the WebView really read the url.

**The cancel path is tested separately, because it is the one that matters
later.** A WebView holds exactly one chooser callback and will not open another
until it is answered, so dropping one does not fail a single upload — it disables
every file input on every later page, silently. The picker was opened, dismissed
with Back, and opened again; the second time proves the cancel was answered.

### Building against Qt 6.11, which found a real break

The list said two cheap things came before the Android work: build the desktop
against 6.11's `gcc_64` kit, and re-run `try_permissions` there. Both are done,
and the second one earned its place.

**It compiles against 6.11 with no errors** — only deprecations, and those are
concentrated in one place: the whole `QWebEnginePage` permission API, plus
`invalidateFilter` in the sort proxy.

**Then `try_permissions` went from 11 of 11 to 7 of 11**, and the interesting
part is that it was three different things wearing one costume.

1. **A genuine break.** `featurePermissionRequested` is documented as deprecated
   but functional. On 6.11 it is not functional for geolocation: the signal never
   arrives, nothing answers, and the page's `getCurrentPosition` callback never
   fires. That is worse than a refusal — a refused page moves on; a page waiting
   on a callback that will never come just stops. Migrating to
   `QWebEnginePermission` fixes it, behind `QT_VERSION >= 6.8` so the declared
   floor stays honest.
2. **A test reading a renumbered enum.** The debug line printed Qt's raw feature
   number and the driver matched on it. `QWebEnginePermission` orders its enum
   differently, so every number in the driver silently meant a *different*
   permission. The line now prints our own feature name, which does not renumber
   when an engine changes its mind, and the driver matches on that.
3. **A word that changed, not a decision.** Chromium says `NotAllowedError` for
   a denied device and `AbortError` when it cannot open one, and which of the two
   `getUserMedia` reports for a refused camera differs between the Chromium in
   6.8.2 and the one in 6.11 — same machine, same run, same decision, different
   word. Both are refusals; the check now accepts either and prints which it saw.

Only the first was a bug in Hydra. The other two were the *test* being specific
about things that were never the point, and both would have been read as
"upgrading Qt broke permissions".

**And the open question is answered.** The geolocation result was neither Qt's
behaviour nor the packaging's: this machine has no location provider at all —
`org.freedesktop.GeoClue2 was not provided by any .service files`. That is why
a granted geolocation still fails, on both versions, and it is a fact about the
machine rather than anything to fix in the browser.

Both suites pass on both Qts now, and the printed refusal word differs between
them in the output, which is the shape a version-dependent detail should have:
visible, named, and not asserted on.

**Then the same comparison, run across every self-contained live driver**, since
one version difference found by accident says nothing about the rest.
`try_cookies` (12), `try_consent` (36) and `try_adblock_fix` (7) matched exactly
on both. `try_subframe` did not: 6 of 6 on 6.8.2, 2 of 6 on 6.11.

It was **the fixture, not the tap**. The MSE fixture asked for
`video/mp4; codecs="avc1.64001E"`, and Qt's own binaries ship without
proprietary codecs, so `addSourceBuffer` threw `NotSupportedError`, nothing was
ever appended, and four checks failed as though the subframe relay were broken.
The driver's own comment already said the appended bytes are not decodable as
anything — the tap counts the handover, not the decode — so the type never
needed to be H.264. It now picks the first type the engine says it supports,
preferring the royalty-free ones, and prints which: VP8, on both. A test of the
tap had quietly become a test of the engine's codec licensing.

One detail worth keeping: the fixture reports with `console.warn`, not
`console.log`. Qt's `js` logging category prints warnings and errors and drops
info, so a `console.log` would have been invisible in exactly the run where the
message matters — the one where no type is supported at all.

`try_downloads` and `try_capture` are excluded from this comparison on purpose:
they assert nothing, so they cannot disagree.

### The filter list was never enforced

Setting out to test the ad-host predicate turned up something larger: **every
rule the filter-evolution loop ever produced did nothing.**

`filter_list::blocks()` existed, was correct, and had no caller in the request
path. Rules were proposed by the AI, put through the breadth check and the
dry-run, accepted by the user, written to `filters-ai.txt`, reloaded at startup
and listed in the settings dialog — and then no request was ever compared against
them. The architecture assigns filter enforcement to spine 1, the interceptor
(§12, table row "Filter evolution … Spines 1+3"), so this was a missing wire, not
a deferral. Every part of the loop worked except the one that mattered, which is
why nothing looked wrong.

`request_filter` now takes the list and consults it, gated behind the same
per-site `ads` setting as the seed hosts: turning ads back on for a site the
shield says is broken has to turn *all* of this off, or the escape hatch only
half works and the page still fails for a reason the user was told they had
disabled.

**And the thing that looked like the next obvious fix was a trap.**
`filter_rule::scope` is documented as "domain for a site-specific rule" and
`blocks()` ignored it, so honouring it looks like a bug fix. It is not:
`parse_rule` fills that field with two different things — the site for a cosmetic
rule, and *the host being blocked* for `||host^` — and `evaluate()`'s breadth
check depends on the second meaning. Comparing a blocked host against the
visiting site would have matched almost nothing and silently disabled every
network rule. A whole feature turned off by a change that reads as a repair. The
suite now pins both meanings so it is not attempted twice.

What that ambiguity *had* broken is smaller and real: the settings dialog's
"Applies to" column printed `scope` directly, so a global tracker rule was
listed as though it only applied on the tracker's own domain. Network rules are
global here — per-site ones want `$domain=`, which this parser does not read —
and the column says so now.

**Proved end to end, and it finally unblocks the ad-host predicate**, which the
notes have carried as "mechanism verified, matching untested" because pointing a
name like `doubleclick.net` at a local server needs `/etc/hosts` or
`--host-resolver-rules`, and Qt mangles the latter by splitting the environment
variable on spaces. The predicate never cared what the name was. `try_filters`
puts the page on `127.0.0.1` and its beacon on `127.0.0.2` — two hosts, both
loopback, no DNS — and runs the controlled pair: with `||127.0.0.2^` accepted the
beacon never reaches the server; with ads allowed for the site it does; blocked
again, it stops; with the rule removed, it arrives. Fresh url each time, because
a cached image goes unrequested for reasons that have nothing to do with
filtering and looks identical in the log.

### The cosmetic half, and the bug it uncovered

`##` rules hide elements rather than blocking requests, so they cannot ride the
interceptor — they have to reach the page. `cosmetic_filters` is that path: the
shell says which host is on screen, an injected script asks for that host's
selectors, and writes them into one stylesheet. `display: none !important`
rather than removing nodes, because a removed node changes the page's own DOM in
ways its scripts notice, and a stylesheet also applies to elements that do not
exist yet — which is most of them, since this runs before the page's content.

The page never names the host, the same way the consent bridge works: otherwise
any site could ask what rules exist for any other, which leaks what the user has
been doing.

**It worked on the desktop and did nothing on Android**, and finding out why was
worth more than the feature. The bridge was reachable there — `hydraCosmetic`
appeared in the page's bridge list with no Android-specific work, which is what
the shared `bridge_invoker` was for — but it answered `[]`. Rather than reason
about it, I put the two facts on the wire behind `HYDRA_FILTER_DEBUG`: the rule
file **was** read, one rule; `set_page_host` was **never called**.

The cause was in the shell, not in either backend:

* **Switching tabs never updated the page context at all.** It was set in exactly
  one place — the current view's `url_changed` — and activating an already-loaded
  tab navigates nothing, so the consent blocker went on answering `active_now()`
  and `rules_json()` for the *previous* tab's site. A bridge built so the page
  cannot name its own host is not much use if the shell then names the wrong one.
  That was a desktop bug too, and had been one for as long as the bridge existed.
* **The ordering only ever worked by accident.** `activate_node()` calls
  `view->load()` before `setCurrentWidget()`. Qt WebEngine emits `urlChanged`
  asynchronously, so the signal landed *after* the switch and the
  `view == current_view()` test passed. The Android backend emits it from inside
  `load()`, synchronously, so it arrived while the previous view was still
  current, the test failed, and nothing was ever set.

`sync_page_context()` now reads whatever is current, after the switch, and is
called from both places. It cannot depend on which way a backend emits, because
it does not listen for an emission.

Verified on both: on the desktop the advert computes to `display: none` in the
page's own view while the element beside it does not, and on the phone the same
page comes up with the red block gone and the green one intact —
`selectors=[".ad-banner"]`, `style_el=present`, `ad=none`, `keep=block`, over the
same bridge, with no Android code written for it.

### The KeePassXC bridge finally met KeePassXC

`keepassxc` is installed now, so the last of "wired but never run" could run —
and this project's defect history says that category is where the defects are.

Set up so it disturbs nothing: its own config file, its own database, one entry,
browser integration on. The socket appears at exactly the path `socket_path()`
computes, `start()`'s change-public-keys exchange **completes**, the connection
stays up after it, and a saved-but-unknown pairing is refused with KeePassXC's
own answer rather than a guess — *"association failed, try again (code 8)"*. That
is the case on every first run after settings are copied to a new machine, and
"no" is the right answer for it.

So the transport, the framing and the sodium key exchange work against the real
other end, first try. Seven checks, none of them previously exercised by
anything.

**And the driver's own precondition was lying.** It reported "KeePassXC is
listening where the bridge expects it" after checking `QFile::exists` on the
socket path — which is a symlink into the runtime directory that **outlives the
process**. When KeePassXC exited, the driver announced a listening server and
then failed the handshake: the one check whose job was to establish the
precondition was the one making it up. It connects now, and distinguishes "no
socket at all" from "a stale socket left by a KeePassXC that has exited". A test
that reports a passing precondition it never tested is worse than having none.

**Pairing is not automated, deliberately.** `associate()` makes KeePassXC ask a
human whether this program may read the vault — that prompt *is* the security
boundary, and a browser able to answer it for itself would be the bug. The driver
skips it unless `HYDRA_KEEPASS_INTERACTIVE=1` says a person is watching, and
prints which checks went unrun rather than passing quietly without them. The
remaining ones are behind that flag: association, a login request for a url the
vault knows, and one for a url it does not.

### Handing a stream to a player, on a phone

The desktop names a player and starts a process. Android has neither, so §19's
answer is an intent: `ACTION_VIEW` with the url and a media type, and whichever
app the user has takes it. `player_launcher` grows one entry there — "System
player", always present, always cautious about manifests, because which app
answers is the system's choice and not knowable from here.

The media type matters more than it looks. With none, the chooser offers every
app that claims `http`, which on most devices means a browser — and handing the
stream to a browser is a loop back to where it came from. `media_mime_for()` is
shared and tested for that reason, and unrecognised urls get `video/*` rather
than a guess at the container.

**Driven on the device, and it turned up two bugs that had nothing to do with
intents.**

**Dialogs were invisible.** Tapping "Media (1)" depressed the button and showed
nothing at all. The header had always said the native WebView sits above
everything Qt draws; what it had not said is that this makes Qt's own dialogs
unreachable, which is not a caveat but a bug. Qt announces the condition — a
window covered by a modal dialog is sent `WindowBlocked`, and `WindowUnblocked`
when it closes — so the view hides for exactly that span rather than guessing.

**And then the dialog did not fit.** It came up wider than the screen, list
visible and buttons off the right edge, with no way to scroll a dialog to reach
them. Every dialog here was laid out for a desktop where the screen is wider than
the contents ask for. On Android they now fill the available screen, applied by
one application-wide event filter rather than thirty constructors.

With both fixed: the media dialog shows `clip.mp4`, **Watch** hands it over, and
`com.android.gallery3d/.app.MovieActivity` comes to the front with our url. It
then fails to play it — `MediaPlayerNative error (1, -2147483648)` and no request
ever reaching the server, which is that app's own cleartext policy rather than
ours. The handoff is the part under test and the part that works; what the
receiving app then does with a url is exactly what handing it over means.

### A resumed download against a server that ignores Range

Downloads work on Android — 195.3 KiB of a 195.3 KiB file, app-private storage,
no permission asked for. Downloading the *same* file a second time reported
**390.6 KiB**, which is exactly twice, and that is a bug in shared code that had
nothing to do with phones.

`http_download_source` sees a file already on disk, asks for `Range: bytes=N-`,
opens the file in `Append`, and writes whatever comes back. **Range is a request,
not a command.** A server without range support answers `200` with the whole
body, which is correct HTTP — and the complete body then lands after the bytes
already there, producing a file of twice the right length with stale bytes at the
front, reported as done at 100%.

Only `206` means "the rest of it". Anything else means "all of it", so the resume
is now abandoned: seek to zero, truncate, start again. Checked once per transfer
from whichever of `metaDataChanged`, `readyRead` or `finished` gets there first,
because a small reply can arrive complete before anything has looked at its
headers.

**The test that should have caught this existed and could not.** There is a
resume check in `test_seam` — "the file ends up the right size, not appended
twice" — but it runs against a helper that always answers `206`, and only when a
server URL is passed on the command line, which no ordinary run does. So the
covering test was both blind to this case and usually skipped. The new one stands
up its own server, in-process and unconditional, that ignores `Range` on purpose.
It fails without the fix — 52345 bytes where 40000 was wanted, which is precisely
the 12345 already on disk plus the full body — and it checks the first bytes as
well as the count, since appending would leave the stale ones at the front and
still be the wrong file at any length.

Found on a phone against python's `http.server`. It should not have needed a
phone, and now it does not.

### Downloads that can be found afterwards

Qt's download location on Android is app-private external storage. Writing there
needs no permission and always works, which is why the download stack worked on a
phone the day it was built — and it is also **invisible**: no file manager lists
it, no other app can open it, and it is deleted when Hydra is uninstalled. A
browser whose downloads cannot be found afterwards has not really downloaded
anything.

A completed file is now copied into `MediaStore.Downloads`, the shared collection
every file manager shows. That needs no permission either — an app may always
insert its own entries. **Chosen between two honest options**: the other is
asking with the Storage Access Framework where each file should go, which is what
"save as" is for and not what a browser should do to every download. The copy is
a copy, not a move: it costs the space twice until the app's data is cleared, and
it means a failed publish leaves a download that still exists rather than one
that succeeded and then vanished.

Measured: `content query --uri content://media/external/downloads` lists
`clip.mp4, _size=200000` — the exact byte count, which also confirms the Range
fix above, since the same file downloaded twice before produced 400000. A second
download became `clip (1).mp4` rather than overwriting, which is MediaStore's own
naming and the behaviour a browser should have.

The media dialog said "Queued download to
/storage/emulated/0/Android/data/org.qtproject.example.hydra/files/Download" —
a path that is long, unopenable, and no longer where the file ends up. On Android
it now says "Queued. It will appear in Downloads when it finishes."

**A flake, recorded rather than explained.** `test_extloop` has now failed four
times — 28, 21, 27 and 27 of 34 — and passed more than forty times in between,
so it is real and it is rare. **Every failure has happened inside a run of the
whole suite list, and not one has ever reproduced on demand.** Eliminated by
trying: running it alone (twelve consecutive passes), running it with four cores
deliberately busy, running it immediately after `test_settings` and after
`test_extractor`, replaying the exact sequence that failed, three further
complete passes of the whole list, four more passes saving every run's output in
case one broke, and — the one hypothesis that looked promising, since three of
the four failures followed a build in the same command — running it three times
immediately after forcing a relink, in case a cold binary was the difference. It
was not.

Its assertions include wall-clock ones — "the timer fired *while* the script was
running, 120 ms against 419 ms" — which makes scheduling the leading suspect, but
that is a hypothesis and this file does not record hypotheses as findings. What
is worth knowing is that **the suite already prints the numbers it compares**, so
whenever it next fails the output will say which deadline was missed and by how
much. Chasing it further has been stopped deliberately rather than forgotten:
three attempts to reproduce have cost more than the failure does, and the next
occurrence inside a full run will be more informative than another hour of
guessing at it now.

### Autofill on Android is the platform's, and the menu now says so

`keepass_bridge::supported()` checked for libsodium and nothing else, so on
Android the Tools menu offered "Connect to KeePassXC…" — an action that cannot
work there and whose failure message told the user to start a program that does
not exist on a phone. KeePassXC's browser integration is a Unix socket belonging
to a desktop application; there is nothing to connect to.

`supported()` now means what it says, and `unavailable_reason()` carries *which*
of the two reasons applies, because "no libsodium" is a thing to fix and "wrong
platform" is not. On Android it says the system autofill service fills forms
instead — which is true, needs nothing from this browser, and is where someone
looking for autofill on a phone should be pointed.

That is §19's autofill item, and the answer is that most of it is not ours to
implement. **It is not verified end to end**: the emulator has no autofill
service configured (`settings get secure autofill_service` is empty), so what can
be said is that the menu no longer offers something impossible, not that filling
works. Naming that gap is the point of writing it down.

### A parser that said it was tested

`hls_playlist.h` describes itself: *"the parsing is where the fiddly cases live
(relative URIs, byte ranges, VOD versus live), so it is separated out and tested
on its own."* It was separated out. Nothing tested it — and the claim sitting
there unaccompanied is what made it worth looking at, because this is a parser
for text that arrives from a CDN, and the quality list, the assembler and the
media dialog all believe whatever it says.

Thirty-five checks later, one real bug, and it is in the fiddliest of the cases
the header names.

**A byte-range playlist is one file cut into slices**, and it states an offset
once: `#EXT-X-BYTERANGE:1000@0`, then `2000`, then `1500`. RFC 8216 §4.3.2.2 says
an omitted offset means *the byte after the previous sub-range*. The parser
reported `-1` for it and the assembler read `-1` as zero — so every segment after
the first would have been fetched from the start of the file. The result is not a
failed download or an empty one: it is a video file of the right length,
assembled out of the same opening slice repeated, which is the worst way to be
wrong because nothing reports it.

Resolved at parse time now, where the previous segment is known, and only when
that previous segment is a range of the same resource — which is what the spec
requires and also what stops a stray range leaking onto an unrelated segment. The
assembler needs no rule of its own; it sees concrete offsets.

The rest of the suite is the cases a manifest can be awkward in: CRLF line
endings, a quoted `CODECS` containing commas, an `#EXTINF` title that also
contains one, relative and absolute URIs, absolute URIs on another host, no base
url at all, and the two tags that carry their URI *inline* — `#EXT-X-MEDIA` and
`#EXT-X-I-FRAME-STREAM-INF` — either of which would invent a variant and steal
the following line if it were mistaken for `#EXT-X-STREAM-INF`. They are not, and
now there is something that would notice if they became so.

### The tree file lost urls, and the title was to blame

The tree file is the source of truth for structure and order, written on every
change and read on every launch. Nothing tested it. The first thing a round-trip
test found is a data-loss bug that needs no unusual input at all.

Fields are separated by `" | "`, and the parser read them left to right: type,
title, url. **A title containing that separator shifted everything after it** —
so `Some Article | The Daily Example` came back as title `Some Article`, url
`The Daily Example`, and the real url landed in a position nothing reads. On the
next save it was written from what had been parsed, so **the address was gone**.

`Article Title | Site Name` is one of the commonest shapes a page title takes on
the web. This was not a corner case; it was most of a news site.

The fix reads the fields **from the right**. The title is the only free-form
field: a url cannot contain an unencoded space and neither can `created=` or
`seen=`, so working inwards from the end is unambiguous where working outwards
from the start is not. No format change, so files already on disk read correctly
— and one written by the old parser is beyond help either way, since the url it
recorded is the one it invented.

The same bug, from the same shortcut, was in `tree_serializer` — the AI
reorganizer's payload. There it went further than a reload: a proposal parsed
that way carries a wrong url into the diff the user is asked to accept, so the
reorganizer could have been *offered* a tree with addresses replaced by fragments
of their own titles.

Twenty-nine checks now, including the ordinary things that did work and should
keep working: nesting and dedenting, order preserved rather than sorted,
`created`/`seen` timestamps, tags after the url, and a proposal arriving wrapped
in prose and a code fence the way a model actually answers.

### Two tabs, one history

`state_store` keeps a suspended tab's navigation history in a file named after
the node id, with unsafe characters replaced by `_`. That mapping is **not
injective**: `a b` and `a_b` both become `a_b`, so two suspended tabs shared one
file. The second to be restored came back wearing the first one's past — a tab
claiming a history that belongs to a different page, which is worse than a tab
that has forgotten.

Reachable because ids come from the tree file, and that file is documented as
human-editable and canonical. Not something the application generates on its own,
which is why it had never been seen; entirely something it would accept.

Fixed by appending a short hash of the original id — **only when sanitising
actually changed it**. That detail is the whole compatibility story: every id the
app generates is already safe, keeps the filename it has on disk, and loses
nothing on upgrade. Without it the fix would have silently orphaned every
suspended tab's history: no error, no warning, just tabs that had forgotten where
they had been. There is a test for that specifically, which writes a blob the way
the old code did and insists it is still found.

An over-long id gets the same treatment, since truncating to fit a filename
collides with every other id sharing its first hundred characters.

The rest of the suite is the ordinary contract nobody had written down: a blob
round-trips byte for byte, a shorter save leaves no tail of the longer one behind,
**binary is binary** — a serialized history is full of zero bytes, and anything
treating it as text ends the history wherever a NUL happens to fall — an empty
blob counts as present rather than absent, an id containing `../` writes inside
the store and not above it, and state outlives the object that wrote it.

### The crypto shim, which turned out to be right

`box_crypto` was the last of the never-tested files, and unlike the three before
it there was nothing wrong with it. That is worth recording as plainly as the
bugs: thirty checks, no fix.

What was tested is not the cipher — libsodium's own suite does that far better —
but the **shim's edges**, which are the part that would be our mistake. A wrong
key, a tampered byte, a truncated ciphertext, a reused-but-wrong nonce, a third
party's key on either side: all refused, and nothing written to the output on the
way out. That last one matters more than it reads. A shim that returns `false`
on failure is the whole requirement; a shim that returns `true` with garbage
would hand the bridge a forged reply and be indistinguishable from success.

Sizes are checked before libsodium is handed a buffer, which is the difference
between a wrong answer and a read past the end of an array — and the bridge
builds its arguments from what arrives on a socket, so "the caller would never"
is not an argument available here.

The suite runs in both builds. Without libsodium the contract is that every call
fails and `available()` says so, which is what makes the password manager report
itself unusable rather than pretend; that path was compiled and run too, not
assumed from reading the `#else`.

**The tally for this sweep**, four files that no test had ever named:
`hls_playlist` assembled video from the same opening slice repeated,
`tree_outline` and `tree_serializer` deleted urls whenever a page title contained
`" | "`, `state_store` handed one tab another tab's history — and `box_crypto`
was correct. Three of the four produced a plausible-looking wrong answer rather
than a crash, which is exactly the failure this project's notes keep warning
about, and the reason the fourth being clean is information rather than a
formality.

**What is not in doubt:** the seam. `shouldInterceptRequest` onto the shared
`request_filter`, `addJavascriptInterface` for the content scripts, and
`shouldOverrideUrlLoading` for `magnet:` are all still to write (§19.5), and
none of them are blocked by this — they are Java-side work that plugs into
C++ that already exists.

## What is next (in order)

1. **Take a second evidence set, from a different site.** Everything measured
   here is dramafren, and the working runs match `/cf-master.` — that site's
   fragment. Until a second site is captured, "the loop finds the stream" means
   "the loop finds this stream". This is also where the fragment-first line for
   rule 2 should be tried, rather than against the fixture it was derived from.

   **The second mirror is exhausted**: it starts once ads are unblocked, then
   stalls, and the tap confirms nothing is being fed to a MediaSource — see the
   section above. A different site is the answer, and the levers to reach one
   are now in `try_extract` (mirror chooser, click count, ads, popups).
2. **Finish the helper tier (arch §11.5.1).** The fetch half is built,
   permissioned and proven against a live CDN: it identified the disguised
   manifest, followed it to a variant the page never requested, and the gate
   accepted that as *followed rather than invented*, in four calls and 320
   bytes. What remains is the DOM half behind §13.2, and deciding whether it is
   wanted at all — nothing has yet needed it.
3. **Decided: when blocking ads breaks the page, fix it and say so.** The open
   question was whether the browser should offer the change or make it, and the
   answer was to make it. On a page whose anti-adblock refuses to start, ads are
   allowed for that site, the page reloads, and the status bar says what happened
   and where to undo it — "so ads are now allowed for this site and it is being
   reloaded — undo it in the shield", for fifteen seconds. Once per host, so a
   site that keeps asking does not keep toggling.

   **A site the user has already ruled on is left alone.** If there is an
   explicit per-site ads setting, the detector says the site is checking and
   that the setting stands, rather than overriding a decision someone made on
   purpose. That is the line between helping and interfering, and it is the part
   worth keeping if the rest is ever revisited.

4. **Exercise the rest of what is wired but untested.** **The ad-host half is
   done** — see the filter-enforcement section above; it needed no DNS trick at
   all, only two loopback hosts, and finding that out uncovered that the whole
   filter list was never consulted. The KeePassXC bridge has now met a real
   KeePassXC — see the section above — and everything short of the pairing
   dialog passes. **What is left needs a person**, not a machine: run
   `HYDRA_KEEPASS_INTERACTIVE=1 ./tests/build/try_keepass` and accept the prompt
   to check association and login lookup.
   This project's defect history is almost entirely in this category — see the
   cautions at the top of this file. **The cosmetic half of §12 is done too** — see the section
   above — and it uncovered a page-context bug that had been live on the desktop
   for as long as the consent bridge has existed.
5. **Android phase — no longer blocked on tooling.** `~/Qt/6.11.1` carries all
   four Android ABIs and `~/android-ndk-r29` is present, so the reason this was
   deferred (nothing to build with) is gone; what remains is the work itself.
   The seam is ready and the constraint is confirmed rather than assumed — the
   Android kits ship no WebEngine, so the System WebView backend is the first
   piece, then the adaptive drawer layout, Intent-based player handoff, Android
   Autofill and SAF downloads (arch §19).

   **Both cheaper things are done** — see the 6.11 section above. It builds
   there with no errors, `try_permissions` passes 11 of 11 on both Qts after one
   real fix and two test corrections, and the geolocation result turned out to be
   neither Qt's doing nor the packaging's: this machine has no GeoClue2 service,
   so there is no location provider to grant access to.

   **The configure has been tried, and it gets exactly as far as the design
   said it would.** Two flags are needed and neither is obvious:

   ```sh
   ~/Qt/6.11.1/android_arm64_v8a/bin/qt-cmake -S . -B build-android \
       -DQT_HOST_PATH=$HOME/Qt/6.11.1/gcc_64 \
       -DANDROID_NDK_ROOT=$HOME/android-ndk-r29
   ```

   `QT_HOST_PATH` because a cross build needs the *host* Qt's tools and
   otherwise picks up the system 6.8.2 ones, which it then rejects — the error
   names `Qt6WidgetsTools`, not the host path, so it reads as a missing Widgets
   module. `ANDROID_NDK_ROOT` because the kit's toolchain file points at
   `/opt/android/android-ndk-r27c`, which does not exist here. **Note the
   version gap**: that path says Qt 6.11 expects NDK r27c and r29 is what is
   installed, which is worth remembering when something odd happens later.

   With those two set, it failed on `find_package(... WebEngineWidgets)` — the
   Android kits ship no WebEngine, exactly as §19.2 says. **That first step is
   now taken**: the engine component and the four `qtwebengine_*` files are
   behind `if(ANDROID)`, with an empty `HYDRA_BACKEND_SOURCES` where the System
   WebView backend will go. Desktop configures and builds unchanged, which is
   the property that matters — the seam was supposed to make this a two-line
   split and it was.

   **The next blocker is not Android's, it is the install's.** Configure now
   stops on `WebChannel`, and that component is missing from *both* kits in
   `~/Qt/6.11.1` — there is no `Qt6WebChannel` CMake package for the desktop
   kit either. It is an installer component that was not ticked, not something
   Android lacks; Qt ships WebChannel for Android perfectly well. **Add Qt
   WebChannel in the Qt Maintenance Tool for both the desktop and the Android
   kits** and this moves on. (The desktop build here is unaffected because it
   uses the *system* Qt 6.8.2, which has it.)

   Worth knowing for when the port is written: `QWebChannel` is what the
   autofill, picker, MSE and consent bridges ride on, and Android's System
   WebView has no equivalent — `addJavascriptInterface` over JNI is the
   counterpart. That is behind the seam already (`set_script_bridge`), so it is
   a backend concern rather than a shell one. Checked while here: the mentions
   of `QWebChannel` in `mse_tap.h` and `filter_signals.h` are comments, not
   includes, so nothing outside the backend is coupled to it.

## Sharing rule sets: the transport is open, and deliberately

**Why it is deferred, recorded so it is not mistaken for an oversight:** whether
Hydra becomes part of `../fuzzypickles`, borrows its transfer machinery, or
grows its own peer-to-peer layer is undecided. Until that is settled, picking a
transport here would be committing the answer by accident.

**What is already built does not depend on it.** `judge_import()` takes a
*document*, not a file or a socket, and `export_learned()` produces one. The
safety core — what a received rule must prove — is where the risk lives and it
is transport-free. Whatever eventually carries the bytes plugs in underneath.

**What fuzzypickles already has**, since it is one of the three options and the
answer turned out to be "most of it":

- **Content-addressed blobs are implemented** (`core/src/blob_internal.h`, their
  §11): chunked, each chunk independently AEAD-sealed, with a Merkle tree over
  the **ciphertext**. Both verifiers, bisection addressing, have-sets,
  want/serve assignment, assembly, the wire exchange, and a CLI (`blob-add`,
  `blob-export`, …). Their own note is that stickers, file transfer and the
  global store are *one* mechanism, not three — a rule set would be a fourth
  user of it rather than anything new.
- **The Merkle-over-ciphertext ordering gives integrity without trust.** Any
  host verifies chunks against the root with no key at all, so a relay can serve
  bytes it cannot read. For a shared rule corpus that is exactly the property
  wanted: distribution without the distributor being able to alter it.
- **Sealing is deterministic**, so two people who export the same rules produce
  the same blob — dedup and a stable identity for "this rule set" come free.
- **A capability model and TOFU contact cards exist**, which is where an
  *authorship* answer would come from if one is wanted.

**And their §3 already settles the question I had been holding open.** Their
`config_sync` header states it plainly: an authenticated link does **not**
authorize what travels over it, so a config change carries its own proof rather
than relying on the channel. Applied here that means `why_unsafe` keeps running
even over a trusted peer link — a rule set from a friend is still a licence to
click buttons on pages you are signed into, and a friend can be careless or
compromised. The check is not a stand-in for a transport that has not arrived
yet; it is the part that stays.

So the three options differ in what they *add*, not in what they replace:

| option | what it supplies | what it costs |
|---|---|---|
| part of fuzzypickles | blobs, identity, capabilities, an existing peer network | Hydra stops being a standalone Qt app with no daemon |
| use its transfer machinery | the same blob mechanism, as a dependency | a C core dependency and a running daemon to talk to |
| roll our own | no new dependency | rebuilding chunking, verification and peer discovery, all of which exist next door |

Nothing here needs deciding to keep going: rule sets exchange as files today,
and every option above is additive to that.

## Open decisions and risks

- **Decided:** AI provider = local-first, Claude default external, others later
  (arch §9.1).
- **Spikes to run early:** the `QGraphicsProxyWidget` geometric-scale path for
  kiosk (may render black on some GPUs), and local-model tree-sort quality.
- **Interceptor limits:** request-only — no inline-script blocking, no response
  headers; the optional local proxy (arch §10) is the upgrade path.
- **Thread note:** `qtwebengine_interceptor::interceptRequest`, and so
  `request_filter::decide`, may run off the UI thread; the policy_engine is only
  mutated on the UI thread and reads tolerate a stale snapshot. Revisit if
  mutation frequency grows.

## Code style and file naming

Same three rules as `../fuzzypickles` (`code-style.md` there has the worked
examples and the reasoning):

- **`snake_case`, not `camelCase`,** for identifiers this project defines. This
  holds in Qt C++: call Qt's own `camelCase` API exactly as it is
  (`setSourceModel`, `addWidget`), and keep every Qt virtual you override under
  its real name (`interceptRequest`, `closeEvent`, `lessThan`,
  `filterAcceptsRow`, `rowCount`, `data`) — but names *you* introduce stay
  `snake_case`, including classes (`policy_engine`, not `PolicyEngine`),
  enums, signals, and slots.
- **Tabs for indentation, spaces for alignment** — one tab per nesting level,
  spaces after the tabs for anything lined up within a line, so alignment
  survives at any tab width. **Do not run `clang-format`**; with no config it
  defaults to spaces and silently undoes this.
- **Lowercase filenames,** `snake_case`, except where a tool won't accept it
  (`CMakeLists.txt`, `LICENSE`, `AndroidManifest.xml`).

Hydra-specific, on top of those:

- C++ member variables take an `m_` prefix and are otherwise `snake_case`
  (`m_views_by_id`, `m_save_timer`).
- Pointers and references bind to the name: `QWidget *parent`,
  `const QString &path`.
- No project-wide identifier prefix (fuzzypickles' `fzp_`). Deliberate: the
  codename is provisional, and a prefix would make renaming cost more than the
  one `project()` line it currently costs.
- `#pragma once`, headers alongside sources, one class per file.
- Qt 6, C++17, Qt **Widgets only** — do not introduce QML/Qt Quick.
- Keep the platform-neutral core (`tab_tree_model`, `tree_outline`,
  `policy_engine`, `state_store`, `tree_sort_proxy`) free of platform APIs;
  platform-specific behavior goes behind interfaces (the WebView-backend seam,
  arch §19.2).
- Persisted files live next to the tree file: `policy.json`, `state/<id>.blob`.

One naming wrinkle worth knowing: the architecture doc's tri-state `Default`
state is spelled `policy::setting::unset` in code, because `default` is a
keyword. It means the same thing — no rule at this scope, fall through to the
global default.

## Commit conventions

- **No AI-attribution trailers** (`Co-Authored-By: Claude ...` and the like) in
  commit messages, and nothing elsewhere in the repo indicating AI involvement.
  Messages end at their real content.
- Documentation changes ride along with the code commit they describe rather
  than landing on their own.

## Licence

**GPL-3.0-or-later.** Full text in `LICENSE`; every source file carries an
`SPDX-License-Identifier: GPL-3.0-or-later` line rather than a copyright block.

This sits correctly under the Qt WebEngine dependency, which is
LGPLv3 / GPL / commercial (arch §2) — note that Qt WebEngine's own licensing
constrains distribution independently of what Hydra declares, so re-check it
against any distribution plan.

## Naming

"Hydra" is a working codename. Renaming = the `project()` line in
`CMakeLists.txt` plus the `hydra` target; nothing else depends on the name. The
architecture doc still carries the older codename "Browser Overlord" in its
title — same project.

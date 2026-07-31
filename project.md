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
prompt change measured at ten runs an arm and reverted for making things worse.

**Not measured, and known.**
- Capture under *live network* conditions — the mechanism is proven locally,
  the timing against a real site is not.
- The ad-host list at runtime; the cookie filter; the permission callbacks.
- The KeePassXC bridge above the crypto layer — `keepassxc` is not installed.
- ~~Whether the extractor prompt generalises past one synthetic evidence set.~~
  **Answered, badly:** it does not. Against evidence captured from the real
  site the loop returned prose and no parser in five runs out of five, so every
  hit-rate in this file describes the synthetic fixture and nothing else.

**A caution learned repeatedly.** Six separate defects this project has hit were
wiring that existed but was never exercised — a signal never connected, a
message written into a label something else overwrote, a store that was saved
and never read. Treat "wired but untested" as "probably broken", and prefer a
test that drives the real widget over one that calls the function underneath it.

**And its mirror image, learned once and worth the same weight.** The extractor
gate had 27 checks and still accepted the page's own url as a stream, because
every check asked whether a *wrong* answer was refused and none asked what the
obvious lazy answer would be. A suite can be thorough about the failures its
author imagined and blind to the one the model finds in ten tries. When a real
model is available, running it is a cheaper source of adversarial inputs than
inventing them.

## The icon

`icons/` holds the app icon and `icons/build_icons.py` regenerates it from
`hydra-master.png`. Two cuts, because one drawing cannot serve the whole range:

- **48px and up** are downscales of the master. The master is the artwork with
  its white plate and cream halo flood-filled away and cropped hard to the ink —
  the source render was 1024x1024 but only 774x853 of it was drawing, so a
  quarter of every icon would otherwise have gone on nothing. It is taller than
  it is wide and an icon slot is square, so the last 9% is squashed rather than
  letterboxed; invisible at these sizes, and it buys back the margin.
- **32px** is that plus a light unsharp pass, which is where losing local
  contrast starts to matter. Not applied below, where it only adds confetti.
- **16px is drawn pixel by pixel**, in `ICON16` in the build script. Measured
  rather than assumed: a downscale at 16 spends most of its budget on
  antialiased grey belonging to no shape, and in a tab strip it reads as a
  muddy speck while the drawn one still reads as a creature. Three green heads,
  lit eyes, a fire body, water up the left, full-strength colour, outline only
  where two fills meet.

All seven sizes are compiled in through `icons/hydra.qrc` and added to one
`QIcon` in `main.cpp`, so Qt picks per use rather than rescaling one image —
which is the whole reason the 16px cut exists. `packaging/install-icons.sh`
lays the same files into a `hicolor` theme with the desktop entry.

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
runtime. The cookie filter and the permission callbacks are also still
unexercised.

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

Persistence: `policy.json`, `state/<id>.blob`, and the tree file all sit next to
the outline file passed on the command line.

## The WebView seam (step 3.5, done)

The shell no longer names Qt WebEngine anywhere — `grep QWebEngine src/` hits
only the four `qtwebengine_*` files, and `main_window.{h,cpp}` is clean. The
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
shared like the filter list. **71 checks** cover the accept path, both invented
cases, the segment rule, the page-url rule and its two edges, the furniture
rule and its mixed-type edge, the `type`/`kind` split, which addresses are worth
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

**The review loop is exercised with a stub provider** (14 checks): the payload
is sent folded, a valid proposal becomes acceptable and stores with its fence
stripped, an invented URL leaves the accept button disabled and stores nothing,
and a stored script stops matching when the evidence changes.

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

## What is next (in order)

1. **Give the extractor better evidence, rather than a better prompt.** Four
   prompt iterations against captured real evidence took the model from prose,
   to code, to the right signature, to attempting the real task — and never to
   a working extractor (section above). On that site there is no extension
   signal, no host signal, and every failing run reached for an extension. The
   thing that would settle it is the local proxy's content-type tier (§10),
   which is already the recorded answer to the same problem in
   `media_detector::classify()` — **now built**, see the content-type tier
   above, and measured against the real site: the manifest declares
   `application/vnd.apple.mpegurl` once it is asked with the page's context.
   What remains is to send the model what each candidate actually serves,
   rather than only its address. The two cheaper things the runs turned up are
   both done: the gate refuses what the browser fetched as an image or a
   script, and a request's browser type is called `type` now rather than
   sharing the name `kind` with the stream type it is not. Neither of them
   makes the loop work on that site; they remove two ways of failing that were
   ours rather than the model's.
2. **Try the fragment-first prompt line — but only on new evidence.** The
   query-string line was tried and reverted: 3 of 10 against 8 of 10 for the
   original, and it failed to prevent the very `endsWith` it was written for
   (section above). What every clean pass actually did was match a path
   fragment, so the next hypothesis is to strengthen rule 2 rather than rule 3.
   Do it *after* step 1, on a second evidence set: tuning further against the
   one synthetic fixture fits the fixture, not the problem.
3. **Exercise what is wired but untested**, in rough order of how much is
   riding on it: the ad-host list at runtime, the cookie filter, the permission
   callbacks, and the KeePassXC bridge above the crypto layer (which needs
   `keepassxc` installed). This project's defect history is almost entirely in
   this category — see the caution at the top of this file.
4. **Finish the helper tier (arch §11.5.1).** The fetch half is built ahead of
   a site that demands it, on the reasoning that we cannot meet every site
   others will and a tier designed against one example fits that example.
   `helper_allowlist`, `helper_host` and the budgets are done and tested
   offline; `hydra` appears in the sandbox only when a host is supplied, so the
   pure tier cannot see the surface exists. The transcript is in the review
   dialog: every call, what the server answered, refusals marked in the margin,
   and shown on rejection as well as acceptance — that being the case where it
   matters most. The blocking fetcher is built too: its own thread, a nested
   loop where there is nothing to re-enter, a refusal rather than a deadlock if
   called from its own thread, and a bounded timeout. Judging now happens off
   the UI thread as well, so a slow CDN no longer freezes the window — asserted
   as an ordering, not a feeling: a UI timer fires at 120 ms while the script is
   still blocked at 410 ms. The permission is in too: `extractorFetch` and
   `extractorDom` are ordinary §7 tri-states defaulting to block, they appear in
   the site editor without touching it because that editor iterates the feature
   list, and `main_window` hands the dialog a `helper_host` only where the site
   has been granted the first.

   **And it has now been run against the real site, which is where it earned
   its keep and where three defects were found that no fake could have shown.**
   `test_helpers_live` drives the tier with a hand-written extractor, so a
   failure is the tier's and not a prompt's. On the third attempt: the
   disguised manifest ranked first, `head` identified it as
   `application/vnd.apple.mpegurl`, `text` fetched 160 bytes and taught the
   allowlist four new addresses, and the script returned the variant — an
   address **the page never requested**, accepted by the gate because a fetched
   document named it. Four calls, 320 bytes. The returned variant was then
   fetched independently and answers 200 `application/vnd.apple.mpegurl`, so
   the answer is usable and not merely well-formed.

   The first two attempts failed, and both failures were ours: the budget was
   spent left-to-right on stylesheets and beacons before the video was reached
   (the sandbox could not see the ranking the C++ side already computed, so
   `hydra.candidates()` now hands it over), and then a `log()` call — which
   costs nothing to serve — consumed the last fetch slot, so a script that
   explained itself ran out of room to work. Notes are bounded separately now.

   What remains is the DOM half behind §13.2.
5. **Android phase (deferred).** System WebView backend, adaptive drawer layout,
   Intent-based player handoff, Android Autofill, SAF downloads (arch §19).

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

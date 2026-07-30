# Browser Overlord — Architecture Design Document

**Status:** Design (pre-implementation)
**Target platforms:** Linux / X11 (first-class, built first); Android (first-class, deferred until the desktop version is complete) — see §19
**UI toolkit:** Qt 6, Qt Widgets (no QML / Qt Quick) on both platforms
**Web engine:** Qt WebEngine (`QWebEngineView`, Chromium/Blink)
**Design ethos:** compact, privacy-first, local-first, single-window

---

## 1. Overview and scope

Browser Overlord is a Qt Widgets application that presents itself as a single browser with a side-tree of tabs, but is really a management shell over its own embedded Chromium engine (Qt WebEngine). It aggregates links and tabs into a hierarchical tree, renders any selected node in a chrome-less web view, and layers on five capability subsystems: a per-site security policy engine, a kiosk presentation mode, an AI assistant that reorganizes the tree and evolves ad filters, a media-download detector, and a password manager that integrates with KeePassXC.

The product feel is "a normal browser with powerful side tabs," but internally it is a state model (the tree) plus a rendering surface (the web views) plus a set of shared services (interceptor, policy engine, AI provider). Everything the user manipulates is a node in one tree; everything the app enforces flows through one policy engine; everything intelligent flows through one AI provider and one non-destructive diff/accept pipeline.

### Design goals

The system favors a small, comprehensible core over feature sprawl. Three shared "spines" carry almost all functionality, and each new feature is expressed as a consumer of an existing spine rather than a new subsystem. Data stays on the machine by default: only lightweight metadata ever leaves, and only when the user asks. The tree, the policy, and the filters are all human-inspectable files on disk.

---

## 2. Key decision: embed our own engine, not wrap installed browsers

An earlier design considered *wrapping* the user's installed browsers — launching e.g. `chromium --app=<url>` and reparenting the resulting X11 window into a Qt widget via `QWindow::fromWinId()` + `QWidget::createWindowContainer()`. That approach is genuinely possible on X11, but it carries two disqualifying problems for this product:

First, foreign-window embedding relies on XEmbed, and a real browser window is not a cooperating XEmbed client, so keyboard focus, input-method handling, and click-focus routing are chronically unreliable. Second, and decisively, a wrapped browser runs *its own* settings — you cannot impose a JavaScript whitelist, a cookie policy, or an ad filter on a Chrome process you are merely hosting. The security policy engine, the kiosk controls, and the filter subsystem all require that the application own the engine.

The design therefore standardizes on **Qt WebEngine**: Blink gives best-in-class web compatibility, `QWebEngineView` drops into a widget layout natively, and the app gets full control over navigation, requests, cookies, permissions, and downloads. The trade-offs to accept and document: Qt WebEngine is a large dependency, it is licensed LGPLv3 / GPL / commercial (check against distribution plans), and it deliberately exposes only a subset of Chromium's knobs — if deep per-origin content settings become a hard requirement later, the fallback is raw CEF, and the `AIProvider`/engine boundaries are kept clean so that migration stays localized.

One consequence reaches all the way to the Android target (§19): **Qt WebEngine is not available on Android** — Chromium isn't built for it. So "the app owns its engine" holds on both platforms, but the *engine* differs (Qt WebEngine on desktop, the native Android System WebView on Android). This is why the shell must talk to the web view through a small backend interface rather than to `QWebEngineView` directly; introducing that seam is a prerequisite for the Android phase.

---

## 3. High-level architecture

The application is organized around three shared spines and a set of feature modules that consume them.

```
                         ┌─────────────────────────────────────────┐
                         │              Qt Widgets Shell            │
                         │  QWidget · Menu/tool/status bars         │
                         │  · QSplitter · Sidebar tree              │
                         │  · QStackedWidget of QWebEngineViews     │
                         └───────────────┬─────────────────────────┘
                                         │
        ┌────────────────────────────────┼────────────────────────────────┐
        │                                │                                 │
   ┌────▼─────┐                    ┌─────▼──────┐                    ┌──────▼──────┐
   │  SPINE 1 │                    │  SPINE 2   │                    │   SPINE 3   │
   │ Interceptor                   │ TabTree    │                    │ AIProvider  │
   │ (sensor) │                    │ Model      │                    │ + diff/     │
   │          │                    │ (state)    │                    │ accept      │
   └────┬─────┘                    └─────┬──────┘                    └──────┬──────┘
        │                                │                                 │
   ┌────┴───────────┐            ┌───────┴─────────┐            ┌──────────┴─────────┐
   │ ad-block       │            │ sidebar views   │            │ tree reorganizer   │
   │ media detector │            │ sort/filter     │            │ filter-evolution   │
   │ filter signals │            │ persistence     │            │ (both use diff UI) │
   └────────────────┘            └─────────────────┘            └────────────────────┘

                         ┌─────────────────────────────────────────┐
                         │   SINK: PolicyEngine (rules + enforce)   │
                         │   consulted by interceptor, cookie       │
                         │   filter, per-page settings, permissions │
                         └─────────────────────────────────────────┘
```

**Spine 1 — the interceptor** is a single `QWebEngineUrlRequestInterceptor` installed on the profile. It sees every network request and is the shared sensor behind ad-blocking, media detection, and filter-evolution signal collection.

**Spine 2 — the TabTree model** is the single source of truth for what the user has: folders, open tabs, unopened links, and suspended tabs. The sidebar, the sort/filter views, the AI reorganizer, and the on-disk file are all views over this one model.

**Spine 3 — the AIProvider plus the diff/accept pipeline** is the shared intelligence path. Tree reorganization and ad-filter evolution are two clients of the same "serialize → send → receive → diff → accept" machinery.

**The sink — the PolicyEngine** holds all per-site rules and is consulted by every enforcement point.

---

## 4. Core data model

### 4.1 Node schema

Every entry in the tree — folder or leaf — is a `Node`:

```
Node {
    id:        NodeId        // short, opaque, stable for the node's lifetime
    type:      Folder | OpenTab | UnopenedTab | SuspendedTab
    parentId:  NodeId
    order:     int           // sibling ordering within parent (canonical tree order)
    title:     string
    url:       string        // empty for pure folders
    created:   timestamp
    lastSeen:  timestamp     // last visited / focused
    tags:      [string]
    policy:    PolicyOverrides?   // optional per-node rule overrides
    stateRef:  StateBlobId?       // present only for SuspendedTab
}
```

### 4.2 Node types and where their weight lives

The three leaf types differ only in how much runtime weight they carry, and that weight is always attached *locally by `id`*, never embedded in the node metadata that travels:

- **OpenTab** — has a live `QWebEngineView` instance attached to its `id`.
- **UnopenedTab** — just a URL and title; nothing loaded. The cheap default for imported links and AI-suggested nodes.
- **SuspendedTab** — was open, now hibernated; its scroll position, form state, and navigation history are serialized into a **state blob** stored separately and referenced by `stateRef`.

Because live views and state blobs are keyed by `id`, moving or re-parenting a node in the tree never disturbs its runtime payload — the payload follows the id. This property is what makes AI reorganization safe (§9).

### 4.3 Stable IDs

Node ids are opaque and independent of title/URL. They are the linchpin of two features: the AI diff pipeline references nodes by id so a folder rename never looks like a delete-plus-create, and the local diff is computed by reconciling ids rather than fuzzy-matching titles.

### 4.4 On-disk canonical file — the source of truth for structure

The tree's structure and canonical order live in a single human-readable text file, an ID-tagged indented outline (OPML-ish / Markdown-ish):

```
- [f0] Folder: Work
  - [a2] (open)      Jira board       — https://jira.example.com/board
  - [a3] (suspended) Design doc        — https://docs.example.com/xyz
- [f1] Folder: Reading
  - [a4] (unopened)  Recipe            — https://cook.example.com/stew
```

This file is the authority for hierarchy and manual order. State blobs and live views are *not* in this file — they are sidecar data keyed by id. The file being plain text means it is inspectable, diffable, hand-editable by power users, and is exactly what gets serialized to the AI.

---

## 5. Tab-tree, session model, and the sorting/filtering layer

### 5.1 The model

`TabTreeModel` is a `QAbstractItemModel` backed by the node set loaded from the canonical file. It exposes node attributes through custom data roles so that views and proxies can sort/filter without the model caring how:

```
TitleRole · UrlRole · CreatedRole · LastSeenRole · TreeOrderRole ·
NodeTypeRole · TagsRole · PolicyRole
```

### 5.2 Powerful sorting — two proxy strategies

Sorting is the responsibility of a proxy layer stacked on `TabTreeModel`, never the source model. Two distinct strategies cover the requirements, because "sort siblings inside the tree" and "flatten everything into one sorted list" are different operations:

**Hierarchical sort (preserves nesting).** A `QSortFilterProxyModel` with a selectable `sortRole` re-orders each parent's children while keeping the tree shape. Choosing the role selects the mode:

- **Tree structure** — sort by `TreeOrderRole` (the `order` field from the on-disk file). This is the canonical, manually/AI-authored arrangement and the default view.
- **Alphabetical** — sort by `TitleRole` (with a URL secondary key option).
- **Date created** — sort by `CreatedRole`.
- **Date last seen** — sort by `LastSeenRole`.

**Flat sort (dissolves nesting).** For "show me *every* tab by last-seen, regardless of folder," a small **flattening proxy** presents all leaf nodes as a single flat list, which a `QSortFilterProxyModel` then sorts by the same roles. This is the mode users reach for when hunting ("what did I look at yesterday?") rather than organizing.

The sort mode is a pure *view* state. It never rewrites the canonical file — the file's order is only touched when the user explicitly chooses "apply this ordering to the tree," at which point the current sorted order is written back into the `order` fields and persisted. So you can browse by date without disturbing your hand-built hierarchy.

### 5.3 Filtering and search

The same proxy layer carries filtering, since `QSortFilterProxyModel` does both: a live search box (title/URL substring or fuzzy), plus structured filters — by node type (show only open / only suspended / only unopened), by tag, or by policy state (e.g. "sites where JS is blocked"). Filtering and sorting compose, so "all suspended tabs from last week, alphabetical" is just three predicates on one proxy stack.

### 5.4 Session lifecycle

Nodes move between types as a lifecycle: an UnopenedTab becomes an OpenTab when selected (a `QWebEngineView` is instantiated and attached), an OpenTab becomes a SuspendedTab under memory pressure or an idle timer (its state serialized to a blob, the view destroyed), and a SuspendedTab restores to OpenTab on demand (state blob rehydrated into a fresh view). The tree can therefore hold thousands of nodes while only a handful are live views at any moment. Persistence writes the canonical file on change (debounced) and the state blobs on suspend.

---

## 6. The shell and web-view embedding

The shell presents itself as a conventional desktop application in the older style: a **menu bar** across the top, the compact toolbar beneath it, the content area, and a **status bar** along the bottom. Structurally it is a plain `QWidget` rather than a `QMainWindow`, with all four laid out explicitly in one `QVBoxLayout` — `QMenuBar`, `QToolBar`, a horizontal `QSplitter`, `QStatusBar`. Both bars are ordinary widgets and work perfectly well outside a `QMainWindow`; the only thing that has to be reproduced by hand is status-tip routing (a `QMainWindow` forwards `QEvent::StatusTip` to its status bar automatically, so the shell handles that event itself). Laying the furniture out directly keeps the window's structure explicit, which is what kiosk mode (§8) and the Android drawer layout (§19.3) both need in order to rearrange or strip it later.

The menus cover what the app already does — File (save tree, quit), Go (back/forward/reload), View (sort mode, expand/collapse), Tools (site controls), Help — with the sort actions driving the toolbar's sort combo rather than the proxy directly, so the two controls cannot drift apart. The status bar carries transient messages (the current URL, menu status tips) on the left and a permanent live-view count on the right.

Note that "chrome-less" below refers to *per-tab browser* chrome drawn around the web views, not to the application window's own furniture: the app window keeps its menu bar, toolbar, and status bar; what it does not do is wrap each web view in a Chromium frame.

The left pane is a `QTreeView` bound to the sort/filter proxy stack over `TabTreeModel`. The right pane is a `QStackedWidget` holding one `QWebEngineView` per open tab; selecting a tree node either switches the stack to that node's live view, instantiates a view for an unopened node, or rehydrates a suspended one. For true side-by-side viewing, the right pane can itself be a nested `QSplitter` hosting two live views.

Chrome-less presentation is inherent: the app draws no per-tab browser chrome around the web views — navigation and controls live in the app's own compact toolbar (address field, the policy shield, media badge), not in a Chromium frame. All web views share a single `QWebEngineProfile`, which is where the interceptor, cookie filter, and download handler are installed once and apply everywhere.

---

## 7. PolicyEngine — per-site security whitelist/blacklist

### 7.1 Rule model

A rule is a pattern plus a packed set of per-feature tri-states, chosen to keep the whole ruleset tiny:

```
Feature  = { JS, Cookies, ThirdPartyCookies, Ads, Popups, Images,
             Autoplay, Geolocation, Camera, Mic, Notifications, Referer, WebRTC, ... }
Setting  = Default(0) | Allow(1) | Block(2)      // 2 bits per feature
Rule     = { pattern: "*.example.com", bits: uint64 }   // up to 32 features packed
```

Each rule is one string plus one 64-bit integer; the ruleset serializes to a small JSON file.

### 7.2 Matching precedence

`decide(feature, url) → Allow | Block` resolves in this order: an exact-host rule beats any wildcard; among wildcards the most specific (most labels matched) wins (`*.mail.google.com` > `*.google.com` > `*`); if no rule sets that feature it stays `Default` and falls through to the **global default mode** for that feature. That per-feature global default *is* the whitelist/blacklist switch — `JS default = Block` yields opt-in-per-site JavaScript (whitelist mode); `Ads default = Block`, `Cookies default = Allow`, and so on. Exact hosts live in a hash map; wildcards in a suffix-sorted list, so lookups are fast.

### 7.3 Enforcement map — each feature to its Qt WebEngine hook

| Feature | Enforcement mechanism |
|---|---|
| JavaScript | `QWebEngineSettings::JavascriptEnabled` set per page on navigation (top-level host policy); plus interceptor blocking `ResourceTypeScript` from non-allowed hosts for per-origin control |
| Cookies / third-party cookies | `QWebEngineCookieStore::setCookieFilter()` — callback gets `origin`, `firstPartyUrl`, `thirdParty`; return false to block |
| Ads / trackers | The shared interceptor — `info.block(true)` on filter-list matches |
| Popups | `QWebEngineSettings::JavascriptCanOpenWindows` (per page) + overriding `QWebEnginePage::createWindow()` to return `nullptr` when blocked |
| Images | `QWebEngineSettings::AutoLoadImages` per page, or block image requests in the interceptor |
| Autoplay media | `QWebEngineSettings::PlaybackRequiresUserGesture` |
| Camera / mic / geolocation / notifications | `QWebEnginePage::featurePermissionRequested` signal, granted/denied from policy instead of prompting |
| Referer / custom headers | Interceptor `setHttpHeader` (strip or rewrite `Referer`) |
| WebRTC IP leak | `WebRTCPublicInterfacesOnly` |

The whole enforcement surface is one interceptor, one cookie filter, per-navigation attribute application, and two page-level overrides — all reading `PolicyEngine::decide(...)`.

**Known limitation:** the interceptor is request-only, so it cannot block *inline* `<script>` blocks (they are not separate requests) and cannot rewrite response headers (so it cannot inject CSP). Per-origin external-script control works; inline-script suppression falls back to the per-page JS toggle. Full uMatrix-grade control including inline scripts is the one case that would require CEF's native content settings.

### 7.4 Editor UI

A shield/gear button sits beside the address field. Clicking it opens a compact frameless popup showing the current host and a tight grid: one row per feature, each a three-state control (Allow / Default / Block), with a scope selector at the top (**this host** / **\*.domain** / **global default**) so a single click writes the rule at the right breadth. Changes apply live and reload. A small count badge shows how many requests were blocked on the current page. A "Manage all rules…" link opens the full editor: a `QTableView` with the pattern in column one and one tri-state column per feature. The popup and the table are two views over the same `QAbstractTableModel` wrapping the PolicyEngine, so nothing is duplicated. Adding a feature is one enum value, one packed slot, one UI row, and one hook.

---

## 8. Kiosk mode

Kiosk mode is a *presentation mode plus a policy preset*, not a new engine. It takes a `KioskController` config `{ url, designSize?, fitMode, alignment, hideCursor, disableInput, home/idleReset, watchdog }` and, on entry, calls `showFullScreen()`, strips all chrome, sets `ShowScrollBars=false` and `setContextMenuPolicy(Qt::NoContextMenu)`, routes new-window/fullscreen/popup requests through the PolicyEngine so the page cannot escape, and applies scaling and cropping.

### 8.1 Scale and crop are independent operations

**Scale** has two flavors, both exposed. *Reflow zoom* — `QWebEngineView::setZoomFactor(screenW/designW)` — relays out the page at a new scale; robust, cheap, correct for normal reflowable content and the default. *Geometric scale* — render at a fixed design resolution and transform the pixels via a `QGraphicsView` + `QGraphicsProxyWidget` with `setTransform(QTransform::scale(...))` — preserves exact layout without reflow, for pixel-exact content (canvas apps, signage, games).

**Crop** does not need a transform: because child widgets clip to their parent's bounds, placing the web view at a geometry larger than its container with a negative offset crops the overflow, with input still mapping correctly and rendering fully normal. This is the robust crop path.

### 8.2 Fit modes

A single declarative `fitMode` composes scale and crop like CSS `object-fit`: **Contain** (whole page visible, letterboxed), **Cover** (fills screen, overflow cropped — the classic signage mode), **Stretch** (independent X/Y scale, distorts aspect), **Actual/None** (native size, cropped or letterboxed). An `alignment` decides which edges crop in Cover mode.

### 8.3 Caveat and kiosk extras

**Caveat:** `QWebEngineView` inside a `QGraphicsProxyWidget` is historically fragile — the engine renders through a native/GPU-composited surface and may render black in a graphics scene. Qt 6 / RHI is better but not guaranteed across drivers. So geometric scale is the *optional* mode, tested early on the target GPU/compositor, with reflow-zoom as the reliable default; crop-via-clip avoids the graphics scene entirely.

**Spike result — it works here.** Tested on Qt 6.8.2 / X11, embedding a live `QWebEngineView` in a `QGraphicsProxyWidget` and transforming it 1.5× from a 1280×720 design onto a 1920×1080 screen: the page renders correctly, not black. So the path is viable on this hardware. Two caveats stand. It remains per-GPU/compositor — this is one data point, not a guarantee, and it still wants re-testing on any new deployment target. And embedding has a hard precondition that fails *quietly*: `QGraphicsProxyWidget::setWidget()` only accepts a top-level widget, so a widget that still has a parent is refused with a console warning and the scene simply stays empty — indistinguishable from the black-render failure it is meant to guard against. The implementation detaches the widget first and falls back to reflow zoom if the embed returns null.

Kiosk extras that turn "a fullscreen page" into a deployable kiosk, each a few lines: an **idle-reset** timer that returns to the home URL after inactivity (the single most-used real kiosk feature), a **watchdog** that auto-reloads on render-process crash so an unattended screen self-heals, **cursor auto-hide**, an **off-the-record profile** so the kiosk leaves no trace between sessions, and an escape-gesture lockdown.

---

## 9. AI integration — provider and tree reorganization

### 9.1 Provider abstraction

An `AIProvider` interface fronts pluggable backends with honest trade-offs: a **local model** backend (Ollama / llama.cpp on localhost — zero data egress); an **external API** backend (an API key, cleanest structured output, metadata egress); and optionally a **logged-in web-session** backend (drive an authenticated chat view — least preferred: DOM-scraping fragility and ToS friction). "We are logged in" simply means the provider holds whatever credential its backend needs, abstracted from the rest of the app.

**Decided default resolution (v1):** local-first — if a local model is present and enabled, it handles requests with nothing leaving the machine. If the user configures an external provider, that is used when selected; the default external adapter is **Claude (Anthropic API)**, with additional providers pluggable behind the same interface later. Because the interface is uniform, adding a provider is one adapter class; the reorganizer, filter-evolution loop, and diff/accept pipeline are all provider-agnostic. When an external provider is active, the review-before-send gate defaults on, since URLs and titles are themselves sensitive.

### 9.2 Non-destructive pipeline

The reorganizer runs: serialize the tree to the canonical text outline → (optionally let the user review the exact payload) → send to the provider → receive a proposed outline → compute the diff locally by reconciling ids → render the proposal as a shadow tree (never mutating the live tree) → let the user accept all or cherry-pick per change → apply accepted changes to the model. The live tree is untouched until accept; "update widget after receive" means populate a proposal overlay, not replace the working model.

### 9.3 What travels and what stays

Only metadata leaves: per node, `id`, parent/depth, title, URL, type, tags. State blobs (scroll, form data, history) and live views **never** go to the provider — they stay local, keyed by id, and ride along when a node moves. So reorganization is a pure re-parenting of ids; a live tab or a suspended tab's stored state cannot be disturbed or lost because the AI never held it.

### 9.4 Safety invariants

Before any proposal is shown, a **"no node left behind"** check runs: every original leaf id must appear exactly once in the proposal. Dropped, duplicated, or invented ids are auto-repaired or rejected rather than surfaced as a diff that could lose a tab. New folder nodes carry model-proposed new ids flagged as new. The AI is constrained to return the same id-based outline (reordered/re-nested, allowed to create and rename folders), and a single undo snapshot makes any accepted change one keystroke to revert.

### 9.5 Diff and accept UI

**Status: done.** `tree_serializer` builds the metadata-only payload, `tree_diff` holds the invariant check and the change derivation, `reorganize_dialog` is the review-and-cherry-pick UI, and `ai_provider` fronts `ollama_provider` (local, preferred) and `claude_provider` (external). Not yet built: the undo snapshot, the web-session backend, and acting on duplicate-URL merges.

From the two id-keyed trees the app derives atomic changes — moved, re-parented, reordered, new folder, renamed folder, and optionally "duplicate URLs suggested for merge" — rendered as an annotated proposal tree with per-change badges, each individually toggleable. Applying a change is a model reparent/insert; because payloads follow ids, accepting is instantaneous and lossless.

---

## 10. The interceptor as shared sensor

A single `QWebEngineUrlRequestInterceptor` on the profile is the sensor behind three consumers: ad/tracker blocking (§7.3), the media detector (§11), and filter-evolution signal collection (§12). Its structural limitation is that it sees **requests, not responses** — no response headers or bodies, so media Content-Types and manifest contents are not directly visible at interception.

**Optional local-proxy upgrade:** for full response inspection (real Content-Types, manifest bodies, segment lists) and to assemble streams, the app can host a small local HTTP proxy that Qt WebEngine routes through. This is designed as an optional tier: URL-pattern detection works without it; the proxy is the "detect everything reliably" upgrade. Both the media detector and any response-level filtering degrade gracefully to request-only heuristics when the proxy is off.

---

## 11. Media-download detector and download manager

**Status: done**, minus the local-proxy tier. `media_detector` rides the interceptor's observer seam and classifies by URL shape; `player_launcher` probes PATH and routes by capability; `download_manager` queues direct files with Range resume. Segment assembly, the ffmpeg remux, and the proxy that would inject request context remain the next increment.

**The downloads window is done.** `downloads_dialog` (Tools → Downloads…, Ctrl+J) is the one list §11.2 asks for: every source in the same table, with progress bars, per-job Pause/Resume/Cancel gated on `source_capabilities`, multi-file jobs expanding to their files, and indeterminate jobs drawing a busy bar rather than a false zero. It carries the §11.4 visibility obligation as a permanent `⇅ public` row marker plus a footer, rather than relying on the one-time consent dialog. It contains no test for a transport: everything it varies comes from capabilities and the job's own fields.

**The transport seam is done** (§11.4): `download_manager` no longer contains a transport. It owns the queue, the destination, consent and the job records; a `download_source` owns the bytes, and `http_download_source` is the first one. The job model now carries the states a torrent needs and HTTP never enters — `resolving` (a magnet with no metadata yet) and `seeding` (complete but still working) — plus multi-file jobs, per-source concurrency, and the consent gate that enforces the §11.4 privacy obligation structurally. Verified with 77 checks, including a fake torrent source that exercises the whole torrent-shaped lifecycle without libtorrent present.

### 11.1 Detection

Riding the interceptor, the detector classifies three kinds of saveable media: direct files (`.mp4/.webm/.mkv/.mp3/.m4a/.pdf`…) by URL/extension and Content-Type; HLS manifests (`.m3u8`); and DASH manifests (`.mpd`). Obfuscated manifests are still betrayed by their segment requests (`.ts`, `.m4s`). Reliable Content-Type and manifest-body classification uses the optional local proxy (§10).

### 11.2 UI and download manager

A "media on this page" toolbar badge lights with a count when saveable resources are found; clicking opens a compact list (type, size from `Content-Length`/HEAD, stream quality variants from the manifest), each with a Save button. Downloads flow into one manager fed by two sources — page-initiated downloads via `QWebEngineProfile::downloadRequested` / `QWebEngineDownloadRequest`, and detector-initiated media saves — with queue, resume (range requests), progress, and organization *against the tab tree* (a download belongs to the node it came from). Clear segmented streams have their segments fetched and concatenated; if `ffmpeg` is present the manager remuxes to a clean `.mp4`/`.mkv`, degrading to raw-segment save without it (optional dependency). A per-site "auto-detect media" toggle lives in the PolicyEngine.

### 11.3 Open in external player (streams only)

**Controls UX.** The two actions — **▶ Watch in player** and **⬇ Download video** — are surfaced by a single media affordance in the toolbar (next to the policy shield), then fades in with a count badge. Detection is progressive: many sites only request the manifest when their player initializes or the user presses play, so the control appears a beat after load rather than instantly. A single click runs the default action on the *primary* stream (default action: **Watch** — the whole point is that the site's player is broken); a chevron opens a compact popup listing every detected stream, each row offering both **▶ Watch** and **⬇ Download**, plus a per-site "prefer external player" toggle. The primary stream is chosen by heuristic — the highest-bandwidth/resolution manifest variant, or the one whose segments are actively being fetched. A keyboard shortcut mirrors the default action so the mouse is never required.

**Request context.** A naked stream URL frequently returns 403, because the CDN expects the same `Referer`, cookies, and `User-Agent` the page carried. The player must receive that context. The clean, player-agnostic way is to point the player at the app's **local proxy** (§10), which injects the observed headers/cookies upstream and serves the stream on localhost — this avoids depending on any given player's header flags (which vary and, for mplayer, are limited). Direct header flags are the fallback where a player supports them.

**Player detection and radio choice.** On startup (and refreshable on demand) the app probes `PATH` with `QStandardPaths::findExecutable()` for known players — mplayer, mplayer2, mpv, vlc, smplayer, ffplay — and builds the set that is actually installed. The settings UI presents them as a **radio-button group**: installed players are selectable; players that aren't found are shown **greyed/disabled** with a "not installed" hint (so the user sees the full menu of what's supported and what to install to unlock it), and a final **Custom…** radio exposes an editable command template for anything else. The default selection is resolved from what's present — never assuming mpv exists — so on a machine with only **mplayer**, mplayer is auto-selected. Each known player ships with a correct command template (the flags for URL, headers/cookies, and cache differ per player).

**Capability-aware routing.** Players differ in what they handle well, and the app routes accordingly rather than feeding every player the same thing. mpv handles HLS/DASH and seeking natively, so it can take the manifest URL directly. Classic **mplayer** is strong on direct/progressive files and local files but weak at native HLS/DASH, so for an HLS/DASH source the app prefers to let the local proxy do the manifest work — assembling or remuxing the segments into a single seekable progressive feed (or the tee-to-disk file below) — and hands mplayer that, instead of relying on mplayer's HLS. So "only mplayer installed" still yields a good, seekable experience; the app compensates in the proxy for what the player lacks.

**Seekability.** Two rules preserve it. First, **hand the player a URL, never a stdin pipe** — a pipe has no random access, so `mplayer -`/`mpv -` can't seek; a URL lets the player issue its own range/segment requests. Second, the **local proxy must be range-transparent**: when the player sends `Range: bytes=X-Y`, the proxy issues the same range upstream (with injected headers) and relays the `206 Partial Content` with `Accept-Ranges`/`Content-Range` intact; for HLS it relays the manifest and segments verbatim, and if it rewrites the manifest it preserves the full segment list and timing tags (`#EXTINF`, `#EXT-X-MEDIA-SEQUENCE`, `#EXT-X-BYTERANGE`). With those in place, seekability follows the source: a direct file with server Range support and a **VOD** HLS/DASH playlist (ends in `#EXT-X-ENDLIST` / static MPD) are fully seekable end to end, while a **live/windowed** stream is seekable only within the window the server still exposes. To make a live stream fully scrubbable, **tee segments to disk while playing** (reusing the download manager's assembly) and point the player at the growing local file — live becomes a local VOD, giving full backward/forward seek plus a saved copy in one step. Player cache flags (`--cache`, generous back-buffer) widen backward seek within a live buffer even without recording.

### 11.4 BitTorrent downloads (scope decided; not implemented)

**Status: implemented on desktop.** `torrent_download_source` runs libtorrent-rasterbar behind the §11.4 seam — magnet links, `.torrent` files (local or fetched over HTTP), multi-file jobs, resume data keyed by info-hash, seeding with a ratio policy, and the connection caps raised off their defaults. Verified end to end against a real swarm: a seeder session and the source exchanging a torrent over loopback, byte-identical, plus magnet metadata fetched from a peer over the wire. libtorrent is an optional dependency on the libsodium pattern — without it the source reports itself unavailable and the shell does not add it, because a torrent engine cannot be faked.

The decisions that produced it: Two decisions have been made and are recorded below: BitTorrent is a **first-class download source**, not a side feature or a handoff; and **VPN/proxy binding is not a launch requirement**, because that belongs to the system layer. The engine recommendation follows from the first and is argued from evidence rather than reputation. What remains is implementation.

**The scope decision: first class.** Torrents are to be a peer of HTTP downloads, not a bolted-on extra — the thing other browsers conspicuously will not do. Concretely that means a magnet link behaves like any other download link: same queue, same progress, same pause/resume, same association with the node it came from, same completion notification, no separate window and no separate mental model. The user should not have to know which transport carried the bytes.

That is a deliberately higher bar than "we support torrents", and it is what decides the engine. The download manager (§11.2) is already *one queue fed by multiple sources*, so the shape is right: BitTorrent is a third source, not a new subsystem.

**Trigger surfaces.** Three, all of which the existing spines already see. A `magnet:` URI is a navigation the shell can intercept before the engine tries and fails to load it. A `.torrent` link is an ordinary download the manager already receives. A response typed `application/x-bittorrent` is only visible with the local proxy tier (§10) — request-only interception sees the URL, not the Content-Type, so extension-based detection is the fallback exactly as it is for media.

**The engine decision — three options, with the trade-off that actually separates them.**

1. **Embed a library.** ⚠️ **Two different libraries are both called "libtorrent" — check which one any advice refers to.**

   | | libtorrent-**rasterbar** | libtorrent (**rakshasa**) |
   |---|---|---|
   | Home | libtorrent.org (Arvid Norberg) | rakshasa.github.io/rtorrent (Jari Sundell) |
   | Debian package | `libtorrent-rasterbar-dev` 2.0.11 | `libtorrent-dev` 0.13.8 |
   | Embedded by | qBittorrent, Deluge | **rTorrent** |
   | Licence (Debian copyright) | BSD-3-clause | GPL-2+ (some MPL-1.1 parts; OpenSSL exception) |

   **Both are licence-compatible with a GPL-3-or-later application** — rakshasa's is GPL-2-*or-later*, not GPL-2-only, so it can be combined under GPL-3. Licence does not decide this.

   **What rakshasa's is actually known for.** Not "running light" in the trivial sense of lacking a GUI — the reputation is *connection scaling and throughput*: holding very large numbers of simultaneous peer connections and sustaining far higher up/down rates than contemporaries, which is why it dominated seedbox use. That is a property of the **library's** networking and disk architecture, not of rTorrent's ncurses front end, so unlike a UI's memory footprint it **does** transfer to an embedder. Nabeel puts the gap at roughly 30× against other clients from direct experience; that specific multiple is unverified here and should be measured rather than repeated, but the directional claim is well attested and the mechanism is plausible.

   **The insight behind that reputation outlives the comparison.** BitTorrent aggregate throughput is dominated by *how many peers you are talking to*, not by how fast any one of them is: the swarm population is mostly slow, distant, choked, or half-dead, so speed is the sum of many poor contributions rather than a few good ones. More connections also means more candidates for the choking algorithm to reciprocate with, which compounds it. rakshasa built around that; a client tuned for peak rate over a handful of fast peers is optimising the wrong axis, and benchmarks reporting exactly that number were common then and still are.

   **Caveat on vintage, from the source of the claim:** the observation dates to roughly 2010 and the landscape has moved — rasterbar has had substantial work since, including the 2.0 memory-mapped disk I/O rework. Treat the ranking as *unverified today*, but treat the criterion as durable.

   **Which makes the evaluation concrete.** Whichever library is trialled, measure the axis that matters rather than the one that is easy: throughput as a function of peer count against a realistically *poor* swarm, and the ceiling where it stops scaling. Watch file-descriptor limits, per-connection memory, and event-loop behaviour at thousands of sockets — and check each library's default connection caps and whether they can be raised, since a default tuned for a desktop GUI may hide the ceiling entirely.

   **Which one, decided from the shipped headers rather than from reputation.** Unpacking both Debian dev packages and probing their public headers (word-boundary matches, after `utp` was found to match `output`) is unambiguous about what each library *is*:

   | Capability (in public headers) | rasterbar 2.0.11 | rakshasa 0.13.8 |
   |---|---|---|
   | Public headers shipped | 273 | 64 |
   | Magnet URI (BEP 9) | `magnet_uri.hpp` | **absent** |
   | DHT | `dht_*.hpp` ×5 | `dht_manager.h` |
   | µTP / LEDBAT | `utp_stream.hpp`, `utp_socket_manager.hpp` | **absent** |
   | UPnP / NAT-PMP | `upnp.hpp`, `natpmp.hpp` | **absent** |
   | Local peer discovery | `lsd.hpp` | **absent** |
   | Streaming piece order | `set_sequential_download`, `set_piece_deadline` | **absent** |
   | Build integration | pkg-config + CMake package config | pkg-config only |

   Read the right way round, this **confirms** the scaling story rather than undercutting it. rakshasa's 64 headers are a *peer-scaling core* and nothing else: `choke_group.h`, `choke_queue.h`, `choke_status.h`, `connection_list.h`, `peer_list.h`, `throttle.h`, `resource_manager.h`, `poll_epoll.h`/`poll_kqueue.h`/`poll_select.h`. The reciprocation and connection machinery is the exposed API surface — which is exactly the thing it is famous for, deliberately factored out. rTorrent supplies everything else around it.

   **But "everything else" is precisely what first-class-in-a-browser is made of.** Magnet parsing, port mapping so it works without the user configuring a router, and local peer discovery are not garnish here; they are the difference between "it works when you click the link" and "it works after you read a wiki page". Their absence means writing BEP 9 and NAT traversal ourselves, which is option 3 wearing a disguise.

   **The structural argument is µTP, and it is specific to living inside a browser.** µTP's LEDBAT congestion control yields to competing traffic; plain TCP BitTorrent does not, and will saturate an uplink until interactive browsing in the *same application* becomes unusable. A torrent that makes its own browser unresponsive is not a first-class download by any definition. rasterbar has µTP; rakshasa's headers do not.

   **And `set_piece_deadline` is the feature that makes torrents first class rather than merely present.** Sequential and deadline-driven piece priority means a torrent can be served through the local proxy (§10) as a Range-served growing file — the same trick §11.3 already uses for live HLS. **Watch** on a torrent then works exactly like **Watch** on a stream, through the media affordance that already exists. That is a genuine capability, not a convenience, and it is unavailable in a handoff design at any price.

   **Recommendation: rasterbar** — chosen for feature completeness against this specific product, not for peak throughput. The scaling insight above is respected by *configuration* instead: raise `connections_limit` and the per-torrent cap well above the desktop-GUI defaults, lift the fd ceiling to match, and measure against the criterion recorded above rather than trusting either library's defaults. If measurement later shows rasterbar's peer handling genuinely capping out at a level that matters to real use, that is the moment to revisit — and the `download_source` seam below is what keeps that affordable.

   **Naming hazard worth writing down:** both packages install a pkg-config file named `libtorrent`. `pkg-config --cflags libtorrent` resolves to **rakshasa's**; rasterbar is `libtorrent-rasterbar`. Use its CMake package config (`find_package(LibtorrentRasterbar)`) and the ambiguity disappears.

   Cost of embedding, honestly: a large C++ dependency with its own threading model and state directory, and the whole operational surface below becomes ours.
2. **Hand off to an installed client** — the `player_launcher` pattern (§11.3) applied again. **Rejected for the desktop.** It is cheap and adds no dependency, but progress and completion happen outside the app, so the manager cannot show them and the tab-tree association is lost the moment the torrent starts. That is the direct negation of the first-class decision. It remains the likely shape on **Android** (§19), where background-execution limits make a long-lived seeding process impractical regardless of preference.
3. **Implement the protocol.** No.

**Keep the seam anyway.** A `download_source` interface — the same shape as the WebView backend (§19.2) — is still worth having, for three reasons that survive the decision: Android will need the handoff source, revisiting the engine after measurement should not be a rewrite, and it keeps the torrent dependency behind an interface so the rest of the download manager stays testable without it. The decision is which source ships first on desktop, not whether the seam exists.

**What BitTorrent forces that HTTP does not**, and why this needs a decision rather than just code:

- **Privacy — decided: no VPN feature, but visibility is not optional.** BitTorrent is not a fetch. It announces your IP to a tracker and to every peer, and peers can enumerate what you are fetching, which sits awkwardly beside §1's "data stays on the machine; only lightweight metadata ever leaves, and only when the user asks".

  **The decision is that Hydra does not ship VPN or proxy tunnelling**, and this is the correct call rather than a deferral: a VPN is a system-level concern, the OS and the router already do it properly, and a browser-internal reimplementation would be a smaller and leakier version of something the user can already configure once for every application. Building one would also be scope Hydra has no business owning.

  **What replaces it is an obligation, not nothing.** The first-class decision creates the risk here: making a torrent look exactly like an HTTP download is the goal, and it is also precisely what could mislead someone into thinking it *behaves* like one. So the honest requirement is that the difference stays visible — a distinct indicator on torrent rows in the download list, a plain-language explanation the first time a magnet link is opened (announces your address to strangers; the swarm can see what you are fetching), and never silently starting a torrent from a page's initiative.

  **And one cheap thing that makes the system-level choice actually work:** a `listen_interfaces` setting so a user who *has* configured a VPN at the OS layer can bind the torrent session to it, with announces refused if that interface disappears. That is a settings field wired to a libtorrent option, not a VPN implementation — it respects the decision above by making the layer the user chose reliable, instead of competing with it. Worth having precisely because it is a few lines.
- **Seeding is a policy question, not a default.** Uploading is continued participation after the user's task is done, with bandwidth, legal, and privacy consequences. Ratio, seed time, and whether to seed at all belong in the PolicyEngine as a feature like any other (§7.1), so the tri-state and global default govern it.
- **Inbound connectivity.** Ports, NAT traversal, UPnP. UPnP in particular means asking the router to open a hole, which is a security posture change and should not be silently on.
- **Torrents are not one file.** The manager's model is a job with a path, a size, and progress. A torrent is a set of files with per-file selection, out-of-order completion, and no meaningful single "bytes received / total" until metadata resolves (a magnet has none at the start). Either the manager's job model grows, or a torrent is one job that fans out internally.
- **Content risk.** Swarms carry whatever is in them. The app should not become an execution path for what it downloads, which mainly means keeping the existing rule that a download is written to disk and never opened automatically.
- **Android** (§19) is harder — background execution limits and SAF storage make long-lived seeding awkward — but the intent is still to **embed**, so a torrent is a first-class download there too. A separate side-loaded companion APK is the documented fallback if store policy makes an embedded engine untenable, not the plan; see **§19.6**. Either way it needs nothing from the desktop code beyond the `download_source` seam.
- **Distribution, not timidity.** Worth naming why no mainstream browser does this, so it is not mistaken for a technical obstacle we have solved cleverly: Opera shipped BitTorrent in 2006 and later dropped it, and the reasons since have been liability and app-store policy rather than engineering. That is a real constraint only where a store stands between us and users — it touches the Android target, not the desktop one, and it is a packaging question rather than a design one.

**Implementation order, once started.** Roughly: `download_source` seam → magnet/`.torrent` trigger surfaces wired to the existing manager → metadata-then-progress job model → resume via libtorrent's resume-data blob through `state_store` → seeding policy in the PolicyEngine → `set_piece_deadline` + local proxy for **Watch**. The first three are what make it *work*; the last is what makes it *first class*.

---

## 12. Filter-evolution loop

The AI diff/accept pipeline (Spine 3) pointed at the filter list instead of the tree.

**Status: passive half done.** `filter_signals` collects the passive signals below, `filter_list::evaluate` implements step 4's static rejection plus dry-run simulation, and `filter_dialog` is the step-5 accept UI writing into a separate AI-authored list. The user-driven element picker needs the script-injection and QWebChannel plumbing that arrives with the password manager (§13.2), so it is deferred to that step.

**1. Signal collection.** User-driven: an element-picker ("zap this") captures a leaked ad's selector, attributes, DOM snippet, and associated requests. Passive: the interceptor logs requests that slipped through but match heuristics (third-party, ad-serving shapes, high-frequency beacons) and flags likely anti-adblock overlays (a full-page element appearing right after load).

**2. Context serialization.** The payload — page URL, offending element's selector/attributes/snippet, candidate requests, and which active filters failed — with personal data stripped from the snippet.

**3. AI proposal.** Rules in standard EasyList / uBO syntax — network (`||ads.example.com^`) and cosmetic (`example.com##.ad-banner`) — each tagged with scope (site-specific vs generic) and a breadth/confidence estimate.

**4. Validation with dry-run — the safety core.** Statically reject dangerously broad rules (hiding generic tags globally, matching a whole TLD, blocking a first-party essential). Then *simulate* each rule against the page's captured requests/DOM and show exactly what it would block or hide.

**5. Diff and accept.** Proposed rules render as a diff against the current set, each individually acceptable with its dry-run preview. Accepted rules merge into a **separate AI/user-authored list**, kept apart from imported EasyList so scheduled upstream updates never clobber custom rules (de-dup on import).

**Regression feedback.** A small "known-clean" page set is re-run when filters change to catch a new rule that broke a page (false-positive hide), paired with a one-click "this rule broke the site" revert that feeds back as a negative signal. Anti-adblock countermeasures are just another rule category the AI proposes into when the user flags a nagging site — measured and rule-based, no separate subsystem.

---

## 13. Password manager (KeePassXC integration)

"That just works" points at a specific design choice: **do not build a password manager, and do not parse `.kdbx` or ever touch the master password.** Become a first-class client of the vault the user already runs. (KeePassX proper is unmaintained and has no browser-integration protocol; the maintained fork **KeePassXC** ships the protocol its official extensions use, and is the target here.)

**Status: done**, unexercised against a live KeePassXC. `keepass_protocol` holds the wire format and nonce discipline as pure functions, `crypto_box` is a thin libsodium shim (optional at build time), `keepass_bridge` is the socket client, and `autofill_controller` plus the injected script are the §13.2 layer. Autofill is a policy feature. Not built: the entry-picker UI, `set-login`, `generate-password`, and encrypted-at-rest storage of the association key.

### 13.1 Approach — speak the KeePassXC-Browser protocol

KeePassXC exposes a local **BrowserServer** that its official extensions talk to. Because Browser Overlord *is* a native app that owns its engine, it can be that client directly:

- **Transport** — connect straight to the Unix domain socket (`$XDG_RUNTIME_DIR/org.keepassxc.KeePassXC.BrowserServer`). The `keepassxc-proxy` helper only exists to bridge stdio for sandboxed browser extensions; a native app skips it and connects to the socket itself.
- **Crypto** — the protocol is end-to-end encrypted with libsodium `crypto_box` (X25519 + XSalsa20-Poly1305): a `change-public-keys` handshake, then per-message nonces.
- **Association** — associate once; the user confirms and names the connection inside KeePassXC, which returns an association id + key. Store it encrypted at rest and `test-associate` on launch, so the pairing survives restarts.
- **Requests** — `get-logins(url)` returns matching entries (KeePassXC does its own URL matching and can prompt the user per site); `set-login` creates/updates an entry when the user submits a new credential; `generate-password` asks KeePassXC to generate per its own policy; `get-databasehash` plus lock detection tells us database state.
- **KeePassXC stays the source of truth** — it holds the vault, the master password, and the unlock. If the database is locked, requests fail and we prompt the user to unlock KeePassXC. We never take the master password ourselves.

This is *why* it "just works": KeePassXC is already an unlocked local daemon in the user's session, so we add no new vault, no crypto we own, and nothing leaves the machine — a perfect fit for the local-first ethos.

### 13.2 Autofill mechanics in Qt WebEngine

Since the app owns the engine, autofill is clean plumbing rather than scraping: inject a content script into every page via `QWebEngineScript` (isolated world), and wire a bidirectional channel with `QWebChannel` exposing a C++ bridge object to the page. The content script detects login forms (username/email/password fields, `autocomplete` attributes, multi-step flows), asks the bridge for credentials for the current origin, the bridge queries KeePassXC, and the returned matches are filled. UI affordances: a key icon in the field/toolbar, a dropdown to pick among multiple entries, a save-prompt on new-login submit, and generate-password on registration fields. Field detection is the fiddly "just works" part — the same challenge every password manager faces — handled with standard signals (`type=password`, `current-password`/`new-password` autocomplete, aria labels).

### 13.3 Security and policy integration

We never store or see the master password. The association key is stored encrypted at rest (Secret Service / libsecret, or the app's encrypted config). Origin matching is strict to resist phishing autofill: KeePassXC's URL matching plus our own exact-origin gate, care with cross-origin iframes, an HTTPS-only-fill option, and no auto-submit by default. Autofill folds into the **PolicyEngine** as just another per-site feature — `Autofill = Allow | Block | Default` — so the tri-state URL-bar editor and the global default mode govern it like everything else, and a policy-blocked site is never autofilled.

### 13.4 Optional offline fallback

If KeePassXC isn't running and only a `.kdbx` file exists, an *optional, opt-in* read-only mode could open the file with a kdbx-parsing library — but that means the app handles the master password and crypto itself, which is more attack surface and less "just works." The recommendation is KeePassXC-as-daemon as the primary path, with direct-kdbx an explicit fallback the user must enable.

**Fold-in:** a new `KeePassBridge` service (protocol client) plus an autofill content-script/`QWebChannel` layer. It consumes the PolicyEngine (the Autofill feature) and the engine's script-injection plumbing; KeePassXC remains the vault. It introduces `QWebEngineScript`/`QWebChannel` wiring not previously used elsewhere, so that plumbing is new even though the vault is not.

---

## 14. Persistence and on-disk layout

Everything the user owns is inspectable files under the app's config/data directory:

- **Tree canonical file** — the id-tagged outline (§4.4); source of truth for structure and order.
- **State blobs** — per-suspended-tab serialized scroll/form/history, keyed by node id.
- **Policy rules** — the packed-tri-state ruleset as JSON (§7.1).
- **Filter lists** — imported EasyList (refreshed on schedule) and the separate AI/user-authored list (§12).
- **Downloads index** — queue/history, cross-referenced to node ids.
- **App config** — provider selection/credentials reference, kiosk presets, default policy modes.
- **Credential association** — the KeePassXC-Browser association id/key, stored encrypted (Secret Service / libsecret or app-encrypted config). Never the vault, the entries, or the master password — those stay in KeePassXC.

Writes to the tree file are debounced on change; state blobs are written on suspend; rule/filter files on edit.

---

## 15. Cross-cutting concerns

**Threading.** Network fetches, AI provider calls, stream assembly, and ffmpeg muxing all run off the UI thread; results marshal back to the Qt main thread for model updates. The UI never blocks on a provider round-trip — proposals arrive asynchronously into the shadow-tree overlay.

**Privacy.** Local-first by default (recommended local-model provider), metadata-only egress, a review-before-send gate, human-readable on-disk state, and an off-the-record profile option for kiosk.

**Platform caveats.** X11 only for reliable behavior; force `QT_QPA_PLATFORM=xcb` and run under XWayland where the session is Wayland. Watch HiDPI/device-pixel-ratio interactions with kiosk scaling. The graphics-proxy rendering caveat (§8.3) gates geometric-scale kiosk.

**Dependencies.** Qt 6 + Qt WebEngine (license note §2), optional `ffmpeg` (media remux), optional local model runtime (Ollama/llama.cpp), optional local proxy component, `libsodium` (KeePassXC-Browser protocol crypto), and a running **KeePassXC** for the password manager (note: KeePassX proper is unmaintained and lacks the browser protocol), and one or more external media players (mplayer, mpv, VLC, smplayer, ffplay — auto-detected from `PATH`, none assumed present) for the open-in-player handoff.

---

## 16. Risks and open questions

The interceptor's request-only visibility (inline scripts, response headers) caps policy fidelity short of uMatrix-grade without CEF. The `QGraphicsProxyWidget` rendering path for geometric kiosk scaling must be validated per-GPU early. The web-session AI backend is fragile and should not be the default. The provider default is **decided**: local-first, with an external provider used when configured (Claude the default external adapter, others pluggable later), and review-before-send defaulting on whenever an external provider is active. What remains a *spike* rather than an open decision is whether a small local model produces genuinely useful tree reorganizations — testable once the serialize→diff pipeline exists. Password-manager autofill inherits the field-detection fragility every password manager faces, and demands strict origin matching to avoid filling on phishing lookalikes; it also depends on KeePassXC (not KeePassX) being installed and unlocked.

---

## 17. Suggested build order

1. **Shell + engine + tree model** — `QWidget`/`QSplitter`, one `QWebEngineView`, `TabTreeModel` loading the canonical file, basic open/suspend lifecycle.
2. **Sort/filter proxy layer** — hierarchical and flat proxies, the four sort modes, search.
3. **Interceptor + PolicyEngine** — rule model, `decide()`, the enforcement hooks, the URL-bar tri-state editor.
4. **Kiosk mode** — fullscreen chromeless, reflow-zoom scale, crop-via-clip, fit modes; geometric scale behind a tested flag.
5. **AIProvider + diff/accept** — local-model backend first, tree serialization, the non-destructive reorganizer with the no-node-left-behind invariant.
6. **Interceptor consumers** — media detector + download manager, then the filter-evolution loop reusing the diff UI.
7. **Password manager** — `KeePassBridge` speaking the KeePassXC-Browser protocol over the BrowserServer socket, the `QWebChannel` autofill content-script layer, and Autofill wired in as a PolicyEngine feature.

Two cross-platform notes fold into this order. **Step 3.5 (recommended, before step 4 grows the view code):** refactor the shell so it talks to a `WebViewBackend` interface instead of `QWebEngineView` directly (§19.2) — no user-visible change, but the prerequisite that keeps the Android port from becoming a rewrite. **Android phase (deferred):** after the desktop feature set is complete, add the Android `WebViewBackend` (System WebView), the adaptive drawer layout, and the platform backends for player/password/download (§19) — a distinct phase, not interleaved with desktop work.

Each step builds on a spine the previous steps already established, so no subsystem is blocked on another's internals.

---

## 18. Component summary

| Subsystem | Responsibility | Key Qt classes | Shares |
|---|---|---|---|
| Shell | Window, menus, sidebar, view stack | `QWidget`, `QVBoxLayout`, `QMenuBar`, `QToolBar`, `QSplitter`, `QTreeView`, `QStackedWidget`, `QStatusBar`, `QWebEngineView` | — |
| TabTree model | State: folders/tabs/links, lifecycle | `QAbstractItemModel` | Spine 2 |
| Sort/filter | Powerful sorting + search | `QSortFilterProxyModel`, flattening proxy | Spine 2 |
| PolicyEngine | Per-site security rules + enforce | `QWebEngineSettings`, `QWebEngineCookieStore`, `QWebEnginePage` | Sink |
| Interceptor | Request sensor + blocking | `QWebEngineUrlRequestInterceptor` | Spine 1 |
| Kiosk | Fullscreen scale/crop presentation | `QGraphicsView`/`QGraphicsProxyWidget` (optional), `setZoomFactor` | Policy preset |
| AIProvider | Pluggable AI backend | (custom) + `QWebEngineView` for web-session | Spine 3 |
| Reorganizer | AI tree sorting, non-destructive | diff/accept over TabTree model | Spines 2+3 |
| Media detector | interceptor + optional local proxy, `QWebEngineDownloadRequest` | Spine 1 |
| Filter evolution | AI-evolved ad/annoyance filters | diff/accept over filter lists | Spines 1+3 |
| Password manager | Autofill via KeePassXC, no local vault | `QWebEngineScript`, `QWebChannel`, `libsodium`; KeePassXC BrowserServer socket | PolicyEngine + engine |

The whole system is seven subsystems resting on three spines and one sink.

---

## 19. Cross-platform: Desktop and Android

Both Linux/X11 and Android are first-class targets, both built with **Qt Widgets only** (no QML), each behaving the way its platform expects. Desktop ships first; Android is deferred until the desktop version is complete. The point of writing this section now is that a handful of *seams* must exist in the desktop code from the start, or the Android port becomes a rewrite. The good news: the platform-neutral core is most of the value and ports unchanged.

### 19.1 What is platform-neutral (reused as-is)

The data and logic layers carry over verbatim: the `TabTreeModel`, the canonical outline format and `TreeOutline` parser, the `PolicyEngine` rule model (packed tri-states, precedence, JSON), the `StateStore` blob model, the sort/filter proxy, the AI serialization and diff/accept pipeline, and the filter-list format. None of these touch platform APIs. That is the majority of the interesting code, and it is why the abstraction is worth it — the seams are thin edges around a shared core.

### 19.2 The critical seam — a WebView backend

Qt WebEngine does not exist on Android, so the single most important structural rule is: **the shell never references `QWebEngineView` directly.** It talks to a `WebViewBackend` interface — load URL, navigate history, serialize/restore session state, apply per-page policy toggles, receive navigation/permission callbacks, expose a request-interception hook. Two implementations:

- **Desktop** — wraps `QWebEngineView` / `QWebEnginePage` (what steps 1–3 already do, to be refactored behind the interface).
- **Android** — wraps the platform **System WebView** (`android.webkit.WebView`), reached via `QtWebView` and/or JNI. Android's WebView has its own hooks — `shouldInterceptRequest`, `CookieManager`, and `WebSettings` for JavaScript/images/zoom — so a *reduced* but real version of the PolicyEngine's enforcement is achievable there. It is not Chromium-with-CDP: expect coarser control (limited per-origin script blocking, no rich interceptor resource typing), and document the reduced fidelity rather than pretending parity.

Introducing this seam is the recommended **step 3.5** on the desktop side (a refactor with no user-visible change) precisely so Android later slots in as one new backend class instead of a rewrite.

**Status: done.** The desktop code now has it. `web_view_backend` is the per-page interface and `web_view_factory` makes views and owns the profile-wide machinery behind them; `qtwebengine_view`, `qtwebengine_factory` and `qtwebengine_interceptor` are the desktop implementation and the only files in the tree that name Qt WebEngine. The decision half of interception was split out into a neutral `request_filter` — what to block is policy plus a host list and is the same on every platform, so Android's `shouldInterceptRequest` reuses it rather than reimplementing it (§19.5). `main()` is the one place that names a concrete backend, injecting the factory and the policy engine into the shell.

### 19.3 Adaptive layout — one widget tree, two form factors

The desktop layout (a horizontal `QSplitter` with the tree beside the web view) does not fit a phone. Using the same Qt Widgets, the shell switches layout by screen size/DPI: on a small screen the splitter collapses to a **single pane with the tree as a slide-in drawer** (hamburger / edge-swipe), the web view filling the screen, and navigation following Android's up/back model. Touch idioms replace mouse ones — larger hit targets, long-press for the context menu that is right-click on desktop, swipe to open the drawer, no hover affordances. This is achievable with Widgets but is real work and is the reason "first-class Android with Widgets only" is ambitious: Widgets are not touch-idiomatic by default, so the adaptive layer earns the "behaves natively" claim.

### 19.4 Platform behaviors that must be correct on each

- **Back button / navigation.** On Android the hardware/gesture Back must walk web history, then close the drawer, then go up the tree — never insta-quit. On desktop, Back is a toolbar action. Same intent, platform-correct binding.
- **App lifecycle.** Android suspends and kills apps freely, so the app persists aggressively on `Qt::ApplicationSuspended` (the suspend/restore + `StateStore` already built for desktop pays off directly here). Desktop persists on close/debounce.
- **Storage.** Paths come from `QStandardPaths` (AppDataLocation), which resolves to app-scoped storage on Android and `~/.local/share` on desktop — the tree file, `policy.json`, and `state/` follow automatically. Downloads use SAF / MediaStore on Android vs a plain path on desktop.

### 19.5 Feature subsystems, per platform

Each edge subsystem gets a platform backend behind the same interface the desktop already uses:

- **External player (§11.4).** Desktop launches mplayer/mpv/VLC by command. Android has no such binaries; the handoff becomes an **Intent** (`ACTION_VIEW` with the clear stream URL + MIME) so the user's installed player (VLC/MX Player) opens via the system chooser.
- **Password manager (§13).** Desktop speaks the KeePassXC-Browser protocol to a running KeePassXC. Android has no KeePassXC daemon; instead the app integrates with the **Android Autofill Framework**, letting the OS route to the user's chosen provider app (KeePassDX / Keepass2Android). Different mechanism, same "the vault lives elsewhere, we never hold the master password" principle.
- **Kiosk (§8).** Desktop uses fullscreen + chromeless + scale/crop. Android adds **immersive mode** and, for locked deployments, **lock task mode / screen pinning**; the scale/crop logic applies inside the WebView on both.
- **Ad/filter enforcement & media detection (§11/§12).** On desktop these ride the Qt interceptor; on Android they ride the System WebView's `shouldInterceptRequest`, with the same PolicyEngine deciding — reduced resolution, same rules.

### 19.6 BitTorrent on Android: a separate side-loaded APK

**Status: contingency, not the plan.** The intent is to **embed the engine on Android too**, exactly as on desktop, so that a torrent is a first-class download on every platform rather than one that degrades into a second app. This section is kept because the split is the fallback if — and only if — store policy makes shipping the engine inside the browser untenable on that platform. Everything below is therefore written as *what we would do if forced*, not as what we intend. §11.4 decided that desktop embeds a torrent engine and that handoff was rejected there because progress and tab-tree association are lost the moment the torrent leaves the app. This section is about the observation that on Android those two objections mostly evaporate — *if we write the other end ourselves*.

**It is a third option, not the handoff §11.4 rejected.** Handing a magnet to LibreTorrent or Flud is a one-way door: the browser gets no progress, no completion signal, and no way to associate the result with the node it came from. A **companion app we ship** is a different proposition, because we define the interface. Progress comes back, the association survives, and the download list can show a torrent beside an HTTP file exactly as on desktop. The user-visible result is closer to embedding than to handoff; only the process boundary differs.

**What still argues for embedding, even on Android.** Two installs is the objection that outranks the rest: §11.4's whole point is that a torrent behaves like every other download, and a feature that first requires the user to find and side-load a second APK is not that. Version skew, a second toolchain, two entries in the launcher and two notification streams all follow from the boundary and none of them exist without it. So the default remains embed, and the reasons below are the price of the fallback rather than an argument for taking it.

**What the boundary would buy, if policy forces it:**

- **Background execution.** This is the strongest reason and it is not about policy at all. Android aggressively curtails background work, and a browser is exactly the kind of app the system expects to suspend when it leaves the foreground. Seeding — and even a long download — needs a **foreground service with an ongoing notification**, which is a natural thing for a dedicated transfer app to be and an awkward, battery-suspicious thing for a browser to claim. The companion keeps running while the browser is swapped out or killed.
- **Attribution the user can act on.** Android reports battery and mobile-data usage per app. A separate app tells the truth: the user can see what the torrenting cost and restrict it — background data off, unmetered-only — with the OS controls they already know, rather than through settings we would have to invent.
- **Process isolation against hostile input.** A torrent engine parses data from arbitrary strangers — peer handshakes, bencoded metadata, DHT traffic. That is a large attack surface in a memory-unsafe library, and putting it in its own process, in its own app sandbox, is defence in depth of exactly the kind Chromium's own architecture is built on. A bug there should not be a bug in the thing holding the user's sessions and passwords.
- **Distribution — the only reason that would actually trigger the split.** Standalone torrent clients do exist on Google Play, so there is no flat prohibition, but a *browser* with a built-in engine is a less predictable review category. If that turns out to block distribution, splitting keeps the browser conventionally installable while the companion goes via F-Droid or direct APK, and a policy problem with one cannot take down the other. **Check this against current Play policy before acting on it** — and note that F-Droid-only distribution of a single embedded app is also a legitimate answer, and a simpler one than splitting.

**How the two would talk.** Android offers a spectrum, and the useful answer is a combination rather than a single mechanism:

- **Start a job:** an `Intent` to the companion, carrying the magnet or `.torrent` plus our node id as an extra. Simple, and it works even if the companion is not running.
- **Observe:** a **`ContentProvider`** in the companion exposing one row per job. The browser queries it for the download list and registers a `ContentObserver` for live progress. This maps onto the existing model well — a cursor of jobs *is* the job list — and it survives both processes being restarted, which a bound-service callback does not.
- **Collect the result:** `ContentProvider.openFile()` hands back a file descriptor. In the SAF era this is the clean way to get at the finished file without the browser holding broad storage permissions.
- **Watch while downloading:** the companion runs a small **loopback HTTP server** over the growing file, and the browser points the player at it — which is the desktop `local_proxy` design (§10, §11.3) unchanged, and the natural home for the sequential-piece trick from §11.4.

A **bound service with AIDL** is the classic answer here and would also work; the reason to prefer the provider is that progress observation is inherently a "watch this list" problem, and `ContentObserver` already is that, with lifecycle handling we would otherwise write ourselves.

**The security problem this creates, which must not be waved through.** An exported service or provider that starts downloads is, by construction, a way for *any* app on the device to make the user download and seed arbitrary content. It must be protected. A `signature`-level custom permission is the correct mechanism and costs nothing at runtime, but it requires both APKs to be signed with the same key, which constrains distribution — notably, an F-Droid build signed by F-Droid's per-app keys will not automatically satisfy it. Either the pairing is preserved via developer-signed reproducible builds, or the companion falls back on **confirming each new torrent in its own UI**, which is distribution-agnostic and honest but adds a tap. Decide this before writing the manifest, not after.

**The costs, which are why this stays a contingency:**

- **Two installs is in tension with "first class".** §11.4's whole point is that a torrent should behave like every other download. On Android it would behave like every other download *after the user installs a second app*, which is a meaningfully worse experience than on desktop. The browser needs a graceful story for the un-installed case — explain, offer a link, and fall back to the system chooser so a magnet still opens *something*.
- **Version skew.** Two independently updatable APKs need an interface version exchanged on first contact and a refusal path when they disagree. This is the standard cost of an IPC boundary and it does not go away.
- **A second toolchain.** The companion is most naturally Kotlin plus libtorrent through the NDK, not Qt — so it is a separate codebase with a separate build, not another target of this one.

**What this changes in the code today: nothing, which is the point.** The `download_source` seam (§11.4) already describes this: an `android_companion_source` implements the same interface, reporting `seeds = true`, `public_participation = true`, `multi_file = true` and a `max_concurrent` the companion decides. The manager neither knows nor cares that the bytes are moving in another process — it owns the queue, consent and the job records either way. The seam was built for the desktop/Android split and this is the first real test of it passing.

**Decision rule.** Build Android with the engine embedded. Reach for the split only on evidence that the store will not carry it, and prefer changing distribution channel over changing architecture. If the split does happen, the background-execution and process-isolation gains above are real consolations — but they are not worth paying two installs for on their own.

**A desktop echo worth noting but not acting on.** The process-isolation argument is not Android-specific: running the torrent engine out-of-process on desktop too would give the same hardening against hostile peer input, behind the same seam, without changing anything above it. That is a possible later refinement, not a reason to complicate the first desktop implementation.

### 19.7 Honest constraints

Qt Widgets on Android is supported but is the less-trodden path in the Qt ecosystem (QML is the norm), so the adaptive-layout and touch work is non-trivial and should be scoped as its own phase. The Android System WebView cannot match Chromium/CDP fidelity, so a few desktop-grade controls will be coarser or unavailable on Android; that is a documented difference, not a bug. And the reparenting/foreign-window discussion (§2) is desktop/X11-only and simply does not apply on Android — which is fine, because the design already chose the own-engine path.

**Net:** keep the core platform-neutral, put every platform-specific edge behind an interface (WebView, player, password, download, layout), build the desktop implementations now, and add Android implementations as a later phase. The one thing not to defer is the *seam* for the web view — that belongs in the desktop code (step 3.5) before it grows further. Every feature you asked for is a consumer of infrastructure that already exists for another feature — which is what keeps it compact.

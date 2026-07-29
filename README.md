# Hydra — build-order steps 1–3 skeleton

Working name (rename freely — it's one `project()` line in `CMakeLists.txt` plus
the `hydra` target). This covers **steps 1–3** of the architecture doc's build
order: the shell + engine + tree model, the suspend/restore lifecycle and
persistence, and the PolicyEngine + request interceptor (the security spine). It
compiles and runs; the remaining subsystems (kiosk, AI, media, password
manager) are not here yet, but the seams for them are marked in comments.

## What works

- **Shell** — `QMainWindow` with a horizontal splitter: tab tree on the left,
  a `QStackedWidget` of chrome-less `QWebEngineView`s on the right.
- **Tree model** — `TabTreeModel` (a `QAbstractItemModel`) loads the canonical
  outline file `sample-tree.txt`, exposes custom sort roles, and keeps an id
  index for O(1) node lookup.
- **Powerful sorting** — the toolbar "Sort" box switches between *Tree order*,
  *Title A–Z*, *Newest (created)*, and *Recently seen*, via `TreeSortProxy`
  (folders grouped first). A "Search tree" box filters live, keeping ancestors
  of matches visible.
- **Tab lifecycle** — unopened → open (live view) → suspended. Opening a node
  creates or restores a view; a live-view cap (`kMaxLiveViews = 4`) suspends the
  least-recently-used tab automatically. Right-click a tab for Open / Suspend.
- **State blobs** — a suspended tab's navigation history is serialized via
  `StateStore` into a `state/` sidecar directory keyed by node id, and restored
  on reopen. Live tabs are suspended-to-disk on close and restore next launch.
- **Persistence** — structural changes write the canonical tree file back
  (debounced 1.5 s), plus a save on close.
- **Visual state cues** — open tabs bold, suspended italic, unopened muted.
- **Basic nav** — back / forward / reload and an address bar drive the current
  view.
- **PolicyEngine** — per-site rules as packed 2-bit tri-states (`Policy.h`),
  precedence (exact host > `*.domain` > global default), and JSON persistence to
  `policy.json`. Features: JavaScript, cookies, third-party cookies, ads,
  popups, images, autoplay, location, camera, mic, notifications, referer.
- **Interceptor + cookie filter** — one `QWebEngineUrlRequestInterceptor` on the
  shared profile blocks ads (seed host list), per-origin scripts, and per-site
  images, and strips Referer per policy; the cookie filter enforces cookie and
  third-party-cookie rules.
- **Per-page enforcement** — JS / images / autoplay / popups applied to each
  page on navigation; geo/cam/mic/notification permission prompts auto-answered
  from policy.
- **Shield editor** — the "Shield" toolbar button drops down a per-site tri-state
  editor (`SitePolicyDialog`) with scope this-host / this-domain / global; edits
  write straight into the PolicyEngine and reload the page.

## Requirements

- Qt 6 with the **Widgets** and **WebEngineWidgets** modules
  (e.g. Debian/Ubuntu: `qt6-base-dev qt6-webengine-dev`;
  Arch: `qt6-base qt6-webengine`).
- CMake ≥ 3.19, a C++17 compiler.
- **X11 / XWayland** — the app forces `QT_QPA_PLATFORM=xcb` (see `main.cpp`),
  matching the X11-only design decision.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hydra                 # loads ./sample-tree.txt (copied next to binary)
./build/hydra my-tree.txt     # or point it at your own outline file
```

## File format (canonical tree)

Two spaces per depth level; fields separated by ` | `:

```
- [f0] folder | Work
  - [a1] open | Qt Documentation | https://doc.qt.io | created=... | seen=...
  - [a2] unopened | Wikipedia | https://www.wikipedia.org
```

First field is the node type (`folder` / `open` / `unopened` / `suspended`);
trailing `created=` / `seen=` are optional ISO-8601 timestamps.

## Where the next steps plug in

- `MainWindow` holds the shared `QWebEngineProfile` — the request interceptor,
  cookie filter, and download handler attach there (steps 3 and 6).
- `TabTreeModel` is the single source of truth the AI reorganizer and the
  persistence layer build on (step 5).
- Suspend/restore lifecycle (open ⇄ suspended state blobs) is the next
  increment on top of `openNode()`.

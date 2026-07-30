# Hydra — build-order steps 1–3 skeleton

Working name (rename freely — it's one `project()` line in `CMakeLists.txt` plus
the `hydra` target). This covers **steps 1–3** of the architecture doc's build
order: the shell + engine + tree model, the suspend/restore lifecycle and
persistence, and the policy_engine + request interceptor (the security spine).
The remaining subsystems (kiosk, AI, media, password manager) are not here yet,
but the seams for them are marked in comments.

See `project.md` for conventions, current status, and what to do next;
`docs/architecture.md` for the full design.

## What works

- **Shell** — classic desktop furniture in a plain `QWidget` (not a
  `QMainWindow` — see `docs/architecture.md` §6): menu bar, compact toolbar, a
  horizontal splitter with the tab tree on the left and a `QStackedWidget` of
  chrome-less `QWebEngineView`s on the right, and a status bar.
- **Menus** — File (save tree, quit), Go (back/forward/reload), View (sort mode,
  expand/collapse all), Tools (site controls), Help. The sort actions drive the
  toolbar combo, so the two stay in sync.
- **Status bar** — transient messages (current URL, menu status tips) on the
  left, a permanent live-view count on the right.
- **Tree model** — `tab_tree_model` (a `QAbstractItemModel`) loads the canonical
  outline file `sample-tree.txt`, exposes custom sort roles, and keeps an id
  index for O(1) node lookup.
- **Powerful sorting** — the toolbar "Sort" box switches between *Tree order*,
  *Title A–Z*, *Newest (created)*, and *Recently seen*, via `tree_sort_proxy`
  (folders grouped first). A "Search tree" box filters live, keeping ancestors
  of matches visible.
- **Tab lifecycle** — unopened → open (live view) → suspended. Opening a node
  creates or restores a view; a live-view cap (`k_max_live_views = 4`) suspends
  the least-recently-used tab automatically. Right-click a tab for Open /
  Suspend.
- **State blobs** — a suspended tab's navigation history is serialized via
  `state_store` into a `state/` sidecar directory keyed by node id, and restored
  on reopen. Live tabs are suspended-to-disk on close and restore next launch.
- **Persistence** — structural changes write the canonical tree file back
  (debounced 1.5 s), plus a save on close.
- **Visual state cues** — open tabs bold, suspended italic, unopened muted.
- **Basic nav** — back / forward / reload and an address bar drive the current
  view.
- **policy_engine** — per-site rules as packed 2-bit tri-states (`policy.h`),
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
  editor (`site_policy_dialog`) with scope this-host / this-domain / global;
  edits write straight into the policy_engine and reload the page.

## Requirements

- Qt 6 with the **Widgets** and **WebEngineWidgets** modules
  (e.g. Debian/Ubuntu: `qt6-base-dev qt6-webengine-dev`;
  Arch: `qt6-base qt6-webengine`).
- CMake ≥ 3.19, a C++17 compiler.
- **X11 / XWayland** on Linux — `main.cpp` forces `QT_QPA_PLATFORM=xcb` there
  (unless the environment already set it), matching the X11-only design
  decision. The forcing is guarded to desktop Linux; other platforms keep Qt's
  own default platform plugin.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2       # name a number: bare -j is unlimited under Make
./build/hydra                 # loads ./sample-tree.txt (copied next to binary)
./build/hydra my-tree.txt     # or point it at your own outline file
```

The WebEngine-dependent sources have not yet been compiled against a real Qt
WebEngine — see `project.md` → "Build-verification state" before you start.

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

- `main_window` holds the shared `QWebEngineProfile` — the request interceptor,
  cookie filter, and download handler attach there (steps 3 and 6).
- `tab_tree_model` is the single source of truth the AI reorganizer and the
  persistence layer build on (step 5).
- Suspend/restore lifecycle (open ⇄ suspended state blobs) is the increment
  `open_node()` builds on.

## Licence

GPL-3.0-or-later — see `LICENSE`.

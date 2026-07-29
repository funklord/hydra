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

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hydra                 # loads ./sample-tree.txt
./build/hydra my-tree.txt     # or a custom outline file
```

Requires Qt 6 with **Widgets** and **WebEngineWidgets** (Arch: `qt6-base
qt6-webengine`; Debian/Ubuntu: `qt6-base-dev qt6-webengine-dev`), CMake ≥ 3.19,
C++17. X11 / XWayland only (the app forces `QT_QPA_PLATFORM=xcb`).

## Build-verification state

Partially verified. Against Qt 6.8.2 (Widgets/Core/Gui — `qt6-webengine` is not
installed), every translation unit compiles, all six `Q_OBJECT` headers pass
`moc`, and the whole shell **links and runs**: substituting non-functional stub
headers for the eight Qt WebEngine classes the code touches produces a binary
that loads `sample-tree.txt` and renders the real UI offscreen
(`QT_QPA_PLATFORM=offscreen`) — toolbar, splitter, populated tree with its
bold/italic/muted state cues, and the placeholder pane. So the tree model, the
outline parser, the sort proxy, and the layout are exercised, not just parsed.

**What that does not prove:** the stub headers were written to match what the
code expects, so they confirm internal consistency and say nothing about
whether the real Qt WebEngine API agrees. `main_window.cpp` and
`request_interceptor.cpp` are the two files carrying that risk. Install
`qt6-webengine` and do a full `cmake --build` as the first action — the three
most likely trouble spots, all flagged in code comments:

- `QWebEngineHistory` `QDataStream` operators (`main_window.cpp`,
  suspend/restore).
- `QWebEngineCookieStore::FilterRequest` field names (`main_window.cpp`).
- `featurePermissionRequested` is deprecated in Qt 6.8+ (still functional);
  `QWebEnginePermission` is the migration target once the floor is 6.8.

## What is implemented (build-order steps 1–3)

| Area | Files | Notes |
|---|---|---|
| Shell / window | `main_window.{h,cpp}`, `main.cpp` | plain `QWidget`: menu bar, toolbar, splitter, status bar; tree + stacked chrome-less web views |
| Tree model | `tab_tree_model.{h,cpp}`, `node.h` | `QAbstractItemModel`, sort roles, id index |
| Canonical file | `tree_outline.{h,cpp}`, `sample-tree.txt` | id-tagged indented outline parser/serializer |
| Sorting / search | `tree_sort_proxy.{h,cpp}` | tree-order / title / created / last-seen + live search |
| Lifecycle | `main_window.cpp`, `state_store.{h,cpp}` | open⇄suspended, history blobs, LRU live cap (4) |
| Policy model | `policy.{h,cpp}`, `policy_engine.{h,cpp}` | packed 2-bit tri-states, precedence, JSON |
| Enforcement | `request_interceptor.{h,cpp}`, `main_window.cpp` | interceptor, cookie filter, per-page settings, permissions |
| Site editor | `site_policy_dialog.{h,cpp}` | shield popup, scope this-host/domain/global |

Persistence: `policy.json`, `state/<id>.blob`, and the tree file all sit next to
the outline file passed on the command line.

## What is next (in order)

- **3.5 — WebViewBackend seam.** Refactor so the shell talks to an interface,
  not `QWebEngineView` directly (arch §19.2). No user-visible change; unblocks
  Android without a rewrite. Do this before step 4 grows the view code.
- **4 — Kiosk mode.** Fullscreen chromeless, reflow-zoom scale, crop-via-clip,
  fit modes, idle-reset/watchdog (arch §8).
- **5 — AI reorganizer.** `AIProvider` (local-first, Claude as default
  external), tree serialization, non-destructive diff/accept with the
  no-node-left-behind invariant (arch §9).
- **6 — Interceptor consumers.** Media detector, download manager, and the
  filter-evolution loop (arch §11–§12), reusing the interceptor and the
  diff/accept UI.
- **7 — Password manager.** KeePassXC-Browser protocol bridge + `QWebChannel`
  autofill; Autofill as a policy_engine feature (arch §13).
- **Android phase (deferred).** System WebView backend, adaptive drawer layout,
  Intent-based player handoff, Android Autofill, SAF downloads (arch §19).

## Open decisions and risks

- **Decided:** AI provider = local-first, Claude default external, others later
  (arch §9.1).
- **Spikes to run early:** the `QGraphicsProxyWidget` geometric-scale path for
  kiosk (may render black on some GPUs), and local-model tree-sort quality.
- **Interceptor limits:** request-only — no inline-script blocking, no response
  headers; the optional local proxy (arch §10) is the upgrade path.
- **Thread note:** `request_interceptor::interceptRequest` may run off the UI
  thread; the policy_engine is only mutated on the UI thread and reads tolerate
  a stale snapshot. Revisit if mutation frequency grows.

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

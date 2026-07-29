# CLAUDE.md — Hydra

Project memory for Claude Code. Read `docs/architecture.md` (the full design) and
`HANDOFF.md` (current status + next steps) before making changes.

## What this is

Hydra (working name) is a Linux/X11 desktop browser built on **Qt 6 Widgets**
(no QML) and **Qt WebEngine**. It presents a side-tree of tabs/links over its own
embedded Chromium, with a per-site security policy engine, and (planned) kiosk
mode, AI tree-sorting + ad-filter evolution, a media detector with external-player
handoff, and a KeePassXC-based password manager. **Android is a planned
first-class target, deferred until desktop is complete** (see `docs/architecture.md` §19).

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

## IMPORTANT: not yet build-verified

This code was written and carefully reviewed but has **not been compiled** (the
authoring environment had no Qt6). Your first task on takeover: build it, fix any
compile errors, run it. Most-likely trouble spots, all noted in code comments:
- `QWebEngineHistory` `QDataStream` operators (`MainWindow.cpp`, suspend/restore).
- `QWebEngineCookieStore::FilterRequest` field names (`MainWindow.cpp`).
- `featurePermissionRequested` is deprecated in Qt 6.8+ (still functional).

## Conventions

- Qt 6, C++17, Qt **Widgets only** — do not introduce QML/Qt Quick.
- Keep the platform-neutral core (`TabTreeModel`, `TreeOutline`, `PolicyEngine`,
  `StateStore`, sort proxy) free of platform APIs; platform-specific behavior
  goes behind interfaces (see the WebView-backend seam, arch §19.2).
- Four-space indent, headers alongside sources, one class per file.
- Persisted files live next to the tree file: `policy.json`, `state/<id>.blob`.

## Status (build order from arch §17)

Done: **1** shell+engine+tree model · **2** suspend/restore lifecycle +
persistence · **3** PolicyEngine + interceptor + shield editor.
Next: **3.5** WebViewBackend seam (before Android) · **4** kiosk · **5** AI
reorganizer · **6** interceptor consumers (media detector, filter evolution) ·
**7** password manager · **Android phase** (deferred).

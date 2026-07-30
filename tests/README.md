# Test harnesses

Two kinds live here.

- **`tests/*.cpp`** — offline suites. Deterministic, no display needed. These
  are the ones to run routinely.
- **`tests/live/try_*.cpp`** — live drivers. They build the *whole shell* and
  drive it on a real X display, which is how several defects in this project
  were found that no offline test could see. Expensive to build; see the
  warning below.

Nothing here is wired into CTest, and the app's own `CMakeLists.txt` does not
reference it. Building the app never builds the tests.

## ⚠️ Build this with a job limit

**Do not run `cmake --build tests/build -j` with no number.** Each live driver
compiles ~40 app sources and links Qt WebEngine; there are a dozen of them, and
unbounded parallelism on a many-core machine will try to hold well over 20 GB.
That has taken a desktop session down on this machine — twice — and it is worse
if a local model is loaded at the same time, because a 14B model holds ~10 GB
before the compiler starts.

```sh
cmake -S tests -B tests/build
cmake --build tests/build -j2                    # a limit, always
cmake --build tests/build -j2 --target test_seam # or one target at a time
```

Free memory first if a model is running:

```sh
pkill -f 'ollama serve'
```

## Running them

### Need nothing but a build

| suite | covers |
|---|---|
| `test_seam` | the download transport seam: routing, consent gate, per-source concurrency, cancel, failure paths, pump re-entrancy |
| `test_pick` | which file in a multi-file job Watch aims at, driven through the visible tree |
| `test_extractor` | the generated-extractor sandbox and the rule that a proposal cannot invent a URL, or return a segment |
| `test_extloop` | the review loop end to end with a stub provider standing in for a model |
| `test_settings` | settings persistence, the uninstalled-player fallback, the custom-player command template |

```sh
QT_QPA_PLATFORM=offscreen ./tests/build/test_seam
```

### Need a helper server

Start the helper, run the suite, stop the helper.

| suite | helper |
|---|---|
| `test_headers` | `python3 tests/echohdr.py 8850` — reports the headers it received |
| `test_dlheaders` | `python3 tests/echodl.py 8851` — same, but as the response *body*, so a completed download is the record of what arrived |
| `test_probe`, `test_probe_ui` | a stub Ollama on 8811, plus a "blackhole" listener that accepts and never answers, for the timeout tests |

### Need libtorrent

`test_torrent` and `test_watch` build only when `libtorrent-rasterbar` is found.
They stand up a real seeder in-process and move a torrent over loopback — no
tracker, no DHT, the seeder connects directly.

**If a "throttled" local transfer finishes instantly**, that is not a bug in the
test: libtorrent puts loopback peers in `local_peer_class`, which is exempt from
rate limits. `test_watch` clears that class for 127/8 with
`set_peer_class_filter`, and any new test that needs a slow local transfer must
do the same.

### Need a model

`test_ytdlp_live` takes a URL and needs `yt-dlp` (PATH or the vendored
submodule). `test_live_model` needs Ollama serving; pass the model name:

```sh
QT_QPA_PLATFORM=offscreen ./tests/build/test_live_model qwen2.5-coder:14b
```

## Live drivers

Each builds the real `main_window` with the real factory, policy and filter,
shows it on `$DISPLAY`, and drives it by triggering the actual `QAction`s and
clicking the actual buttons. There is no synthetic input available here (no
`xdotool`), so they trigger actions directly — but everything below that is the
shipping path.

Set `HYDRA_TEST_OUT` to choose where screenshots and captures land; it defaults
to `/tmp/hydra-test/`.

Lessons that cost time, so they are written down:

- **`import -window root` returns black once the screen blanks**, and hangs
  while a modal dialog holds an X grab. Capture with `QWidget::grab()`
  in-process instead; it is immune to both. Use `import` only to prove
  something is genuinely on screen.
- **A tab switch and its screenshot must not share a timer tick** — the capture
  runs before the repaint and you photograph the previous page.
- **Screen recording finds what screenshots cannot.** The first-load window
  flicker was invisible to stills at ~1.5 s per capture; `ffmpeg -f x11grab` at
  30 fps showed the window vanishing for ~12 frames.
- Some drivers modify `sample-tree.txt`, because opening a tab legitimately
  updates it. `git checkout -- sample-tree.txt` afterwards.

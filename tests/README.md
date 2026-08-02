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
| `test_fetcher` | the blocking fetcher: a real socket, the page's context, the cap, the timeout, and that a blocked caller times out rather than hanging |
| `test_helpers` | the §11.5.1 helper tier: the allowlist, the budgets, the transcript, and a script following a manifest through the sandbox |
| `test_streamtype` | the §10 content-type tier: classification, and a fake origin answering as the real one does |
| `test_extractor` | the generated-extractor sandbox and the rules that a proposal cannot invent a URL, return a segment, or return the page itself |
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
| `test_helpers_live` | the helper tier against a real CDN, with a hand-written extractor so a failure is the tier's and not a prompt's — takes a fresh capture from `try_extract` |
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

With a second argument it replays evidence captured from a real page by
`try_extract` instead of the built-in synthetic set — which is the only way to
measure anything that transfers, since the two disagree (real segments arrive
disguised as `.woff2` web fonts, the synthetic ones as `.ts`):

```sh
DISPLAY=:0 ./tests/build/try_extract https://site/watch/thing/ 60 /tmp/ev.json
HYDRA_MODEL_TIMEOUT_MS=900000 QT_QPA_PLATFORM=offscreen \
    ./tests/build/test_live_model qwen2.5-coder:14b /tmp/ev.json
```

`HYDRA_PROBE_DEBUG=1` prints what each content-type probe concluded, which is
the fastest way to see whether the budget went to the stream or to the page's
beacons.

**Point `try_extract` at the player mirror, not the watch page.** The measured
site's player fails to initialise when embedded — "Failed to setup player" —
and a capture from the watch page is 60-odd requests of furniture with no stream
in it. The mirror on its own (`https://<player-host>/#<id>`, the address the
watch page's iframe uses) initialises immediately and streams. The screenshots
`try_extract` writes are how you tell the two apart in ten seconds.

**Unless the mirror refuses to be loaded on its own**, which the second one
does — it bounces to its vendor's homepage without the embedding page's referer.
For those, pass a CSS selector as the fourth argument and the driver clicks it
before hunting for the player, which is how a user reaches it too:

```sh
DISPLAY=:0 ./tests/build/try_extract https://site/watch/thing/ 80 /tmp/ev.json \
    'ul.mirror li:nth-child(2) a'
```

`HYDRA_CLICKS` (default 3) sets how many times the player is clicked.

**If a player ignores every click, suspect our own ad blocking before the
driver.** The second mirror loads `fuckadblock.min.js` and will not start while
it sees a blocker; its play button simply stays put, which looks exactly like a
click that missed. `HYDRA_ALLOW_ADS=1` turns the ad list off for one capture and
`HYDRA_ALLOW_POPUPS=1` lets the page open windows. Both are off by default,
because a capture that quietly stopped blocking would be measuring a browser
nobody runs — and when you use them, say so beside the numbers.

`try_extract` writes its own one-node tree beside the output and starts it on
`about:blank`, so each run gets a fresh state directory and the repo's
`sample-tree.txt` is left alone. That is not tidiness: sharing the tree meant a
run restored whatever the last one left open, and a previous page's slow
subresources land in this capture's evidence under this capture's host once the
navigation commits.

Recapture immediately before each run. The CDN tokens are short-lived, so
evidence even an hour old probes as 403, and the run then measures the
un-annotated case while looking like it measured the annotated one — the
harness prints "N addresses answered" so this cannot pass unnoticed.

Raise `HYDRA_MODEL_TIMEOUT_MS` (default 240000) for real evidence: the payload
is far longer than the synthetic one and a 14B on CPU scales with it, so the
default ceiling expires mid-answer. Captured evidence carries live CDN tokens
and analytics ids, so keep it out of the repo and recapture rather than reusing
a stale file — the tokens expire regardless.

## Live drivers

Each builds the real `main_window` with the real factory, policy and filter,
shows it on `$DISPLAY`, and drives it by triggering the actual `QAction`s and
clicking the actual buttons. There is no synthetic input available here (no
`xdotool`), so they trigger actions directly — but everything below that is the
shipping path.

Set `HYDRA_TEST_OUT` to choose where screenshots and captures land; it defaults
to `/tmp/hydra-test/`.

`try_cookies` needs no server of its own and no network — it stands one up in
process and serves the page from `127.0.0.1` and a third-party image from
`127.0.0.2`, which is what makes a stored or refused cookie attributable. It
prints pass/fail and returns non-zero on failure, so it can be run like a suite:

```sh
DISPLAY=:0 ./tests/build/try_cookies
```

It runs the same three cases twice, over plain HTTP and over TLS, and only the
TLS ones can say anything about the third-party branch: over HTTP the engine
refuses such a cookie whatever our filter says, because `SameSite=None` needs
`Secure` and `Secure` needs TLS. Those HTTP rows are printed as notes rather
than asserted — do not "fix" them by asserting the observed behaviour, which is
indistinguishable from a working filter.

The TLS origin needs `openssl` on PATH; the certificate is generated per run
rather than committed, and the driver sets
`QTWEBENGINE_CHROMIUM_FLAGS=--ignore-certificate-errors` for its own process so
the engine will load a self-signed page. Without `openssl` those cases are
skipped and say so.

`try_permissions` is the same shape for geolocation, camera, microphone and
notifications, and also exits non-zero on failure. Two traps it encodes, both of
which produced a convincing false failure first:

- **A permission answer is remembered per origin, and an origin includes the
  port.** Asking twice on one port means the second answer comes from Chromium's
  memory and never reaches our decider. It uses a fresh port per case.
- **Geolocation cannot be judged from the page here.** With no location provider
  in the build, a *granted* request still ends as `PERMISSION_DENIED` — the same
  code as a refusal. `HYDRA_PERM_DEBUG=1` makes `qtwebengine_view` log what was
  asked and what was answered, which is where that case is checked instead.

`try_subframe` covers the MSE tap when the player is in an iframe — same origin
and cross-origin — which is the shape most watch pages have. Offline and
self-checking. Three things it encodes, each of which produced a wrong answer
first:

- **A control comes before the cases.** Its first phase loads the same player as
  its own document, so a later silence cannot be confused with a fixture that
  never played anything. That is not hypothetical: the fixture *was* wrong the
  first time.
- **The hook only reports past 256 KiB of appends.** A smaller fixture looks
  idle while working perfectly.
- **It waits for a report, not for a number of seconds.** The relay cannot
  deliver until its QWebChannel connects, and on a page with an iframe that took
  longer than the fixed window this driver used to allow — which read as "the
  channel never connects" and produced a confident, wrong diagnosis that reached
  a commit. Any new phase here should use the same wait.

`try_consent` drives the cookie-consent blocker against fixture banners — a
reject-offering one, an accept-only one, a decoy that is not a consent banner,
and one that locks scrolling — plus the policy side and the option turned off.
Offline, self-checking, non-zero on failure. Fixtures rather than a live CMP on
purpose: a test pinned to one vendor's markup measures that vendor and expires
with it.

It also covers the discovery half — a banner in a language the built-in patterns
do not know, recorded rather than silently skipped, and turned into a generic
rule flagged for the shipped defaults.

Note that it starts its policy phase from cookies **blocked**. An earlier
version did not, so the relaxation had nothing to do and the check passed while
testing nothing.

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
- **A click at the middle of the viewport misses the player.** The video is
  usually an iframe below the fold, so the centre of the *view* lands on its
  top edge or on the page behind it. `try_extract` runs `scrollIntoView` on the
  iframe first and clicks the centre of the rect it reports back; before that
  it looked exactly like a site that requests nothing until you press play.
- **Screenshot before concluding "nothing happened".** A player that failed to
  initialise and a player nobody clicked produce the same empty request log.
  One screenshot said "Failed to setup player" and saved the guessing.

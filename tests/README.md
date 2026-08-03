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

**Do not run `cmake --build tests/build -j` with no number**, and do not run
`make -j` either. Each live driver compiles **~61** app sources and links Qt
WebEngine; there are **21** of them, and unbounded parallelism on a many-core
machine will try to hold well over 20 GB. (Both numbers said something smaller
for a long time, because nothing counts them — `ls src/*.cpp | wc -l` and
`ls tests/live/try_*.cpp | wc -l` do.)
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

Twelve of these were written in one sweep through code that no test had ever
named. Four of the nine files that sweep covered were wrong — see `project.md` —
and every one of those bugs produced a plausible answer rather than a crash, so
the suites below are worth more than their pass counts suggest.

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
| `test_ytdlp` | parsing yt-dlp's answer and the format preference — the risky part is reading the JSON, not running the process |
| `test_bridge` | calling a shell object from a page by name, and above all what it refuses — `deleteLater()`, signals, unchecked argument types |
| `test_hls` | the HLS manifest parser: variants, segments, byte ranges, and the tags that carry their URI inline and would steal the next line |
| `test_assembler` | assembling a stream into one file against an in-process CDN that honours `Range`, including the byte-range playlist the parser fix was for |
| `test_tree` | the tree file and the AI payload, round-tripped — including a title containing the field separator, which used to lose the url |
| `test_state` | per-node suspended state: byte-for-byte blobs, binary with NULs, and two ids that must not share one file |
| `test_crypto` | the crypto shim's edges — wrong key, tampered byte, short buffer — and the contract when libsodium is absent |
| `test_picker` | what "zap this" hands the shell, and that a page cannot say which site it is |
| `test_autofill` | the gate between a page and the vault: origin, HTTPS, policy, and the order the refusals come in |
| `test_diff` | "no node left behind" — the reorganizer's repair rules and the undo snapshot |
| `test_model` | the tree model against Qt's own `QAbstractItemModelTester`, plus sorting and the search that must keep a hit's ancestors |
| `test_signals` | the evidence the filter loop reasons from, and why observed and suspect are different lists |
| `test_kiosk` | kiosk mode's borrow-and-return contract: the widget goes back where it came from |
| `test_bundle` | every setting through one INI file and back, and the refusals — an import that quietly applies nothing looks exactly like one that worked |
| `test_theme` | which colour scheme the desktop is in: `decide()` over what each source said, including the combination this machine produces, where Qt's own answer is `Unknown` |
| `test_credstore` | where the KeePassXC pairing lives between runs: the encoding exhaustively, and — given a Secret Service and `HYDRA_SECRET_KIND` — a real save/load/replace/clear round trip |

```sh
make test                    # all of them, from the repo root
make test-one T=test_seam    # or just one
```

`make test` builds and runs every suite in this section, reports each one's
count, and names the ones it did **not** run and why — a suite that needs a
helper server or a model is listed rather than quietly skipped. It sets
`QT_QPA_PLATFORM=offscreen` and gives `test_credstore` a keyring item of its
own, which are the two things easy to forget by hand. Driven directly it is:

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

### Needs a running KeePassXC

`try_keepass` drives the §13.1 bridge against a real KeePassXC. Set up one that
touches nothing of yours — its own config, its own database:

```sh
KP=/tmp/hydra-kp; mkdir -p $KP
printf 'hydratest\nhydratest\n' | keepassxc-cli db-create $KP/test.kdbx --set-password
printf 'hydratest\ns3cr3t-from-vault\n' | keepassxc-cli add $KP/test.kdbx \
    --username alice --url http://127.0.0.1:9931 --password-prompt "Test Site"
printf '[Browser]\nEnabled=true\nAlwaysAllowAccess=true\n' > $KP/keepassxc.ini
printf 'hydratest\n' | keepassxc --config $KP/keepassxc.ini \
    --localconfig $KP/local.ini --pw-stdin $KP/test.kdbx &
./tests/build/try_keepass
```

**Check the database actually opened.** `--pw-stdin` is taken from standard
input, and a shell that backgrounds the process may hand it nothing — the window
then sits on a password prompt with the vault locked, and the driver reports
*"Database not opened (code 1)"* where it would otherwise say *"association
failed (code 8)"*. Both are correct refusals, so the run still passes; but
pairing needs the vault open. Unlock it in the window with `hydratest` if it is
waiting.

Everything up to pairing runs unattended: the socket, the key exchange, and the
answer a saved-but-unknown pairing gets. The precondition **connects** rather
than checking the path exists — that path is a symlink which outlives the
process, so an exited KeePassXC leaves one behind and a driver that only looks
for it reports a listening server that is not there. **Pairing itself needs you**, because
KeePassXC asks a human whether this program may read the vault, and a browser
that could answer that for itself would be the bug. For the rest:

```sh
HYDRA_KEEPASS_INTERACTIVE=1 ./tests/build/try_keepass   # then accept the dialog
```

**Start this only when you are at the machine.** It waits three minutes for the
dialog and then reports the pairing as failed, which is honest but measures
nothing — several runs were lost that way. An unconfirmed run now says so and
declines the checks that depend on it, rather than reporting passes for an id
and key the test itself planted.

**And only the first run needs you.** The pairing is stored in the session
keyring, so every later run restores it, `test-associate`s it, and goes straight
to the login requests with no dialog at all. That is the whole point of
`credential_store`, and it is why `get-logins` is reachable unattended.

**Restart KeePassXC before an interactive run.** Measured, after five attempts
that produced nothing: a freshly started KeePassXC raises the *New key
association request* window, and an instance that has already served one
association stops raising it — same unlocked vault, same socket, same code-8
answer to a bogus pairing in the same run. One instance also exited mid-run
having written the association to the vault, leaving a stale socket behind, so
check `pgrep keepassxc` afterwards: the driver's precondition connects, and a
server that was alive when it connected can still be gone a minute later.

**Type a name in the dialog.** Accepting it with the name box empty creates no
association and sends no reply at all, so the driver waits out its full three
minutes and reports exactly what it reports when nobody clicked. The vault's
modification time is how to tell the two apart afterwards.

The driver writes to its **own** keyring item (`HYDRA_SECRET_KIND` defaults to
`keepassxc-association-try-keepass` here) so a test run can never overwrite or
delete a pairing you actually use. `test_credstore` refuses to touch the service
at all unless that variable names something other than the real item.

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

**`HYDRA_DUMP_PAYLOAD=<path>` writes exactly what was sent to the model**, and
it is what turns a miss into a finding. The run already prints how many
addresses were annotated, but a count cannot say *which* — and a miss with the
stream absent from the payload measures the probe budget, while a miss with it
present and annotated measures the model. Those are different bugs in different
files. The second site's 0 of 5 was the first kind and was read as the second
for a while; do not repeat that by reasoning from the counts.

**Point `try_extract` at the player mirror, not the watch page.** The measured
site's player fails to initialise when embedded — "Failed to setup player" —
and a capture from the watch page is 60-odd requests of furniture with no stream
in it. The mirror on its own (`https://<player-host>/#<id>`, the address the
watch page's iframe uses) initialises immediately and streams. The screenshots
`try_extract` writes are how you tell the two apart in ten seconds.

**Look at the player in the screenshot before believing anything else.** A
poster image behind the play button means the mirror is alive; **a black
rectangle with only a play triangle means it is not**, and that is the cheapest
triage available — it costs one glance and saves a capture, five model runs and
the write-up of a number that measured nothing. Confirmed the expensive way:
a black-player capture of dramafren's VidHide mirror produced exactly **one**
request to the media host, no segments at all, and a manifest that answered 502
to everything. Everything downstream of that is arithmetic on nothing.

The reason it is worth stating as a rule: **a mirror that has no video still
loads, still runs its ads, and still looks like a player.** These are
ad-revenue pages, and showing you a play button for something that is not there
is not a malfunction from their point of view. So "the page loaded and the
player appeared" is not evidence that there is a video, and neither is a capture
with eighty requests in it.

**Mirrors are not interchangeable, and which ones work is platform-dependent.**
On Linux, dramafren's **Upnshare** and **VidHide** are the ones worth trying;
the others may be Windows-only or simply broken. When one gives a black player,
try the next rather than concluding the site is down — measured the same
afternoon, VidHide gave a dead 502 manifest and Upnshare gave a complete stream
from the same episode.

**Find the mirror's own address rather than clicking blind.** dramafren's mirror
list carries each player's iframe base64-encoded in a `data-em` attribute, so
the direct url is one decode away and can be handed straight to `try_extract`:

```sh
curl -s <watch-page> | python3 -c "
import re,sys,base64
for m in re.finditer(r'data-em=\"([^\"]+)\"\s*data-index=\"(\d)\"[^>]*>\s*(\w+)', sys.stdin.read(), re.S):
    print(m.group(2), m.group(3), re.search(r'SRC=\"([^\"]+)\"', base64.b64decode(m.group(1)).decode(), re.I).group(1))"
```

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

**What exists.** The ones that assert print `N passed, M failed` and return
non-zero on failure, so they can be run like suites; the rest are observational
and report what they saw, which is the honest shape for a driver pointed at a
site nobody controls.

| driver | needs | what it drives |
|---|---|---|
| `try_cookies` | nothing | the cookie filter, first- and third-party, over HTTP and TLS |
| `try_consent` | nothing | the consent blocker end to end, and that it follows the visible tab |
| `try_permissions` | nothing | geolocation, camera, microphone, notifications, observed from the page's side |
| `try_filters` | nothing | whether an accepted filter rule actually stops a request, and the cosmetic half |
| `try_adblock_fix` | nothing | a page that will not run with ads blocked, fixed automatically — and a site the user ruled on being left alone |
| `try_subframe` | nothing | whether the media tap sees a player inside a third-party iframe |
| `try_settings_ui` | nothing | the settings window's layout and its site-defaults page |
| `try_keepass` | a running KeePassXC | the §13.1 bridge — see above for the setup |
| `try_flicker` | nothing | what is actually on screen in the moments after a tab opens |
| `try_downloads`, `try_watch` | network | a real HTTP download and a real torrent side by side in the downloads window |
| `try_capture` | a site url | arming a capture from a page and watching it land in the downloads window |
| `try_cancel` | a site url | cancelling a capture mid-recording |
| `try_tap`, `try_taprow` | a site url | the MSE tap on a real player, and how its row reads in the media dialog |
| `try_mse` | a site url | whether a main-world tap sees the bytes whatever the transport |
| `try_media` | a site url | the media path pointed at a real site, reporting what it sees |
| `try_frame` | a site url | whether a player iframe loads in a *plain* view, with none of our machinery |
| `try_extract` | a site url | a full turn of the extractor loop on evidence from a live page |
| `try_autofill` | nothing | the key on the toolbar: that it appears for a page with a login form *whether or not* the fill is allowed, carries the reason, answers a click, and goes down on navigation. Needs no KeePassXC — autofill is HTTPS-only, so a login form over plain http is refused for a reason the shell knows on its own |
| `try_ytdlp` | a site url | yt-dlp resolution driven through the shell's own menus |
| `try_settings` | nothing | the Settings dialog driven through the real window |

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
do not know, recorded rather than silently skipped, turned into a generic rule
flagged for the shipped defaults, accepted **through the dialog** by clicking it,
and then applied to the same banner on the next load.

It removes `consent-rules.json` from the output directory at startup, and that
is not tidiness: the shell loads rules from there, so a file left by the previous
run means the next one begins already knowing what it was supposed to learn.

Note that it starts its policy phase from cookies **blocked**. An earlier
version did not, so the relaxation had nothing to do and the check passed while
testing nothing.

`try_settings_ui` drives the settings window offscreen — the category list and
page stack, every per-site feature having a control, the kiosk round trip, filter
removal, and the learned-rule list with its import judgement. It needs no display
server and no network, so it runs like an offline suite despite living here.

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

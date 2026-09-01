# project.md — Hydra

Working notes and conventions for this repo: what exists, what is next, and the
rules to follow while working in it. The full design lives in
`doc/architecture.md` — read it before making changes; where this file and the
architecture doc disagree about intent, the architecture doc wins, and where
they disagree about *current state*, this file wins.

## What this is

Hydra (working name) is a Linux/X11 desktop browser built on **Qt 6 Widgets**
(no QML) and **Qt WebEngine**. It presents a side-tree of tabs/links over its
own embedded Chromium, with a per-site security policy engine, and (planned)
kiosk mode, AI tree-sorting + ad-filter evolution, a media detector with
external-player handoff, and a KeePassXC-based password manager. **Android is a
planned first-class target, deferred until desktop is complete** (see
`doc/architecture.md` §19).

## Resuming work here

Read `doc/architecture.md` for what the design *intends*, this file for what
currently *is*, and `test/README.md` before running anything. Where the two
docs disagree about intent the architecture doc wins; about current state, this
file wins.

This file is a running log and is long. The fastest orientation is: **What is
implemented** (the table below), then **What is next**, then the section for
whatever you are touching.

The `##` headings past the feature sections group the log rather than order it.
Sections were inserted wherever the writer happened to be reading, so the file
is **not in date order** and a heading that implied one would be lying: the
groups say what a run of sections is about, not when it happened. Two of them
are single pieces of work and read as one -- *The GUI pass* and *Light and
dark*; the rest are runs that only share a subject.

**And the log itself can go wrong in the way the code does.** A revision that
rewrote a finding — the `test_extloop` flake, once "recorded rather than
explained" and later explained — was *added* rather than applied, leaving
**254 lines duplicated** and the two copies contradicting each other about
whether the cause was known. It survived ten commits, because a section that
reads correctly reads correctly the second time too. Found by
`grep '^###' project.md | sort | uniq -d`, which is worth running after any
large edit here: this file is the thing the next session trusts, and a stale
copy of a section is indistinguishable from a current one — the same shape as
the stale rule file that quietly stopped `try_consent` testing anything.

**Two checks worth running on this file, because both have caught real drift:**

```sh
grep '^###' project.md | sort | uniq -d          # a section said twice
# every filename in the implemented table still exists
sed -n '/^| Area | Files | Notes |/,/^$/p' project.md |
  grep -oE '`[a-z_]+\.\{h,cpp\}`' | tr -d '`' | sed 's/\.{h,cpp}/.h/' |
  sort -u | while read f; do [ -f "src/$f" ] || echo "MISSING src/$f"; done
```

The second one found `crypto_box` and `consent_rules` in the table long after
they had become `box_crypto` and `site_rules` — and this same file documents the
`site_rules` rename in prose a thousand lines further down. **A table of
filenames rots silently**, because nothing compiles it and the prose beside it
stays true.

**And it rots in the other direction, which neither of those could see.** Both
ask whether what the table names still exists. Neither asks whether what exists
is named — and a third of `src/` was not: the tree view, every chrome dialog,
the shared UI helpers, the page tools, the extractor files, media assembly,
session import, the settings bundle, scheme rules and the whole Android backend,
under a heading that says *What is implemented*. A table that is silently
partial is worse than a short one, because its shape promises completeness.

```sh
# every header in src/ is named by the implemented table
table=$(sed -n '/^| Area | Files | Notes |/,/^$/p' project.md)
for f in src/*.h; do b=$(basename "$f" .h)
  echo "$table" | grep -q "$b" && continue
  stem=${b%%_*}; rest=${b#*_}      # the table writes siblings as a_{b,c}
  echo "$table" | grep -q "${stem}_{.*${rest}" || echo "not in table: $b"
done
```

### On this machine

| thing | state |
|---|---|
| `libsodium` | installed — KeePassXC bridge builds |
| `libtorrent-rasterbar` 2.0.11 | installed — BitTorrent builds |
| `Qt6::Qml` | required, and only for `QJSEngine` (the extractor sandbox). No QML in the UI |
| `third_party/yt-dlp` | vendored submodule. Clone with `--recurse-submodules` |
| `yt-dlp` on PATH | installed, `/usr/bin/yt-dlp` 2025.04.30 — so the PATH branch is the one taken and the vendored copy is not exercised here |
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

Of the two things the desktop `gcc_64` kit made newly checkable, one is now
settled and one is not:

- **It does build against 6.11**, warning-clean, and is built that way routinely
  now alongside 6.8.2 and Android. The system Qt here is still 6.8.2, so where a
  measurement in this file does not say otherwise it was taken on 6.8.2.
- **Whether the geolocation gap is Qt's or the build's.** `try_permissions`
  records that a *granted* geolocation request still ends as PERMISSION_DENIED
  here because this build has no location provider. A second Qt build is the
  cheapest way to find out whether that is universal or packaging.

### What does not survive a session

Out-of-tree build dirs, screenshots and captured evidence are written to the
session scratchpad, which is cleared when the session ends. `build/` and
`test/build-make/` are in-tree and do survive, so the desktop suite runs without a
rebuild; the Qt 6.11 and Android trees do not, and cost a full configure and
build to recreate.

**The part worth knowing before relying on it:** the raw `ev-*.json` evidence
captures from the extractor-loop runs were scratch too, and every one taken
before 2026-08-03 is gone. Every conclusion drawn from them is written up in the
sections below — that is what those sections are for — but the captures
themselves went with their sessions, so *re-analysing* one of those old runs
means re-capturing it, which is not the same thing: the site has moved on since.

**There is somewhere durable to put them now: `evidence/`.** It is in
`.gitignore` beside `state/`, so captures survive the session without being
committed — a capture is a few dozen urls belonging to somebody else's site,
some with tokens in them, and publishing one is a separate decision from keeping
one. `evidence/README.md` says what each capture is and how to replay it. Put a
capture there at the time if its numbers are going to be argued with later.

### The log was rewritten, and every hash before f6e28a6 changed

The 252 commits this project had were reformatted to the convention in
`build-and-commit.md` and became 221. Anyone holding a clone taken before that
has a history that no longer exists upstream and must reset to it rather than
merge, or git will present the two as parallel work and offer to combine them.

What changed, and what did not:

- **Subjects.** The 202 commits written before the convention arrived — it
  starts at `tools: track the commit-msg hook` — gained a prefix from the set
  the tree already used, plus `android:` for the port. The log filters by it now:
  62 `feature`, 38 `fix`, 35 `ui`, 34 `test`, 18 `build`, 15 `android`, 6 `tree`,
  6 `tools`, 5 `rework`, 2 `packaging`.
- **Bodies.** 163 were wrapped at 78–82 columns and are at 75 now. No word
  changed: the rewrapper refused to write unless the words came back identical,
  and a separate check confirmed all 66,538 body words survive in order.
- **31 documentation-only commits are gone as commits**, folded into the code
  commit each describes — backward where it recorded what had just happened,
  forward where it decided what came next. None of their prose was dropped; each
  one's subject survives as a sentence in the message it joined. There are zero
  documentation-only commits in the history now.
- **No file content moved.** Each commit reuses a tree object git already had
  rather than reapplying a diff, so the trees could be compared one by one, and
  `git diff` between the old tip and the new one is empty.

The old history is at the tag **`pre-msg-rewrite`**, and the branch
`worktree-agent-a32c3fa286a892379` still points into it.

**Two things the rewrite left behind**, both worth knowing before touching the
log again:

- **One message cites a sha that is no longer in the history.** `test: repair
  the build, which had been passing on stale binaries` (now `1be8c16`) refers to
  "a mass edit in af66464". That resolves today only because the backup tag keeps
  the old commit reachable; delete the tag and it dangles. Its replacement is
  `25a0db6`. Fixing it means rewriting from that point again, which changes every
  hash after it.
- **One squashed commit contradicts itself.** `build: pin the Qt floor at 6.4`
  ends by saying nothing has yet driven a real navigation through the
  interceptor, and the write-up folded into it immediately reports that
  verification. Both were true when written. Resolving it means editing prose
  rather than reformatting it, so it was left alone.

### Five commits fail this project's own commit-msg hook

007, 026, 027, 140 and 141 in the old numbering — the ones that name the Claude
API, `claude_provider` and `ANTHROPIC_API_KEY`. All predate the hook, and all
are correct: the global rule bans *attribution*, not the subject, and a browser
with an AI provider has to be able to name the provider it implements.

The hook cannot tell the two apart. It spares exactly two literal names,
`.claude` and `CLAUDE.md`, which is enough to write about the tooling tree and
not enough to write about the feature. This is the same false-positive class its
own comments already record, one step further out. Nothing was changed on either
side: the messages are accurate, and widening a gate copied from
`~/.claude/tool/commit-msg` is a convention change to raise rather than make in
passing. It will refuse the commit if one of those five is ever amended.

**Eight more appeared to fail it, and none of them did.** The hook holds body
prose to 75 columns as of this pass — the limit the convention always stated
and only the subject was ever checked against. It reported eight commits here
over the limit, and every one was **exactly 75 columns and correct**.

The gate was counting bytes. `${#var}` in `/bin/sh` is a byte count, so a line
carrying one em dash or ellipsis measured 76 or 77 — and this project's
messages use typographic punctuation, which is why it bit here and not in the
trees whose logs are ASCII. A character three bytes wide still occupies one
column.

**The rewrap was built, run, and thrown away**, which is the part worth
keeping. All eight were rewrapped and verified — trees identical, every
character surviving, the merge commit's parents preserved — and then the
before/after measurement showed *zero* of them had ever exceeded 75
characters. Eight messages reflowed to satisfy a checker that could not model
what it was checking. Reset, and the count fixed instead: the hook strips
UTF-8 continuation bytes before measuring, which is the character count
without depending on a locale it cannot control.

So the log stands untouched, and 0 of 269 commits are refused for length. The
five above remain, and remain correct.

### The ASCII rule is unenforced here, and 854 characters walked in

`ascii_only` is off in `.style-gate.toml`, and the reason for switching it off
was sound: the gate reads whole files as bytes for every language but Python,
and this tree's user-facing strings genuinely need Unicode — the toolbar's
`☰`, the media dialog's `▶ Watch` and `⬇ Download`. Those are output, which
the rule has always allowed.

What the exception *also* covers is every comment in the tree, which is not
what it was for. Counting the 233 C and C++ files the gate reads: 1114
non-ASCII characters, of which 260 sit inside string literals and are output,
and **854 sit in comments, spread over 163 of the 233 files**. The comment
half is 437 em dashes, 369 section signs and 32 ellipses — every one of them
a character the rule names by example and gives an ASCII spelling for, `--`
and "section". `src/main_window.cpp` alone holds 54.

This is the incident the global `code-style.md` now records, reproduced here
at two orders of magnitude: a project switched the check off to keep two
glyphs and switched it off for its prose as well. There it was one em dash
found by grepping. Here nothing had grepped.

Measured by walking each file with a small state machine that separates code,
`//` and `/* */` comments, and string literals including the `R"(...)"` raw
strings the embedded JavaScript uses — then spot-checked against the lines it
named, both directions: `src/ai_provider.h:8` (`§9.1` in a comment) and
`src/media_dialog.cpp:97` (`▶ Watch` in a `QPushButton` argument).

Closing it was two separable pieces of work. **The second is done**, and it
was done first because it is what makes the first one checkable:

- **Give the gate a C++ scanner** — done, in the shared tool, so `ascii_only`
  can distinguish a comment from a button label the way it already does for
  Python's f-string ticks. See below.
- **Spell the comments back to ASCII** — not started. A mechanical bulk edit
  over 163 files, so it carries a proof rather than a sampled diff. The
  invariant is cheap and exact: strip every comment from each file before and
  after, and the results must be byte-identical — that is precisely the claim
  that only comment text moved. `gcc -fpreprocessed -E` does the stripping
  without needing this tree's include paths.

### The gate can now read C++, so the 854 are a work list

`tool/style_gate.py` gained a C/C++ literal scanner, spread from the source
the way the tokenize change was. Turning `ascii_only` on in this tree today
reports **exactly 854 findings, all of them real** — 434 in `.cpp`, 420 in
`.h`, and not one false positive on `☰`, `▶ Watch` or `⬇ Download`. Nothing
outside C++ contributes: no Python, no Makefile, no `.pro` file, no
`debian/rules`. Before the scanner the same flag failed 163 files on their
first non-ASCII byte, glyphs included, which is why it was off.

The flag stays off until the comments are cleaned, because 854 findings is a
failing gate. But the list is now precise enough to work from, which it was
not.

**How the scanner was checked, since a gate that under-reports reads exactly
like a clean tree.** Twenty cases cover the shapes that classically
desynchronise a hand-written C scanner — a backslash-continued `//` comment,
C++14's `1'000'000'000` digit separator (spelled like a character literal and
not one), `L'x'` and `u8'x'`, quotes inside comments, `//` and `/*` inside
strings, raw strings holding both, escaped quotes, and the three unterminated
forms, which return None so the caller falls back to the byte check.

Then the whole tree was checked against **GCC**, which lexes C for a living
and shares no code with this: `gcc -fpreprocessed -E` strips comments without
following includes, so what it keeps is the literals and what it drops is the
prose. Across all 233 files and 1114 characters the two agree on every one,
no file refused, none in code context. That is corroboration from an
independent witness rather than two readings by the same hand — which matters
here, because the first count of these 854 was made by a throwaway script
written in the same session.

### ⚠️ Do not build with unbounded `-j`

The live drivers under `test/live/` each compile ~40 app sources and link Qt
WebEngine, and there are a dozen of them. `make -j` with no number has
exhausted memory and taken the desktop session down on this machine, twice
— worst when a model is loaded, since a 14B holds ~10 GB before the compiler
starts. Use `-j2`, or name a single target. Stop Ollama first if it is running.

The mechanism, since it is easy to underestimate: `make -j` with no number is
*unlimited*, not
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
in 5**, having been 0 in 5 every time it was asked before; and the same 2 in 5
on a **third site** whose manifest shares no fragment with it, with the payload
dumped and read to confirm the evidence carried the manifest annotated, so those
five runs measure the model rather than the probe budget.

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
- ~~The KeePassXC bridge above the crypto layer — `keepassxc` is not installed.~~
  **Answered, all of it.** Socket, framing, key exchange, a refused unknown
  pairing, `associate()`, a stored pairing restored and accepted, and
  `get-logins` returning a real entry and an empty answer for a url the vault
  does not know. §13 has no unexercised path left, and because the pairing
  persists it re-runs unattended rather than once.
- ~~Whether the extractor prompt generalises past one synthetic evidence set.~~
  **Answered, badly:** it does not. Against evidence captured from the real
  site the loop returned prose and no parser in five runs out of five, so every
  hit-rate in this file describes the synthetic fixture and nothing else.
- ~~Whether it does better now that the payload carries what each address
  actually serves.~~ **Answered: not on its own.** Carrying the content types
  changed nothing — 0 of 5 — until the payload's *tail* said to use them. What
  the tier supplies and what the model reads are two different questions, and
  only the second one moved the number. See the four arms below.
- ~~Whether any of this transfers to a second site.~~ **Answered on a third
  one: it transfers, and not by the mechanism built for it.** kisskh scored 2 of
  5 with the manifest present and annotated in the payload — but both hits came
  from an `.m3u8` extension fallback and no run read the annotation, so the
  result would be 0 on a site that disguises its manifest. Site 2 remains
  unmeasured under working conditions: its 0 of 5 predates the round-robin
  budget that would have reached its media host.
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

**And two more from the same family, both found by running everything at once.**
A driver that asserts nothing exits 0 whatever it saw, so a sweep reading "no
`N passed` line" as failure reports false alarms — four of this project's
drivers are report-only and `test/README.md` now names them. Worse,
`try_settings` had been writing **no screenshots at all**: `import` cannot
create a directory, so with a fresh output path every capture failed while the
run printed "done" and exited 0. A driver whose entire output is pictures,
reporting success having produced none.

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

### Files this program writes are INI unless something defeats it

Default to INI for anything Hydra persists. Reach for JSON, or anything
nested, only where the data is genuinely shaped and an INI would have to
encode structure into key names to survive.

Asked for directly: *"I want primarily inifile type format, other formats
where the structure needs to be more complex"*, and immediately after, *"if
the format is good enough to contain that kind of data then convert
policy.json and site-rules.json to ini too"*. So it governs what already
exists as well as what is added -- a format that turns out to be sufficient
is a reason to migrate, not to leave the old one alone.

Applied so far to the settings bundle, `policy.ini` and `site-rules.ini`.
Each still reads the older JSON at the same path once and rewrites it as INI
on the next save, so an upgrade is silent and nobody loses rules to a
migration step they had to run. Keep that shape when converting the next one.

The reason is legibility under pressure: a key=value file can be read by a
person, diffed by a tool, and edited by hand when something is broken. That
is worth more here than the ability to nest.

### Build the generic design; do not wait for a case to force it

Where a design is sound but no concrete case demands it yet, build it anyway
rather than recording it as "wait for a site that needs this". The reasoning,
in the user's words: *"We can't experience everything everyone else will."*
Waiting means every user meets the gap before we do, and a feature designed
against the single example that finally forced it fits that example rather
than the problem.

This overruled the opposite recommendation and was immediately vindicated.
The extractor helper tier at 11.5.1 was proposed as a design to write down
and defer, citing this project's own measure-first habit; built instead, it
found three defects on first contact with a real CDN that no amount of design
review had -- including evidence-folding that had been silently measuring
nothing.

**Measure-first still governs claims, not whether to build.** Never assert a
hit rate or a fix that has not been run; the ledger above exists for exactly
that. Propose the generic design, build it behind a default-off permission or
a seam where that applies, and say plainly which parts are proven and which
are merely built.

## Tabs move like files now (§4)

The tree calls itself a side-tree of tabs and, until this, **a tab could not be
dragged into a folder**. `tab_tree_model` implemented the read-only half of
`QAbstractItemModel` — `index`, `parent`, `rowCount`, `columnCount`, `data` —
and nothing else, so the view refused every drag before it started. Nodes moved
only through `apply_reorganization` (the AI diff) and `restore_snapshot` (undo):
the machine could rearrange your tabs and you could not.

**Move by default, Ctrl to copy**, which is what a file manager does within one
tree. A move keeps the id, and that is the whole reason moves are by id and not
by url: `state/<id>.blob` and the outline file are both keyed by it, so a tab
carries its history and its suspended state to the new folder. A copy gets a
**fresh** id — two nodes sharing one would share a state blob, and one tab's
scroll position and half-filled form would be restored into the other — and is
demoted to `unopened`, because a copy is a second bookmark of an address rather
than a second live view of it.

### Who decides, and the field that had to go

The first arrangement put a `reorder_allowed` bool on the model and had the
shell set it from the sort combo. Two calls that had to be kept in step by hand
— so any other route to changing the sort, a settings restore or a shortcut or
a test, would leave the model believing a stale answer and dropping rows at
positions that mean nothing. That is the same shape as every "wired but never
exercised" defect recorded above: state that must be *remembered*, in an object
that cannot check it.

**The view decides now**, because it is the only object that can see both
things: the model cannot see which sort is active, and the proxy cannot see
where the pointer is. `tab_tree_view` refuses a between-rows drop by ignoring
the drag event when the indicator is between rows and the proxy is not in tree
order — no indicator, no drop, and a drop *onto* a folder still works in every
mode.

**And the answer is derived rather than stored.** The proxy already encodes it:
`sortRole() == tree_order_role` *is* the question. So there is no second copy to
keep in sync — not a flag on the model, not a mirrored enum on the proxy. The
model simply honours whatever row it is handed, the view never hands it a
meaningless one, and a sort changed by any route at all is reflected
immediately.

That is also what makes the subclass worth having rather than configuring a
plain `QTreeView` at the call site: the gesture set lives in one place, so a
second tree — or a test — cannot get a differently configured one.

**Filtering was the other worry, and it is Qt's problem rather than ours.**
Checked rather than assumed: with a search active, proxy row N is not source row
N, and a drop aimed at the one visible row lands beside *that* row rather than
at the raw index. `QSortFilterProxyModel::dropMimeData` maps it, and there is a
test that would notice if that ever stopped being true.

**Reparenting always; reordering only in tree order.** A drop *between* two rows
means "put it here", and that has no stable meaning sorted by title or by date —
the row would jump back the instant it re-sorted, which reads as the app
ignoring you. A drop *onto* a folder is unambiguous in every mode. Firefox and
Chrome's bookmark managers make the same split. The happy consequence is that
the sort proxy's index mapping stops being a problem at all: reordering is only
live in the one mode where the proxy's order and the model's are the same.

**The move that would eat the tree is refused**: a folder dropped inside its own
child makes a ring, the outline writer recurses forever, and everything below
the drag disappears from the file. §9.4 refuses the same move for the reorganizer
and this is that rule one gesture closer to the user.

### A tab's name, and the two different things it can be

Reported rather than found: browsing to another page left the old label in
place. The cause was one layer down — **`web_view_backend` had no title at
all**, no signal and no getter, so a tab wore whatever the tree file said or
somebody typed, permanently. The seam carries one now and the desktop backend
forwards Qt's, which already falls back to the url for a document with no title.

**The interesting half is the distinction.** A title that arrived from the page
should follow the page. A title a person typed should not be quietly replaced
the next time that tab loads something — a tab called "Bank — statements" that
renames itself to "Log in" the moment it is used has lost the thing it was
called for. So the node records whether it was *chosen*, and that is stored
rather than derived: a title is a string, and "did a human pick this" cannot be
recovered from the string afterwards.

Only the properties editor and the rename prompt route through `update_node`,
so that is where being chosen is marked — and only on an actual change, so
opening the editor and pressing OK does not pin a title nobody touched.
`set_page_title` refuses on a chosen node and returns whether anything changed,
which lets the caller skip a save for a title that is already right.

**Clearing the name gives the tab back to the page.** Emptying the field is the
natural way to say "stop calling it that"; the alternative, a checkbox marked
*follow the page title*, explains a mechanism where the gesture already says it.
The label falls back to the address meanwhile, which is what an unvisited tab
wears anyway.

**In the file it is one more trailing key, written only when true** (`named=1`),
so an ordinary tree does not grow a column of `named=0` and a file written
before this existed reads as "not chosen". That last part needed care rather
than luck: the reader takes trailing `key=value` fields from the right and stops
at the first it does not recognise, so an unknown key at the end would have
stopped `created=` and `seen=` from ever being read. There is a check that a
pre-flag file still parses its dates.

**Saving is coalesced**, because titles arrive in bursts — the url, then the
real title, sometimes a revision from script. It reuses the debounce the shell
already had rather than adding one beside it; the first attempt built a second
timer and the compiler caught the duplicate member, which is the same "two
records of one thing" as the reorder flag and the live-view map.

**Driven through the dialog**, since that is the only route by which a tab is
deliberately named: `try_rename` fills the real modal from a timer while `exec`
is blocking — the typed name reaches the tree and is marked chosen, the page
cannot take it back afterwards, Cancel changes nothing, emptying the field hands
the tab back, and the file carries no marker for a tab that follows its page.
F2 opens it too, because that is what a hand reaches for; Delete is deliberately
not bound, since a stray key should not take a folder and everything in it.

**The driver crashed first, and it was the same defect as `try_keepass`.** Its
dialog-filling helper captured the title and the accept flag *by reference* in a
timer lambda, and the helper returns before the dialog exists — so 500 ms later
those parameters were dead stack and it segfaulted inside `findChildren`.
Knowing the shape did not prevent writing it again, which argues for the rule
rather than against it: this one was immediate and obvious, where the earlier
one corrupted a heap and surfaced three checks later.

### You could not make a tab

Asked how to create one, the honest answer was that you could not. A tab could
arrive from the tree file, from a duplicate, from a browser mirror or from the
AI reorganizer — and that was the whole list. The File menu offered Save Tree,
Quit, Back, Forward and Reload. **A browser with no New Tab**, which had gone
unnoticed because every test and every driver started from a tree that already
had tabs in it.

`New Tab` (Ctrl+T) and `New Folder` (Ctrl+Shift+N) are in the File menu now, and
both are on the context menu as *New Tab Here* / *New Folder Here*. A new node
goes **beside whatever is selected**, or at the top level when nothing is:
dropping it at the root regardless is simpler and wrong, because a tab made
while working inside a folder belongs in that folder. Asked for beside a *tab*
it lands next to it rather than within — a tab holds no children, so "in here"
has no meaning.

A new tab is opened immediately and the address bar takes focus, because an
empty tab is a question about where to go; a new folder asks for its name on the
spot, because a folder called "New folder" is one somebody has to come back and
rename, and they will not.

**And the right-click menu was nearly empty.** It offered Open and Suspend, and
returned early for folders — so the containers everything lives in could not be
renamed, emptied or added to, and a right-click on blank space did nothing. It
now has duplicate, copy address, new folder, delete (which names how many items
go with a folder, since deleting one takes what is inside it) and a properties
editor: title, address, tags, with the id shown and **not** editable. Retyping an
id would orphan a tab's saved state with no warning, which is the exact class of
silent loss this file keeps recording.

Every one of these saves through one signal, `structure_changed`, so a drag, a
rename and a new folder all persist the same way rather than three ways.

**The menu lives in the view**, with two signals out for the only entries it
cannot carry out itself: opening needs an engine and the stacked widget,
suspending needs the state store. That moved 113 lines out of `main_window` and
removed a second source of truth on the way — the menu used to ask the shell's
map of live views whether a tab was open, which is a fact the node already
carries, since `open_node` sets its type and `suspend_node` clears it. Two
records of one fact is one of them being wrong later.

It is opened rather than assumed: `try_import` posts a context-menu request and
inspects the popup while `exec` is blocking, because a menu that moved between
classes is exactly when a connection quietly stops being made.

**33 checks** in `test_model`, covering the drag flags a view asks about before
it will start a drag at all, that a drag carries ids rather than urls, the
ring-refusal in both directions, that a copy's id is new and both nodes stay
findable in the index, that the properties editor cannot change an id, and that
deleting takes the subtree but refuses the root.

**Found while testing, not fixed:** `tree_outline::load` returns an empty root
when the file cannot be opened, and `tab_tree_model::load` reports success — so
a mistyped tree path is indistinguishable from an empty tree. It is probably
deliberate, since a first run has no file yet, but nothing says so and the two
cases deserve different words. Left alone rather than changed blind, because
changing it would change what happens on somebody's first launch.

## Locked tabs and sub-tabs (§5.5, done)

A tab can be pinned from the tree's context menu (**Loc&k** — `k`, because
Dup&licate already holds `l`). A locked tab keeps its page and its place:
navigating it opens a **sub-tab** below it and browsing continues there, and the
node cannot be dragged, reordered, or moved by the AI reorganizer.

**The feature was mostly a restriction being lifted.** Four places said a tab
could hold no children — `add_tab` and `add_folder` redirected to the nearest
folder, `flags()` refused the drop, and `tree_invariants` counted it a
violation. `struct node` had carried a `children` list all along, so nothing in
the data model or the file format objected: `write_node` already recursed into
any node's children and the reader nests by indentation without consulting the
type. The invariant's stated reason — "the tree file cannot express that" — was
never true, and it was enforcing a model rule while citing a format limit.

**And it settled the contradiction this file recorded.**
`main_window::open_new_window` was commented "Under the tab that asked, where
the tree shows the relationship" and parented the new node at `n->parent`,
making a sibling. The comment described the design and the code described the
restriction. With the restriction gone the comment is now true, so a window
opened from a link records where it came from.

**Three things the implementation had to get right, none of them obvious:**

- **A node's url does not follow the page.** Only the title does
  (`set_page_title`); nothing writes a browsed address back to the node. So
  pinning to `n->url` would pin to wherever the tab was *created*, not what is
  on screen. The lock is applied through the shell, which reads the live view's
  url and writes it as the pin — which is also why the tree emits
  `lock_requested` rather than calling the model itself.
- **The pin cannot be compared against the view's url.** Opening a locked tab
  loads its page through the same navigation check, and at that moment the view
  is empty — comparing against it made a locked tab spawn a sub-tab of itself
  instead of loading. It compares against the node's pinned url.
- **The sub-tab is created a turn of the event loop later.** The check runs
  inside the engine's navigation decision, and building a second view there
  means creating a page, wiring a profile and starting a load re-entrantly. The
  engine gets its `false` immediately; the tab is queued, and carries its parent
  by id rather than by pointer.

**What the tests caught, which is the argument for having written them:** the
first version deleted `ItemIsDragEnabled` while rearranging `flags()`, so
*nothing* in the tree could be dragged — and `test_model`'s `holds()` harness,
which re-checks the invariants after every section rather than testing the
operation just performed, is what reported the tab-with-children violation in a
section that was not looking for it.

**Android implements the decider now.** `shouldOverrideUrlLoading` asks the
shell through a new JNI entry point, and returning true there means "handled,
do not load" -- so a refusal is the negation of permission, which is the one
place this reads backwards from the desktop. The Java asks only about main
frames and only after the external-url question, so the three rules match the
desktop's without the C++ having to repeat them.

Allowed is the answer when nothing is listening: a view that has gone away
between the question and the lookup, or one whose shell never set a decider.
Refusing either would turn a missing answer into a browser that will not browse.

**Verified as far as it can be without a device**: the APK builds, and the
library exports `Java_se_vibes_hydra_HydraWebView_allowNavigation` -- checked
against the Java's own declaration, eight wanted and eight exported. That the
lock actually holds on a phone needs a phone.

## Another browser's tabs, in a folder of their own (§4)

Tabs rather than bookmarks, because a bookmark is something filed once and the
thing worth bringing across is the working set someone actually has in front of
them. **Tools ▸ Import Tabs from Firefox** reads what Firefox last wrote and
shows it; it never attaches to a running browser, needs no extension installed,
and cannot disturb what it reads.

**The file is not JSON.** Firefox writes `mozLz40\0`, a little-endian uint32 of
the decompressed size, then a raw LZ4 block. On this machine that is 1.5 MB
expanding to 5.8 MB and holding **81 open tabs**.

**Both decoders are kept, and the reason is the machine this was written on.**
There was no `liblz4-dev` here at the time, so as an optional dependency the
feature would have been dead exactly where it was being built — untestable, and
"install something first" is a poor answer for reading a file the user already
has. So `session_import` carries a small bounds-checked LZ4 block decoder.
liblz4 arrived later and is now used where present, because it is audited and
this parses a file another program wrote; the built-in stays as the fallback.

**The suite drives both against an implementation nobody here wrote** —
python's `lz4.block` — on the real session file, and they agree byte for byte
over 5,791,500 bytes. That comparison is the only reason to trust a
decompressor someone wrote by hand. The built-in is exercised *even though the
build does not use it*, because a fallback nothing runs is a fallback that has
already stopped working.

**The profile trap, which would have made this look like it worked.**
`profiles.ini` here marks `Default=1` on a profile holding four certificate
databases and nothing else — no session, no history — while the profile actually
in use is named by an `[Install…]` section. An importer that trusts `Default=1`
imports zero tabs and reports success. The install-locked entry wins, and
`Default=1` is used only when nothing overrides it.

**`recovery.jsonlz4` over `sessionstore.jsonlz4`**, deliberately: the second only
exists after a clean shutdown, so preferring it would import a stale set from a
browser that is running right now — which is the common case for someone
reaching for this.

**A tab's `index` is where it is, not where it ended up.** It is 1-based and
points into that tab's own history, so a tab someone pressed Back on twice
imports as the page they are looking at rather than the one they navigated away
from. `_closedTabs` is deliberately ignored: those are what the user closed, and
resurrecting them answers a question nobody asked.

### The mirror, and the one thing it must never do

Imported tabs go in a folder marked `mirror`, and **a mirror is never written to
the tree file**. `tree_outline::write_node` returns early on one, subtree and
all. Saving it would resurrect a stale copy of another browser's tabs on the
next launch, indistinguishable from tabs the user had filed themselves — and
they would keep coming back, since nothing would ever delete them.

Re-reading **replaces** the folder rather than merging into it: a merge leaves
tabs closed elsewhere sitting here for ever, which is the failure mode of every
stale mirror. That is also what makes the polled version later the same
mechanism rather than a second one.

Ids are scoped to the mirror (`fx-0`, `fx-1`, …) because a mirrored tab is in
`m_id_index` while it is on screen, and an id colliding with a real tab's would
make `node_by_id` — which the lifecycle and the AI payload both use — answer
with somebody else's session.

**Dragging one out is how you keep it.** The drop makes a copy with no `mirror`
set and an id of its own, so it becomes an ordinary tab in your tree, saved like
any other. That fell out of the drag-and-drop work rather than needing anything
of its own.

#### It only worked if you held Ctrl

"That fell out of the drag-and-drop work" was true of exactly one of the two
gestures, and the other one lost the tab. A **copy** is `deep_copy`, which
never took the `mirror` field and so produced a clean node. A plain drag is a
**move**, and the move branch reparented the node without touching the mark --
so the row landed in the user's own folder, looked filed, and was then skipped
by `write_node` for belonging to somebody else. It was gone at the next launch.

Nothing reported it, and nothing could have: the tree on screen and the tree on
disk simply disagreed, and only the second one survives a restart. It is the
same shape as the sample tree coming back -- a save that silently declines to
record what is in front of you.

The mark is cleared on the moved subtree now, decided by the **destination**
rather than the source, since that is what the question is about.

`tree_invariants` had half the rule: it refused an *unmarked* node inside a
mirror, which is the half that would write foreign tabs into the file. The
other half -- a *marked* node inside the user's tree, which drops one of theirs
out of it -- was not checked, which is why this survived. Both are refused now,
with the tree root exempt because the mirror folder legitimately hangs off it.
The suite asserts the bad shape is caught **and** that a whole mirror at the
top still passes, so the new rule is known to discriminate rather than merely
to fire.

#### And the id was the mirror's as well

Making the move work exposed the next layer, which the driver found by printing
what it had kept: the tab came back as `firefox-0`. A mirror's ids are scoped
to it *on purpose* -- the paragraph above says why -- and `replace_mirror` mints
those same names on every refresh. A kept row holding one collides with a
mirrored tab in `m_id_index`, and `node_by_id` (the lifecycle and the AI payload
both use it) answers with whichever won. They would also share
`state/<id>.blob` and `state/<id>.history`.

So leaving a mirror re-mints the id, out of the ordinary `t`/`f` namespace. The
mark and the namespace belonged to the mirror equally, and they are given up
together.

That is the one case in this program where **an id changes during a node's
lifetime**, which everything else is entitled to assume it never does. It is
announced -- `id_changed(was, now)` -- because the shell keys live views, the
recently-used list and the state sidecar by id, and a mirrored tab can be open
at the moment it is dragged. A view left under a name nothing resolves is
invisible, uncloseable, and still holding a slot against the live-view cap.

The first version of the helper cleared and re-minted unconditionally, so
*every* dragged row was renamed. Three existing tests caught it within a minute
by asking whether a moved tab was still itself. The guard is that a node with no
mark has no marked descendant either -- a mirror is a whole subtree -- so an
unmarked node returns immediately, which is correctness rather than an
optimisation.

### Following it, which needed two problems solved first

**Tools ▸ Keep Firefox Tabs in Sync**, off unless asked for: reading another
program's files on a schedule is not something to start doing because the
feature exists.

**Polled, not watched.** `QFileSystemWatcher` is the obvious tool and the wrong
one. Firefox writes its session by creating a temporary file and renaming it
over the old one, so the inode the watcher holds stops being the file: the watch
fires once and then never again. Watching the *directory* trades that for every
unrelated write in the profile waking us. A timer that stats one path is duller
and does not stop working.

**A file change is not a tab change, and that is the whole design.** Firefox
rewrites that file constantly — scroll offsets, form state, which tab has focus
— so refreshing on every write would rebuild the mirror every few seconds while
the set of tabs sat perfectly still. The parsed result is reduced to a
fingerprint over each tab's address and title, and nothing moves unless *that*
changes. Cheap check first (size and mtime), the expensive read only when it
fires, and the answer thrown away when it turns out to say the same thing.

The fingerprint deliberately ignores which window a tab is in — dragging a tab
between two Firefox windows is not a change worth rebuilding for — and
deliberately is not a count, because closing one tab while opening another is a
change a count would call identical. Both are checked.

**And the refresh had to stop being destructive.** `replace_mirror` used
`beginResetModel`, which collapses every folder in the tree. Fine for a menu
click; on a timer it would fold up the user's own work every time Firefox
happened to save. It uses row signals now — one row out, one row in, at the root
— so a poll touches nothing but the mirror. The menu click still expands the
tree afterwards and a poll does not, for the same reason.

**28 checks** in `test_session`, including a real poller driven against a real
mozlz4 file rewritten underneath it: an untouched file reports nothing, a
rewritten file holding the *same* tabs reports nothing, and a genuinely
different tab set reports exactly once.

### Chromium, which writes a command log rather than a document

**There is no snapshot to read.** The file is `SNSS` and a version, then a run
of records — a uint16 length, a one-byte command id, a payload — and the set of
open tabs is what you get by *replaying* them in order. A tab that navigated
twenty times is twenty records; a tab that was closed is still all of them,
followed by a close.

**Every constant came from Chromium's own source, which is already in this
tree**: Qt WebEngine bundles Chromium, so `components/sessions/core/` is sitting
in the Qt sources this project builds against. The container framing is
`command_storage_backend.cc`, the ids are `session_service_commands.cc`, and the
navigation payload is a `base::Pickle` whose field order is
`serialized_navigation_entry.cc`. Each was then checked against a live file
rather than trusted.

The layouts, since they are the part that rots: a navigation is a pickle —
its own uint32 size, then `int32 tab_id`, `int32 index`, a length-prefixed utf-8
url padded to four bytes, a length-prefixed **utf-16** title whose length counts
*characters*. The others are raw structs, not pickles: selected-index and
set-tab-window are two `int32`s, and the close commands lead with the id, which
is all this reads — so nothing here depends on how a compiler padded them.

**The version is refused by number when it is not one this knows.** Versions 2
and 4 are the encrypted variants and there is no key here; anything higher is
newer than this reader. Saying *which* version was found beats "could not read
it", because this is internal API with no stability promise and the number is
the first thing worth knowing when it stops working.

**Replayed live: 129 tabs, every one a valid address with a label.**

**The honest gap: there is nothing to compare against.** The Firefox decoder
could be checked byte for byte against python's `lz4.block`, and nothing else on
a normal machine reads an SNSS file. So the suite checks structure and
plausibility — a parser with the offsets wrong produces mojibake and fragments
of neighbouring fields, not 129 valid urls — and it builds a log of its own to
drive the properties only a log has: that a tab sits at the entry it *selected*
rather than the last one written, that a tab closed later in the log does not
come back however many navigations it accumulated first, that closing a window
takes its tabs, and that a half-written final record is normal rather than
corruption, because the file is being appended to by a running browser.

**And it is the fresher of the two.** `command_storage_manager.cc` sets
`kSaveDelay` to 2500 ms, against a Firefox interval this project could not read
off disk at all. The harder format is the more current one.

### Driven through the shell, both at once

`try_import` clicks the menu items on a real window against the live profiles:
**81 tabs from Firefox and 129 from Chromium**, two mirrors side by side above
the user's own tree, every row carrying a label and an address.

Then it saves, which is what the shell does on any structural change, and reads
the file back. With 210 of somebody else's tabs on screen the tree file is still

```
- [f0] folder | Mine
  - [a1] unopened | Blank | about:blank | created=… | seen=…
```

That is the invariant worth driving rather than asserting: getting it wrong
writes two hundred foreign tabs into the canonical record, indistinguishable
from tabs the user filed, returning on every launch with nothing to remove them.

A detail that fell out rather than being designed: mirrored rows render in the
muted style the model already gives an unopened tab, so they read as *not yours*
without any new styling.

### Where a tab had been, kept (§4.2)

Both session formats carry each tab's whole back/forward list, and both parsers
read it in order to answer one question -- *which entry is this tab on?* -- and
then threw the rest away. Firefox writes `entries[]` with a 1-based `index`;
Chromium writes one `TabNavigationPath` command per entry and a second command
naming the selected one. The list was already in memory in both cases. What it
cost to keep it was a field.

It is kept as **a record, not a Back button**, and the distinction is the whole
design:

- **Only urls and titles cross.** That is all one browser's session file offers
  another. No scroll position, no form contents, no cache keys, no engine
  state -- and nothing that could be reconstructed later, because the pages
  have moved on.
- **It is stored in our own format**, `state/<id>.history`, beside the engine's
  opaque `state/<id>.blob` and never inside it. The two have different
  lifetimes: a blob is Qt WebEngine's, is unreadable to anything else, and is
  rightly discarded when the engine version moves on. This has to survive
  exactly that, so it is line-based text in the tree file's own shape --
  ` | `-separated, editable by a person, split on the *first* separator because
  a title may contain one and a url may not.
- **`state_store::remove` deletes both.** A history left behind by a delete is
  the collision that file's own comment already warns about, one file over: the
  next node handed that id would open wearing a stranger's past.

**Opening an entry makes a sub-tab** (§5.5), below the row it came from. Not a
navigation: sending the tab back into its own past would rewrite the address
the record exists to preserve.

Three places show it, and the split is deliberate. The row carries a count and
nothing else -- `Music  · 2 back` -- because a record nobody can see is one
nobody reads, and because *how many* is the question people actually have.

**It is a column, not a suffix on the title, and that was the second
correction.** Appended to the label it was the first thing elision ate: on a
tree panel narrow enough to be useful beside a page, a row read
`PDA / RetroGameHandhelds  - 6...`, cutting off the one part that was not
already obvious from looking at it. A column of its own is sized to its
contents and cannot be elided, and the title elides around it exactly as
before. Right-aligned, so the numbers line up down the tree, and muted,
because it is an annotation rather than something competing with the title.

Two things had to follow from a second column, and only one was obvious.
`QHeaderView` stretches its last section by default and ignores the resize
mode until told not to, which puts the count hard against the panel edge with
a gulf between it and its row. The other is that `QAbstractItemView` selects
**items**, not rows -- invisible while a model has one column, and the moment
it has two a click on the count selects that cell alone, leaving
`selected_node` (which takes column 0 and skips the rest) with nothing to
open, rename or delete. The whole context menu would have gone quiet on
exactly the rows this feature is about.

The model was already written for this: `mimeData` filtered non-zero columns
with the comment "one entry per row, not per column", `rowCount` answered 0
for them, and `dropMimeData` had `Q_UNUSED(column)`. Qt's own
`QAbstractItemModelTester` passes over two columns with no complaints.

The first version counted only backwards, which left a hole of exactly the
kind the suffix exists to close: a tab sitting at the *start* of its history
has pages only ahead of it, so the count was zero and the row said nothing at
all. Measured on the recovered Chromium tabs, 30 of 1144 records were that
shape -- a record with no way to discover it exists. A row with nothing behind
it now reads `· 3 ahead` instead. Only ever one of the two, and back wins where
both apply: the suffix is a hint that there is something to open, not a summary
of it, and two numbers cost more width than they buy when the dialog states
both anyway. The
properties dialog carries the list, capped at four rows high, since one imported
tab here had ninety and a dialog that grows to fit its longest field goes off
the bottom of the screen for the tab that most needs reading. The entry the tab
stands on is marked in the list rather than merely selected, because selection
is the user's and moves the moment they click.

The suffix is **drawn, not stored**: appended in `DisplayRole` only, while
search matches the node's own title and url and sorting reads `title_role`. A
test asserts all three, with a positive control on the search -- a proxy that
matched nothing would report the same zero.

#### The crossing that had to work

A mirror is never written to the tree file and is replaced wholesale on every
refresh, so an imported history exists only until the source is re-read. The
one moment it can be kept is `deep_copy`, which is what dragging a row out of
the mirror runs. It copies for the same reason `tags` does and the state blob
does not: it describes the *address*, not a live view of it. `test_model`
drives that path directly, because a history that does not survive it is one
that was never anything but a dialog.

#### What was found on the way

- **The position cannot be located by matching the url.** The first version
  searched the built list for the tab's own address, which answers 0 for a tab
  that went A, B, A and is standing on the second A -- two steps further back
  than it really is. It is counted as the list is built instead, which also
  handles entries with no url being dropped: a position in `entries` is not a
  position in `history`. The Chromium side never had the bug, being keyed by
  navigation index rather than by content.
- **`QHash` has no order.** Chromium's navigations are collected into one, so
  the keys are sorted before the list is built. Unsorted, the record reads as a
  tab that had visited its own past at random.
- **A test that passes is not a test that discriminates.** The duplicate-url
  assertion was checked by reintroducing the defect and watching it fail; the
  neighbouring check -- that the position points at the address the tab
  imported as -- passed throughout, because entry 0 happens to hold the same
  url. Only the explicit `== 2` catches it.

## The icon

`icon/` holds the app icon and `icon/build_icons.py` regenerates every size
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

All seven sizes are compiled in through `icon/hydra.qrc` and added to a single
`QIcon` in `main.cpp`, so Qt chooses per use instead of rescaling one image;
adding only the large one would quietly discard the tuned small ones.
`packaging/install-icons.sh` lays the same files into a `hicolor` theme with
the desktop entry.

**Judge small icons at their real size.** `test/` has nothing for this because
it is not a testable property, but the working method is: render at 16 and 32,
magnify with nearest-neighbour on both a light and a dark ground, and look at
the pixels. Every mistake in this section — the fringe, the ringing, the
strawberry, the loose crop — was invisible at 256px and obvious at 16.

## Build & run

```sh
make                          # build
make run                      # build and run it on sample-tree.txt
make test                     # the 24 suites that need nothing but a build
make help                     # android, install, clean, DEBUG=1, SANITIZE=1
./build/hydra my-tree.txt     # or a custom outline file
```

**The Makefile is the single entry point, and it wraps qmake.** It exists
because this tree was the odd one out: beerssh and fuzzypickles' `gui/` subtree
both present `make` / `make test` / `make android` with `DEBUG=1` and
`SANITIZE=1`, and hydra presented two different build invocations plus a
per-binary test run you had to know to prefix with `QT_QPA_PLATFORM=offscreen`.
Now all three look the same from outside.

**Two build systems are maintained, and CMake is not one of them.** The
migration this section used to describe as deferred is closed, and the
`CMakeLists.txt` files are gone from the tree:

- **qmake, driven from the Makefile** (`hydra.pro`) builds the app and the APK.
- **plain Make** (`test/Makefile`) builds the test tree.
- **fmake** builds the same sources from no build file at all, as a cross-check
  and a second opinion. It needs six annotations in the sources — one `@target`
  and five `@pkg_optional` — and nothing else.

The Makefile's own header records what the migration had to solve, so none of
it is rediscovered: 72 executables, globbed rather than listed, so adding a
suite costs no build-system work; the two unrelated libraries both called
`libtorrent`, where `find_package(LibtorrentRasterbar)` used to do the
disambiguating and `hydra.pro` now asks for `libtorrent-rasterbar` by name;
the host pkg-config answering cheerfully about the host during an Android
cross build, which `hydra.pro` avoids by asking for no optional packages under
`android {}`; and the APK, which CMake alone used to build through `qt-cmake`
and which `make android` now drives through the kit's own `qmake`.

Requires Qt 6.8 or newer with **Widgets** and **WebEngineWidgets** (Arch:
`qt6-base qt6-webengine`; Debian/Ubuntu: `qt6-base-dev qt6-webengine-dev`),
C++17. Clone with `--recurse-submodules`, or run `git submodule update --init
--depth 1` — `third_party/yt-dlp` is vendored for the site-extractor work
(arch §11.5) and is source and tooling, not something the build compiles.
On Linux this is an X11 / XWayland app: `main.cpp` forces
`QT_QPA_PLATFORM=xcb` there unless the environment already set it. That forcing
is guarded to desktop Linux, so other platforms keep Qt's own default plugin.

Three optional dependencies. Two are on the same pattern — found, and the
feature is on; absent, and it reports itself unavailable with no degraded mode:
`libsodium` (KeePassXC bridge) and `libtorrent-rasterbar` (BitTorrent
downloads). The third, `libsecret`, degrades rather than disappearing: without
it the KeePassXC pairing still works and simply does not survive a restart, so
its absence is a configure-time `STATUS` and not a `WARNING` — the build is
smaller, not broken. On Debian/Ubuntu:
`libsodium-dev libtorrent-rasterbar-dev libsecret-1-dev`.

**Watch the library name.** Both rasterbar's and rakshasa's unrelated libraries
install a pkg-config file called `libtorrent`, and the bare name resolves to
rakshasa's. `hydra.pro` asks for the *qualified* `libtorrent-rasterbar`
pkg-config name, and `torrent_download_source.cpp` carries the same qualified
name in an `@pkg_optional` beside the include, so fmake asks for the same one.

### Tests

`test/` holds the harnesses, built separately from the app — `hydra.pro` never
references them, and `make test` is what builds and runs them:

```sh
make test                              # every offline suite
make test-one T=test_seam              # one of them
make -C test -j2 offline              # or drive the test tree directly;
                                       # a job limit, always: see the warning above
QT_QPA_PLATFORM=offscreen ./test/build-make/test_seam
```

`test/README.md` says which suites need a helper server, libtorrent, or a
model, and records the traps that cost time — screenshots going black when the
screen blanks, `import` hanging against a modal grab, and libtorrent exempting
loopback peers from rate limits so a "throttled" local transfer finishes
instantly.

## Build-verification state

**Builds and runs.** Verified on Debian 13 with Qt 6.8.2 and
`qt6-webengine-dev` 6.8.2: a clean `make` produces `build/hydra` with
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

`test/live/try_cookies` drives it the way the interceptor was driven, and for
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

`test/live/try_permissions` drives geolocation, camera, microphone and
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

`hydra.pro` requires **Qt 6.8**, and refuses at configure time rather than
letting the compiler discover it. The floor used to be stated as 6.4, derived
once from the menu bar's `addAction(text, shortcut, receiver, member)` argument
order and never derived again as the code moved. Two things raised it since and
neither updated the number: `theme.h` names `Qt::ColorScheme`, which arrived in
6.5, and `qtwebengine_view.cpp` includes `QWebEnginePermission`, which arrived
in 6.8 — and that include sits *outside* the `#if QT_VERSION >= 6.8` guard
around the code using it, so the guard never bought anything.

CI on Qt 6.4 found it, as two hundred lines of "'ColorScheme' is not a member
of 'Qt'" a long way from any statement about versions. A configure that stops
and names the number is worth more than every one of them. 6.8.2 and 6.11 are
what it is actually built against; 6.5 to 6.7 are untested and are excluded by
that include rather than by evidence.

Qt WebEngine is a **system dependency, not a vendored one** — it bundles
Chromium, must be ABI-matched to the rest of Qt, and is LGPLv3/GPL/commercial
(arch §2), so linking the platform's build is far simpler than carrying it. How
it arrives differs per platform, but the build side is identical everywhere
(`QT += webenginewidgets` in `hydra.pro`, with the kit's own `qmake` on PATH,
or `QT_ROOT` pointed at the Qt install where needed):

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
| Policy model | `policy.{h,cpp}`, `policy_engine.{h,cpp}` | packed 2-bit tri-states, precedence, INI (`load_json` is private and reads a legacy file once) |
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
| Password manager | `keepass_protocol.{h,cpp}`, `keepass_bridge.{h,cpp}`, `box_crypto.{h,cpp}` | KeePassXC-Browser client; no vault, no master password |
| Pairing at rest | `credential_store.{h,cpp}` | the association key in the session's Secret Service, libsecret optional; without it the pairing does not survive a restart and the app says so |
| Autofill | `autofill_controller.{h,cpp}`, `autofill_script.h` | QWebChannel bridge, origin gate, policy-governed |
| Passkeys | `webauth_dialog.{h,cpp}` | WebAuthn's state machine as a dialog; engine-neutral, driven from `qtwebengine_view` |
| Consent banners | `consent_blocker.{h,cpp}`, `site_rules.{h,cpp}`, `consent_dialog.{h,cpp}` | answers "accept cookies?" dialogs; rules as data, shareable later |
| Anti-adblock notice | `antiadblock_watch.{h,cpp}` | says so when a page is checking for a blocker, and names the lever |
| Shared rule store | `site_rules.{h,cpp}` | consent-banner and detector rules as one file, with provenance; the unit a future exchange would move |
| Tree view | `tab_tree_view.{h,cpp}`, `tree_invariants.{h,cpp}` | context menu, drag and drop, properties editor; the invariants a rewrite must not break |
| Chrome dialogs | `auth_dialog.{h,cpp}`, `cert_dialog.{h,cpp}`, `annoyed_dialog.{h,cpp}`, `annoyance_log.{h,cpp}` | what the network puts in front of you -- password, client certificate -- and the one-click "something got through here" report |
| Shared UI helpers | `empty_state.{h,cpp}`, `flow_layout.{h,cpp}`, `theme.{h,cpp}` | the message an empty list shows, a button row that wraps instead of squeezing, and the colour and icon scheme |
| Page tools | `find_bar.{h,cpp}`, `element_picker.{h,cpp}`, `picker_script.h`, `cosmetic_filters.{h,cpp}` | find in page, pick an element to block, and the rules that hide it |
| Site extractors | `site_extractor.{h,cpp}`, `extractor_signals.{h,cpp}`, `extractor_dialog.{h,cpp}`, `extractor_helpers.{h,cpp}`, `stream_probe.{h,cpp}` | a per-site script that names the stream, the evidence it is written from, and the budgeted fetch a helper may make |
| Media assembly | `hls_playlist.{h,cpp}`, `hls_assembler.{h,cpp}`, `network_fetcher.{h,cpp}`, `local_proxy.{h,cpp}`, `capture_source.{h,cpp}` | segments to one file, and a recording in progress presented as a download |
| Session import | `session_import.{h,cpp}`, `session_mirror.{h,cpp}` | another browser's open tabs, read and then followed |
| Settings file | `settings_bundle.{h,cpp}` | everything on the settings pages plus site exceptions and accepted rules, in one INI you can read and carry |
| Scheme rules | `scheme_rules.{h,cpp}` | which urls the engine renders itself and which are somebody else's to open |
| Android backend | `android_view.{h,cpp}`, `android_downloads.{h,cpp}`, `android_intents.{h,cpp}`, `android_dialogs.{h,cpp}`, `bridge_invoker.{h,cpp}` | System WebView behind the same seam, plus the platform's downloads, external links and phone-sized dialogs |

Persistence: `policy.ini`, `state/<id>.blob`, and the tree file all sit next to
the outline file passed on the command line — checked against
`main_window.cpp`, which is the only place that names the path, because this
line said `policy.json` for as long as the INI migration had been done and
written up elsewhere in this file.

The one thing that does **not** sit there is the KeePassXC association key,
which is in the session keyring instead (§13.1, §14). It is the only secret this
project keeps, and a file beside the tree is exactly where it must not be.

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
  That last clause was doing more work than it should have: `apply_settings`
  and `set_zoom_factor` were `Q_UNUSED` stubs on Android, so the settings
  dialog offered five toggles and a zoom control, the shell recorded every
  answer, and the page carried on exactly as before with nothing said
  anywhere. That is the worst shape a setting can have — a control that
  looks connected. Both are real now, over JNI: four of the five map onto
  `WebSettings` calls meaning what the desktop's `QWebEngineSettings`
  attributes mean, and `scrollbars` onto the View's own scrollbar flags.
  The mapping is written beside each call in `HydraWebView.applySettings`
  rather than only here, because a mapping recorded away from the code that
  implements it is one that drifts.

  One field is still not honoured, and deliberately: `setSupportMultipleWindows`
  would route `window.open` to a `WebChromeClient.onCreateWindow` this class
  does not implement, and an unhandled one drops the request without a word,
  so switching it on would make "popups allowed" mean "popups silently
  discarded". Left off, an allowed `window.open` navigates the same WebView —
  not the desktop's new tab under the page that asked, but it happens, and it
  still goes through the navigation decider. Wiring `new_window_requested`
  through from `onCreateWindow` is the real answer and is a piece of work
  rather than a flag.

  Zoom has a platform difference worth stating rather than papering over.
  Android's `setInitialScale` is a viewport scale in device pixels, so 100 is
  not the identity — a literal 100 draws the page at a third of its size on a
  3x phone — and only the density-relative value means "unzoomed". What could
  not be settled from here is whether it re-scales a page that is already
  loaded; the desktop's `setZoomFactor` does, Android documents an *initial*
  scale and says nothing about the rest, and driving the View menu over adb
  kept leaving the app instead of opening it. Recorded as open, in the Java
  where somebody with the phone in their hand will find it.
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
`disableInput`, and an escape *gesture* as opposed to the flag. ~~And any
settings UI — `kiosk_config` is currently code-level defaults with no way to
edit it from the app.~~ **That one was built** — see "Kiosk, which nobody could
configure" below. This line went on claiming otherwise for as long as the
section describing the fix sat a thousand lines further down, which is the third
time this file has contradicted itself about work it records elsewhere.

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

**Not done from §9:** the web-session provider (§9.1 rates it least preferred)
and merging on the duplicate-URL changes — those are detected and listed, never
pre-selected, and applying one is currently a no-op. The undo snapshot was in
this list until 2026-08-27 and had been built for some time; see *Reorganizer
undo* below.

### What a duplicate-URL merge would have to decide, before anybody writes one

Designed ahead of the code because the decision is the work here, and because
one of the constraints is not obvious until it is looked for.

**The blocking one: today's undo cannot undo a merge.** §9.4's snapshot is
`tree_snapshot_entry` -- id, parent, title, order, folder -- and its own
section says why that is enough: "structure is the only thing a reorganization
changes ... live views and state blobs are keyed by id and were never stored on
the node". A merge breaks that premise. It deletes a node and it changes
content on the survivor, and neither url, tags nor history is in the snapshot.
Worse than losing the undo, the undo would still *appear* to work: restore
skips an id that is no longer in the model rather than resurrecting it, so
Undo Reorganize would report success and quietly not bring the tab back. Any
merge that ships before this is answered is a destructive change with a broken
revert sitting next to it.

Two ways out, and they are genuinely different features:

- **Extend the snapshot** to carry a deleted leaf whole -- url, tags, history,
  created, last_seen -- so undo is lossless. That is the honest merge, and it
  costs a wider snapshot and a restore that can re-create a node rather than
  only re-parent one.
- **Do not delete.** Move the duplicate under the survivor, or into a folder,
  so a "merge" is a *move*. The existing snapshot handles that losslessly and
  for free, because it is exactly the structural change it was built for. But
  it does not merge anything: two entries remain, with their tags and histories
  apart, and the list still shows both.

**If it is the real merge, five things need answers, and only one is obvious.**

- **Tags: union.** The only lossless choice; dropping one side's tags is data
  loss nobody asked for.
- **Which id survives.** Ids key the live view and the state blob, so the
  survivor keeps its open page and the other's is discarded. Preferring one
  that is currently open, then the earlier `created`, then tree order, loses
  the least.
- **History.** The two are different sessions of the same address and cannot
  be concatenated into one back/forward list meaningfully. Keeping the
  survivor's discards a real record; keeping the longer one is defensible
  because history is content and moves independently of the id.
- **Title.** They differ or there would be nothing to choose. The later
  `last_seen` is the better guess than the survivor's.
- **created and last_seen.** Earliest created, latest last_seen -- the merged
  tab has existed since the first and was seen at the second.

**A recommendation, since one is wanted rather than a menu.** Extend the
snapshot and build the real merge, with tags unioned, the open node preferred
as survivor, the longer history kept, and the later title. The cheap version
is worth naming only to reject: a "merge" that leaves both entries in the list
is the reorganizer offering a control whose name does not match what it does,
which is the defect that was just removed from this same dialog.

**And the no-op was reachable, which is the part that was a defect rather than
a gap.** `tree_diff` calls the duplicate kind advisory and its apply arm does
nothing, deliberately, because merging is destructive and unbuilt. But
`reorganize_dialog::show_diff()` made *every* row user-checkable without
looking at its kind, so a duplicate could be ticked and applied — and applying
it did nothing at all: no merge, no refusal, and no entry in the count, so the
"Applied N changes" line then under-reported. A control offered and ignored is
the shape this tree has spent a day removing, and it was sitting in the one
dialog whose entire promise is that you choose what gets applied.

Duplicate rows are advisory in the list now — greyed, no check box — rather
than hidden, because the duplicate is worth knowing about and hiding it would
lose the only place the tree mentions it. The check box comes back when the
merge does.

One thing was worth probing rather than assuming, since `on_accept()` reads
`checkState()` back for every row including the ones it no longer offers: an
item with no check state ever set answers `Unchecked`, not something that
would mark a duplicate accepted. Measured, because the failure would have been
silent and would have applied a destructive change nobody selected.

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

**Everything from §11–§12 is built.** The regression set is the last of it, and
it found a defect on its first run.

**A corpus rather than more single cases, because the direction that rots is
false positives.** Per-url checks catch a manifest that stops being recognised;
what they do not catch is the badge filling with things that merely look like
media, and that is the failure that worsens quietly, since a detector finding
too much still finds the real stream. The set is one page's whole request
stream -- a manifest, two of its segments, a direct file, an audio file, and
eight things a page of that shape also loads: a script, a stylesheet, an SVG, a
photograph, a font, an API call, a beacon whose *query* mentions mp4, and an
opaque `.bin` chunk. It asserts the count, then names every row, because a
count alone lets one new false positive hide one new false negative.

Synthesised deliberately. `evidence/` holds real captures and is gitignored
precisely because a capture is somebody's browsing with tokens in the query, and
a regression corpus is a thing that gets committed.

**What it found: every unrecognised request was being credited to the nearest
manifest.** `classify()` returned `media_kind::segment` for anything it could
not place -- documented as "not saveable; caller checks the flag" -- and
`on_request` does check the flag, then asks a second question, "is this a
segment?", to which the answer was yes for the script, the stylesheet, the
font, the beacon and the rest. Each took the segment path and incremented the
nearest manifest's `hits`. On this corpus the manifest reached ten hits from
two segments.

That matters because `hits` is what decides the primary stream, and the primary
is what §11.3 hands a player. On a page with one manifest nothing shows; on a
page with variant manifests under `/hd/` and `/720p/`, which is the ordinary
shape, the stream chosen was partly decided by how much unrelated traffic
happened to share a directory prefix with it. Unrecognised requests have their
own `media_kind::unknown` now, and the arms are named rather than defaulted, so
the one consumer that had to change was a compiler warning rather than a
silent "Unknown".

The corpus asserts `hits == 2` and not `>= 2`, which is the shape that let this
through: the first version of the check passed against a manifest sitting on
ten.

**The ffmpeg remux is built.** §11.2 asks that "if `ffmpeg` is present the
manager remuxes to a clean `.mp4`/`.mkv`, degrading to raw-segment save without
it", and `media_remux` is that step: `-c copy`, never a re-encode. The segments
are already H.264/AAC and the only thing wrong with them is the container, so
rewrapping takes seconds where transcoding would take minutes and lose a
generation for nothing. A stream `-c copy` cannot rewrap keeps its `.ts` and
says so, which is the honest outcome rather than spending somebody's CPU on a
conversion they did not ask for.

**It runs on save and not on watch**, and that is not an oversight. A player
already has the assembled file open and has been reading it since the first
segment landed -- the tee-to-disk trick above -- so rewrapping then would
replace a file underneath a running player to gain a container nobody is about
to seek around.

**The exit code does not decide; the artifact does.** ffmpeg can return zero
having written nothing usable, a truncated input being the ordinary way, so the
output is asked whether it exists and has bytes before anything is claimed and
before the input it replaces is deleted. A zero-length stub is removed rather
than left, since a file that is there and empty is worse than one that is not.
Removing the raw concatenation is best-effort: failing the save because a
delete failed would be losing the good outcome over the tidy one.

Verified in two halves, because the halves need different instruments. The
decisions -- what the output is called, what ffmpeg is told -- are pure and sit
inline in the header, tested in `test_streamtype`; the one that matters most
there is that a name the code does not recognise gains `.mp4` rather than
losing three characters, because trimming blindly would turn `movie.webm` into
`movie.w` and write beside a file nobody asked about. The behaviour needs a
process and was probed separately: a real MPEG-TS is rewrapped and the raw file
removed, and an input ffmpeg cannot read reports failure, keeps the original,
and leaves no stub. The argument list was also run against a real transport
stream produced for the purpose, and `ffprobe` confirms the result is genuinely
an MP4 rather than a file with the right name.

**The per-site auto-detect toggle is built**, and the architecture doc had
already decided where it goes: "a per-site 'auto-detect media' toggle lives in
the PolicyEngine" (§11). So it is a `policy::feature` like any other, which
made it four edits and no new UI at all -- the shield and the settings page
iterate `feature_count()` and picked it up unaided, as they did for clipboard
reading and pointer lock.

**It refuses at the detector, not at the badge**, and the difference is the
whole point. Gating the badge would have left `media_detector` recording every
stream a site served and merely declining to mention them, so turning the
setting off would have looked like privacy and been bookkeeping. The check sits
at the top of `on_request`, so a site that is switched off has nothing written
down about it in the first place.

Allowed by default, because noticing media is most of what the badge is for and
a browser that saw nothing until told to would be the wrong default. The policy
engine is consulted from the interceptor thread, which is what `policy_engine`
already licenses -- the rule set is mutated only on the UI thread and reads
tolerate a stale snapshot, the same terms `request_filter` runs under from the
same callback. A detector built without a policy watches everything, so the
drivers and tests that never set one up are unchanged.

Four cases in `test_settings`, and they are the same request three times with
only the policy moved: recorded when allowed, nothing recorded when blocked for
that site, still recorded for a site the rule was not set on -- which is what
catches a gate that turned the detector off everywhere -- and still recorded
with no policy at all. Checks that did not differ between those would have been
testing that a url is saveable, which `test_streamtype` already covers and
which would pass whatever the gate did.

**This list named seven things on 2026-08-27 and five of them were built.**
Checked one at a time against the code rather than against the file names,
which is the distinction that matters -- `hls_assembler.cpp` existing proves
nothing on its own, and its header is what settles each of these:

- **segment assembly** -- built; that is what `hls_assembler` is.
- **tee-to-disk for scrubbing a live stream** -- built, and by the same class.
  Its header says so outright: segments are written as they arrive, so a live
  stream becomes a local VOD with full backward seek.
- **the local proxy injecting headers for CDNs that 403 a naked URL** -- built.
  `local_proxy.cpp` sets `Referer` and `User-Agent` from the stream context.
- **a downloads window** -- built, `downloads_dialog`, on Ctrl+J in Tools, and
  named in this document's own layout table a few hundred lines above.

What survives is the ffmpeg remux, which `hls_assembler`'s header explicitly
disclaims -- "that is the ffmpeg step sec 11.2 describes and this does not
do", the reason being that concatenated MPEG-TS is directly playable while
fMP4 would need its init segment prepended -- the per-site auto-detect toggle,
which is absent, and the media regression set, which is not `test_replay`:
that re-scores recorded model replies for the extractor and is a different
thing wearing a similar description.

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
wrong nonce and a wrong key all pass. ~~What remains unexercised is everything
above the crypto — the socket handshake, association, and `get-logins` have
never run, because KeePassXC itself is not installed.~~ **All of it runs now.**
KeePassXC is installed, and the handshake, association, a stored pairing
restored and accepted, and `get-logins` against a real vault are measured — see
"The KeePassXC bridge finally met KeePassXC" and "A pairing that survives a
restart". Also not done from §13: `set-login` on new-credential submit, and
the optional direct-`.kdbx` fallback, which §13.4 recommends against anyway.

**This list used to be longer and was wrong.** It named the key icon, the
entry-picker UI, `generate-password` and storing the association key via
Secret Service as outstanding -- while pointing, in the same sentence, at the
sections describing two of them working. All four are built:
`QInputDialog::getItem` is the picker, `credential_store` is the Secret
Service half, Tools -> Passwords carries Generate Password, and the key has
had an icon for some time and is permanent now. **A list of what is left to do
is the part of a document that rots first**, because finishing something
changes it and nothing points back here.

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
again the fix is invisible. Seven checks (`test/live/try_adblock_fix`), driven
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
`site_rules` is therefore a file (`site-rules.ini`, beside the tree and
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

**Verified through the real shell, 34 checks** (`test/live/try_consent`),
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

**Edit → Undo Reorganize (Ctrl+Shift+Z)**, enabled only after an accepted
reorganization. It was in Tools when this section was written, at the bottom
below four AI actions; it is first in Edit now, which is where three decades of
software has put undo, and the code comment records the move. §9.4 asks for "a single undo snapshot [that] makes any accepted
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

**15 checks** (`test/live/try_settings_ui`): the list and stack exist and are
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
`test/live/try_subframe` is the reproduction and the proof, offline and
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

Found in one of two ways, in this order: the vendored submodule under
`python3`; else `yt-dlp` on PATH. Neither means `available()` is false and the
action reports why — nothing else degrades.

**The vendored copy is preferred because it is pinned**, and that order was
the other way round until it was measured. The reasoning for PATH-first was
that a package manager keeps that copy current, which is exactly what yt-dlp
is for; the premise turned out to be false here, and a stable distribution is
where it is falsest. This machine had 2025.04.30 on PATH against 2026.07.04
vendored — fourteen months of extractors, in the wrong direction. Devuan
backports offers 2026.03.17, which narrows the gap without closing it.

Currency is not the whole argument, though, and determinism is why the order
should stay this way even on a machine whose PATH copy is newer. What the
resolver returns is handed to the model as worked reference and used as
ground truth (§ extraction), so preferring PATH makes that pipeline's input
depend on whatever the distribution shipped, and the same page can resolve
differently on two machines. The pin makes the extractor set a fact of the
tree, and bumping the submodule is the one reviewable act that changes it —
at the cost that nothing bumps it on its own.

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

**yt-dlp is vendored** at `third_party/yt-dlp` (Unlicense, so it imposes
nothing whatever this tree is eventually licensed under; shallow submodule,
~15 MB). Three jobs: try it first and skip
the model entirely where it supports a site, since it is free and maintained
by people tracking site changes; hand its nearest extractor to the model as
worked reference when it does not; and use it as ground truth where it does.
Not a build dependency — neither `hydra.pro` nor `test/Makefile` refers to it.

**The pin tracks `master`, not a release tag**, and that is worth stating
because two things mislead otherwise. Upstream cuts releases every few weeks
while extractors are fixed daily, so the newest tag is routinely behind the
branch — when this was last bumped the tag `2026.07.04` was nineteen days
older than the commit already pinned, and "move to the latest release" would
have been a step backwards. And the version yt-dlp reports is the last
release's until the next one cuts, so `--version` says `2026.07.04` for
commits well past it: the sha is the only honest answer to what is vendored,
which is why bumping is read from the log rather than from a version string.

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

The step every prompt number in this file was waiting on. `test/live/
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

  **The "popups allowed" half of that was not a real arm, and the finding
  should be read without it.** Nothing implemented `newWindowRequested` at the
  time, so a `target="_blank"` link did nothing whether popups were permitted
  or not -- allowing them changed a setting that had no path to the behaviour.
  The blocked case was genuine (`JavascriptCanOpenWindows` does follow the
  policy), which is exactly why the gap survived: a refused popup and an
  unhandled one look identical from the page. What the run establishes is that
  the player does not stream *with ads allowed*; whether a window it wanted to
  open would have changed that is untested, and is now testable.

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

## The loop on a third site: 2 of 5, and for the wrong reason

The measurement the extractor-loop item existed for. Everything before it was dramafren, where
both winning runs matched `/cf-master.` — that site's own fragment — so "the loop
finds the stream" meant "the loop finds this stream". A fresh capture of kisskh
(89 requests, episode 24 of drama 10826) and five runs of `qwen2.5-coder:14b`
answer it.

**The headline is real: 2 of 5, and the two are exact.** Both returned
`https://hls.cdnvideo11.shop/hls07/10826/Ep24.v990_index.m3u8`, the gate accepted
both, and it confirmed HLS from the body rather than the name. That is the same
rate as dramafren's best, on a site that shares no fragment with it. The loop is
not pinned to one site.

**And the payload is exonerated, which is the part that had to be checked first.**
Site 2's 0 of 5 turned out to be a probe budget that never reached the media
host, so a miss here proves nothing until the payload is read. It now can be:
`HYDRA_DUMP_PAYLOAD=<path>` writes exactly what was sent. The manifest is there,
annotated correctly, and it is the *only* address that answered — the other four
probes went to Firebase and Google endpoints that 403'd or 404'd. The round-robin
budget and the playlist-first ranking spent their one useful answer on precisely
the right line. **So these five runs measure the model.**

### Both hits found it by extension, and none of the five read the note

Which is why "2 of 5" was the wrong number to remember.

| run | verdict | how it tried |
|---|---|---|
| 1 | rejected | `url.includes('->')`, then fell back to `ngsw-worker.js` — Angular's service worker |
| 2 | nothing | `url.includes('->')` and nothing else |
| 3 | nothing | looked for the literal `/manifest.m3u8`, which no site here serves |
| 4 | **accepted** | three `url.includes('->')` branches, then `path.endsWith('.m3u8')` |
| 5 | **accepted** | `url.includes('->')` first, then `url.endsWith('.m3u8')` |

**Four of the five tested the url for `->`.** The annotation was printed on the
same line as the address, and the model read it as part of the address — so
those branches matched nothing, every time. Both hits reached the manifest
through a *fallback* on the `.m3u8` extension. **Not one run used the note.**

kisskh is the undisguised control: its manifest honestly ends in `.m3u8`, so an
extension test finds it. dramafren's is `cf-master.<digits>.txt` and its
segments wear `.woff2`. The content-type note exists precisely to survive that,
and on this evidence it was never once consulted — so the same five runs would
have scored 0 on a capture that hides its manifest.

### Three arrangements of one fact, and the third one works

The fix took three tries because each one fixed the previous symptom and
produced the next. All three were measured, five runs a site, and the whole
progression is worth keeping because the shape of it generalises: **the model
believed what the layout implied, not what the prose said**, every time.

| arrangement | what the model then wrote | dramafren | kisskh |
|---|---|---|---|
| note appended to the url, `url -> type` | `url.includes('->')`, 4 of 5 | not run | 2 of 5, both by `.m3u8` fallback |
| note as a column named `serves` | `request.serves`, 8 of 10 | 0 of 5 | 0 of 5 |
| note as a legend keyed by request number | `url.includes('cf-master')` | **3 of 5** | 0 of 5 |

**The prose never changed its meaning across those three.** It said the url ends
where the note begins, then that `serves` is a column and not a field, then the
same thing again. What changed was where the note was *printed*, and that is
what moved the number both times.

**Arrangement two is the instructive one.** Moving the note into its own column
with the url last did exactly what it was designed to do — `url.includes('->')`
went to **0 of 15 runs** and never came back. And the result got *worse*,
because the misconception it replaced had an accidental fallback and the new one
did not: a model reaching for `request.serves` gets `undefined`, returns null,
and the run reports "the script found nothing". Fixing the symptom removed the
crutch that had been carrying the score.

**Arrangement three prints the note away from the rows entirely** — under the
table, keyed by request number, phrased as something that *turned out to be*
rather than something a request carries — and splits the work in two in the
prompt: read the notes now, find the address again by its shape in the function.

**On the site that needs it, three runs in five now return this:**

```js
if (request.url.includes('cf-master')) { … }
```

A stable path fragment, no tokens, picking the master manifest on a site with no
`.m3u8` anywhere and segments disguised as web fonts. That parser survives the
next visit, which nothing this project produced before today did.

### What kisskh's 0 of 5 measures, and it is not blindness

Checked before it was written down: its manifest **is** in the payload and **is**
annotated — *"request 44 turned out to be application/vnd.apple.mpegurl (HLS)"*
— so these runs measure the model. Every failure was refused by a different
layer, and none was a false accept:

| run | refused by |
|---|---|
| 1, 3 | the token check — this visit's ids written into the script |
| 2 | the content-type tier — it picked a Google Analytics beacon |
| 4, 5 | the furniture rule — the browser fetched that as an image |

It is worth being plain that this is **worse than the 2 of 5 it started with**,
and worth being equally plain that the 2 of 5 was never capability: it was an
extension test on the one site where an extension test works. What the loop does
now is reach for the note, and kisskh is the harder needle for that — one
annotated address among eighty-nine requests, most of them Google and Firebase
furniture, where dramafren's media host offers three in a row.

### The gate had to grow twice on the way

Neither of these is a wrong *answer*. Both are answers that pass every other
check and leave a stored extractor that fails on the next visit — accepted,
saved for the host, and broken later with nothing pointing back at the moment it
was accepted.

- **Tokens written into the script.** A model wrote a lookup table of the five
  annotated urls, tokens and all, and searched it. Genuinely requested,
  genuinely a manifest, genuinely fetched once. `embeds_a_token` asks the
  question `shape_of` already answers for addresses — query values of eight
  characters or more, digit runs of six or more in the path are the parts that
  rotate — and refuses a script repeating one verbatim. It runs *after* the
  invented rule, because a url the page never requested is a more basic fault;
  putting it first broke the test that pins that, and the test was right.
- **Matching on `order`.** Anticipated rather than measured, and flagged as such
  in the code: the legend is keyed by request number, and `order` is a real
  field, so joining on it compiles, runs, and is right about this capture only.
  Guarded before it could be accepted and stored.

And a script reading the note at run time is refused *before* it runs, because
running it hides the cause: it returns null and the gate says "found nothing",
which reads as a model that could not find the stream rather than one that found
it and asked the wrong object.

### The timing note was optimistic

This file says a 14B proposal takes "a minute or two" on this machine. Measured
across five: **333 s, 124 s, 173 s, 311 s, 274 s.** The first includes loading
9.9 GB of weights; the rest do not, and still average over three minutes on a
9.3 KB payload. Budget twenty minutes for five runs, not ten.

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

## The profile forgot everything, and nothing meant it to

**Measured, not inferred.** `qtwebengine_factory` took
`QWebEngineProfile::defaultProfile()` and nothing anywhere called
`setPersistentStoragePath`, `setStorageName`, `setPersistentCookiesPolicy` or
`setHttpCacheType`. In Qt 6.8 the default profile is off-the-record. The only
directory any run had ever produced was
`~/.local/share/Hydra/QtWebEngine/OffTheRecord/`; there was no `Default`
beside it.

So nothing a site stored survived quitting: cookies, localStorage, IndexedDB,
and the HTTP cache, which was memory-only. **Log in, quit, come back, and you
were logged out**, with nothing said about why.

It bit harder here than it would in most browsers, because this one
deliberately persists the tab tree *and* each tab's history. A restored tab
reloaded a site you were signed into and showed you signed out -- the tree
remembered and the session did not. It also contradicted a stated design: the
consent-banner work says "answering has to stick, which is why this touches
cookie policy... a consent choice is itself recorded in a cookie". The choice
could not stick in a profile that forgets.

**And the document had it exactly backwards.** §"Not done from §8" lists the
off-the-record profile as an unbuilt *kiosk* feature -- something to add when
a kiosk needs to forget. The whole browser had been off-the-record the entire
time, by inheriting a Qt default nobody chose. That is the fourth time this
file has contradicted itself about work it records elsewhere, and the first
time the contradiction was in the direction of a feature being *accidentally
present* rather than accidentally missing.

**Fixed, by the holder's instruction.** The factory constructs
`QWebEngineProfile("hydra")` instead of taking the default. A profile with a
storage name is persistent. The cookie policy and cache type are set
explicitly rather than left to a default, because the default is exactly what
was wrong before and a reader deserves to see the intent.

**A named profile has two homes, and this section claimed one.** Qt derives
them from different `QStandardPaths` roots through separate setters:
`persistentStoragePath()` is `AppDataLocation/QtWebEngine/<name>`, which for
this app is `~/.local/share/Hydra/QtWebEngine/hydra` -- beside the tree file
and the state directory, as this section said -- while `cachePath()` is
`CacheLocation/QtWebEngine/<name>`, which is `~/.cache/Hydra/QtWebEngine/hydra`
and nowhere near it. Both were read back off a profile constructed exactly as
the factory constructs it, with `XDG_DATA_HOME` and `XDG_CACHE_HOME` pointed
at separate scratch directories so that one root could not be mistaken for the
other. The sentence that said the profile lives under `AppDataLocation` would
send somebody clearing this browser's state by hand to a directory holding
none of the HTTP cache.

**A persistent profile also arrived with a second permission authority, and it
was refused.** `QWebEnginePage::permissionRequested` -- the only place the
shield's decider is consulted -- fires only while a permission's state is
`Ask`, and a named profile's default is to write a grant to disk. The engine
would then stop asking: a site blocked in the shield afterwards would keep the
camera it was once given, and no screen in this app lists or clears what
Chromium chose to remember. `AskEveryTime` is set explicitly. That is not a new
policy but the behaviour that was always here, now that it has to be asked for
rather than inherited from a profile that could not remember anything.

**Owning it is safe for a reason worth naming.** A `QWebEngineProfile` must
outlive every page using it, and `main()` declares the factory *before* the
window, so the window and all its pages are destroyed first. That comment in
`main()` about declaration order is load-bearing: the destructor now deletes
the profile, which would have been a crash under any other order and was
forbidden before, when the profile was Qt's own.

**Verified by restarting, not by reading.** A local server set a persistent
cookie and reported on each request whether it came back:

    REQUEST cookie_present=False    <- first visit, cookie set
    REQUEST cookie_present=True     <- second process, after a full quit
    REQUEST cookie_present=True

and the cookie is in the on-disk SQLite `Cookies` database in the new profile
directory, beside `History`, `Favicons` and `Local Storage`. The old
`OffTheRecord/` directory is left where it is; nothing reads it now.

**`try_cookies` had to be repointed at the factory's profile, and the size of
that was overstated when it was fixed.** The driver builds the real shell and
then took `defaultProfile()`'s cookie store, which after the switch is a
profile no page here is ever loaded into. The commit that fixed it said every
case in the only driver exercising the cookie filter would report the filter
broken. That was a prediction, carried over from the reconnaissance that found
it, and it was never run -- the fix was made blind and the driver not executed
until later.

Measured since, by putting `defaultProfile()` back and rebuilding: **8 passed,
3 failed**, against 12 passed with the fix. The three are the ones that ask
whether a cookie was *stored* -- "the page's own cookie is stored", "still
stored", and "stored over TLS" -- which are exactly the checks that have to
look in a jar. The other nine ask whether a request carried a cookie or was
refused, and a page can be observed doing that whichever store is being
watched.

So the fix is load-bearing and the pass means something, which is the point of
having run the control at all. But the driver is markedly less sensitive to
which profile it watches than the prediction implied, and a reader who took
the commit at its word would be surprised by eight green checks on a driver
pointed at the wrong store.
Still open, and sharper than it was: **kiosk mode now leaks.** The note above
lists an off-the-record profile as an unbuilt kiosk feature, and that is still
true as a *kiosk* feature -- it just is not the whole browser's business. What
changed is the cost of not having it. While the whole browser forgot
everything it was a nice-to-have; now `kiosk_controller` has no profile of its
own and shares the factory's persistent one, so the idle timer resets the
*navigation* and nothing else, and the previous person's cookies and logins
are on disk for whoever touches the screen next. Wiring one for kiosk is its
own piece of work.

## A page could not download anything, and never said so

Clicking a download in a page did nothing at all. Not an error, not a status
message -- nothing, which is indistinguishable from a page that ignored the
click. The cause is one line that was never written: **nothing was connected
to `QWebEngineProfile::downloadRequested`.** Chromium asks the application
what to do with a download, and Qt cancels a request nobody accepts. Every
page-initiated download in this browser's life had been silently cancelled.

The factory's own class comment had been promising it since step 6 -- *"The
download handler attaches here too"* -- so this was a known gap that stopped
being visible once it was written down.

**The engine keeps the transfer**, which is the design decision worth stating.
`download_manager` fetches a url itself, and that is right for the things it
was built for: a magnet link, or a stream the media dialog found -- things the
page never had in its hands. A download a page starts is the opposite case. It
may be a `blob:` the page assembled in memory, which exists nowhere but that
renderer, or a url that means nothing without the session's cookies and
headers. Refetching either from outside the engine gets a login page or an
error. So the engine downloads and the shell only reports.

The seam gains `set_download_handler`, called when a transfer starts and again
when it ends with a flag saying whether it succeeded. **Reported both times on
purpose**: a download that fails silently is the exact shape this feature
existed to fix, and a start message alone would replace one silence with a
half-truth.

The destination is `QStandardPaths::DownloadLocation` and the name is
Chromium's suggestion, put through a check for a free one -- `report (1).txt`
beside `report.txt`, numbered rather than timestamped so the order is legible,
and the extension kept where there is one. Writing the suggested name blind is
how a second download destroys the first. A leading dot is treated as the
whole name rather than an extension, so `.bashrc` numbers as `.bashrc (1)`
instead of growing a suffix in front of itself.

**Driven end to end against a real engine**, on a local server serving both
shapes:

    report.txt          224 bytes   Content-Disposition: attachment
    made-in-page.txt     26 bytes   blob: built by the page's own script

Both landed in `~/Downloads` with the right contents. Run a second time
without clearing, both numbered rather than overwrote. The blob case is the
one that matters most and the one a url-refetching design could never have
served: it proves the transfer is the engine's.

Android is not wired and says so at the seam rather than accepting the handler
and dropping it. Its path is the platform `DownloadManager` in
`android_downloads`, fed by the WebView's own `setDownloadListener`.

## OAuth popups, and the opener that was never wired

Signing in to claude.ai with Google produced a blank tab and no session. The
popup was not blocked -- the tree recorded four of them opening as sub-tabs --
and the url said what the flow needed:

    display=popup   response_mode=form_post   origin=https://claude.ai
    redirect_uri=gis_transform

That is Google Identity Services' popup flow, which does not redirect back. It
posts the credential to **`window.opener`**, and `window.opener` was null.

`qtwebengine_view` took `requestedUrl()` out of the engine's
`QWebEngineNewWindowRequest` and had the shell load that url into a fresh tab.
That looks identical in a screenshot and is a different thing: Chromium wires
the opener only when the request itself is handed to a page, with
`request.openIn(page)`. Loading the url is a copy of the destination, not the
window that was asked for.

The tree recorded both failure modes side by side, which is what made it
diagnosable at all. Two of the four popup tabs were titled `Sign In - Google
Accounts` and two were still titled `accounts.google.com` -- the url standing
in as the title because no document ever titled itself. **The white page was
on disk.**

**This reverses a decision the code had made deliberately, and that is the
part worth arguing rather than the mechanism.** The comment that stood there
said `openIn` was not called on purpose, because dropping the opener stops a
page reaching back into the window that spawned it. That is the tabnabbing
vector and the protection is real. What the comment did not say is the price,
which is total for one class of site: a popup with no opener cannot answer the
page that opened it, so every OAuth sign-in renders a blank window and nothing
completes. A protection whose cost is "this category of site does not work"
should say so where it is claimed.

The line drawn instead: **only a window the person asked for gets an opener.**
The shell fills the adopting view in on the `user_initiated` branch and leaves
it null otherwise, so a script-initiated popup that policy lets through still
gets the old treatment -- url copied into a fresh page, no opener, nothing to
reach back through. That keeps the protection exactly where a window nobody
asked for comes from, and spends it where somebody clicked "sign in".

The residual risk is stated rather than waved away: a popup you opened by
clicking can reach `window.opener`. Every mainstream browser makes the same
trade, which is a reason to think it defensible and not a reason to skip
saying it.

So the seam's `new_window_requested` gains an out-parameter: a receiver that
wants the window to be real sets `*adopt` to the backend that should take it,
synchronously, because the engine's request object is only valid for the
duration of the emit. Filling it in is what `openIn` needs; leaving it null
still means "I have handled the url myself", which is what a receiver that
merely files a bookmark does.

`open_node` grew a `load_now` flag for the same reason. An adopted request
performs its own navigation, and loading the url here as well would fetch a
one-time OAuth url twice -- spending the state parameter before the real
navigation used it.

One thing that fell out and is worth knowing: **Qt resolves a
pointer-to-member connection on the whole signature, so a defaulted argument
does not make a slot narrower than its signal.** Adding `load_now` broke
`connect(m_tree, &tab_tree_view::open_requested, this,
&main_window::open_node)` at compile time, with a static assertion about the
slot requiring more arguments than the signal provides. A lambda at the call
site is the fix.

Android is untouched: `android_view` never emitted this signal, so it has no
popup path to break. The Android WebView's analogue is `onCreateWindow` with a
`WebViewTransport`, and it is not written.

### A crash that was the build, and a false proof that it was not

`try_navigate` segfaulted, and this section said it was pre-existing and
asynchronous. **Both halves were wrong, and the reasoning that produced them
is the part worth keeping.**

It was a **stale object**. `set_obscured` was added to `web_view_backend.h`
by the drawer fix; `main_window.o` was compiled against the header with it and
`qtwebengine_view.o` against one without. The vtable in the linked binary was
one slot short, so `update_navigation`'s call to `can_go_back()` reached
`can_go_forward()`, and its call to `can_go_forward()` dispatched one word
past the end of the vtable -- a zero -- and the program called address 0. It
died on the first `open_node`, because that is the first virtual dispatch onto
a backend: with no page open `update_navigation` short-circuits before any
virtual call.

**The false proof is the lesson.** It was declared pre-existing on an A/B: the
change stashed, driver rebuilt, same crash; change restored, same crash. That
looked conclusive and was worthless, because neither arm ever reached a
consistent build. In the stashed arm the header lost `set_obscured` while
`main_window.o` still had it -- skewed the other way. `make` had nothing to do
in between, so both arms relinked the same bad object and reported the same
address.

An A/B is only evidence if **each arm is internally consistent**. Rebuilding
is not the same as rebuilding *everything that depends on what changed*, and a
build system that decides by timestamp cannot tell you which you got. Measured
afterwards: 192 of 198 objects in `test/build-make` were older than the newest
header.

From a clean build of the same commit in a private `BUILD_DIR`, the driver
runs **33 passed, 0 failed**. Nothing in the source was ever wrong.

Two smaller things fell out. The dependency tracking is *not* at fault -- the
`.d` file does list `web_view_backend.h`, and `make -n` wanted to recompile;
the tree simply moved under a build that had already finished. And the same
shape bit again five minutes later, when a header was edited while a
verification build was running and the link failed on a signal moc had not
seen. **Do not edit while a build you intend to trust is running.**

## Android: it runs, and it browses

**A second platform behind the seam, in use rather than in principle.** The
seam has claimed since step 3.5 that adding a platform is "one new backend pair
plus a different two lines in `main()`". That claim has been measured twice
over: once when the core compiled for `arm64-v8a` unchanged, and again every
time something desktop-only turned out to need an Android answer rather than an
exemption.

**What exists now**, verified against the source rather than restated from the
paragraphs below: `src/android_view.cpp` is a real backend, 21 member functions
of JNI against `se/vibes/hydra/HydraWebView`, with per-request blocking through
a native `shouldBlock`, navigation interception via `shouldOverrideUrlLoading`,
url-change callbacks, and a script bridge that registers. Three more pairs sit
beside it -- `android_downloads`, `android_intents`, `android_dialogs` -- with a
Java class each for the first two. The subsections below are the record of how
each arrived, in order.

**Run on hardware, 2026-08-26.** A Galaxy Z Fold3 (SM-F926B, Android 15, SDK
35, arm64-v8a) over adb: `hydra-0.1-arm64-v8a-debug.apk` installed, launched,
and loaded `example.com` typed into its own address bar. The page rendered.
logcat shows `com.google.android.webview` loading and chromium starting its
network stack — the Android side of the seam doing the job Qt WebEngine does on
the desktop. No fatals, no ANR, 217 MB PSS with one page open, and the status
bar reading `1 / 4 live`. Everything before this had been driven on an
emulator; this is the first time it has been a phone.

### Two defects the phone found, 2026-08-26

Driving tabs and history on a **Galaxy Note 9 (SM-N960F, Android 10, SDK 29)**
turned up one crash and one dead control. Both are portrait-phone problems, so
neither had shown on the emulator or the Fold3.

**Any menu aborts the app.** Tapping File or View kills it within a second,
reproducibly, on every launch:

    Abort message: 'Failed to acquire deadlock protector for
    QAndroidPlatformOpenGLWindow::eglSurface().
    ... while already locked by 'QtAndroidAccessibility::runInObjectContext()'.

Every frame of the backtrace is Qt's -- `QRhi` to `QOpenGL` to the Android
platform plugin to `qFatal`. It is Qt's own deadlock protector firing, not
hydra's code, and the condition is an **accessibility service being active**:
this phone runs `com.jamworks.bxactions` with `accessibility_enabled=1`.
Opening a `QMenu` creates a native window, the render thread asks for the EGL
surface, and Qt's accessibility bridge is already holding the lock.

Not fixed here, and not obviously fixable from this side: the Qt Android plugin
exposes no switch for its accessibility bridge that a search of the shipped
`.so` and jar turns up. Worth reporting upstream with the abort message, which
is specific enough to search for.

**Re-measured against Qt 6.12's plugin**, since the original search was of an
older one and a switch could have arrived meanwhile. It has not. The complete
list of environment variables that plugin reads is sixteen long and none of
them concerns accessibility: fonts, icon size, mouse handling, an assets cache,
`QT_ANDROID_NO_EXIT_CALL`, `QT_BLOCK_EVENT_LOOPS_WHEN_SUSPENDED`,
`QT_QPA_NO_TEXT_HANDLES`, `QT_USE_ANDROID_NATIVE_DIALOGS`,
`QT_USE_ANDROID_NATIVE_STYLE`, `QT_ANDROID_RASTER_IMAGE_DEPTH` and
`QT_ANDROID_SURFACE_CONTAINER_TYPE`.

One of those is a lead rather than an answer, and it is worth naming because
the next person will otherwise start where this did. The abort is not raised
by the accessibility code -- that path only *warns*, in as many words
("Could not run accessibility call in object context, accessing main thread
could lead to deadlock"). It is raised in `QAndroidPlatformOpenGLWindow`, when
a second native window asks for an EGL surface and cannot take the lock the
bridge is holding. So anything that changes how a secondary window gets its
surface is the shape of a workaround, and
`QT_ANDROID_SURFACE_CONTAINER_TYPE` is the only knob on offer that touches it.
Untried: it needs a phone that is unlocked, and setting an environment
variable for an Android app is itself awkward enough to be part of the
experiment.

**It is wider than a menu, and narrower than was claimed for a few minutes on
2026-08-27.** Reaching for the settings dialog aborted the app on the File menu
as described. Tapping the **Shield button on the toolbar** then aborted it
again, with the identical message -- and the Shield opens no menu, so `QMenu`
is not the condition.

That was written up as "every secondary window", which was wrong, and the way
it was wrong is worth keeping. Both samples shared a property nobody had
checked: `site_policy_dialog` sets `setWindowFlags(Qt::Popup)` -- it is the one
dialog in this tree that does. So a menu and a popup-flagged dialog had been
observed, one conclusion about *windows in general* had been drawn from them,
and the generalisation covered a class the evidence never touched. Two samples
agreeing are one sample when they agree because of a shared property.

**Settled on an unlocked phone: it is every secondary window.** A plain
`annoyed_dialog` -- `class annoyed_dialog : public QDialog`, no popup flag in
the file, opened as `dlg.exec()` from the toolbar -- aborts with the identical
message and kills the process. Three kinds are now measured rather than
inferred: a `QMenu`, a `Qt::Popup` dialog, and an ordinary modal dialog.

This section briefly carried that conclusion, withdrew it, and has arrived
back at it, and all three steps were right. The withdrawal was right because
the two samples then in hand were both popups and agreed for a reason nobody
had checked -- two witnesses that are one witness. The conclusion is right now
because a third sample was taken that shares no flag with them. Reaching the
same answer by generalisation and by measurement are not the same act, and
only the second licenses acting on it.

At full width: **on a phone with any accessibility service running, this
application can browse and can do nothing else.** Settings, the shield,
downloads, the media list, every password prompt and every confirmation is a
window of its own, and each aborts the process on the way up. Anything
reachable only through one is not reachable at all there -- which includes the
whole of clear-browsing-data, and is why its Android half stays unverified end
to end.

**Confirmed by removal, 2026-08-27, which is the control the whole diagnosis
was missing.** The holder uninstalled `com.jamworks.bxactions`;
`enabled_accessibility_services` is now empty and `accessibility_enabled` is
`0`. On the same phone, the same build, the same taps:

    File menu        opens -- "New Tab  Ctrl+T" on screen, process alive
    annoyed_dialog   opens -- the full report dialog rendered, process alive
    aborts in logcat 0

Both were reliably fatal an hour earlier. So the condition is established
rather than inferred: it is **not** that Qt Widgets cannot open a secondary
window on Android, it is that it cannot while an accessibility service is
running. Every earlier statement here that reads as the former should be read
as the latter.

**And that makes the impact sharper, not milder.** The service this was found
under is a gesture app, which sounds like a minority taste. `TalkBack` is an
accessibility service too. On the reading now confirmed, this application
aborts on the first menu for exactly the users who most depend on the platform
telling them what is on screen -- so the population hit is not "people with an
unusual launcher" but "people using a screen reader", and a browser that dies
on its File menu for them is not a browser they can use at all.

Not yet established: whether another accessibility service reproduces it.
`TalkBack` would be the one to try, since it is stock and it is the case that
matters; that is a change to somebody's phone and is theirs to make, not this
session's.

**Qt knows about the abort, has not fixed it, and recommends a workaround
that our own evidence says is not sufficient here.** `QTBUG-141579` reports
the identical message from the identical function -- and with a *different*
subsystem holding the lock: `QAndroidInputContext::runOnQtThread()` rather
than `QtAndroidAccessibility::runInObjectContext()`. So the deadlock
protector aborting is a general fault of the Android plugin with at least two
triggers, and ours is not the reported one.

Secondhand and marked as such: the tracker itself is a single-page app that
cannot be read without a browser, so this comes from the Qt forum thread
about that bug rather than from the issue. What it reports is that the
regression spans **6.8.3 through at least 6.12.0** -- which brackets the 6.12
this project builds against, and means upgrading is not today's answer -- that
the fix is architectural (making the Java/C++ calls asynchronous rather than
synchronously waiting) and is being landed incrementally, and that the
workaround Qt developers recommend is **to stop calling `exec()` on Android**
and use `open()` with signals instead.

That workaround is worth knowing for two opposite reasons. It is the same
direction as the redesign sketched above, which makes the redesign the
upstream-sanctioned shape rather than something invented here -- and 21
`exec()` call sites is exactly the cost already counted. But **it does not
cover the case measured here.** `site_policy_dialog` is opened with `show()`,
never `exec()`, and it aborted. Whatever the IME variant is, ours fires on
creating the window, not on running a nested loop inside it, so replacing
`exec()` with `open()` would fix the dialogs that use `exec()` and leave the
shield exactly where it is. Anyone starting that work should test the
`show()` path first, because it is the one that decides whether the whole
approach is sufficient or merely partial.

**The one knob Qt offers could not be tested, and both routes to it are
recorded so the next attempt starts further on.**
`QT_ANDROID_SURFACE_CONTAINER_TYPE` is the only setting that touches how a
secondary window gets its surface. `qputenv` at the top of `main()` changes
nothing and cannot: Qt for Android starts its Java side first and runs `main()`
on a later thread, so the container is chosen before that line runs -- borne
out by the run still logging `SurfaceView` nineteen times and `TextureView`
never, which is what stops that being read as a negative result. Injecting it
into the process environment instead, through Android's `wrap.<package>`
property, is refused by the device, since the property must be declared in the
property contexts and a shell cannot create it. And
`android.app.environment_variables`, which older Qt read from the manifest, is
absent from this Qt's jar. The knob is untried rather than ruled out.

**Corroborated from another application, independently.** The beerssh session
was testing its own first Android build on this same phone and saw an abort in
the same Qt subsystem on its first launch after install: SIGABRT, "JNI
CallVoidMethodV called with pending exception", through
`QtNativeAccessibility.populateNode` under
`QtAccessibilityDelegate.notifyValueChanged`. It did not reproduce across five
cold starts, and it was found before this one and without knowing of it.

Two different signatures, two different Qt applications, one phone, one
accessibility service, one subsystem -- and this side has the deterministic
trigger the other lacked. That is worth more than either finding alone, and it
is the reason to report it upstream rather than work around it: **the same
lock-ordering fault showing two faces is a Qt bug, not two application bugs.**

**The obvious mitigation does not work, measured rather than assumed.**
`QAccessible::setActive(false)` is public API and was the one lever on our
side: turn Qt's accessibility bridge off, lose the abort. It would have cost
screen-reader support, which is a poor trade and one for the copyright holder
rather than a session to make -- so it was measured first.

It makes no difference. Built with `setActive(false)` before any widget,
confirmed by the app's own log (`isActive=0`), then the deterministic
procedure three times: **crashed on all three, with the identical abort.**
Qt's Java `QtAccessibilityDelegate` is installed by the Activity whatever the
C++ flag says, so `QtAndroidAccessibility::runInObjectContext()` still runs and
still takes the lock.

That is worth knowing precisely because it is the first thing anybody will
reach for. **There is no trade to make**: the option that looked like it cost
accessibility support does not buy anything, so the question of whether to pay
never arises. What remains is an upstream report, and the pair of findings from
two applications is what makes that report worth filing.

**The tab drawer does not open** -- fixed, and the cause was not the drawer.

Instrumented, the drawer turned out to be working perfectly. On the tap the log
said `x=0, 337x703, isVisible=1`, parent the main window, animation run and
finished: correctly positioned, correctly sized, and on screen by every measure
Qt has. It was **underneath the page**.

`android_view` puts the real `android.webkit.WebView` into the *Activity's*
view hierarchy -- the Java class says so in its own header comment -- and keeps
a `QLabel` on the Qt side to hold the geometry. An Android native surface
composites above everything Qt paints, and `raise()` only reorders Qt siblings,
so no amount of raising can put a Qt widget over the page.

**The discriminating experiment**, same build and same taps, one variable: with
no page open the tree appears; with one loaded it does not. That is what turned
a fortnight of plausible guesses into a diagnosis, and it is the shape worth
copying -- every Qt-side check said the widget was fine, because every Qt-side
check was asking the wrong process.

The fix was half-written already. `android_view` had `m_blocked`, which hides
the native view while a modal `QDialog` is up, under the comment "the widget is
perfectly visible, it is just underneath something". That is the same sentence
about the drawer, arriving from the shell instead of from a dialog. So the seam
gained `set_obscured(bool)` -- a no-op wherever Qt draws the page, which is why
it defaults to nothing rather than being pure virtual -- and `set_drawer_open`
calls it. The page is restored on the close animation's `finished`, or it
reappears over a drawer still in motion and the animation reads as the tree
vanishing rather than closing.

Verified on the phone: tree over a loaded page, page back when it closes.

**The general rule this leaves**, because it will come back the next time
anything overlays a page on Android: *a Qt widget cannot be raised above an
Android native surface, and every Qt-side check will tell you the widget is
fine.* Geometry, visibility, parent and animation were all correct and all
irrelevant.

**What it looked like before the diagnosis.** In portrait the tree becomes an overlay
behind the button left of the address bar, and the empty page says so. Tapping
that button does nothing visible -- the status tip appears, the button takes
its focus frame, and no tree arrives. Repeated taps only toggle a state nobody
can see. A sliver a few pixels wide sits at the left edge, which is the shape
you would expect from a sidebar parked at x=0 with almost no width.

The code reads as though it should work: `resizeEvent` calls
`update_layout_mode()` first and then resizes the sidebar to
`min(width * 0.82, 420)` before moving it, and the narrow-mode placeholder text
proves `update_layout_mode` ran with `narrow` true. So the fault is somewhere
between that and what the compositor draws, and it is **not** the button, the
tap coordinates or the animation, all of which were eliminated by measurement.

Rotating to landscape puts the tree back in the splitter and everything works,
which is how the rest of this was tested at all.

**What did work, once the tree was reachable.** Tabs open from the tree and the
row goes bold with the live count moving to `1 / 4`; the sample tree seeded on
first run is intact; a tab created in an earlier session had persisted through
a crash and a relaunch. Back and Forward both work -- wikipedia.org to
example.com and back again, confirmed by the address bar each time. Note that
`can_go_back()` was not overridden by `android_view` and so inherited
`web_view_backend`'s `return true`, which meant those two buttons were always
enabled rather than reflecting the page; they were nonetheless correct here.

**Fixed by pushing the state rather than asking for it.** A WebView may only
be touched on the thread that made it, so a synchronous `canGoBack()` from the
Qt thread would have to block on Android's UI thread -- which is the shape of
deadlock this tree has already met once, from Qt's accessibility bridge, and
not one to walk into deliberately. Java reports instead, from
`doUpdateVisitedHistory` (Android's own "the back/forward list changed"
callback) and from `onPageStarted`, through a new `onNavState` native method
that marshals onto the Qt thread exactly as `onUrlChanged` does. The backend
caches the two booleans and emits `history_changed`, which the seam already
had and which the shell already wires to `update_navigation`.

**Driven on the device**, with the pushed values logged out of
`on_nav_state_from_java`:

    after the first page    back=0 forward=0
    after the second page   back=1 forward=0
    after pressing Back     back=0 forward=1

which is the whole state machine. The desktop is unaffected by construction:
`hydra.pro` excludes `android_*.cpp` off Android, and `make` plus `test_seam`
(74 checks) pass.

**And a separate thing that measurement turned up: you cannot see the
difference.** The arrows come from `QStyle::SP_ArrowBack` on Android, there
being no icon theme, and the style draws a near-black triangle. Disabled, the
proxy style desaturates it and lifts it toward the background -- but on a dark
theme the background is dark too, so a near-black glyph moves almost nowhere.
Measured on the toolbar with Back unavailable and Forward available in the same
screenshot: mean 144.1 and 142.8, a difference of one part in a hundred and
twenty. The enabled state is now correct and invisible.

That is worth separating from the fix rather than folding into it. The state
being right is what the shell reasons about; the state being *legible* is a
contrast problem in the style's own artwork on a dark palette, and it is the
same shape as the pale key -- an icon the theme gives us that does not carry to
this background. Not fixed here.

Two details that run settles. The toolbar's **Key button falls back to its
word** on Android, exactly as designed: there is no desktop icon theme,
`QIcon::fromTheme` returns null for `dialog-password` and `password`, and the
fallback is `SP_CustomBase`, which means "no icon, keep the text". The arrows
and reload *do* draw, because those name a `QStyle::SP_` fallback the platform
style provides — and reload turns blue the moment a page loads, so the
enabled/disabled rendering carries across.

### How it started, and why the opening used to say otherwise

The first version was **a placeholder behind the seam**, and this section
opened by describing it in the present tense for a long time after it had
stopped being true. What that paragraph said:

> `android_view` is honest about what it is: it renders a message saying the
> web view is not written yet and shows the address it was asked to open. A
> stub that displayed a blank page would be indistinguishable from a real
> backend that is broken. `set_script_bridge` does nothing on purpose — on
> Android that becomes `addJavascriptInterface`, and pretending to register a
> bridge that cannot deliver would make every script that waits for one hang
> rather than fail.

Both halves were replaced by the work recorded below -- the WebView renders,
and `set_script_bridge` calls `m_bridges.add` -- and nobody came back to the
top. It is kept here, quoted and in the past, because the reasoning was right
and is the reason the stub was safe to ship: **a stub that says so is worth
more than one that looks like a broken backend.**

It is also the specific failure this document is most prone to. A section
opens with a summary written on day one, gains twenty subsections of
correction underneath, and the summary is the part nobody re-reads. Anyone
reading only the first screen of this section would have concluded Android was
a stub, on the day it browsed the web on a phone.

**The core is genuinely platform-neutral.** Fifty-one translation units
compiled for `arm64-v8a` with **no errors** and no changes. The only link
failure was the three `qtwebengine_factory` symbols `main()` names — exactly
the file the design says should be the only one that knows.

#### This heading used to cover more than Android

Measured while correcting the summary above: of the 1649 lines then under
`## Android`, **944 in twenty subsections never mentioned android, adb, apk or
a phone.** The KeePassXC work, the settings pages, the INI migration, kiosk
mode, the crypto shim and the reorganizer gate were all filed here because
that is when they were written, not because they were Android's.

**Fixed by naming the runs, not by moving them.** This document is a
chronological record and its `##` headings are milestones in it, so reordering
the entries to group them by topic would have destroyed the one property that
makes it readable -- that it says what happened, in the order it happened. The
non-Android runs are contiguous, so five accurate headings were inserted at
the points where the subject changes and **not one line of content moved**:
filtering, KeePassXC, Android's media and downloads, the suites and gates, and
settings.

The proof is the shape of the diff: **zero deletions, 28 added lines, and all
8959 original lines consumed in order.** A restructure that moves nothing can
be checked exactly, which is why it was done that way rather than by cutting
and pasting nine hundred lines and reading a sample of the result.

Both Android headings now measure 0 lines that are not about Android, by the
same test that found the 944.

**The core is genuinely platform-neutral.** Fifty-one translation units
compiled for `arm64-v8a` with **no errors** and no changes. The only link
failure was the three `qtwebengine_factory` symbols `main()` names — exactly
the file the design says should be the only one that knows.

#### Open: this heading covers more than Android

Measured while correcting the above: of the 1649 lines under `## Android`,
**944 in twenty subsections never mention android, adb, apk or a phone.** The
KeePassXC work, the settings pages, the INI migration, kiosk mode, the crypto
shim and the reorganizer gate all sit here because that is when they were
written, not because they are Android's.

Left alone deliberately. They are interleaved with the Android ones rather
than trailing after them, so there is no line to cut at, and moving 944 lines
of a chronological record is a decision about how this document is organised
rather than a correction to something false. **Recorded so it is a question
somebody answers rather than a shape nobody notices.** The heading is accurate
for what it claims; it is merely not exhaustive of what it contains.

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
  everything this program keeps lives *beside* the tree file — `policy.ini`,
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

## The filter list, and the cosmetic half

Filtering, on both platforms. Recorded here because this is when the work
happened; it is not Android's, though Android is where some of it was
driven.

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

## KeePassXC, from bridge to a closed §13

The password work, continuing §"Password manager (step 7, done)" above.
None of it is platform-specific: the bridge speaks to a vault over a local
socket and does not care what is drawing the window.

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

#### Pairing was confirmed once, and it aborted the driver

**`associate()` works.** With the dialog accepted, KeePassXC answered *"Paired
with KeePassXC."* — which requires `parse_associate` to have decrypted the reply
and found a real id in it, so the encrypted round trip is proven in both
directions, not just the handshake.

**Then the process died with `corrupted double-linked list`**, and the fault was
the driver's. The core dump put the abort inside
`keepass_protocol::get_logins_request`, which is nothing but `QJsonObject`
inserts — so the heap was already broken and `realloc` merely noticed. The cause
was three checks earlier: the bogus-pairing block connects a lambda capturing its
own locals **by reference** to `associated_changed`, and never disconnects, while
the bridge outlives the block. A successful pairing emits that signal a second
time, so a dead lambda ran `message = m` against reclaimed stack and freed
whatever the old string's d-pointer had become.

It could only ever fire on the one path nobody had reached — the signal fires
twice exactly once, when pairing first succeeds — and it took the run down before
`get-logins` was ever sent. **A scoped connection has to be scoped at both ends**,
which is the same lesson as the null observer registered one line early: the
damage lands somewhere unrelated to the mistake.

**And the fix exposed the check underneath it.** With the crash gone, a run whose
dialog went unconfirmed reported *nine* passes including "it hands back an id to
save" and "and a key with it" — against `hydra-not-a-real-pairing`, the string the
test had planted three checks earlier via `set_association`. They were asserting
on the test's own writes. Cleared before pairing and gated on it now, the same
failing run reports seven and says which checks it declined to make.

**`get-logins` was reached later the same session**, once the pairing was made to
persist — see "A pairing that survives a restart" below. What follows is why it
took six attempts to get one confirmation, and it is worth keeping because none
of it was ours.

**And what stopped those four is not ours, on the evidence.** Two distinct
behaviours, both KeePassXC's:

- **It exited mid-run, once.** The vault's `Last saved` is the minute that run
  was waiting, so the prompt appeared and was accepted and the association was
  written — and then the process was gone, the reply never came, and it left
  behind exactly the stale socket the precondition was written to catch. The
  precondition cannot help here: it connected to a server that was alive at the
  time and died afterwards.
- **A restarted instance stopped prompting at all.** With an `associate` request
  pending for a full three minutes against an unlocked vault, KeePassXC showed
  nothing: no dialog, no inline banner. Not inferred from a timeout — a
  full-screen capture, a capture of the KeePassXC window itself, and a window
  poll every five seconds all agree, while the same instance answered the
  handshake and refused the bogus pairing with code 8 in the same run.

**The window check was wrong first, in the usual way.** `xwininfo -root
-children` lists only *direct* children of the root, and a reparenting window
manager puts every real window a level down — so it reported KeePassXC as a
single 1×1 window and would have supported "there is no window there" no matter
what was on screen. `-root -tree` finds the real 800×600 one. The conclusion
happened to survive the correction because the screenshots were taken too, which
is the argument for taking them: a window query answers what you asked, and a
picture answers what is there.

### More than one login is a question, and the passwords stay out of the page

The §13.2 gap that mattered most, and it turned out to be a leak rather than a
missing convenience. `get-logins` returns every entry KeePassXC matched; the
controller serialised **all of them** into the page's isolated world, and the
injected script's answer to more than one was `if (entries.length > 1) return;`.

So a vault holding a work and a personal login for one site put **two passwords
across the boundary and filled neither**. No fill, and credentials delivered for
a fill that never happened — the exact thing §13.3's "held only for the fill that
asked" is about. It had been that way since step 7 and read as a deliberate
choice, because the note beside it said multiple matches are "left alone rather
than guessed at". They were left alone *after* being sent.

**The decision moved into C++**, which is the only side that is trusted. One
match fills as before; several raise a picker; none says so instead of leaving an
empty form that looks identical to a broken bridge. What the picker shows is the
login and the entry name — what tells two accounts apart — and **never the
password**: a picker is a window, and windows get photographed, screen-shared and
left open. The passwords stay in the controller until an index comes back.

**The suite found a seam between two of the rules**, which is the part worth
recording. Credentials must not survive a navigation, so `set_page_origin` clears
what is waiting — and that also erased the fact that a question was open, so a
user who clicked *OK* after the page moved got silence. Both rules are right; the
fix is that the entries go and one bool stays, so the answer can be explained
rather than ignored. Written as a check first, which is why it was found at all.

**17 checks**, including that no label contains a password, that the delivered
JSON contains the chosen entry and *not* the other one, that answering twice
fills once, and that a choice arriving after a navigation fills nothing and says
why. The script keeps its `length > 1` guard as belt and braces: it should never
fire now, and if it does, filling the first of several is a guess about which
account someone wanted.

### The key (§13.2), which is permanent now


It is always on the toolbar, and **before the gate runs** rather than after --
which is the whole design. A key that showed up only when a fill succeeded
would be absent from exactly the pages where somebody needs to know why nothing
happened. Clicking re-runs the whole gate rather than re-opening a cached
answer, because caching the answer means holding credentials past the fill that
asked for them.

**It used to appear only when a page raised a form, and that was wrong for a
reason the earlier design missed.** A control that appears unasked reads as a
notification; one that is always in the same place can be *looked for*. Hidden
until needed, the feature was invisible to anyone who had not already met it --
and its absence said nothing, because absence is also what a browser with no
password support looks like.

So the four states are now present-and-greyed, `Key` (this page has a form),
`Key ✓` (filled, click to fill again) and `Key ✕` with the refusal in its
tooltip. Greyed carries a reason too: either *no login form on this page* or,
where KeePassXC cannot be reached at all, that -- the platform's answer
outranks the page's, since "no login form here" would send somebody looking at
the wrong thing.

**Disabled rather than live-and-explaining**, and the reason is this project's
usual one. With no fields on the page a fill has nothing to write, so a live
button would query the vault, offer a picker and then appear to do nothing --
silence, which is the failure mode §13.2's affordance exists to prevent.
`blocked_reason` has no case for "no form" and should not grow one: the page
either has a password field or it does not, and the shell knows that without
asking anybody.

It also removes a question the implementation would otherwise have had to
answer. The page script requests a fill automatically when
`passwordFields().length` is non-zero, and `requested` is emitted inside
`request_credentials` -- so a *manual* click would raise the same signal as a
detected form, and a flag meaning "this page has a login" would set itself. The
disabled state makes the click impossible until a form has been found, so there
is nothing to tell apart. **The state that could be got wrong stopped existing
rather than being tracked correctly.**

One function, `reset_key_action`, holds the resting state for both creation and
every navigation. While the button was hidden the two did not have to agree --
absent is absent -- and making it permanent turned that into two places
describing one state in words that could drift.

**An icon, not a word.** This deviated from §13.2 for a while and the note here
outlived the deviation: the toolbar was text actions, and inventing one glyph
for one affordance would have made it the odd one out. Answered by not
inventing one -- `dialog-password` and `password` are standard freedesktop
names, so the key is the desktop's key, drawn to match the arrows beside it. If
a theme has neither the icon comes back null and the action keeps its word.
Measured on crystalsvg: `dialog-password` is absent, `password` resolves at six
sizes.

**Every toolbar icon goes through the weighting, not just the key**, so there
is one rule rather than one adjusted glyph. What that turned up is worth more
than the change: **the back and forward arrows are blue discs**, measuring 114
on the colour scale where a grey glyph measures 0. They look grey because they
are usually *disabled* -- there is nowhere to go back to -- and Qt draws a
disabled icon by desaturating and lightening it. The pale grey is the state,
not the artwork, so darkening the source would not have touched it; reload is
the same disc and turns vivid blue the moment a page loads.

So the colour guard excludes them, correctly, and what it adjusts is the
monochrome set: the key, the drawer's list glyph and the media badge. Asked to
darken "the other toolbar icons", the honest answer is that most of them were
never pale -- they were switched off.

**And how a disabled icon is drawn, which is the style's job.** `QIcon` asks
the current style to generate the disabled pixmap, so there is no per-icon
place to change it -- a `QProxyStyle` is how the question is reached at all.

Qt's default desaturates and then lifts the result toward the background.
Measured per icon rather than off a screenshot, which cannot compare an icon
with itself: `go-previous` has a normal mean of 137.9 and Qt hands back 180.2,
giving away 40% of the remaining contrast on top of every trace of colour. On
this toolbar that is most of what the arrows ever look like, since there is
usually nowhere to go back to.

**The desaturation is kept and the lift is halved**, to 0.20. Colour is the
honest signal that a control is unavailable and costs nothing to read; the
lift is what removes the shape. `go-previous` now comes back at 157.7 --
plainly lighter than its own enabled 137.9, so the signal is intact, and no
longer a smudge.

A screenshot suggested for a moment that the new disabled arrow was *darker*
than the enabled reload, which would have inverted the whole thing. It was
comparing two different icons: `view-refresh` is a lighter drawing than
`go-previous`. **The only comparison that means anything is an icon against
itself**, which is why the numbers above come from rendering both modes of one
icon rather than from cropping a window.

`shell_fixture` and `try_autofill` install the style too, for the reason the
fixture already applies the palette and the icon theme: a driver that skips it
photographs arrows faded further than the application fades them.

**Weighted for the background it is drawn on.** crystalsvg's key is a pale
outline: measured off the rendered window, its darkest pixel was 142 against a
toolbar around 240, and in the greyed state 181. Legible once you know it is
there, easy to miss otherwise, and visibly fainter than the arrows beside it.

The shape stays the desktop's -- inventing a glyph is the thing this section
already argues against -- and only the weight is adjusted, by multiplying the
theme's own pixels rather than replacing them, so the shading that makes it
read as a key survives.

Two things keep that from being a blunt filter. It is **measured, not
hardcoded**: the mean is taken over what is actually drawn, weighted by alpha,
and scaled only if it is lighter than the target, so a theme whose key is
already dark is left exactly as it is. And it is **applied only on a light
background**, because pale-on-dark is correct and a scheme-blind darkening
would take an icon that reads well on dark chrome and erase it.

Measured after: darkest pixel 98 enabled, 154 greyed, and the icon's own mean
109 against a target of 110. `try_autofill` asserts the mean rather than
eyeballing a screenshot, so it holds without a compositor -- guarded against
the vacuous pass, since an empty pixmap would report a luminance of zero and
sail through.

**And that assertion immediately caught the driver rather than the code**,
which is this tree's recurring defect showing up for the third time.
`try_autofill` builds its own `main_window` and never applied the icon theme,
so every `QIcon::fromTheme` in it came back null and the toolbar it had been
making claims about was not the toolbar the application draws. It had been
asserting the key's *text* for as long as it existed and never noticed. The
same fault is recorded above for the screenshots that turned out to be of the
harness, and `shell_fixture` exists precisely to prevent it -- this driver
predates it and does not use it. One line, matching `main`, and the icon
appeared.

**The tooltip is where the reason lives**, not the status bar. A status message
is gone in six seconds and the empty form is still sitting there; this project
has already recorded a defect where a message was written into a label
something else overwrote.

`try_autofill` drives it through the real shell, **14 checks, no KeePassXC
needed** -- autofill is HTTPS-only by default, so a login form served over plain
http is refused for a reason the shell knows on its own, which makes the whole
chain observable without a vault or a pairing dialog. Two defects in the driver
before it worked, both this project's own recurring shapes: it took the first
`QLineEdit` it found, which is the sidebar's *search* box, so every navigation
filtered the tree instead of loading anything; and it never activated a tree
node, so there was no view to navigate at all. Both failed by blaming the
feature.

**Everything from §13.2 is built**, and this line said otherwise until
2026-08-27. `generate-password` runs from `main_window` through
`autofill_controller::request_generated_password` to the bridge. `set-login`
on new-credential submit is the longer chain and it is complete end to end:
the injected script calls `offer_to_save` when the page is submitted,
`confirm_save` answers the user's decision, and `keepass_bridge::save_login`
sends the `set-login` message.

Worth saying how that was got wrong, because the first look agreed with the
line. `set_login_request` and `parse_set_login` exist in the protocol layer
and nothing in `main_window` or `autofill_controller` calls them, which reads
exactly like a message implemented and never wired. The call is
`save_login`, one layer down in the bridge, and the trigger is in
`autofill_script.h` rather than in C++ at all. Two greps that both looked
conclusive, and the chain was found only by walking it.

### A pairing that survives a restart (§13.1, §14)

Built because the alternative was worse than inconvenient: with the association
in memory only, **the one step that needs a human was needed on every launch** —
and `get-logins`, the last unexercised part of §13, could only ever be reached in
the same run that showed the dialog. Four attempts and one heap corruption later,
that had happened exactly never.

`credential_store` puts it in the **session's Secret Service** (libsecret,
optional at build time on the same terms as libsodium and libtorrent). The
refusal that shapes it: §14 also permits "the app's encrypted config", and an
encrypted config the app opens unattended must keep its key on disk beside it —
obfuscation wearing the word encryption. There is no third option that is honest,
so where there is no Secret Service the pairing simply does not persist and the
app says which of the two reasons applies. Missing libsecret is a `STATUS`, not a
`WARNING`, unlike the other two optional dependencies: it costs convenience, not a
feature, and a warning would say something is wrong about a build that is merely
smaller.

**Only the key needs protecting, and both halves travel together anyway.** The id
is the name the user typed into KeePassXC's dialog; it is not secret, but half a
pairing is not a pairing, so they are stored as one blob — `base64(id)` and the
key separated by a space, which is unambiguous precisely because neither half can
contain one. That encoding is a pure function, tested against names with spaces,
apostrophes, newlines, non-ASCII and 300 characters, because a blob that decodes
to the wrong id produces a pairing KeePassXC refuses and an error message about
*association* rather than about storage.

**It is stored at the one moment it is known good** — inside the `associate`
reply handler, not by the caller — so no path can pair and forget to save.
Failing to store is reported as its own outcome rather than as a failure to pair,
because this run works either way and the difference only shows up next launch.

**A refused `test-associate` deliberately does not delete it.** A locked vault, a
different database, and a pairing KeePassXC has genuinely forgotten are the same
answer from out here, and dropping the pairing on that would make locking your
vault cost you the one step that needs a human. Forgetting is an explicit act:
**Tools ▸ Forget KeePassXC Pairing**, confirmed, and enabled only when there is
one to forget. A stored secret with an on switch and no off switch is the wrong
half of the feature to build.

**A comment that had been describing a thing that could not happen.** The shell
already said "a stored pairing is tested rather than re-created, so the user is
not asked to confirm on every launch" — and `associated()` only ever answered for
this process's memory, which on a fresh launch is empty, so the test branch was
unreachable and every launch re-paired. The comment was right about the intent and
wrong about the code for as long as there was nowhere to store anything.

**Two traps, both paid for.** `libsecret` pulls in gio, `GDBusInterfaceInfo` has a
member called `signals`, and Qt `#define`s `signals` to `public` — so the include
has to come before any Qt header or the build dies inside a system header with an
error that blames glib for something Qt did. And the calls are synchronous
because libsecret's async API wants a GLib main loop a Qt app is not guaranteed to
be running; they can block on a locked keyring, so every caller is a user-driven
moment and none of them is startup.

**30 checks** (`test_credstore`), of which 10 are a real save/load/replace/clear
against the running gnome-keyring — including that a second save *replaces* rather
than accumulating, since two items under one set of attributes means lookups
return whichever the service felt like.

**The suite refuses to touch the service unless told where to write.** Every call
addresses one item by fixed attributes, so a suite exercising save-and-clear under
the real name would delete the user's actual pairing — silently, and only on the
machines where the feature works. `HYDRA_SECRET_KIND` renames the item; the suite
declines and says why when it is unset, and `try_keepass` sets its own before
anything can reach the store. Refusing rather than skipping quietly, because a run
that deleted a real pairing would look exactly like one that passed.

**And the harness lied once more, in a new way worth recording.** Three checks
read `check(load(&id), QString("…(%1)").arg(id))` — and **C++ does not order the
evaluation of a call's arguments**, so the message was formatted before `load()`
wrote to `id`. The output said `comes back byte for byte ()` beside a passing
check, and the next line printed the *previous* pairing's name. The assertions
were correct and their evidence was not, which is the same failure as every
entry in the apparatus list above: load first, then report what was seen.

### And it closed §13: `get-logins` ran, unattended

**One dialog, once.** A confirmation was accepted, the pairing went to the
keyring, and every run since has restored it, had it accepted — *"Existing
pairing accepted."* — and gone straight to the vault with no human anywhere:
`alice` and a 17-character password out of a real KeePassXC, over a real socket,
through the real crypto. **15 passed, 0 failed**, and it repeats. The last part
of §13 that had never run has now run, and can be re-run on demand, which is the
part that matters more than the first success.

**And the first unattended run immediately found a defect nothing else could
have.** A url the vault has no entry for came back as *"No logins found (code
15)"* — KeePassXC reports "nothing stored" as an **error** — and `handle()`
routed every error to `error()`. So `request_logins` for a site not in the vault
emitted no `logins` signal at all, and `autofill_controller`, whose `m_pending`
only clears on that signal or on a navigation, left the fill pending until the
page changed. **That is every site not in the vault, which is nearly all of
them**, and it is the ordinary case rather than an edge one.

Error 15 is now delivered as an empty result. The code travels as a *string*, so
`error_code()` reads it as one: a numeric read of a JSON string is 0, which is
also the answer for "no error", and getting that wrong would silently turn every
failure into a success. Six checks pin that offline; the routing itself sits
behind a socket and a handshake, so the driver is what proves it.

**The durable caution, since it cost five attempts and none of it was ours:
restart KeePassXC before an interactive pairing.** A freshly started instance
raises the association window; one that has already served an association stops
raising it, while still answering the handshake in the same run -- so the
transport looks healthy and the dialog that should appear simply does not. And
the dialog **requires a name**: dismissing it empty creates no association and
sends no reply, which is indistinguishable from never having clicked.

This was in the next-list for a while, attached to an item that was finished.
It belongs here, beside the pairing it is about, because that is where anyone
hitting it will be looking.

**The lesson is about what persistence bought, not about the bug.** The defect
was reachable only from a request that needed a pairing, and a pairing needed a
person — so for as long as the association lived in memory, this could only have
been found by a human sitting through a dialog and then thinking to ask about a
site that is *not* in their vault. Making the pairing durable turned a
once-ever, human-gated path into one that runs on every build, and the first
time it did, it failed.

## Forgetting: a browser that could not, a kiosk that would not

Making the profile persistent gave this browser a memory and no way to lose
it. Cookies, the visited-link database and the http cache went to disk and
stayed there, and the only `forget_*` calls in the tree are about other things
entirely -- tabs, imported site rules, a KeePass pairing. The policy on the
privacy page governs what a site may *store* from now on and has never had
anything to say about what is already stored.

Clearing is a profile-wide operation, so it lands on the factory rather than a
view -- one page's cookie jar is every page's -- and it crosses the seam as
plain structs. Two shapes in that API are worth naming, because both exist to
stop a familiar lie. `clear_state` has **`unconfirmed`** and **`refused`**
alongside `done`: Qt confirms the cache clear with its own completion signal
and confirms nothing whatsoever about visited links, and reporting the second
as success would be exactly the blind claim the call exists to avoid. And
`cookies_removed` starts at **-1**, not 0, so "there were none" and "nobody
counted" stay distinguishable. The whole thing is a callback rather than a
return value because none of these stores empties synchronously.

**The checkbox says "Cookies" and not "cookies and site data", and that is a
finding rather than a wording choice.** Qt 6.8 exposes `clearHttpCache`,
`clearAllVisitedLinks`, `clearVisitedLinks` and `visitedLinksContainsUrl` on
the profile, and `deleteAllCookies`/`deleteSessionCookies` on the cookie store.
**There is no call that clears localStorage, IndexedDB or service-worker
storage** -- Chromium's `BrowsingDataRemover` is not wrapped. Every report
therefore carries a note naming what was *not* cleared and where it lives. Two
ways round it were considered and rejected in the code: deleting those
directories before the profile is constructed duplicates Qt's path derivation,
which this document already records as subtle enough to have two roots; and
deleting them from a running engine is worse, because on Linux an unlink
succeeds against an open LevelDB and the engine carries on writing to a file
nothing can reach -- a corrupted profile in exchange for a checkbox.

Clearing is the one control on the settings dialog that acts immediately
rather than on OK. There is no undo for a cookie, and "pending until OK" would
leave somebody unable to tell whether anything had happened. The completion
lambda is held through a `QPointer` to the dialog, because the dialog is
stack-allocated and `exec()`d -- pressing Clear and then OK destroys it
mid-deletion.

**Android forgets more than the desktop, and the report says so.**
`WebStorage.deleteAllData()` covers localStorage, IndexedDB and WebSQL for
every origin, which is precisely the gap Qt leaves, so it goes with the cookie
clear and the note points it out rather than leaving it a pleasant surprise.
`removeAllCookies` answers whether anything went but never how many, so the
count stays -1 there too. The cache has no completion callback at all and is
`unconfirmed` however well it goes, and is `refused` outright when no view is
open, because Android clears it through a view rather than a manager. Visited
links are **refused**: Android has no visited-link store, and
`WebView.clearHistory()` is the back/forward list -- which is the shell's own
data, shown in the tab tree. Mapping one onto the other would delete something
nobody asked about while reporting success for something that never happened.

Both backends put a ten-second deadline under the whole thing, so a report
always arrives. The Android one needs it more rather than less: its answer
comes back through a Java callback on the UI thread, and if the Activity has
gone or the JNI call did not land there is nothing that would ever fire. It
was written without one, and the second failure is the worse of the two --
the caller waits for ever *and* the in-flight flag stays set, so every later
clear is refused as "one is already running". A missed callback would have
turned the button into one that never worked again. When the deadline fires
the state is `unconfirmed` and never `done`, because nothing came back and
saying so is what that state is for.

### Kiosk clears the shared profile rather than getting its own

The idle timer walked home and left the last person's logins on disk. The fix
is not a second profile, and the reason is that kiosk **borrows** a view the
shell already has: `enter()` takes a backend and hands it back, the tab's
history and tree entry come with it, and a blank home means "whatever tab you
were on". A profile of its own means a view of its own, which means the thing
on screen is no longer that tab -- a different feature wearing the same name,
plus a second profile lifetime to answer for against `main()`'s declaration
order.

So it clears the shared stores, at three moments: **entering**, because the
operator's logins must not be handed to the public; **the idle timer**, which
is the only signal a kiosk gets that somebody walked away -- and navigation is
issued first, so the screen does not sit on a stranger's page while the store
empties; and **leaving**, because the public's session must not be handed back.

**The idle moment is driven now, and it turned up something the ordering did
not promise.** The walk-home request the timer issues arrives at the server
carrying *no* cookie. Navigation is issued first in program order -- which is
what that sentence claims, and it is still true -- but the clear reaches the
cookie store before the request goes out. The effect is welcome, since the
kiosk's own walk home is then not authenticated as the person who just left,
and it is a race rather than a guarantee, so it is recorded here and
deliberately not asserted by the driver. A test that pinned it would be
pinning the scheduler.

Both halves of what the timer does are checked, because either alone looks
like a working kiosk: the navigation as a request the server was sent, the
forgetting as the `Cookie:` header it stops being sent. Telling the three
clearing moments apart took work of its own, since they call the same function
on the same stores -- the cookie is re-established only after the entering
clear has been *seen to finish* by its own log line, every reading is taken
while kiosk is still up so the leaving clear cannot be the cause, and the log
slice covering the measurement must name the idle clear and neither of the
others.

**It is off by default, and the default is load-bearing.** Page-requested
fullscreen routes through the same `toggle_kiosk()`, and that path now clears
the flag explicitly alongside the ones it already clears for `allow_escape`
and the watchdog. Without that line the leak fix becomes a data-loss bug for
anyone who turns it on: a video going fullscreen is not consent to be signed
out of every site.

### The kiosk settings were in a different file, under an error

`settings_store::kiosk()` used a default-constructed `QSettings` while every
other accessor goes through `open_settings()`. With no organisation name ever
set, that resolves to `~/.config/Unknown Organization/Hydra.conf` -- which
exists on this machine, dated 20 August, holding exactly the nine keys
`set_kiosk` writes, while `~/.config/hydra/hydra.ini` has every other group and
no `[kiosk]`.

Two things the probe corrected before anything was changed. **It worked**: the
defect is the location, not a failure, so moving the accessor without more
would have stranded a working configuration. And **`status()` returns
`AccessError` even on a successful read**, so a migration gated on the status
would have refused exactly the case it exists for. The migration gates on
finding keys instead.

It carries the values over once, on the first read that finds no `kiosk/home`
in `hydra.ini`, and **leaves the old file alone** -- a kiosk config is
somebody's deployment, possibly `allowEscape=false` on a screen with no
keyboard, and losing it silently costs more than the few lines. The old file is
not ours to delete and is harmless once nothing reads it.

**What is verified**: the factory path, against a real engine with a scratch
profile -- 16 checks, the measurement being the `Cookie:` header a local server
was sent, present before the clear and absent after, with the on-disk cookie
database and the http cache both shrinking. Two instrument faults were caught
before that result was believed: the first version pointed data and cache at
one directory, so "the cache" was the whole profile, and it asked
`visitedLinksContainsUrl` after reloading the page, which re-registers the
visit and would have reported a failure of its own making.

**And since then, the whole path through the shell.** `test/live/try_forget`
opens the settings dialog the way `open_settings()` does, ticks the boxes,
presses Clear, answers the confirmation, and then asks the loopback server
whether it is still sent a `Cookie:` header -- so nothing rests on a status
label or a report struct, both of which are the program's own account of
itself. It covers kiosk clearing on entering and on leaving too.

The check worth having is the negative. Entering kiosk by way of a *page*
asking for fullscreen must not clear, and that route runs through the same
controller, so its silence has to be shown to be a measurement rather than an
absence. It is proved by mutation: commenting out the one line in
`toggle_kiosk` that clears the flag turns exactly two checks red and no others,
with the controller's own log line appearing where it had been silent.

The driver refuses to run at all unless test mode is on *and* the profile it is
about to empty reports a path under `~/.qttest` -- read off the object being
cleared, not off a claim that something else arranged it. A test that deletes
cookies is not one to point at a directory by accident.

**What is still not verified**: the idle-timer clearing moment, which is the
third of the three and the only one an unattended screen actually reaches; and
the Android implementation, which compiles on the device build and has never
run -- and cannot be reached on the test phone at all, for the reason recorded
under the Android abort above.

## A page's own Print button, and the probe that proved nothing

`QWebEnginePage::printRequested` had nothing connected, so `window.print()`
and every Print control a site draws in its own chrome did nothing at all --
no dialog, no message, no error. File > Print worked the whole time, which is
why it survived: printing was tested the way the menu offers it.

It is connected to the same `print()` the menu action calls, so a
page-initiated print gets the same dialog, the same printer and the same
status-bar answer, and it shares printing's re-entry guard because a page may
call `window.print()` in a loop.

**The first attempt to verify it proved nothing, in a way worth writing down.**
A probe page calling `window.print()` was opened by passing a `file://` url on
the command line, no dialog appeared, and that was nearly recorded as a
negative result. But `main()` accepts only `http` and `https` from argv -- a
deliberate decision so that `hydra ./tree.txt` keeps meaning the tree -- so the
page never loaded and the silence was about nothing. The first tool reached for
to look for the dialog, `xdotool`, is not installed on this machine either,
which would have produced the same empty answer for a second, independent
reason. Two ways to be wrong, stacked, both looking exactly like a finding.

Served over http instead, from a scratch profile, it is verified:

    0x560002c "Print Page": ("hydra" "Hydra")  351x199+0+0  +415+891

That is `QPrintDialog` with the title `print()` gives it. The probe is known to
be capable of seeing windows because the main window appears in the same
listing -- without that line the absence of a dialog would again have meant
nothing.

## The page requests nothing was listening to

Four `QWebEnginePage` signals had nothing connected. Qt does not treat that as
an error -- it proceeds with a default, which for every one of these is some
flavour of "no" -- so each was a feature that silently did nothing, and none of
them could be told apart from a page that had simply not asked.

**Passkeys were the one that mattered**, because signing in is what this whole
run of work has been about. Unhandled, `webAuthUxRequested` leaves a passkey
sign-in waiting forever with no window and nothing said.
`webauth_dialog.{h,cpp}` is the state machine as a dialog, and it names no
engine type: it declares its own `pin_reason`, `pin_error` and `failure`
enums and answers with plain signals, while `qtwebengine_view.cpp` maps Qt's
enums onto them arm by arm rather than by cast, so a member Qt adds later is a
compiler warning instead of a silent mis-map.

This is the one request here *not* answered while its signal runs, and that is
the API's shape rather than a choice -- Qt hands over a `QObject` carrying a
state machine, so the request and the dialog are both held in a `QPointer`.
Three departures from Qt's own simplebrowser example, each for a reason:

- the dialog is parented to the **view**, not the window, so closing a tab
  cannot leave a dialog whose lambdas point at a deleted backend;
- tearing it down **disconnects before hiding**, because the dialog reports a
  closed window as a cancellation, and telling the engine to withdraw a request
  it has just completed would throw the sign-in away at the last step;
- it uses `deleteLater()` rather than `delete`, because the teardown runs from
  `stateChanged` and the state can change inside `cancel()`, which is itself
  called from one of the dialog's own handlers. Qt's example deletes outright
  and gets away with it only while Chromium answers asynchronously.

`remainingAttempts` is shown only after a key has actually refused a PIN. On a
first prompt a zero would tell somebody their key was one mistake from locking
before they had made any.

**`registerProtocolHandlerRequested` and `fileSystemAccessRequested` now ask**,
with "Not now" as both the default and the escape. The file-system prompt says
read or change, file or folder, and the path -- and for a folder adds that it
covers everything put there afterwards, which is the part of that grant people
do not expect. Both share a re-entry guard with printing, because a page can
ask in a loop and each question runs a nested event loop the page keeps running
underneath. Qt's example puts a `Q_UNREACHABLE()` on an unhandled flag
combination; that is deliberately not copied, since it turns an engine that
grows a third flag into a browser that aborts.

**Clipboard read and pointer lock fall through the permission mapping**, and
did so silently. `policy::feature` has no member for `ClipboardReadWrite`,
`MouseLock`, `DesktopVideoCapture`, `DesktopAudioVideoCapture` or
`LocalFontsAccess`, and none was invented -- adding one is a change to the
enum, the INI encoding, the settings page and the shield, which is its own
piece of work rather than something to take in passing. Each is named as its
own `case`, so a new Qt member warns rather than joining the refused pile, and
the refusal is now reported under `HYDRA_PERM_DEBUG` by the enum's *key* rather
than its number:

    permission: http://localhost:8731/ asked for ClipboardReadWrite -> denied, no policy feature covers it

That probe is known to be capable of a negative as well as a positive: the
first attempt failed with `NotAllowedError: Document is not focused` and
produced no line at all, so an absent line and a refusal line are
distinguishable.

**They have their features now**, `clipboard_read` and `pointer_lock`, and the
reason to add them is the distinction `policy.h` already draws against
`extractor_dom`. That one stays absent because the *power does not exist*: a
permission for it would be a promise nobody keeps. These two are the opposite
case -- the capability is real and the engine asks about it on its own; the
only thing missing was somewhere to record an answer, so the refusal was made
by a `default:` arm rather than by anybody.

Adding one turned out to cost four edits, because the design had anticipated
it: the enum entry, a row in `policy.cpp`'s info table, a global default, and
the mapping in `qtwebengine_view`. The shield and the settings page iterate
`feature_count()` and pick a new feature up on their own, and rules persist by
name rather than by position, so nothing on disk depends on where it sits.
Fifteen features occupied fifteen of the thirty-two slots a `quint64` at two
bits each allows, so there was room.

**Both default to block, which is exactly what the `default:` arm did**, so
nothing changes for anyone who does not go looking. What changes is that
saying yes became possible at all. A browser that can only ever refuse pointer
lock is not offering a setting, it is stating a limitation -- and the earlier
argument for leaving it refused (that a prompt with nowhere to record its
answer would ask on every call) was an argument for giving it somewhere to
record one, not for keeping the refusal.

**The passkey dialog has never been seen driven by the engine, and the reason
is worth recording.** With no authenticator present Chromium simply waits --
the `create()` promise stays pending, `webAuthUxRequested` never fires, and
there is nothing to see on a machine with no security key. Driving it with a
DevTools virtual authenticator **segfaults**, before any UX state is delivered,
at a virtual dispatch through a bad pointer inside `libQt6WebEngineCore.so.6`
with nothing from the hydra image on the stack.

That is not ours, and it was proved rather than assumed: swapping in the
committed `qtwebengine_view.{h,cpp}`, in which nothing connects
`webAuthUxRequested` at all, and rebuilding gives an identical crash at an
identical program counter and return address. A pre-existing Qt 6.8.2 fault on
the virtual-authenticator path. Whether a real security key takes the same
route is unknown.

What *is* verified is the dialog itself, driven offscreen by a scratch driver
through all four states: accept disabled until an account is picked, the
account list gone rather than stacked when the PIN question arrives, accept off
below the minimum length and off while the confirmation differs, the old PIN
not left in the box, retry present for a soft block and absent for a hard one,
Cancel becoming Close, and an unknown failure still producing a sentence.
`registerProtocolHandler` was verified on screen against a real page.
**`fileSystemAccessRequested` is driven both ways now**, by
`test/live/try_files`, and getting there needed three things worth recording.
Reaching it at all takes a real gesture, so the driver sends an actual
`QMouseEvent` to the render widget and then *measures* that it worked by
reading `navigator.userActivation.isActive` back off the page rather than
assuming the click counted. The chooser Chromium opens first is Qt's own
`QFileDialog`, so the driver sets `AA_DontUseNativeDialogs` -- on itself, not
on the browser -- and answers it from `topLevelWidgets()`, then answers the
prompt behind it. And the refusal case uses a *different* folder from the
allow case, because Chromium remembers a grant per path and would otherwise
answer the second question itself.

**The re-entry guard could not be reached, and the driver says so instead of
passing.** Three findings stack up to that. The obvious test -- two requests
from one script -- passes whether or not the guard exists, measured: the two
questions arrive in sequence because the second is only delivered once the
first is answered. A driver's own sequential code cannot act while a question
is up either, since a modal `exec()` runs a loop deeper than the driver's
spin, so anything that must happen during a question happens in a timer. With
both fixed and a timestamped trace, the answer is unambiguous and is about
the engine rather than the test: while one of these questions holds the loop,
nothing from the page reaches the browser at all -- a second click went in
with the box up and the page did not report seeing it until twelve
milliseconds *after* the box closed.

So those sections print INCONCLUSIVE and assert nothing about `m_prompting`.
They still assert the property a user would notice, that two questions never
appear together, and they will assert the guard itself, unedited, on any
build where a second request does get through. The guard remains
reviewed-only code, which is a smaller claim than "tested" and the true one.

## One Hydra per profile, because two stopped being harmless

While the profile was off the record, two copies of this browser did not meet:
each process allocated its own storage. The named persistent profile put both
into one directory of leveldb databases and one SQLite `Cookies` file, none of
which arbitrates between writers. A double-clicked launcher reaches that, and
so does anyone who starts the app with a copy already open — it happened twice
by accident in the session that added the profile.

`src/single_instance.{h,cpp}` holds the lock. **`QLockFile` decides who runs
and `QLocalServer` carries the hand-over**, and the split is the point: the
socket cannot arbitrate, because its staleness test is "connect, and if that is
refused, `removeServer()` and listen" — `removeServer()` unlinks
unconditionally, so two launches milliseconds apart both fail to connect before
either has listened, both unlink, and both listen. That was measured with a
probe rather than argued: against a live owner, a second process got
`removeServer=1` then `listen=1`, two servers where the design permits one. A
double-clicked launcher is exactly that race. `QSharedMemory` was rejected for
the opposite failure — on Unix the segment outlives the process, so one `kill
-9` leaves the browser permanently convinced it is already running — and a
hand-rolled pid file for reimplementing what `QLockFile` already does, with a
create/check race to get wrong.

Both are keyed on `AppDataLocation`, the directory that actually holds the
profile, rather than on a fixed name, so a scratch `XDG_DATA_HOME` is a
different application by construction. The socket lives in `XDG_RUNTIME_DIR`
under a hash of that path, because a unix socket path lives in a 108-byte
`sun_path` that a deep data directory overruns silently.

**A stale lock does not brick the browser, which is the requirement that
matters and the one such schemes usually fail.** `QLockFile::tryLock` clears a
lock whose owner is gone, and every branch was measured: a live owner refuses;
an owner killed with `SIGKILL` is taken immediately, with `setStaleLockTime` at
30000 *and* at 0, so the age limit is not what saves it; a recycled pid running
another program is taken, because Qt compares the executable name; and a lock
naming another host is taken only once older than the age limit, which is the
one case pid-liveness cannot answer and the reason the limit is stated rather
than set to zero.

One trap was checked because the whole guard rests on it: Qt writes the
**executable** name into the lock file, not `applicationName()`. Had it written
the latter, `Hydra` would never match the `hydra` that `/proc` reports, and
every live owner would have been declared stale — the guard would have run
backwards, refusing nothing and permitting exactly what it exists to prevent.

What a second instance does depends on what it was asked for. An http or https
url is handed over and the running instance opens it; no argument at all is a
launcher clicked twice, so the running window comes forward; **a tree path is
refused**, because swapping the open tree under somebody is not what
`hydra other-tree.txt` means. A hand-over that cannot be delivered exits
without starting rather than falling back to opening the profile anyway.

**`show()` is deliberately not called on the hand-over, and kiosk mode is
why.** Entering kiosk hides the main window on purpose — the stage is its own
fullscreen window — so an unconditional `show()` would put the browser chrome
back on screen underneath a locked-down kiosk because somebody clicked a link
in another application. A minimized window is still `isVisible()`, measured on
this desktop as 1/0 for shown, 1/0 for minimized and 0/1 for hidden, so
restoring one does not need `show()` either. The only thing it would add is the
case that must not happen.

A real defect the runs found, and the reason one of the scenarios exists: the
first version removed the socket in its destructor unconditionally, so a
*refused* instance unlinked the socket the running one was still listening on.
The first link click would have worked and every one after it would have been
told the browser was not answering — and nothing failed at the moment it went
wrong.

**Android is excluded, in the build and in the source.** The system runs one
process per application and a launcher tap resumes the task that exists, so
there is no second process to keep out and links arrive as intents rather than
as argv. A lock there would guard nothing and be one more thing left behind.

## A bank turned this browser away, and the user agent is why

Reported 2026-08-29: seb.se answering with "we no longer support the version
of Google Chrome you are using". Measured rather than guessed -- a local
server that logs what asks:

    Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko)
    QtWebEngine/6.8.2 Chrome/122.0.6261.171 Safari/537.36

**Two faults in one string.** Chrome 122 shipped in February 2024, so a site
gating on a version sees a browser years out of date. And `QtWebEngine/6.8.2`
is a token no real browser sends, so a checker working from a list of known
browsers has an unknown one -- which is the worse of the two, because a version
comparison can be generous and a whitelist cannot.

Corrected to what a current Chrome actually says:

    Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko)
    Chrome/140.0.0.0 Safari/537.36

Derived from Qt's own string rather than written out, so the platform stays
true on whatever this is built for. `Chrome/140.0.0.0` and not
`Chrome/140.0.6261.171`: replacing only the major left 140 wearing Chromium
122's build numbers, a combination that has never shipped and is a *worse*
fingerprint than the honest string. Real Chrome has sent a reduced user agent
since version 101, freezing everything after the major to zero.

**It is a claim about a version we do not have.** The engine really is
Chromium 122, so a site needing something only 140 has now fails later instead
of turning us away up front. That is the worse failure in general and the
better one here: being refused at the door is unconditional and cannot be
worked around from inside the page. The number is a named constant because it
has to be maintained -- nothing in the code can work out what today's Chrome
is, and left alone for two years it recreates the bug it was written to fix.

**What it does not fix, measured on the same server:** `sec-ch-ua` still says
`"Chromium";v="122"`. Chromium builds client hints from its real version and Qt
exposes no override, so a site reading `navigator.userAgentData` rather than
the string sees through this.

**Two attempts to close that gap are recorded as inconclusive rather than
failed, which is the honest reading and the useful one.**
`QTWEBENGINE_CHROMIUM_FLAGS=--enable-features=UACHOverrideBlank` -- the
Chromium feature that blanks the client hints when the user agent is
overridden, and whose name is in the shipped library -- changed nothing, and
neither did `--disable-features=ClientHints`. But the control failed too:
`--lang=de` produced no observable change either, and this build sends no
`Accept-Language` at all, so there was nothing to see. **The flag mechanism was
never shown to reach the engine**, which means those two results say nothing
about the features and only something about the experiment. Anybody retrying
should first establish that the environment variable arrives -- a switch with
an unmistakable effect -- before drawing a conclusion from one that appears to
do nothing.

The avenue that would certainly work for the *headers* is the one this browser
already owns: `qtwebengine_interceptor` sees every request and can set them.
That would align `sec-ch-ua` with the string. It would not touch
`navigator.userAgentData`, which is a JavaScript surface an interceptor cannot
reach, so it is half a fix -- and which half matters depends on what the site
actually reads, which is not known, because the page that refuses is behind a
login this session cannot reach. Building it now would be adding machinery on
a guess.

**Not reproduced, and worth being honest about.** The public homepage does not
show the banner with either user agent -- checked with the probe, which loads
the page, waits for its scripts and reads `document.body.innerText`. Whatever
refuses is behind the login, which this session cannot reach. So the fix is
justified by what the string says rather than by watching the banner
disappear, and the person who saw it is the one who can confirm. The probe
deliberately reports one bit and takes no screenshot: the machine it runs on
had a real banking session on screen at the time.

## Three things a phone cannot do, reported 2026-08-30

A url could not be loaded from the address bar, editing in it was "a bit
wonky", and there was no way to reach the tab menu at all. Two causes.

**Nothing in this application had ever set an input-method hint.** Measured:
`grep setInputMethodHints src/` is empty. So on a phone the address bar behaves
like a message box -- the first letter capitalised, predictive text composing
and correcting as you type. That is the wonky editing, and it reaches further
than untidiness: while an IME is composing, the key that should submit commits
the composition instead, so the address never loads and the box just sits
there. `ImhNoAutoUppercase | ImhNoPredictiveText` now.

**`ImhUrlCharactersOnly` is deliberately absent, and the reason is a feature
this box has.** It takes search terms as well as addresses --
`navigate_to_address` decides between them, and `looks_like_address` is a
separate unit because the interesting half is a privacy question. A url
keyboard has no space bar, so that hint would have traded a wonky address for
an unusable search. `ImhLatinOnly` is out for the same reason: a search is
whatever somebody wants to type.

**The tab menu is behind a mouse button a touchscreen does not have.**
`tab_tree_view` sets `Qt::CustomContextMenu`, so rename, new folder, lock and
forget all arrive through `customContextMenuRequested` -- which a finger never
raises, leaving the tree read-only on a phone. Qt's Android plugin can
synthesise the right button from a long press and is asked to, rather than
growing a gesture handler here: the menu then stays one code path, the one the
desktop tests exercise, instead of two that have to be kept in step.

**The long press was tried Qt's way first and was wrong.** Reported back: the
menu came on a *double tap* rather than a hold, and appeared in places it was
not wanted -- the variable applies to the whole application, so it was not the
tree's gesture, it was everyone's. It is gone. The hold is explicit now, in
`tab_tree_view`'s existing viewport filter: half a second, Android's own
threshold, cancelled if the finger travels more than the drag distance so the
tree's drag-and-drop keeps the gesture it already had. Scoped to one widget,
so it cannot surprise the page or the address bar.

**And the Go key is asked for properly rather than worked around.** The button
below stays, but `Qt::ImEnterKeyType` is what actually reaches the keyboard:
Qt's Android plugin builds the `imeOptions` from it -- `imeOptionsFromEnterKeyType`
in its jar -- and a `QWidget` answers it through `inputMethodQuery`, which is
why `address_line` is a subclass rather than a call. Hints describe the text;
this describes the key.

**Driven on the Note 9 afterwards, and the hints were not the whole story.**
The typing came out clean, but the keyboard's action key reads **Next**, not
Go -- and tapping it moves focus out of the field rather than navigating, so
the address sits there and the keyboard closes. A return sent afterwards does
nothing either, the field no longer having focus. Which action an IME offers
is its own decision and Qt exposes no hint that asks for Go, so the fix stops
depending on the keyboard: on Android the address bar carries a **Go button**,
beside the clear cross, which every mobile browser has and which a person can
see. Verified on the device -- tapping it fetched the page from a local server
that logged the request.

**And the first attempt to test it failed for a second reason, which turned
out to be the better find.** `127.0.0.1:8753/typed` did not load even with a
working Go button, because `looks_like_address` classified it as a *search*.
See below.

One thing to look at when it is tried: if a long press already begins a drag in
the tree, the two want reconciling, and the answer is likely a hold threshold
rather than dropping either.

## Two Android reports not yet acted on, with the likely causes

Recorded rather than fixed, because both are larger than an evening and
guessing at them would be worse than saying where to start.

**YouTube: fixed, and the suspect was right.** The report sharpened to "a lot
of flaws in its display, of a lot of things, including the video" -- which is
the signature of a software layer rather than of a video problem, since
transforms, effects, canvas and video all lose GPU compositing together and
video loses most, a `SurfaceTexture` having nowhere to composite to.

`setLayerType(LAYER_TYPE_SOFTWARE, null)` was there for a real reason: with the
default layer the renderer process died immediately. But that was measured on
an **emulator**, and it was avoided rather than diagnosed. Re-measured on a
Galaxy Note 9 against system WebView 151, it does not reproduce: example.com
renders, `m.youtube.com` renders in full -- logo, chips, thumbnails, titles --
and a video plays and is visible, with no renderer death in the log through any
of it. The default is hardware compositing now.

`HYDRA_ANDROID_SOFTWARE_LAYER=1` puts the old behaviour back, in the shape
`HYDRA_ANDROID_WEBVIEW=0` already uses. That is not hedging: the old finding
was true once and may be true again on hardware nobody here has, and its
symptom -- a blank rectangle where the page should be -- is not one a person
could otherwise work around.

**A second thing was verified in the same screenshot**, which is the value of
photographing the whole window rather than the part being asked about: the
keyboard's action key now reads **Go**. It read *Next* two days ago, so the
`Qt::ImEnterKeyType` change is confirmed on the device rather than merely
built.

**Audio stops when the browser is not in front.** Nothing in this tree pauses
the WebView -- there is no `onPause` or `pauseTimers` call anywhere in
`HydraWebView` -- so this is Android doing what it does to a backgrounded
process rather than hydra choosing it. Playing audio while not visible is not a
flag; it is a foreground service with a notification, which is a real feature
with an Android permission behind it and a user-visible ongoing notification.
Worth deciding deliberately rather than adding because a video stopped.

## A local address with a port and a path was sent to a search engine

Found while testing the Go button, because the test address would not load
even once the button worked. Measured:

    127.0.0.1:8753               -> address
    127.0.0.1:8753/typed         -> SEARCH
    192.168.1.1:631/printers     -> SEARCH
    localhost:8080/admin         -> address
    example.com:8080/path        -> address

**The order of two operations, and it is the whole bug.** `looks_like_address`
stripped the port before removing the path, and `without_port` only strips a
trailing `:port` when what follows the colon is digits and nothing else. In
`127.0.0.1:8753/typed` the candidate was `8753/typed`, so nothing was
stripped, the host came out as `127.0.0.1:8753`, and that is not an address.
Path first, then port.

**A hostname survived it by accident**, still looking domain-shaped with the
port attached, so only *bare IPs* fell through -- which is the worst case to
lose. A router page, a printer at `:631`, a service on a machine at home: those
are exactly the addresses somebody types with a port and a path, and this file
exists because sending one to a search engine is a privacy failure rather than
a missed navigation. Its own header says so: a bare hostname must never end up
in a search box.

That the accident spared hostnames is also why nothing noticed. The suite
tested `127.0.0.1:36853` and `localhost:8080`, both of which pass without a
path, and no case combined a port with a path. Three now do, including the
bracketed IPv6 form.

## It asked for no languages at all

Found while chasing the user-agent problem, by logging every header of a real
request rather than the one header being looked for. `Accept`,
`Accept-Encoding`, the `Sec-Fetch-*` set and the client hints were all present.
`Accept-Language` was absent -- nothing ever called `setHttpAcceptLanguage`
and Qt supplies no default.

**What that costs is not abstract.** A site with more than one language has
nothing to negotiate against, so it serves whatever it defaults to -- often
English, sometimes a guess from the address -- and a Swedish reader on a
Swedish site gets the wrong one, with no recourse but to hunt for a flag. It is
also distinctive in its own right: no real browser omits this, so omitting it
is a fingerprint rather than an absence of one.

**The transformation is a pure function because the input is messier than it
looks**, and that was measured rather than assumed.
`QLocale::system().uiLanguages()` answers, on this machine:

    en-US, en-Latn-US, en, en, en-Latn-US, en-US

-- duplicated, and carrying script subtags no browser sends. Chrome sends
`en-US,en;q=0.9`. So the script is dropped, repeats are removed with the order
kept, and everything after the first gets a descending quality. Had the tests
used a tidy invented list, a transformation that did nothing at all would have
passed them, which is why the measured list is the case in the suite.

Qualities stop at 0.1 and never reach zero, because `q=0` means *not
acceptable* -- a long enough language list would otherwise end by refusing
languages it had just asked for.

Verified as a pair rather than a pass: the same full header dump that showed
nothing before shows `Accept-Language: en-US,en;q=0.9` after.

## Android's cookies: one real gap, and one that was imagined

Two things looked wrong on the phone after the desktop gained a persistent
profile. Only one of them was.

**The real one: the third-party cookie rule could not reach Android.** Nothing
in `HydraWebView` had ever touched `CookieManager`. First-party cookies worked
anyway, because `setAcceptCookie` defaults to true — but
`setAcceptThirdPartyCookies` defaults to **false** for anything targeting
Lollipop or newer, so the phone blocked every third-party cookie outright no
matter what the shield said. The desktop meanwhile hands
`QWebEngineProfile::cookieStore()` a filter that calls
`request_filter::allow_cookie(host, third_party)` for every cookie, so a
per-site allow is honoured there. The same switch, in the same dialog, did one
thing on one backend and nothing on the other — and sign-in flows that bounce
through an identity provider are exactly what the blocked case breaks.

It is answered by the same filter now, through a `allowThirdPartyCookies`
native call beside the existing `shouldBlock`, applied per navigation in
`onPageStarted` because the policy is per site. The answer is coarser than the
desktop's and cannot be otherwise: Qt asks per cookie, Android offers one
boolean per WebView, so the most this can express is "third-party cookies, on
this page, yes or no". The first-party half is not asked at all, because
Android has no hook for it and refusing every cookie when first-party ones are
disallowed would break pages far past what the setting says.

**The imagined one: cookies did not need flushing, and the code that flushed
them is gone.** The reasoning was good and the conclusion was wrong.
`CookieManager.flush()` is documented as what forces the write, Android kills
backgrounded apps without running anything, and the desktop had just been
fixed for a bug with exactly that symptom — so a flush went in on
`onPageFinished` and on `ApplicationInactive`, and it was measured rather than
assumed.

The measurement, over `adb reverse` to a local two-page server: set a cookie,
`am force-stop` the app (SIGKILL, no clean shutdown), relaunch, ask the server
whether the cookie came back.

    REQ /set        cookie=[]
    REQ /favicon.ico cookie=[hydraprobe=alive]
    REQ /show       cookie=[hydraprobe=alive]     <- after the kill

It survived. Then the control, which is the half that settled it — the same
test against a build with both flushes removed and nothing else changed:

    REQ /show       cookie=[hydraprobe=alive]     <- also survived

Chromium's WebView commits the cookie to its SQLite store on its own, promptly
enough that a kill eight seconds later loses nothing. The flush bought nothing
measurable, so it is not in the tree. Recorded here because the argument for
adding it is genuinely persuasive and somebody will make it again.

**A note on the first run of that experiment, which said the opposite.** It
reported no cookie database anywhere in the app's data, which read as "cookies
never persist at all". What actually happened is that the phone's screen locked
part-way through, the app went to the background before the page finished, and
`onPageFinished` never ran. The tell was in the server log and was not read at
the time: the successful runs show a `/favicon.ico` request after the page, and
that first one does not. A probe interrupted by a lock screen is not a
measurement of the thing it was pointed at.

**Not verified end to end: the third-party fix itself.** Demonstrating it needs
two distinct hostnames the phone can reach — cookies ignore the port, so a
second port on `127.0.0.1` is the same site — and a per-site allow set through
the shield on the device. It rests on reading Android's documented default and
on the JNI symbol being exported and reached, which was checked; it does not
rest on a round trip.

## Android: media, downloads and the platform's autofill

Back to Android, and the parts of it that are not the web view: what happens
when the screen goes off, where a download lands, and who fills a form.

### Background audio on Android, and why the browser hands over

The need is ordinary: play something, turn the screen off, read a message, keep
listening. **A browser tab is the wrong container for it**, and the reasons stack
in a way worth writing down once so it is not re-derived each time somebody asks.

**The site stops itself.** YouTube pauses on `visibilitychange` when its page is
hidden. That is not Android and not the engine — it is the site, deliberately,
and it is what Premium sells. Beating it means injecting script to swallow the
event.

**Android stops the process.** A backgrounded app with no foreground service is
throttled and then killed. Reliable background audio needs a foreground service
plus a `MediaSession` with a media notification: lock-screen controls, headphone
buttons, the lot. That is real Android work and it is not a WebView setting. Qt's
Android integration also calls `WebView.onPause()` on activity pause, so that
would have to be opted out of as well.

**And it would be a Chromium-class renderer kept alive to play audio**, which is
battery spent for nothing.

**yt-dlp cannot help here, and this was checked rather than assumed.** On the
desktop the vendored copy resolves YouTube fine — it returned an audio-only
stream (itag 251, `audio/webm`) on request. On the device, `which python3
python` returns **nothing**: `ytdlp_resolver` runs `yt-dlp` from PATH or the
vendored copy through `python3`, and Android has neither. So **YouTube does not
resolve on Android**, and the routes to changing that are shipping a Python
runtime in the APK or reimplementing YouTube's signature and `n`-parameter
descrambling in C++ — the second changes weekly and is a standing commitment.

**Even a successful resolve would be the wrong answer.** The url that came back
carried `expire=` about six hours out and is bound to the requesting address. It
plays now and is dead tomorrow, so it is a thing to listen to once and not a
thing to keep.

**So the browser hands over.** VLC, NewPipe and YouTube itself all resolve a
page on their own *and* run a media notification, which is the entire feature.
**Tools ▸ Open This Page in Another App**, and the same entry on any tab's
context menu.

**The type is the whole trick, and it is the opposite of the media path's.**
`open_media` names a media type precisely to keep browsers out of the chooser —
it is handing over a stream somebody already found. `open_externally` names
**no type at all**, because it is handing over a *page*, and the apps worth
reaching are the ones registered for that host. Forcing `video/*` would hide
every one of them behind players expecting a file. Two intents, opposite
decisions, and each is wrong for the other's job.

On desktop the same action goes to the system's default handler, which for an
http address is usually another browser. That is a smaller feature and it says
so rather than pretending to be the Android behaviour.

**Driven on the desktop, and proved by the other application fetching it.**
`try_handoff` serves the address it hands over, so the check is that a second
client comes and asks for it — `QDesktopServices::openUrl` returning true would
prove almost nothing, since it answers true for a great many things it has
merely passed to a launcher. One request arrives after the handoff, from the
desktop's own default browser. `about:blank` is refused rather than handed over,
because it means nothing outside this browser.

**What is not verified.** The emulator has no VLC, NewPipe or YouTube app, so
"the right app takes it and keeps playing with the screen off" is checkable only
on a real phone. What is checked here is that the entry exists and that the C++
side reports a refusal rather than doing nothing when no app answers.

**If an integrated solution is ever wanted**, the shape is known and the cost is
the point: a foreground service and a `MediaSession` on the Android side, script
injection to suppress the site's own pause, and a decision about what to do when
the engine is asked to keep a renderer alive for audio. That is three separate
commitments, and the handoff above buys the whole use case for one menu entry —
which is why it comes first rather than last.

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

**The flake, and a cause found by reading rather than by catching it.**
`test_extloop` failed five times over this session — 28, 21, 27, 27 and 27 of 34
— always inside a longer run, never on demand. Seven attempts to reproduce it
failed: alone, under load, after specific other suites, replaying the failing
sequence, whole-list passes, capturing every run, and a cold-relink theory that
looked promising and was not.

Reading the suite found what watching it could not. Seven places did
`send->click()` and then `spin(400)`, `spin(600)`, `spin(800)` before asserting
on the verdict — **a bet that judging beats a fixed clock.** When it does not,
every check in that section fails together, which is exactly the shape of the
failures: not one assertion, a group. This project has been bitten by a fixed
wait before and wrote it down — *"a fixed wait is an instrument that invents
results"* — in the subframe-tap section of this same file. The instrument was
still in the drawer.

Each is now a wait on the condition itself, with a ten-second deadline and an
early exit, so a fast machine pays nothing and a slow one is not lied to.

**What is honestly claimed:** the fixed waits were wrong regardless, and they are
gone. Twenty-five consecutive runs pass since. What is *not* claimed is a
demonstrated fix — the failure was never reproduced on demand, so it cannot be
reproduced against the change either. If it returns, the suite now prints the
numbers it compared and no longer contains the most likely explanation.

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

## What the suites and the gates found

Verification work, in the order it was done. Several of these found the test
rather than the code, which is the recurring shape in this project and the
reason they are collected rather than scattered.

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

### The gate the reorganizer depends on

`tree_diff` decides whether a language model's rearrangement of the tree is safe
to show a diff for. A bug in it does not produce a wrong pixel — it loses
somebody's tabs to a machine that made something up. It had no test.

Thirty-three checks, no bug, and the subtle cases are the ones worth naming
because each is a decision rather than an accident:

* **A leaf that existed only inside a duplicated subtree is recovered.** Culling
  the duplicate deletes its children with it, so recovery looks at what survived
  the cull rather than at the proposal as it arrived. Doing it the obvious way
  round would lose exactly the tabs the model was most confused about.
* **A tab turned into a folder is refused as an invention, not accepted as a
  rename.** Same id, different kind: accepting it would convert someone's tab
  into a folder, or a folder of tabs into a single tab.
* **An invented folder is fine and an invented leaf is fatal**, which is the
  whole asymmetry the feature rests on — folders are the model's to make up,
  tabs are the user's.
* **Undo deletes invented folders but re-attaches their children first**, so
  reverting a reorganization cannot take a tab with it.

A dropped leaf also comes back with its url rather than as an empty shell, and
returns to its original parent when that parent survived, the root when it did
not — checked, because "put it back" has two plausible meanings and only one of
them keeps a tab reachable.

**Where the sweep stands.** Seven files that no test had ever named, now with
643 offline checks between them: `hls_playlist`, `tree_outline`,
`tree_serializer` and `state_store` were wrong in ways that produced plausible
answers rather than crashes; `box_crypto`, `element_picker`, `autofill_controller`
and `tree_diff` were right. Four and four. The ones that were right are the ones
whose authors wrote down what they were defending against — and that is not a
coincidence worth being coy about.

### The model, checked against Qt's own rules

Everything the user sees of the tree goes through `tab_tree_model` and
`tree_sort_proxy`. A model that lies about its own shape does not produce a wrong
answer — it produces a crash inside Qt's view code, in a stack with no frames of
ours in it. Neither had a test.

The interesting half is not hand-written: `QAbstractItemModelTester` walks a
model and checks the contract — parent/index round trips, row counts, the order
of the signals around a change — which is the part assertions cover badly. Both
the model and the proxy pass it with **no complaints**, and pass it *through* a
sort-mode change and a filter being set and cleared, since a proxy's contract is
easiest to break while it is moving.

The behavioural half pins what a search means, including the part that surprises:
searching keeps a hit **and its ancestors**, not its descendants, so matching a
folder by name shows the folder without its contents. That is what the header
describes, and now a test says it out loud, so changing it is a decision rather
than a slip.

**One crash, and it was the test's.** The first run segfaulted inside
`QAbstractItemModelTester`, in `tab_tree_model::data()`. The cause is that this
is a *GUI* model — it answers `DecorationRole` with a style icon — and it was
being tested under a `QCoreApplication`, where `QApplication::style()` is null.
The fix was the test, not the model: a model handing back blank icons instead of
crashing would be harder to trace than the crash, so there is no guard, and the
requirement is written where the next person will look. A stack frame saying
`data()` does not say "you used the wrong application class".

### The evidence the loop reasons from

`filter_signals` produces two lists and they are not the same thing. **Observed**
is everything a page asked for — the corpus a proposed rule is simulated against,
so the dry-run can say "this would have blocked four of these". **Suspects** is
the much smaller set that got through *and* looks ad-shaped, which is what the
model is shown.

Confusing them is not a crash. It is a dry-run reporting a rule as harmless
because the corpus was too small to contain a counterexample, or a model
proposing rules against the site's own assets. Twenty-five checks, no bug, and
three rules worth naming because each exists to prevent a specific bad outcome:

* **A site's own request is never a suspect**, whatever it looks like — a
  first-party path containing `/ads/` is a house ad, and a rule against one is
  how a filter list breaks the page it was meant to fix. Its subdomains count as
  its own; a host that merely *ends with the same letters* does not.
* **A blocked request is not evidence of a missing rule** — it is the system
  working. It stays in the corpus, because a rule should still be simulated
  against it, but it is never offered as a gap.
* **Neither list can grow without bound.** Both cap at four hundred per site.
  This is fed from the interceptor on every request, and a page that asks for
  thousands of distinct urls — infinite scroll, a tracker with a nonce in every
  path — would otherwise be a memory leak that a site controls.

**Where the sweep ends.** Nine files that no test had ever named, 692 offline
checks: four were wrong — `hls_playlist`, `tree_outline`, `tree_serializer`,
`state_store` — and five were right: `box_crypto`, `element_picker`,
`autofill_controller`, `tree_diff`, `filter_signals`, plus `tab_tree_model` and
`tree_sort_proxy` against Qt's own model contract. What is left unnamed by any
test now needs a window or a network to exercise, and the live drivers already
drive most of it through the shell.

### Assembling the file, not just parsing the manifest

Fixing the byte-range offset in the parser was half the story. What matters is
the file that comes out, so `test_assembler` runs the whole thing against an
in-process server that speaks enough HTTP to be a CDN — including honouring
`Range`, since a server that ignored it would make the case under test pass for
the wrong reason.

**And this is where the original bug shows its real shape.** With the parser fix
removed, the assembled file is *the right length* — 9000 bytes, exactly as
intended — and the wrong bytes: the assembler asked for `0-3999`, then `0-2999`,
then `0-1999`, so the file is the opening of the stream three times over. A test
that compared sizes would have passed. Only comparing the bytes catches it, which
is the whole reason the fixture uses three distinguishable fills rather than
random data.

Also pinned: segments concatenate in order; a master playlist follows the
widest variant and never fetches the others; a missing segment fails once and
does **not** report completion, because a half file that claims to be whole is
worse than an error; and a manifest that is not there fails rather than waiting.

**One more fixture that lied.** The first run failed at three times the expected
length, which looks exactly like the bug it was written to catch. It was not: the
assembler had sent precisely the right ranges, and the *server* was looking for
`Range:` while Qt puts header names on the wire lowercased. Header names are
case-insensitive and the fixture was not. Finding that out took printing the
requests, which is the same move that settled the `Accept`-header business
earlier in this file — when a client and a server disagree, read the wire.

### Kiosk mode, the last feature nothing had run

Kiosk was the only feature in the shell with neither a unit test nor a live
driver, and it is the one that takes a live tab's widget *out of the window*,
reparents it into a fullscreen stage of its own, and promises to give it back. A
mistake there is not a wrong pixel; it is a tab that has vanished when you leave
kiosk mode.

Twenty-two checks, no bug. The contract holds: the widget comes back as a child
of where it came from, entering twice is refused rather than nested, entering
with no view is refused, and exiting when nothing is active does nothing instead
of crashing. Reflow asks for a zoom factor and puts it back to 1.0 on the way
out; `stretch` under reflow — which a single zoom factor cannot express — still
enters and behaves as cover, rather than pretending. Idle reset walks back to the
home url on its own, and **zero seconds means off rather than immediately**,
because a kiosk that resets under someone's hands is worse than one that never
does.

**No web engine needed**, which is why this is a unit test and not a live driver:
kiosk asks a view only for its widget, its url, a zoom factor and a settings
application. A fake backend answers all four.

Two things the first draft got wrong, both mine and both instructive. It asserted
the widget was *visible* again after exit — but the controller's contract is to
hand it back to `restore_to`, and re-adding it to a layout is the caller's job;
`main_window` does exactly that in its `left` handler. Asserting that here would
have been asserting somebody else's work and failing for the right reason. And it
put the fake backend on the stack while parenting it to its own widget, as the
real backends do — so deleting the window freed a stack object and the run ended
in `free(): invalid size`. Qt's ownership graph is not somewhere a stack object
belongs.

**What is not in doubt:** the seam. `shouldInterceptRequest` onto the shared
`request_filter`, `addJavascriptInterface` for the content scripts, and
`shouldOverrideUrlLoading` for `magnet:` are all still to write (§19.5), and
none of them are blocked by this — they are Java-side work that plugs into
C++ that already exists.

### A warning-clean build, measured rather than declared

Turning `-Wall -Wextra` on is easy to do as a gesture. The question worth asking
first is what it costs, so it was measured: the whole of `src/` produced **five
warnings** across three toolchains — gcc against 6.8.2, gcc against 6.11, and
clang for Android — and none of the five was a bug.

Four were `entry{upstream, ctx}` filling six further members by position;
correct today and one reordered member away from not being, so the fields are
named now. The fifth was a range-for over string literals binding a `const
QString &` to a temporary per iteration: safe here, and the kind of safe that
stops being safe one refactor later.

The flags are on for the app's own target, and **not** `-Werror`: a warning that
appears on somebody else's compiler should not stop them building. The point is
that the next warning is visible rather than sixth in a list nobody reads.

One deprecation was left, and it is worth recording what was done about it rather
than only that it was fixed. `QSortFilterProxyModel::invalidateFilter()` is
deprecated from 6.9 in favour of a begin/end pair that does not exist before it,
so both spellings are now present behind a version check. **The deprecated call
was tested before it was replaced** — `test_model`'s twenty-four checks pass on
6.11 with it — so this is forward-compatibility and not a repair. That
distinction matters here because the *other* deprecation this project met, the
WebEngine permission API, was documented as functional and was not: geolocation
had silently stopped arriving. A deprecation warning says nothing about whether
the thing still works. Only running it does.

## Settings, gathered into one INI

Continuing §"Settings (arch §11.3, §11.4)" above: what the pages became, and
the move of every setting into a single file.

### The settings pages, made browser-shaped

The category list and the search box were already there. What was not was the
thing those two exist to serve: a page that explains itself. Every page was a
`QGroupBox` of `"Label:"` and a control — which fits more rows on a screen and
tells the reader nothing, so they have to already know what "Referer header"
means before the page can help them.

Every row is now what Firefox and Chrome both settled on: **the name of the
setting, one line under it saying what it is, and the control on the right.**
The descriptions live in `policy.cpp` beside the labels, so a new feature that
arrives without one is visibly missing it. They describe the *power* rather than
the state — "where you are, when a page asks" is true whichever way the setting
is set, while "sites cannot see where you are" would be a lie half the time.

Sections are a bold heading and a hairline instead of a bordered box: a frame
around every three rows is a lot of furniture for something whose only job is to
say "these belong together".

**Four defects that only a screenshot could show**, which is why this driver now
takes them:

* The category list read **"Privacy && security"**. A `QListWidgetItem` draws its
  text literally — the ampersand escape belongs to menus and buttons.
* The search box was **below** the pages, next to OK, where a filter for a
  *result list* would go. It reads as one there. Both browsers put it on top.
* A horizontal scrollbar across pages that measured as fitting comfortably. The
  vertical scrollbar arrives, takes fifteen pixels off the viewport, and content
  already laid out at the wider size overflows by exactly that much. Sideways
  scrolling is now off: everything on these pages wraps, so a horizontal bar can
  only ever mean the layout is a few pixels out.
* Controls sitting flush against that scrollbar, which reads as clipping and is
  a missing margin. There is a gutter now, set once where pages are wrapped
  rather than in each page.

The disabled BitTorrent section changed shape too. Disabling the whole container
greyed the explanations, and an explanation is exactly what someone wants to read
when a feature is unavailable — *what would this have done, and what do I need?*
Only the controls are disabled now. It also sidesteps a palette fight: a label
with an explicit foreground colour does not reliably follow its parent into the
disabled colour group, which showed up as grey titles above full-contrast
descriptions.

`try_settings_ui` gained a check that no page demands more width than its window
gives it. That is the guard for two of the four, and neither was visible to any
other assertion in the suite.

### Site exceptions, which had nowhere to be seen

The privacy page said "the shield in the toolbar sets exceptions per site, and an
exception always wins over what is chosen here" — and offered no way to find out
whether you had any. The only route to a rule set months ago was to remember
which site it was on, go back there, and open the shield. Every browser with
per-site permissions grew this list for that reason.

It is a table of the sites that have said something, and **what they said** —
`news.example  JavaScript: block, Cookies: allow` — rather than a count or a
checkmark. Removing one sends that site back to the defaults above it, which is
what removal means in a model that has no delete: every feature set back to
`unset`, so resolution falls through.

Two decisions in it are worth stating:

**Removals wait for OK.** The shield applies immediately, because it is a menu
on a live page. This is a dialog with a Cancel button, and one that had already
discarded rules would be lying about what Cancel does.

**A rule that expresses nothing is not listed.** Clearing a rule's last feature
leaves the rule behind in the engine, and offering it for removal would be
offering something that is not there.

The screenshots the driver takes now come from a dialog with exceptions and a
filter rule in it, and it captures the *bottom* of any page that scrolls. Both
because of this feature: the exceptions list sits below the fold on the privacy
page, so a capture of the top of each page would have shown every list in its
empty state — the furniture, and none of the content, which is the half that is
hard to get right.

### Restore defaults, and what it deliberately does not touch

One button, in the standard place, that **names the page it acts on** — "Restore
Kiosk defaults", "Restore Privacy & security defaults". Firefox puts one inside
each section and Chrome has a single global reset; this is the middle, because a
global reset of a browser holding per-site rules and learned filters is a bigger
hammer than anyone reaches for on purpose, and naming the page is what makes one
button unambiguous.

**"Default" means what a freshly built object holds.** The values come from
constructing a new `policy_engine`, `player_launcher`, `download_manager`,
`ollama_provider` — the same state the program runs on the first time it starts
— rather than from a second table of constants that would drift away from the
first one.

**No confirmation dialog, because nothing is written.** Restore changes the
controls; OK writes them. Cancel is the undo, and a dialog that asks "are you
sure?" about something already reversible is asking for a habit of dismissing
questions. The tooltip says so rather than leaving the user to find out.

Three things it leaves alone, each on purpose:

* **Site exceptions.** They are decisions about particular sites rather than
  defaults, they each have their own Remove, and discarding them behind a button
  labelled "defaults" would be exactly the kind of surprise this project keeps
  out.
* **The Claude API key**, which was never stored and so has no default to return
  to — clearing it would throw away something typed this session for no gain.
* **The whole filters page.** It holds rules learned on this machine rather than
  preferences, so "restore defaults" there would mean *delete*, which is not what
  the button means anywhere else. It is disabled there, and the tooltip says why
  instead of leaving a mysteriously grey button.

One bug on the way, and it is the ordinary kind: the label kept Qt's generic
"Restore Defaults" because the function that sets it ran when the first page was
selected — which happens while the button box is still being built, so it landed
on a null pointer and returned. A test that read the label caught it; nothing
that only clicked the button would have.

### All the settings in one file, and the file is an INI

Export and import, on the privacy page beside the site exceptions. The format is
**INI**, chosen rather than defaulted to: everything in the bundle is a value or
a list of flat records, and a `key=value` file a person can read, diff and edit
in an emergency is worth more here than the ability to nest. JSON stays where
data is genuinely shaped.

```ini
[hydra]
format=1

[defaults]
javascript=allow
popups=block

[sites]
news.example="javascript:block, cookies:allow"
%2A.tracker.example=ads:block
```

Two things that INI does, which had to be met rather than fought:

* **A comma means "list".** QSettings quotes what it writes, so our own files
  round-trip — but the whole reason for a readable format is that people edit it,
  and nobody hand-quotes. The reader takes the value as a list and rejoins it, so
  both spellings work. A test writes an unquoted file by hand to prove it.
* **A `*` in a key is written `%2A`**, because that is how an INI key escapes
  one. It reads back as the `*` it was, and the header says so rather than
  leaving somebody to wonder.

**What it deliberately does not carry**, because a backup that quietly omits
things is worse than one that says what it is: the tab tree, which is the
session rather than a setting; the Claude API key, which is never written to
disk; and the learned site rules. That last is not an oversight — those have
their own import on the Filters page, and it exists because rules from elsewhere
are *reviewed* before they take effect. `site_rules::judge_import` deliberately
adds nothing on its own. Carrying them in a one-click restore would route around
the one thing in this file that is a security property rather than a
convenience.

**Import merges, it does not replace.** Someone who takes a backup, accepts a new
filter rule, then restores that backup should not silently lose the rule. Reading
the same file twice adds nothing the second time.

Two orderings that would each have been a quiet bug, both now pinned by tests:
**export applies the controls first**, or it writes the settings as they were
when the window opened — which is the one thing nobody means by "export what I
have". And **import refills the window**, or the dialog goes on showing the old
answers over the new settings and pressing OK writes the stale ones back over
the import.

### policy and site-rules move to INI as well

If the format is good enough for the exported bundle it is good enough for the
files that bundle is made of. `policy.json` is now `policy.ini` and
`site-rules.json` is `site-rules.ini`, in the same shape the bundle uses — and
the line encoding is shared in `policy.cpp` rather than written twice, because
two encoders for `javascript:block` drift and the drift is invisible until a file
written by one is read by the other.

**The migration is the part that could have cost somebody their rules**, so it is
not a step anybody runs. `load()` reads the INI; if there is no INI it reads the
JSON next to it, and the next save writes the new file. Both are tested: an old
JSON file loads, and asking for the `.ini` finds the `.json` beside it — which is
exactly the first run after an upgrade.

Built-ins still stay out of the rules file, as they did in JSON: they come from
the binary, and a copy on disk is a stale duplicate the day one changes. A load
starts from the built-in defaults and adds what was stored, so reading a file
cannot lose them.

**And the live drivers had to be told.** Eight of them remove `policy.json` to
start from a clean slate — which after this change removed a file nothing writes,
leaving a stale `policy.ini` to carry between runs and quietly contaminate the
next one. They remove both now. Nothing failed to catch it; there was nothing
that *could* have, which is the same shape as the tree-file bug earlier in this
session: a rename that keeps compiling.

What stays JSON is the *exchange* document — what `judge_import` reads when rules
come from somebody else. That is a different thing from storage, it is reviewed
rather than loaded, and it has its own trust model; moving it is a separate
decision from moving the file this machine keeps for itself.

## Light and dark, and why Qt's answer was not enough

Asked for late, and the request came with a condition attached — *"this
detection needs to work well"* — which turned out to be the whole job. Default
is to follow the desktop, with light and dark as explicit overrides.

**Qt gets it wrong here, silently.** On this KDE desktop
`QStyleHints::colorScheme()` returns `Unknown`, while the XDG desktop portal
answers "prefer dark" and gsettings agrees. Every example of this in Qt's own
documentation reads the style hint and stops. Doing that would have shipped a
browser that comes up light on a dark desktop with no indication anything had
gone wrong — the failure mode this project keeps running into, where the code
looks right and the answer is wrong.

So detection is a ladder, each rung asked only when the one above has no
opinion: Qt's hint (correct on Windows and macOS, where Qt does read it), the
portal's `org.freedesktop.appearance/color-scheme` over DBus, then the palette
the platform style already handed us — a window darker than its own text is a
dark theme however badly it was announced — and finally light, deliberately,
because a wrong light guess is merely plain while a wrong dark guess is
unreadable text on a pale window.

`decide()` is a pure function of what each source said, which is the only reason
`test_theme` can cover the combination *this* machine produces without needing a
desktop that produces it. DBus is optional and the build says so when it is
missing; the Android build was run to confirm it takes the fallback rather than
failing to configure.

## The extractor loop, packaging, and the tab tree

A long run of sections that had been sitting under the colour-scheme heading
above, which is about `theme.cpp` and about nothing else. They are not one piece
of work and they are not strictly in date order -- this file is a running log
and sections were inserted wherever the writer was reading at the time. What
they have in common is only that they predate the window pass below.

### Pages are a separate mechanism, and the UI admits it

Qt forwards the application's scheme to Chromium by watching
`colorSchemeChanged`. But where Qt reports `Unknown`, `setColorScheme()` is a
*request the platform may ignore* — and this one does, without complaint: the
value reads back unchanged, the signal never fires, so the window goes dark and
every page stays white. Measured before it was worked around, not assumed.

The scheme therefore reaches the engine as a startup flag
(`preferredColorScheme`), which Chromium reads once. The cost is real and is
stated in the settings row rather than left to be discovered: pages pick up a
change when Hydra next starts. `try_filters` runs the driver both ways through
the real startup order and checks what `matchMedia('(prefers-color-scheme:
dark)')` actually reports inside a page.

### The bug the screenshots caught, and the two guards that did not

The descriptions under every settings row were dimmed by taking the current text
colour, lightening it, and writing the result into the widget's palette. That is
a snapshot. The labels are built once, so a colour computed under a dark theme
stayed put when the theme went light — **every description went white on
white**, in the dialog whose own colour-scheme control previews live.

78 assertions passed. It was unmissable in the light screenshot, which is the
case for writing the pictures out, and the case for looking at them.

Writing the regression guard was the more useful half, because two versions of
it passed with the bug still in place:

- **Searching by colour role found nothing.** The buggy code dimmed by writing a
  colour, not by setting a role, so a role-based search matched zero labels and
  the check passed on an empty set. A check that can silently match nothing is
  not a check — the labels are named now and the count is asserted.
- **Building the dialog fresh under each scheme also passed**, because a colour
  frozen under the theme that is still current is the right colour. The bug only
  exists across a *change*. So the guard now builds under one scheme and
  switches to the other, which is both the sequence that breaks and the sequence
  a user performs.

Only after both were fixed did it report the failure: a gap of 16 of 255 with
the bug reintroduced, 120 with it fixed. The fix itself is one line —
`setForegroundRole(QPalette::PlaceholderText)`, which the style recomputes on
every palette change.

---

### The legend helped one site and cost the other 2 of 5

A CPU-free afternoon spent re-reading artifacts rather than producing new ones,
and it overturned the top item on the next list.

The starting question was small: today's kisskh runs all report `1 addresses
answered, 5 refused`, so is the payload even carrying the answer? It is.
Request 44 is the manifest, annotated `application/vnd.apple.mpegurl (HLS)`,
and the five refusals are Firebase, `firebaseinstallations`, `accounts.google`
and `firebaseremoteconfig` twice -- Google furniture that was never a stream
candidate. `candidates()` has not changed since that payload was dumped (the
three commits since all move the *note*, not the selection), so today's runs saw
the same thing.

So the round-robin worked exactly as designed: it put a question to
`hls.cdnvideo11.shop` even though five busier Google hosts were also in the
draw, and that question came back right. **"Spend more probe budget where the
evidence is noisy" was on the next list and it is refuted.** The budget found
the manifest and labelled it. The model then failed to pick it five times out of
five.

Which raised the better question, because there are two five-run sets on this
same evidence file and the same model:

| arrangement of the served-type note | dramafren | kisskh |
|---|---|---|
| `-> application/vnd.apple.mpegurl (HLS)` after the url | — | **2 of 5** |
| legend under the table, keyed by request number | **3 of 5** | **0 of 5** |

The legend was adopted on dramafren's evidence. It cost kisskh both its hits.

**And the reason is not that the older format was better.** Both of the old
accepts wrote a clause matching `url.includes('->')` -- the model reading the
annotation *marker* as if it were text in the address, which matches nothing.
Each then wrote a *second* clause, `path.endsWith('.m3u8')`, and that is what
landed. The 2 of 5 was a naive extension check rescuing a misread note.

That explains the whole table. kisskh's manifest is a plain
`…/Ep24.v990_index.m3u8`: an extension check finds it and the note is not
needed. dramafren's is `cf-master.1774687168.txt`: an extension check cannot
find it and the note is the only route. So the two sites reward opposite
behaviour, and the arrangement that got the model to *use the note* also got it
to stop writing the *fallback* -- under the legend, no kisskh run wrote an
`.m3u8` clause at all. They picked images twice, wrote this visit's tokens
twice, and picked an analytics beacon once.

These are not in tension. One script can hold both clauses, and one of the old
runs already did. The next thing to try is asking for both by name -- match the
annotated address, *and* fall back to a manifest extension -- rather than
arranging the note a fourth time and measuring one site.

The durable caution is the one this project keeps re-learning: **a change
measured on one site is a change measured on one site.** The legend is a real
improvement on dramafren and was committed as one. Nobody re-ran the second
site, and the regression it caused sat in the notes for a day as "kisskh is 0 of
5 and that is the open question" -- with the 2 of 5 it used to score recorded
one directory away.

### Asking for both clauses, and the field the prompt promised but did not deliver

The section above ended with a concrete next step: stop rearranging the note and
ask the model for *both* a note-driven match and an extension fallback, since
one script can hold both and one measured run already did. That is now rule 3,
which used to read "do not decide by extension alone" -- fair warning, and read
by the model as *do not use the extension*, with rule 4 pointing it at the notes
straight afterwards. It now asks for two tests in as many words, says to return
whichever matches with the notes first, and gives the measured reason: a
disguised manifest is only findable by the note, a plain `.m3u8` is found by the
extension for free, and a script carrying one test is wrong on half the sites
this has been measured against.

**Writing that rule found a worse bug than the one it fixed.** The draft told
the model to fall back to an extension on a request "fetched once", which meant
checking that the function could actually see that. It could not. The prompt has
described the evidence as `order | type | seen | url` for some time and tells the
model a manifest is fetched once -- and `site_extractor::check` was building the
request objects with `url`, `type` and `order` and nothing else. A script written
exactly as the prompt instructs reads `undefined`, compares it to a number,
matches nothing and returns null.

That is indistinguishable from a model ignoring the rule, and it has been sitting
under every measurement the loop has produced. It is also precisely the trap the
`kind`/`type` split cost once already, recorded in a comment a dozen lines from
where this one lived: two vocabularies, one name, and a model reading it the
obvious way. The rule was described and never wired.

So the runtime object now carries `seen`, counted by the same `shape_of` the
table and the gate use, and the prompt's signature comment advertises it.

**The check for it is a difference, not a refusal.** Asserting that a
`seen === 1` segment script is unusable proves nothing -- the gate refuses
segments by its own rule and would refuse that pick with the field absent,
wrong, or hard-coded. So the same script runs twice differing only in the `seen`
clause, and the two must fail *differently*:

    without:  Rejected: that address is one of 40 near-identical requests
    with:     the script found nothing

Whether any of this moves the rate is unmeasured. It needs ten runs across both
captures on an idle machine, and the honest expectation is that the `seen` fix
matters more than the rule, because the rule was competing with a field that
was not there.

**A dead parameter went with it.** `summarise()` still took the `served` map it
had stopped using when the note moved to a legend, and a test was comparing the
annotated and un-annotated forms for equality -- true, and true because the
argument did nothing. Removing the parameter makes it structural: there is no
longer a way to ask that function to put a served type in a row, so the arrow
that four runs in five once matched on cannot reappear. It was also the build's
only warning, in a project that claims a warning-clean build a few sections up.

### The intermittent suite failure, explained on its third appearance

`test_extloop` failed three times today under `make test`, always on the first
run after a source change, never reproducible afterwards. It looked like a build
problem and was nothing of the sort.

The first two occurrences left nothing behind. `make test` printed the tail line
and five `FAIL` lines and discarded the rest -- adequate for a suite that fails
every time and useless for one that does not, which is the case where the whole
output matters most. The first was invisible twice over: it was read through a
`tail -8` that cut off the line naming the suite, so it was written down as "an
unexplained non-zero exit" when the summary had in fact named it.

Failing suites now write their full output to `test/build-make/failed/<t>.log`. The
third occurrence landed there and answered it in one read.

**The dialog probes when it opens** -- it asks the server what its candidate
addresses serve -- and keeps Send disabled until those answers arrive. Three
sections opened it, waited a flat `spin(200)`, and asserted Send was offered.
That is a bet that six DNS lookups for hosts which do not exist will all fail
inside a fifth of a second. They usually do. When they do not, the section fails
from the top with "Send is offered when there is evidence", which reads like a
broken dialog rather than a harness that did not wait.

And the "first run after a rebuild" pattern, which sent one investigation into
relink behaviour and a `touch`-and-rebuild probe that found nothing, was real but
backwards. That is simply the moment the machine is busiest.

The three sites now use `wait_for`, the helper this same file already carried,
under a comment that already said it: *a fixed wait is an instrument that
invents results.* It was written for `spin(400)`/`spin(600)` after a click, and
the four waits before one were never revisited. Verified with four load
generators running: 34 of 34, twice.

### Measuring the loop on a machine that is never idle

The last measurement wanted an idle machine and said so. That was the wrong ask:
this machine is never idle, and a plan that waits for one is a plan that never
runs. Three of five runs in that measurement never answered at all, which is a
result about the load and not about the model.

The way out is that **most of what gets changed is not the model.** The gate's
rules, the fields handed to the script, the shape of the evidence — deterministic
code, all of it, and fifteen replies the model has already given were sitting in
`evidence/`. Scoring those again costs milliseconds. `make replay` does it, and
only a change to the prompt or the model itself now needs a live run.

**It is validated against verdicts already recorded**, which is the entire
design: a replay that cannot reproduce what the gate said when a reply was
produced is not a cheaper measurement, it is a different one. That validation
earned its keep immediately, twice.

*It caught the corpus being wrong.* The first version compared **rates**, and on
kisskh it reported 2 of 5 against a recorded 2 of 5 — while disagreeing about
*which two*. One reply had been scraped out of a log that prints replies through
`left(1200)`, so it was cut off mid-statement and failed as a syntax error; a
second was accepted here that the shipping gate had refused. Two errors, one
cancelling the other, and the rate agreed. It compares **per reply** now, and a
rate can no longer hide a pair of mistakes.

*It caught what `check()` cannot see.* Two replies disagreed because the dialog
does something offline checking does not: when the model returns a pick, it
**fetches that address** and refuses it if nothing streams back. That is how an
analytics beacon and a service worker were refused, neither caught by any static
rule. The corpus carries that knowledge as `disproved`, and it is matched by
*prefix* — the logs print a pick through `left(140)` and the beacon in question
is 677 characters, so the address copied out of a log matched nothing at all and
failed silently on the first attempt. Same truncation, third victim.

**Proved by breaking it.** Disabling one gate rule — the one that calls an
address fetched as an image page furniture — and re-running:

    FAIL  kisskh-2026-08-03.legend-run4.js: accepted now, refused when produced
    FAIL  kisskh-2026-08-03.legend-run5.js: accepted now, refused when produced

Exactly the two replies that depended on it, named. Restored, and 15 of 15 pass.

For the runs that do need a model, `test/live/measure.sh` runs them at `nice 19`
with idle IO and renices the Ollama server too, since that is the process doing
the work and nicing only the client would move nothing. It records every reply
whole into the corpus, so the measurement *after* it is free. Runs are
sequential: two 14B generations at once on CPU is not twice the throughput, it
is two runs that both time out.

**What this does not do**, said plainly because the temptation is to forget it:
the corpus is fixed replies, so it cannot measure a prompt change. Today's rule
about writing two clauses is exactly such a change and remains unmeasured. What
the corpus does is make sure that when those runs finally happen, they are the
only ones that ever have to.

### The two-clause rule, measured — and the regression it caused

Both captures, five runs each, through `test/live/measure.sh` at `nice 19` on a
machine sitting at load 85. The answer is **no**, and one of the runs found
something worse than a null result.

| | before | after |
|---|---|---|
| dramafren | 3 of 5 | 2 of 5 (one run never answered) |
| kisskh | 0 of 5 | 0 of 5 |

**The `seen` field is being used**, which it never was: five of the nine replies
that came back test it, against zero in every earlier run. That part worked. The
rest did not — one reply in nine carries both clauses, so the rule asking for two
tests mostly produced one.

**And a failure mode appeared that no earlier run had ever produced.** Four of
the nine answered runs picked *the page's own address*, a refusal that occurs
nowhere in the fifteen replies recorded before today. The cause is mine and it is
plainly readable in what they wrote:

    if (request.seen === 1 && request.type === 'other') { ... }

Row 0 of the table is the page itself: fetched once, type `other`. So the first
thing that filter matches is the document, every time. The rule said `seen === 1`
was how to find a manifest; it is equally true of the page, and `find()` returns
the page first. The prompt now says so — `seen` narrows a search and never
decides one, and the page's own address is to be skipped — but that is a fix
made after the measurement, not measured.

Nothing was accepted wrongly: the gate refused all four. The cost was four
attempts spent on an answer the prompt had aimed at the wrong row.

**This is what the corpus is for.** All nine replies are in `evidence/replies`
with the verdict each got; the corpus is 24 now and `make replay` reproduces
every one. The next change to the gate is measured for free, and the next change
to the prompt starts from a baseline that is written down rather than remembered.

The honest summary of the day's prompt work: the field that was described but
never wired is now wired and used, the rule that was supposed to exploit it
misfires, and the evidence for both is on disk instead of in a log that will be
gone tomorrow.

### The menus, arranged the way people already expect

Tools held twenty items in the order they were built — Downloads, an AI parser,
a video capture, Settings, KeePassXC, then two importers with a sync toggle
wedged between them and an "open in another app" in the middle of the pair.
There was no Edit menu at all, so Undo lived at the bottom of Tools and rename
and delete lived nowhere but a right-click. Every addition had a reason. The
order was nobody's decision.

Rearranged to the conventions desktop software settled on between about 1995 and
2010: **File, Edit, View, Go, Tools, Help**; Quit last in File, About last in
Help, Settings last in Tools under a separator; related items in groups of a few.

- **File** gained New Tab, New Folder, Open Location and an **Import** submenu
  holding both importers, which the flat list had made impossible — they were
  four items apart.
- **Edit** is new: Undo at the top where three decades of software has put it,
  then Copy Address, Duplicate, Rename, Delete, Select All, Find in Tree. Rename
  and Delete had no menu at all before this.
- **Tools** went from twenty flat items to ten, with **Media**, **Passwords** and
  **Follow Other Browsers** submenus. The two sync toggles are together for the
  first time; they are the same feature twice and were four items apart, so
  turning both on meant finding the second by reading.
- The **context menu** put Delete *below* Properties, which is the irreversible
  item where every file manager of the period puts the harmless one. Delete is
  now above it and Properties is last, on its own.

`try_menus` asserts all of it — the bar's order, the three strong tails, that no
menu exceeds twelve items, that both importers sit together, and that Delete
precedes Properties in the context menu.

**And then looking at it found what none of that could.** The driver now grabs a
picture of each menu, and two mnemonic collisions were sitting in plain sight:
Edit had `&Undo Reorganize` beside `D&uplicate`, both claiming Alt+U, and Tools
had `&Cookie Banners We Missed` beside `Site &Controls`, both claiming Alt+C. Qt
matches mnemonics case-insensitively and cycles between collisions rather than
complaining, so nothing looks wrong — the key simply stops doing what the
underline says it does. They are `Dup&licate` and `S&ite Controls…` now, and the
check that would have caught them exists: every menu's Alt keys must be distinct.
Verified by putting one collision back, which fails with `Edit: &Undo Reorganize
vs D&uplicate (Alt+U)`. 28 checks.

One thing worth writing down about the pictures. The first version grabbed the
root window, which on a real desktop means capturing whatever else the person
has open — their terminals, their mail — into a file in `/tmp`, in order to look
at a menu four hundred pixels wide. It grabs the popup widget itself now. That
is both the better picture and the only thing a menu test has any business
seeing. The rules are written as
assertions rather than as a comment because this is a list that rots by
appending, and it will stop being anyone's decision again the moment it is not
checked.

### Proper toolbar icons, and four layers of nothing-happened

Media, Shield and Key were text on a toolbar of icons, under a comment arguing
that inventing a glyph for one affordance would make it the odd one out. The
argument was sound and the conclusion was avoidable: the icons do not have to be
invented. `dialog-password`, `security-high` and `applications-multimedia` are
freedesktop names every theme ships, so the key is the *desktop's* key, drawn to
match the arrows beside it. The navigation icons moved to theme names too, so
the whole bar is the desktop's rather than half Qt's.

Then it did not work, four times, each failure looking exactly like the last one:
a toolbar with no icons, and nothing anywhere saying why.

**One: Qt does not know the icon theme here.** It finds one through a
platform-theme plugin, and ships Plasma's and GTK's. This desktop is Trinity,
reports `XDG_CURRENT_DESKTOP=TDE`, loads neither, and `QIcon::fromTheme` returns
null for everything. That is the same shape as the colour-scheme problem two
sections up and has the same answer: read what the desktop wrote down. It is in
`~/.config/gtk-3.0/settings.ini`, plainly, as `gtk-icon-theme-name=breeze-dark`.

**Two: the parser walked past it.** A kdeglobals needs its sections tracked --
a bare `Theme=` outside `[Icons]` is the colour scheme, and reading it names a
palette as an icon set. GTK's file has no `[Icons]`; it has `[Settings]`, and
the first parser treated any header as leaving the section it wanted. So it
skipped the whole file and reported the fallback. Sections are now tracked only
for the files that have them.

**Three: `hicolor` is not empty.** The guard was "if Qt already found a theme,
it knows better" -- and Qt reports `hicolor`, the freedesktop fallback, which
carries almost no application icons. Not empty, so the guard returned happily
having chosen a theme containing none of the icons about to be asked for. It now
asks whether the current theme can actually draw one, rather than whether it has
a name.

**Four: the screenshots were of the harness.** With all of that fixed the app
logged `icon theme: breeze-dark` and the picture still showed the old toolbar,
because the icon theme is set once in `main()` and `try_menus` has a `main()` of
its own. Every screenshot in this project's last two sections was of a window
that had skipped part of startup. The driver now does what `main` does -- the
palette as well as the icons -- and a light window full of the desktop's dark
icons was the tell.

**And the application had no icon at all, for the same reason as the
generator.** `main.cpp` asks for `:/icon/hydra-16.png`; the resource declared
`prefix="/icons"`. The rename to singular directory names changed the C++ path
and left the prefix, so every `addFile` found nothing, `setWindowIcon` was
handed an empty QIcon, and the window carried no icon anywhere -- taskbar,
switcher, decoration. `QIcon::addFile` reports a missing resource by returning
quietly, which is why nothing ever said so.

Proved rather than argued: with the old prefix restored and rebuilt, `xprop`
reports `_NET_WM_ICON: not found` on the running window; with `/icon`, a 16x16
icon with data. That is the third thing that one rename broke silently, after
`objsets.py` and this section's own stale prose. **A rename is a change to
every string that named the thing, and only the compiler checks some of
them.**

The prefix is `/icon` rather than `:/icons` for a second reason: `:/icons` is
Qt's conventional location for an icon *theme* inside resources, and these are
the app's own marks. Sharing that path with the theme search path would be
asking for the confusion the section below is about.

**Five, on the machine these trees moved to: Qt could not see the disk.**
The toolbar was drawing words again, and startup said `no icon theme found`.
Everything above was still correct and none of it applied. `XDG_DATA_DIRS`
named `/opt/trinity/share:/usr/local/share:/usr/share`, `QStandardPaths`
resolved that to three real icon directories, Trinity's own kdeglobals said
`Theme=crystalsvg`, and `/opt/trinity/share/icons/crystalsvg` held
`go-previous.png` at four sizes. Every part was present and correct.

`QIcon::themeSearchPaths()` returned `:/icons` and nothing else.

Qt6 populates that list from a platform-theme plugin and ships only Plasma's
and GTK's, so on a desktop that loads neither it holds just the resource path
compiled into the binary. Every system directory was invisible -- which meant
the *existence* check in `detect_icon_theme`, the one added because a
configured-but-absent theme fails silently, was itself asking about
directories it could not look in. It rejected a theme that was installed, and
reported the same "nothing found" as a machine with no themes at all. The
paths are seeded from `QStandardPaths` before anything is looked for now, and
startup says `icon theme: crystalsvg`.

Note what changed underneath: the sections above name
`~/.config/gtk-3.0/settings.ini` and `breeze-dark`, and on this machine that
file does not exist and breeze is not installed. Neither statement was wrong
when it was written -- they describe the machine these trees were moved from.
**A finding about a machine is not a finding about the software**, and this
document had no way to say which it was holding.

And a sixth, found while fixing the fifth and not the cause of anything here:
**installed is not usable, one theme further on.** `Adwaita` is present on
almost every machine, ships an `index.theme`, a cursor set and a symbolic
directory, and carries none of the ordinary action icons a toolbar asks for.
It sits ahead of `oxygen` in the fallback list, so on a machine with no
desktop configuration the old code would have chosen it and drawn nothing --
`hicolor`'s trap from *Three*, one name over, in the half of the code that
picks rather than the half that inherits. Candidates are now loaded and asked
for a real icon, first that answers wins. Measured here: of the four installed
candidates, `Adwaita` and `hicolor` draw nothing.

The general lesson from *Four*. **A driver is only a picture of the
application to the extent that it starts the same way**, and every one of these
drivers builds its own `main_window` by hand.

One thing genuinely needed deciding rather than fixing: `breeze` and
`breeze-dark` are the same icons drawn for opposite backgrounds, so following
the desktop's setting is wrong for anyone who chose Light on a dark desktop --
pale icons on a pale toolbar. The variant is matched to the scheme *the window
will paint in*, not to the desktop's.

The parsing is covered offline in `test_theme`, all five cases, because three
separate bugs lived in it and every one ended in the same indistinguishable
place.

### Packaging: a .deb and an .apk that actually install

Both exist now and both were checked by installing rather than by building.

**`make deb`.** Stages through the same `install-icons.sh` that `make install`
uses, so a package and a local install cannot disagree about where things go,
and hands the result to `dpkg-deb`.

The dependency list is *computed*. `dpkg-shlibdeps` reads the binary, resolves
each library it links to the Debian package owning it, and returns a versioned
`Depends:` — nineteen entries here, from `libqt6webenginecore6` down to
`libtorrent-rasterbar2.0t64`. A hand-written list would be wrong the first time
an optional dependency was switched on, since libsecret, libsodium, liblz4 and
libtorrent are all found at configure time or not at all. The right list is a
property of the binary in front of us.

Three things went wrong, and the first is the one worth keeping:

- **The dependency list came back empty**, and said so. `dpkg-shlibdeps` has to
  run from a directory holding a `debian/control`, so it is invoked after a
  `cd` — at which point the *relative* staging path it was given stopped
  resolving. The result would have been a package that installs cleanly and
  then does not start, which is the worst available outcome. It only surfaced
  because the script warns rather than shrugging; paths are absolute now.
- **Every directory in the package was mode 775.** `--root-owner-group` sets
  ownership and not modes, so the build host's umask decided, and a package
  shipping group-writable system directories widens permissions on the machine
  that installs it.
- The icon cache and desktop database are deliberately *not* refreshed while
  staging. That would touch the build host; on the installing machine dpkg's
  own triggers do both after unpacking, which is the only correct moment.

Verified by extracting the package, confirming every declared dependency is
satisfied, and running the stripped binary out of the extracted tree. It starts.

**`make apk`.** The Android build already worked; what it produced was not a
package anyone could ship. `aapt2 dump badging` reported `versionCode=''` and
`versionName=''`, because the manifest carried neither attribute and
androiddeployqt only fills placeholders it can find. **An apk with no
versionCode is one that no later build can ever be an upgrade of** — Android
compares that integer and reads absent as zero. Both now come from
`project(VERSION)` through `QT_ANDROID_VERSION_NAME`/`_CODE`, and the manifest
keeps the literal placeholders rather than hand-written values that would drift.
`ANDROID_VERSION_CODE` is a separate cache variable because a dotted version
cannot be compared as an integer, so it cannot be derived.

The apk is also copied out of `android-build/android-build/build/outputs/apk/debug/`
under a name that says what it is.

Verified with `aapt2` (`versionCode='1' versionName='0.1'`, `native-code:
'arm64-v8a'`, label Hydra) and `apksigner` (debug key, signature valid).

**And that `versionCode='1'` was the defect, not the fix.** It was read as a
pass for a long time because it is a number where an empty string used to be.
`tool/android.mk` derives the real code from VERSION -- major*10000 +
minor*100 + patch, so 0.1 is 100 -- and prints it in the preflight, which said
`versionCode 100` while every apk built carried 1. Nothing passed the make
variable to qmake, and `hydra.pro` assigned `ANDROID_VERSION_CODE = 1`
unconditionally, so the .pro won.

The fragment's own comment describes the consequence exactly: "a hardcoded
number allows exactly one upload and blocks every update after it". It was
describing us.

Found by building and reading the artifact rather than the preflight, which is
the whole point of reading the artifact: **the two disagreed, and only one of
them was going to be installed.** The Makefile passes the derived value on the
qmake command line now and the .pro keeps 1 only as a fallback for a bare
`qmake hydra.pro`. Rebuilt and re-read: `versionCode='100'`.

A second XML comment refused by a parser on the way, for the same reason as
`icon/hydra.qrc` an hour earlier: a double hyphen is illegal inside an XML
comment, and androiddeployqt reports it as "Expected '>', but got ' '" with a
column and no reason. Both comments now say so in themselves.

**The application id was Qt's example namespace and now is not.** It was
`org.qtproject.example.hydra`, carried from the template — it installs and runs,
so it never blocked evaluation, but it collides with every other Qt example app,
and an application id cannot be changed later without every installation
becoming a separate app that cannot upgrade the first. That made it cheap now
and expensive after anyone installs. It is `se.vibes.hydra`.

The rename is nine places and **three of them are strings**:
`android_view.cpp`, `android_downloads.cpp` and `android_intents.cpp` each hold
a JNI class name like `"org/qtproject/example/hydra/HydraWebView"`. Those are
looked up by name at runtime, so a missed one compiles, links, packages and
installs, and then the WebView simply does not come up on a device. The Java
files moved with `git mv`, their `package` lines changed, and the manifest with
them; the manifest's other class references are Qt's own bindings and stay.

One consequence worth knowing rather than discovering: **the app's data
directory moves with the id**, so a device carrying the old build keeps its
files under the old path and the new build starts empty. At 0.1, with no users,
that is the whole cost.

### Trying the package found two things building it could not

The .deb built, declared the right dependencies and installed cleanly into a
temporary root. Running the binary out of it found two defects that no amount of
inspecting the package would have shown, because both are about what happens
when the program is *started the way a package starts it*.

**A url handed in from outside was silently discarded.** The desktop entry says
`Exec=hydra %U` and registers `text/html`, `x-scheme-handler/http` and
`x-scheme-handler/https`, so once installed as the default browser every clicked
link arrives as `argv[1]`. That argument was only ever read as a *tree path*. A
url names no file, so the fallback ran, the window came up with an empty tree,
and the link was gone. From the outside that is a browser that cannot open a
link — installed, running, and useless for the one thing the package claims it
does. `main_window::open_url` exists now and `main()` recognises `http` and
`https`; a path is still a path, and `file:` is deliberately not treated as a
url, since `hydra ./tree.txt` has always meant the tree.

**And the program wrote its state into whatever directory it was launched
from.** With no `sample-tree.txt` in the working directory and none beside the
binary — which is the situation for `/usr/bin/hydra`, and only for an installed
copy — the path stayed *relative*. From a desktop entry that is the user's home;
from a file manager, whichever folder was open. Android had already needed the
answer to this and written down the reasoning; the desktop had the same hole for
the same reason and did not.

It was found the blunt way: running the packaged binary from the source tree
appended two tabs to the repository's own `sample-tree.txt`, a tracked file.
That is as clear a demonstration as the bug will ever give.

Both verified by running the *packaged* binary from an empty directory: nothing
is written there, `~/.local/share/Hydra/` holds `tree.txt` and `state/`, and the
tree contains the address that was passed in, carrying the page title the engine
resolved — which is also the proof that WebEngine renders from the package.

Worth keeping about the method: **the build binary could not have found either
bug.** The build copies `sample-tree.txt` next to it — `hydra.pro`'s
`QMAKE_POST_LINK` — so the beside-the-binary branch
always matches and the installed path is never taken. The first attempt at
testing this "passed" for exactly that reason. Verifying packaging means running
what was packaged, from somewhere the source tree is not.

### A clean target that could have eaten a home directory

Re-reading the global guidelines turned up a rule this project was not
following, and it is one of the ones stated with the failure that produced it:
a `clean` target removes the files it names and lists them, because it is the
one target everybody runs without reading.

This one was:

    rm -rf $(BUILD_DIR) $(TESTS_DIR) $(ANDROID_BUILD_DIR)

Build trees are created by the build and disposable by construction, which is
the one shape where clearing a directory wholesale is allowed at all. But all
three are overridable variables with relative defaults and nothing checked
them, so `make clean BUILD_DIR=$HOME` was a way to lose a home directory, and a
mistyped override was a way to do it by accident.

Each path is now tested before it is removed and named as it goes: non-empty,
relative, no `..`, not `.` itself. Refusals are printed rather than skipped
silently, since a clean that quietly did not clean is its own problem. Verified
by trying to break it -- `make clean BUILD_DIR=/ TESTS_DIR=..` refuses both and
touches nothing.

The same reasoning reached the new `make-deb.sh`, which clears its staging tree.
That path is *absolute* by the time it is used, because `dpkg-shlibdeps` has to
run from elsewhere, so the relative test cannot apply and something had to stand
in for it: the script refuses `/`, `$HOME` and anything containing `..`, and
refuses any existing directory that is not empty and does not already look like
a staging tree. Verified the same way, by pointing it at `src/` and watching it
decline.

### situ, evaluated against the parsers rather than in the abstract

The standing directive is to test `situ` for viability against the project in
front of you. So two schemas were written from `session_import.cpp` and
compiled: `snss.situ` for Chromium's session framing, and `pickle.situ` for
`base::Pickle` as it appears inside those records.

**The result that decides it.** `pickle_string16` is a UTF-16 string whose
length counts *characters*, so the byte run is `length * 2` with `length` a
`u32`. The capability map came back with

    nav_entry.title.data   size=Bounded(0, 8589934590)

which is 2 x 4294967295 -- the overflow this project's `payload_reader` guards
by hand, and guards with a *division* precisely so the multiplication cannot
wrap:

    if (!m_ok || n < 0 || (m_end - m_p) / 2 < n) { m_ok = false; return {}; }

That line took care to write and is invisible on inspection. situ derived the
same fact from the schema and printed it. **That is materially better rather
than tidier**, which is the bar the directive sets.

**What fits.** The SNSS header and its length-prefixed record stream
(`records[] while (...)`, structurally the same as netlink's TLV walk), and the
whole Pickle layer including four-byte padding via
`align_up(length, 4) - length`.

**What does not, and it is the largest piece.** The LZ4 block decompressor is
not a layout at all: it reads a token, a literal run, a back-reference and a
match length, and writes into a *different* buffer with overlapping copies.
No schema describes that, and situ generating no allocation is the reason.
It stays hand-written -- which is fine, since it is the one part already
verified byte-identical against python's `lz4.block` over 5,791,500 bytes.
The replay itself -- command dispatch, last-writer-wins, ordering -- is
application logic and equally out of scope.

**What adoption would cost**, and why this is a recommendation rather than a
change: `situc` is Python 3.11 at build time, and this build has no code
generation beyond Qt's `moc`. Adding a generator means adding a build
dependency to the `.deb` as well. The alternative is committing the generated
`.h`/`.c` and regenerating by hand, which wants a blessed workflow that situ
does not currently document.

So: **worth adopting for the SNSS and Pickle readers, on the committed-
generated-files model, and not for anything else here.** Not done in this pass;
it is a bounded piece of work with a real dependency question attached, and the
decision is the user's. Suggestions for situ itself went to that project.

The measurement stops short of one thing, said plainly: the generated code was
produced and its map read, but it was not compiled by a C compiler, linked into
hydra, or run against a real session file. The next step, if this is taken up,
is to run both readers over the same captured file and diff them -- which is a
harness shape this project already has.

### fmake, run on `src/` rather than reasoned about

The earlier assessment of fmake from this project was written from its
documentation. Running it changed two conclusions, in both directions.

**More was inferred than expected, with no configuration.** 45 moc invocations,
one per `Q_OBJECT` header, matching what `AUTOMOC` does -- including
`network_fetcher.cpp`, a `Q_OBJECT` in a *source* file, which needs the
include-the-output form and was not missed. Qt's whole include and define set.
And a link set derived from symbols that is **smaller** than the one
`CMakeLists.txt` names, with every declined library explained by the header that
suggested it.

**It cannot build this tree**, and the reason is ours rather than fmake's.
`src/` holds four `android_*.cpp` that CMake adds only inside `if(ANDROID)`.
They carry no self-guard, because the build system is what excludes them, so
fmake schedules all four and stops at

    android_view.cpp:12:10: fatal error: QJniEnvironment: No such file or directory

Worth knowing independently of fmake: **those four files depend on a build
system to exclude them and say nothing about it themselves.** Any tool that
reads the tree rather than the build file -- an indexer, a language server, a
static analyser, a new contributor -- meets the same wall.

**And the optional-dependency finding is the sharp one.** `credential_store.cpp`
is `#ifdef HYDRA_HAVE_SECRET` throughout; `theme.cpp` guards its portal query
with `#ifdef HYDRA_HAVE_DBUS`. Both macros are defined by CMake *after*
`pkg_check_modules` finds the library. fmake compiled both files with the macros
undefined, so the bodies vanished, so no symbol needed the libraries, so fmake
correctly declined to link them:

    (not linked)  libsecret/secret.h at credential_store.cpp:9 suggests libsecret-1, but no symbol needs it

Every step is right and the result is a browser with no keyring and no
colour-scheme detection, built without a warning. The chain is worth
understanding because it is not a bug in either tool: fmake reasons from symbols
in objects, and that evidence is downstream of a decision it cannot see.

**Verdict, unchanged in substance and now on evidence:** fmake reads this tree
impressively and cannot build it, and the two blockers are a filename
convention and a way to say "found means defined". Section 17's claim that
fmake would make our static library unnecessary is **still unverified** -- the
build fails before reaching a link, and the 19m17s-to-6m57s figure it quotes
was this project solving that with an archive, not fmake solving it.

Suggestions went to `fmake/suggestions/hydra.md`, which replaced the
documentation-based version wholesale.

### The four files that needed a build system to be correct

Running fmake found them and the fix is worth having whether fmake is ever
adopted. `android_view.cpp`, `android_downloads.cpp`, `android_intents.cpp` and
`android_dialogs.cpp` were added to the build only inside `if(ANDROID)` and said
nothing about it themselves, so anything reading the *tree* rather than the
build file -- an indexer, a language server, a static analyser -- reached
`<QJniEnvironment>` on a desktop and stopped.

The callers were already doing this correctly: `main.cpp`, `main_window.cpp` and
`player_launcher.cpp` all wrap their `#include "android_*.h"` in
`#ifdef Q_OS_ANDROID`. Only the implementations were relying on CMake.

**The guard has a trap in it, and getting it wrong fails in the other
direction.** `Q_OS_ANDROID` is not the compiler's -- the compiler defines
`__ANDROID__`, and `Q_OS_ANDROID` comes from Qt's `qsystemdetection.h`, reached
through `qglobal.h`. A file that opens with `#ifdef Q_OS_ANDROID` before any Qt
header has been included therefore tests false *everywhere*, including on
Android, where it would empty exactly the four files the Android build needs.
So each begins `#include <QtGlobal>` and the include is load-bearing.

**And a passing build proves nothing here**, which is the part worth
remembering. The callers guard their calls too, so an emptied
`android_view.cpp` compiles *and links* and produces an apk -- one with no
WebView in it. `make apk` succeeding was not evidence. What settled it was
looking in the built library:

    android_view         123 defined symbols
    android_downloads     39 defined symbols
    android_intents        2 defined symbols

Desktop: all four now compile to nothing, checked with `-fsyntax-only`. fmake
gets past them and produces a complete plan for the tree, and noticed on its own
that `moc_android_downloads.cpp` now "defines nothing reachable".

### Chromium's mirror, measured rather than guessed — and a sweep that was lying

Item 4 asked whether following Chromium's session more closely was "worth the
reads", which is a question nobody had answered. On a real 2.2 MB session file
holding 131 tabs:

    stat only        0.006 ms per poll
    read + replay    1.3   ms per poll

**The reads were never the constraint.** A poll that finds nothing changed costs
six microseconds; the whole of `replay_snss` over two megabytes costs less than
a frame. Chromium now polls at 5 s rather than 15 s -- not 2.5 s, which would
run in lockstep with the writer for no perceptible freshness and is the interval
most likely to catch a flush half-written. A partial read is survivable rather
than impossible (`replay_snss` treats a truncated tail as normal, so it yields a
prefix and the next poll corrects it), and that risk rises with frequency and is
unmeasured. It is the reason for 5 s and not something faster.

**Then the sweep found what the measurement was standing on.** Running every
driver -- which had not been done since the menus were rearranged -- turned up
regressions in three of them, all mine, all from renaming things:

- `try_import` looked for `Import Tabs from &Firefox`, now `Tabs from &Firefox`
  under **File > Import**; and for `&Duplicate` and `&Properties…`, now
  `Dup&licate` and `P&roperties…` after the Alt-key collisions were fixed.
- `try_handoff` looked for `Open This Page in &Another App`, now
  `Open in &Another App` in File.

Every one of those was introduced by a commit that reported itself green,
because after restructuring the menus I ran **only the driver I had just
written**. `make test` covers the offline suites and says nothing about menu
labels; `try_menus` asserts the structure I designed, not the labels other
drivers depend on. The rule that would have caught it is dull: *a change to
anything a driver asserts on means running the drivers, not the one you are
thinking about.*

**And the sweep itself was reporting nonsense.** It globbed `"$BIN"/try_*`,
which also matches CMake's `try_*_autogen/` **directories** -- and `ls` on a
directory lists what is inside, so it tried to run `timestamp`,
`moc_predefs.h` and `mocs_compilation.cpp` as drivers and reported **165
failures over 13 real results**. It globs executable regular files now. That is
the second time this script's own bookkeeping has been the thing that lied, the
first being report-only drivers counted as failures.

It also now names the two drivers that were never going to run here --
`try_extract` takes a url argument, `try_watch` needs a live network -- rather
than counting them as failures. 14 passed, 11 report-only, 0 failed.

**The sweep runs offscreen now**, which it should have from the start. Twenty-
seven drivers putting real windows on a real screen takes over somebody's
desktop for minutes, and it was doing exactly that while they were working.
Every driver passes under `QT_QPA_PLATFORM=offscreen`, and `try_menus` scores
*better* there -- 28 of 28 against 25 of 26 -- because on a live desktop it
competes with whatever else is mapped, which means the display was not merely
rude but a source of false failures.

**What offscreen does not reproduce is appearance.** With no platform theme the
icon-theme search paths differ, so the toolbar renders with Qt's built-in icons
rather than the desktop's Breeze. Structure, ordering, mnemonics and behaviour
are faithful; colours and icons are not. `SWEEP_ONSCREEN=1` exists for when
appearance is the question -- and using it means taking over a screen, so it is
worth asking first.

### One click for "something got through here"

Asked for as an annoyed/happy pair, built as the annoyed half only, because the
two halves are not worth the same and pretending otherwise would have shipped a
smiley nobody presses.

**Annoyed is strong because it removes the diagnosis step.** The three teaching
tools this project already has — the element picker, filter evolution, the
consent-rule editor — all require knowing *what* went wrong before you can
reach them. The hard part of writing a filter rule is not the rule; it is being
back in the moment where the thing happened with the traffic still in front of
you. A toolbar button costs one click at exactly that moment.

**It captures nothing new**, which is why it is a small class and not a
subsystem. `filter_signals` is already accumulating, per site, the ad-shaped
requests that got through and the whole corpus a candidate rule is simulated
against. A report is a *marker* on evidence that exists.

**On the toolbar rather than in a menu**, and that is the design rather than a
placement preference. Two clicks and a menu somebody has to learn is a button
nobody presses while annoyed, which is the only time it is worth pressing.

**The report is filed before the dialog opens.** Somebody who presses it and
then closes the window has still said something, and losing that because they
did not pick one of three tools would make this a worse version of the tools it
feeds. `try_annoyed` exists mostly to hold that property: it dismisses the
dialog and checks the count went up anyway.

**And the dialog shows the evidence rather than thanking you.** A button whose
click produces nothing visible teaches people it is theatre — the same defect as
a permission for a capability that does not exist, which this project removed
two sections ago. So the suspects are on screen, the counts are stated, and
"Propose Filter Rules" is *disabled* when nothing ad-shaped was seen, with the
tooltip saying why. An empty list says so in words: nothing on the network
looked ad-shaped, so this is likely cosmetic, a consent banner, or the site
itself — which is a more useful sentence than a blank box.

**Stored beside the policy**, as `annoyances.ini`, because a record of what
somebody found annoying is a record of where they have been. It belongs where
they can read and clear it, not in a store they cannot see; `clear_host` and
`clear_all` exist for that and are tested.

**Running it against a real site found a bug that every check had passed
over.** Pointed at a live news front page, the report came back
`0 requests seen, 0 ad-shaped` while all fourteen checks reported success. The
cause: `filter_signals::count_for` counts *suspects*, not everything observed —
and its declaration sat directly under the comment describing `observed_for`,
so the next caller to arrive read it as that list's count. The dialog had been
saying "N requests seen, N of them ad-shaped" on every page, and on a page with
no ad-shaped traffic both numbers are zero, which is indistinguishable from a
report that captured nothing.

Fixed to `observed_for(host).size()`, and the declaration moved up beside the
list it actually counts. The guard that would have caught it is now in the
driver — *any page that loaded at all made at least one request* — and it was
proved by putting the bug back: `0 seen, 0 ad-shaped`, one failure. With the
fix, the same page reports one.

**And then it was demonstrated, on a site chosen for being full of ads.**
kisskh, the second capture site, reports `59 requests seen, 5 ad-shaped`, and
the five are real: a Cloudflare beacon, three Google Analytics `collect` calls
and a `google.se/ads/ga-audiences` tag. So the dialog shows a populated list and
*Propose Filter Rules* is enabled rather than greyed out with a reason.

Two things that run showed, neither of them a defect:

- **All five are analytics and tracking, not display advertising.** What is
  visibly annoying on that site — overlays, popups, creatives — is not in the
  list, because `looks_ad_shaped` sees only network requests that are
  third-party *and* match a URL shape. That is the division the funnel was
  designed around rather than a gap in it, and it is evidence that offering all
  three tools was right: on this site the useful one is **Zap an Element**, not
  the rule proposer.
- **Three of the five are the same endpoint** with different query strings.
  Harmless for proposing a rule, since the simulation runs against the whole
  corpus, but as three lines in a dialog somebody is reading it is noise. Now
  collapsed — see below, including the version of it that did not work.

The earlier run against a news front page returned zero suspects, and that is a
legitimate answer rather than a failure — a front page loaded once without
consent accepted is mostly first-party. Whether `looks_ad_shaped` is well tuned
is a question older than this feature and was left alone.

One detail that decided a design choice: suspects are stored as a `QStringList`
and handed to `QSettings` as one, rather than joined into a string. Real
analytics addresses contain commas — `tag_exp=1~2~3&list=a,b,c` — so a joined
string cannot be split back. The offline suite files a report with two such
addresses and checks they survive the round trip.

**The happy half, built as the thing it is actually good for.** Not a smiley:
as a satisfaction signal it would be sparse and skewed, since people do not
click when things work. What it is worth having for is **confirming that a
newly applied rule did not break the page**, because over-blocking is *silent* —
a rule that kills a player or a login form produces no error, no console
message, and nothing in the request log. A page that is subtly broken looks
exactly like a page.

So it appears only when there is something to confirm. `open_filter_evolution`
records what the list held before the dialog and diffs it after, which names the
rules that would have to be removed to undo the change — the dialog does not
have to report anything, because a rule is its text and the difference of two
sets of texts is exactly the undo set. An empty diff raises no question at all:
an "is it still working?" after a no-op is the kind of prompt that teaches
people to ignore prompts.

**Answering "it broke" removes them and reloads**, which is the only reason the
question is worth asking. `try_confirm` holds that rather than the dialog: it
seeds three rules on disk, says two of them broke the page, and checks those two
are gone from the file the browser actually filters with while the third is
untouched. Proved by breaking it — with the removal disabled, the two file
checks fail and nothing else does.

The reload is not decoration either: a cosmetic rule applies at page load, so
the page in front of somebody who has just said it is broken stays broken until
it is fetched again.

Dismissing the box counts as answered. It means "stop asking about this", not
"ask me again" — the alternative is a prompt that follows somebody around, which
is the behaviour that makes people stop reading prompts in the first place.

### Collapsing the suspect list, and the first answer that collapsed nothing

The dialog now shows one row per *endpoint*, with `\u00d7N` when a row stands for
more than one address and a tooltip saying so. The report keeps every address —
collapsing is for reading, and the full list is the corpus a proposed rule gets
simulated against.

**The obvious implementation does not work, and the measurement is why.**
`site_extractor::shape_of` is this project's shared answer to "same shape", so
grouping by it was the first version and reusing it was the right instinct: one
definition of sameness rather than two. It collapsed nothing. `shape_of` drops
query *values* while keeping their *keys*, because for the extractor two
addresses with different keys are different questions — and kisskh's three calls
to one analytics endpoint carry **41, 42 and 44 different keys**. Three shapes,
three rows, no improvement, in exactly the case the change existed to fix.

So the query is dropped entirely and `shape_of` is asked about the rest. That
keeps the hard part where it already lives: digit runs and long mixed-case path
tokens still fold, so a beacon whose payload is in the *path* —
`beacon.min.js/v4513226cdae…` — collapses with the next one, which dropping the
query alone would not have managed.

Two notions of "same address" now exist on purpose, which is worth naming rather
than leaving to be discovered: **shape_of is for deciding what to fetch, and is
right to care about query keys; this is for deciding what to show, and is right
not to.** Coarser, and only ever applied to display.

Checked against the captured addresses rather than invented ones: five real
suspects read as three endpoints, the analytics row standing for its three
calls, and two Cloudflare beacons differing only by a path token folding into
one. Had the test been written with plausible-looking URLs instead of the ones
the site actually sent, the first implementation would have passed it — the
three analytics calls look identical until you count their query keys.

### The capture command that froze the desktop

Symptom: the whole session unresponsive -- no window manager, no keyboard --
with a crosshair pointer that still moved, and every command this project was
running against `:0` failing to reach the server.

Cause: **ImageMagick's `import` grabs the X pointer.** It was used once, from a
one-off command, to capture a Hydra window by id. It refused that id, fell back
to interactive window selection, and *held the grab* while waiting for a click
that was never going to come. An X pointer grab blocks every other client, so
the desktop stops and so does anything that might have cleared it.

The repository never had this problem and still does not: `test/live/shoot.sh`
uses `xwd -id`, which reads a window's contents and grabs nothing, and
`try_menus` uses `QWidget::grab()`, which renders in-process and never touches
the server. The hazard was entirely in an ad-hoc command typed outside the
tree, which is exactly the kind that leaves no trace to learn from -- hence
this note, and the warning now at the top of `shoot.sh` where somebody
reaching for a capture tool will see it.

The general rule, worth more than the specific one: **a tool that can grab the
pointer has no business running against a display somebody is using.** Offscreen
rendering is the first choice, `xwd` the second, and `import` not at all.

### A restriction that had already been lifted, and nobody told the list

`project.md` said a mirrored tab "cannot be *opened* in place; it has to be
dragged into the tree first, which is defensible and has never been put to
anyone". The code says otherwise, and had for a while: `on_tree_activated`
calls `open_node` for whatever was activated, and `open_node` refuses only a
node with no address. There is no mirror guard anywhere in that path, and the
comment beside `replace_mirror` states plainly that a mirrored tab can be
opened like any other.

**Flagged rather than resolved**, because a document that outranks the code and
disagrees with it has two readings and only one of them is "the note is stale".
The other is that the restriction was intended and the guard was never written,
which would make this a missing check rather than an obsolete sentence. Asked,
and answered: the note is stale.

What makes it stale is a fix from earlier in the same session. The reason to
forbid opening a mirrored tab was that a poll replaces the entire mirror folder,
so a live view could be left pointing at a node that had been deleted -- which
was not hypothetical, since that was the leak where deleting a node with a live
view also stopped the four-view cap working for the rest of the session. Once
`replace_mirror` began announcing each folder it was about to drop, and the
shell began closing the views inside it, the ground the restriction stood on
was gone. The comment in the model was updated at the time. The next-list entry
was not.

`try_import` holds it now, and holds the dangerous half rather than the easy
one: open a mirrored tab, confirm it gives a live view, then rebuild that same
mirror *empty* -- the shape a refresh takes when the other browser has closed
everything, and the harshest case for a view living inside the folder being
replaced -- and confirm the view is closed rather than leaked.

### The tab tree at scale: what actually hurts, and the check that outlives features

The tree is the part of this program everything else hangs off, so it was
measured rather than reasoned about. Two shapes, one probe:

    50,000 tabs, flat        small file,   28 MB resident,  781 ms to load
    16,000 folders, nested   245 MB file, 526 MB resident, 1147 ms to load

**Flat scale is fine and depth is quadratic**, and the cause is the file format
rather than any algorithm. Nesting is expressed as two spaces of indent per
level, so a chain of `d` folders writes `2 + 4 + ... + 2d` spaces: O(d^2) bytes
for O(d) tabs. Sixteen thousand nodes producing a quarter of a gigabyte, while
fifty thousand nodes flat produce almost nothing. It is reachable by dragging a
folder into a folder, repeatedly.

**So depth is bounded at 64 and the excess is flattened, not refused.**
Refusing a file loses tabs; flattening loses only nesting, which is the
cheaper of the two, and `load` reports how many it moved so the shell can say
so -- a tree that quietly changed shape on load is discovered weeks later with
nothing to explain it. Sixty-four is far past any filing anyone does by hand
and bounds the quadratic term at nothing.

**The durable half is `tree_invariants::check`.** Not more cases: one statement
of what must be true, called at the end of every section of `test_model`. Ids
unique, `parent` agreeing with `children` both ways, no cycles, depth within
the limit, a mirror's children all marked as mirrored, and no tab holding
children -- a shape the file format cannot express and would silently reorder
on save.

The reason it is written this way is that the tree is restructured by things
that cannot enumerate their own failure modes: drag and drop, an AI
reorganisation, a mirror refresh replacing a folder while a view inside it is
live. Each arrived with tests for its own behaviour. None could notice leaving
the tree subtly wrong in a way that surfaces in a different feature later.

**Demonstrated rather than asserted.** With `duplicate_node` altered to skip
setting the copy's parent pointer, `test_model` reports:

    FAIL  copying gives the copy an id of its own: the tree is still well
          formed (1 violation: 'a1-2' is listed under 'f1' but its parent
          points elsewhere)
    134 passed, 1 failed

Every assertion that section wrote for itself still passed. The invariant was
the only thing that noticed, which is the whole argument for having one.

The checker walks iteratively rather than recursively, deliberately: it is the
function that has to survive a tree somebody else built badly, and a recursive
walker blows the stack on exactly the input it exists to reject. A crash is a
worse diagnosis than a report.

**The scale suite is `test_tree_scale`**, driven by one generator
(`test/tree_gen.h`) parameterised on the three numbers that matter: how many
nodes, how wide a folder gets, how deep the nesting goes. Modest sizes run in
`make test`; `HYDRA_SCALE_EXTREME=1` enables the rest.

    5,000 tabs flat        save 2 ms, load 19 ms, 421 KB
    a tree at depth 64     6 KB -- what the bounded quadratic actually costs
    4,000-deep file        15.7 MB, loaded in 82 ms, 3,936 nodes flattened
    20,006 nodes           model load 76 ms
    5,001-node subtree     deleted in 4 ms

**One superlinearity, found and bounded rather than fixed.** Duplicating the
same node repeatedly costs 27 ms for 400 copies and 6,849 ms for 4,000 -- ten
times the count for roughly two hundred and fifty times the time. `unused_id`
counts upward from 2 looking for a free id, so the k-th copy of one node costs
O(k) lookups. Nobody duplicates one tab four thousand times, so this is not
worth fixing; it is worth *measuring*, so that a change making it worse shows
up as a number rather than as somebody's impression.

**The suite caps its own memory** with `setrlimit(RLIMIT_AS)` at 2 GB. A stress
test that can take the machine down is worse than no stress test, and this one
deliberately builds expensive shapes: a runaway allocation now fails as a
`bad_alloc` inside the test, which is a result rather than an incident. Each
case also deletes its file as it goes, through a scratch object that cleans up
even when a case fails -- learned by filling a 16 GB tmpfs with the O(depth^2)
probe files that produced the measurements above.

**And it found its first defect immediately.** The depth clamp was off by one:
indentation in the file is 0-based and counts below the synthetic root, while
depth in the tree counts the root as level 0, so a line indented `d` lands at
depth `d + 1` and clamping the indentation to the limit produced a tree one
level past it. Written by hand, checked by nothing, and caught on the first run.

### Running the browser dirtied the repository

`make run` pointed the app at `sample-tree.txt`, which is tracked. The app
saves its tree on exit, so simply starting the browser rewrote a file in git: a
page title where `about:blank` had been, a type changed from `suspended` to
`open`, and a fresh `seen=` timestamp every time.

It was reverted three times in one session, and at least twice the run that
caused it was somebody else's -- which is the tell that this is not a
discipline problem. A tracked file that changes when you run the program is
going to keep changing, and everyone who notices will spend the same minute
working out whether the diff means anything.

`make run` now copies the sample to `$(BUILD_DIR)/run-tree.txt` and runs
against that. The copy is refreshed only when missing, so state survives
between runs -- which is the point of running against a tree at all -- and it
sits under the build directory, which is already ignored. `make run TREE=...`
still overrides it for anyone who means a particular file.

Worth noting for what it says about the shape of the problem rather than the
fix: nothing here was wrong except *which file the convenience target pointed
at*, and the cost was paid by people who had not run the target.

### Looking at the browser, which found three things reading it had not

A capture pass over every surface -- the window wide and narrow, and each
dialog -- grabbed in-process with `QWidget::grab()`, offscreen. The driver is
`try_look`, and it asserts nothing: the other drivers check that a menu is
ordered correctly and a dialog opens, and none of them can see that a panel is
empty, a label is clipped, or that a dialog offers settings for a site that
does not exist. That is found by looking.

**Site controls with no page open were editable, and the rule went nowhere.**
The popup showed `(no page)`, sixteen rows of "Default", and a scope of "This
host" -- and `current_pattern()` returns the host, so touching any control
called `set_setting("")`. That stores a rule keyed on nothing: it matches
nothing, is invisible in the interface, and can only be found by reading
`policy.ini`. The two site scopes are now disabled without a site, the scope
falls back to Global default, and the header says so. The popup stays useful,
because the global defaults are still worth editing from there, and it now
shows what the browser *actually does* rather than sixteen rows of a word that
means "ask somebody else".

**`open_media` returned in silence.** Every other action needing a page says so
-- `learn_this_site`, `toggle_capture`, `find_media_with_ytdlp`,
`start_element_picker` all do -- and this one was the outlier. Hard to reach,
since the Media button is hidden until something is detected, but "the button
did nothing" is the worst thing a button can do and the fix is one line.

**Downloads had no empty state**: column headings above four hundred pixels of
nothing, which reads as broken rather than idle. The comment beside the action
buttons already made exactly that complaint about *them* and dealt with it;
the list was left. It now says what will appear there and why.

Two things checked rather than assumed, both of which would have been wrong
"fixes". The site-controls popup has no close button, which looked like an
omission until the window flags said `Qt::Popup` -- it closes on a click
outside, which is correct and idiomatic. And the download action buttons looked
enabled with nothing selected, which was the offscreen rendering rather than
the state: they are disabled, at two separate places in the code.

**The consent dialog had a column that was never filled.** Two columns, the
second with an empty header and `setStretchLastSection`, so it took most of the
width and squeezed every row of real content into the left third of a window
that looked half broken. Nothing ever wrote to it -- both the site rows and the
button rows beneath them set only column 0.

**And the reorganizer announced a model that was not there.** The banner read
"Local model (Ollama, llama3)" on a machine holding only qwen: `llama3` is the
hardcoded default in three places, and the first anyone learned otherwise was a
failed request after pressing Send.

The fix is smaller than it looks, because the information was already being
fetched and thrown away. `probe()` requests `/api/tags` -- which *is* the list
of installed models -- and kept one boolean out of it. It now keeps the list,
and `name()` says "llama3 -- not installed" when the configured model is absent.
That lands in all three dialogs and any future one, because every use of
`name()` is a label somebody reads: three banners and an "Asking %1..." status.
It only says so once the server has answered; before a probe the list is empty,
and calling that "not installed" would be a guess dressed as a fact.

**With a page loaded, the media dialog counted something other than what it
showed.** Three rows on screen -- an HLS stream and two `Playing` rows with
their buffered sizes -- above a status line reading "1 item(s)". It counted
`items`, the detected streams, while `playing` adds rows of its own beneath
them. A number that disagrees with what is on screen is worse than no number,
because the reader has to work out which of the two is lying. It counts the
rows now.

**And Host was the one column nobody sized.** Type and the button cell were
resized to their contents after populating, Name was the stretch column, and
Host was left with a default width -- so it elided `hls.cdnvideo11.shop` while
Name had several hundred pixels spare. That is the column carrying the point of
the dialog: a media host is *not* the page's host, and `hls.cdnvideo11.shop`
against `kisskh.co` is most of why somebody opens this window. Truncating it hid
exactly what it exists to show. Sized to contents rather than stretched, because
a host is bounded in a way a name is not.

That also caught a habit rather than a bug: `rule(s)`, `change(s)`, `item(s)`
and `imported rule(s)` in four dialogs, in a codebase that spells the plural
properly everywhere else -- `%1 request%2` with an empty string or an `s`. All
four now match.

**The extractor dialog was photographed mid-probe and needed nothing.** Both
actions correctly disabled until the probes answer, a header saying exactly
what it is waiting for, and a horizontal scrollbar that is right rather than
lazy: the payload's columns are aligned, and wrapping would break the alignment
that makes it readable. Worth recording that a surface was looked at and left
alone, since otherwise the only evidence of the pass is the things it changed.

**And the empty-state label took three attempts**, all the same mistake in
different clothes: geometry set from `refresh()` (which runs before the first
layout), then from the dialog's `resizeEvent` (which fires before the list's
viewport settles), and finally from an event filter on the viewport itself,
which is the only thing that knows when it is the size it will be drawn at.
The first two put a centred two-line message clipped into the top-left corner,
and each looked plausible until it was photographed.

### The committed example was being used as somebody's live tree

`sample-tree.txt` was reverted from git five times in one day, mostly by people
who had not knowingly run anything against it, and the reflex fix -- have the
app refuse to save into a git working tree -- was the wrong shape. A tree *is*
a dynamic personal file: it changes on every title a page supplies, every
`seen=`, every tab opened. Refusing to write it would be refusing to do its
job.

The mistake was that one file was serving as two things. `sample-tree.txt` in
the repository is a **committed example**, which should change when the example
changes and at no other time. The tree a person uses is a **personal file**
that changes constantly. Conflating them made every run of the browser a diff.

And the conflation was in the app, not in how people invoked it. With no
argument the search was cwd, then beside the binary, then app data -- so
starting the browser from a checkout picked up the tracked example *as the
working file*.

Now: an explicit argument means exactly that file, because somebody asked for
it. Otherwise the tree is the personal one in app data, seeded on first run
from whichever example can be found -- the checkout's, the copy CMake puts
beside the binary, or the one compiled into the executable, which is the only
one a packaged copy has. The example is only ever read.

That also collapsed an `#ifdef`. Android needed exactly this and for its own
reason -- there is no working directory worth the name there, it is `/` and
nothing in it is writable -- and had been given a separate answer. Two answers
to one question is how they drift; there is one path now.

Verified against a hash rather than an impression, after a first attempt that
proved nothing: the file was already dirty when that test started, so
"unchanged afterwards" said only that it had not got worse. From a clean
baseline, the example's hash is identical after a run and the personal tree
appears in app data seeded from it.

### kisskh's first real accept, and what the page-row fix cost

Five runs against kisskh with the corrected prompt -- `seen === 1` no longer
points at row 0, which is the page itself. Against 0 of 5 before:

    run1  no answer (7 min)
    run2  analytics beacon      refused by fetching the pick
    run3  analytics beacon      refused by fetching the pick
    run4  analytics beacon      refused by fetching the pick
    run5  hls.cdnvideo11.shop/hls07/10826/Ep24.v990_index.m3u8   ACCEPTED

**1 of 5, and it is the real manifest** -- the first genuine accept this site
has produced. Worth stating precisely, because the previous "accept" here was
an analytics beacon that only `check()` liked and the shipping gate refused.

**The regression is gone.** Four of nine replies picked the page's own address
before the fix; none of five did after. That was the whole point of the change
and it is the clearest result in the set.

**The dominant failure moved rather than disappeared**: three of five now pick a
Google analytics endpoint, caught by the tier that fetches the pick before
offering it. Which is the failure mode this site had before the `seen` rule
existed, so the loop is back where it was, one accept better.

**And the correction over-corrected.** No reply used `seen` at all, where five
of nine did before. Telling the model that `seen === 1` is true of the page too
appears to have taken the field out of use entirely rather than qualifying it.
That is not obviously harmful -- `seen` was never load-bearing -- but a field
was wired, described, used, and is now ignored, which is worth knowing before
anybody wires another.

**A harness bug surfaced with the new data.** `QSettings` parses a
comma-separated INI value as a *list*, and `toString()` on a list variant
returns the single element when there is one and an empty string when there are
several. Every corpus field had carried one url until `disproved` carried two,
at which point the field silently vanished and three replies scored as accepted
that the gate had refused. The one-element case had been working by luck for
the whole life of the file. Read as lists now; 28 of 28 reproduce.

### A batch that was two experiments, reported as one score

Five dramafren runs all timed out, which was uninformative -- and the logs
underneath them were not:

    run1  3 addresses answered      run2  0 answered      run3  3 answered
    run4  0 answered                run5  3 answered

Alternating, and not load: every earlier dramafren batch answered 3 on all
five. A run with no annotations is asking the model a **different question** --
the same addresses with none of the content-type notes -- so a batch that mixes
them reports one number for two experiments.

**The mechanism, from the code rather than a guess.** Send is enabled only when
every probe has replied, so this is not the dialog giving up: `m_served` gains
an entry when the host answers, and another when it answers 400 or worse.
Neither branch runs when a probe never *reaches* the host. `0 answered, 0
refused` therefore means every candidate failed to connect, which is a third
state the counts had no way to distinguish from "the tier did not run".

**What was not established**: why it alternates. That would mean probing a third
party's server repeatedly to characterise its rate limiting, which is somebody
else's machine and not obviously worth it. The cause is unknown and recorded as
unknown.

**What was fixed is the reporting.** `measure.sh` now prints the probe result
beside each verdict, so a mixed batch is visible instead of pooled. The harness
already printed the counts; nothing was reading them, which is the same defect
as a log nobody opens.

Two things this does *not* invalidate, checked rather than assumed: every
earlier dramafren batch -- the legend runs and the two-clause runs -- answered
3 on all five, so those scores were measuring one thing. Only the batch that
produced no answers was mixed.

### Drag and drop: what was already right, and the one thing missing

Asked directly whether the tree's drag and drop is smooth, shows where an item
will land, and is stable. Three questions, three different kinds of answer.

**Where it lands was already right.** `setDropIndicatorShown(true)`, and
`DragDrop` rather than `InternalMove` -- deliberately, since the model publishes
urls too, so a tab can be dragged out to another application and `InternalMove`
would refuse to hand anything over. Move by default with Ctrl to copy, and
`ExtendedSelection` so several tabs travel together.

**Stability was already covered**, and by the two cases that actually break tree
drag-and-drop rather than by a general "it works": *the move that would eat the
tree* -- a folder dropped inside its own descendant -- and *dropping while the
tree is filtered*, where the view's indexes belong to the proxy and the model's
do not. Since `tree_invariants::check` runs at the end of every section, a drop
that leaves a dangling parent or a cycle now fails even when the section's own
assertions pass.

**Confirmed by hand, which is the only way some of this could be.** With the
browser running on a real desktop: Ctrl-drag copies and the plus badge appears
on the cursor, several tabs move and copy together, and the drop indicator
reads correctly everywhere it was tried. Those were configured and tested at the
model level and had never been *seen*; an offscreen capture cannot answer any of
them.

The hover delay was the one thing that changed as a result. 600 ms had been
picked because that is roughly what file managers have used for twenty years --
a defensible argument and the wrong kind of evidence for something whose only
real measure is whether it feels stuck. Dragged by hand, 600 does. It is 400
now, chosen with a hand on the mouse.

**Smoothness had a real gap: `setAutoExpandDelay` was never set.** Hovering a
collapsed folder mid-drag did nothing, so a closed folder could not be dropped
into at all -- the drag had nowhere to land, and somebody had to abandon it,
expand the folder by hand, and start again. In a tree whose whole point is
folders that is the difference between drag-and-drop working and merely
existing. 600 ms now, the interval this gesture has had in file managers for
twenty years. `setAutoScroll` was already Qt's default and is now explicit, with
a margin, so it survives somebody tuning the view.

**Ctrl-drag copies, and the plus badge on the cursor is real.** It comes for
free once `supportedDropActions` offers `Move|Copy` and the view leaves
`startDrag` alone: Qt hands both actions to the drag, the platform draws the
badge while Ctrl is held, and `dropMimeData` branches on `CopyAction`.

But that badge is a *promise*, and what makes it true was covered only
indirectly -- through a test of `duplicate_node`, the function the copy branch
happens to call. `CopyAction` appeared nowhere in `test_model`, so a drop that
ignored the action entirely and moved would have passed everything. Now the
copy branch is driven directly: the original stays put, the arrival carries a
fresh id and the same address, and both are findable in the index.

Proved by breaking it -- with the branch disabled, the drop moves and the test
reports the original gone and `a1 vs a1` for the ids. Two failures, and nothing
else in the suite noticed, which is the argument for testing the branch rather
than the function under it.

**The guard is on the properties, not the behaviour, and that is deliberate.**
What a drag *feels* like is Qt's, and driving a synthetic `QDrag` would prove
little about it. What goes wrong in practice is somebody adjusting the view and
quietly dropping a setting -- after which drag-and-drop still "works" and is
worse in a way nobody can point at. Seven checks in `try_import` pin the whole
gesture set, which is exactly why the comment says it lives in one place.

### The collapse count nobody could see, and a pane that must not collapse

Two dialogs show the same suspect list, and looking at both with real evidence
made it clear they want opposite treatment.

**The annoyed report collapses, and its marker was invisible.** Five ad-shaped
addresses read as three rows, which works -- but the `x3` was appended *after*
the address, and these are ninety-character analytics urls in a list that
scrolls sideways. The one piece of information collapsing adds sat off the right
edge, so the row looked like a single request and the feature looked like it had
not run. The count leads now, with single rows indented to match.

**The filter-evolution pane shows the same five uncollapsed, and that is
correct.** It is the literal payload being sent to the model, under a dialog
whose promise is *review exactly what will be sent*. Collapsing it would make
the review a lie. Same evidence, opposite rule, because one list is for acting
on and the other is for checking.

Worth stating as a rule rather than an incident: **a display that summarises is
right when somebody is choosing what to do, and wrong when somebody is checking
what will happen.** The two are easy to confuse because they show the same data.

### A systematic pass that found nothing, and fixed the tool instead

With every surface photographed, looking harder at the same pictures was going
to run out. So `try_look` gained two checks it can run on each dialog as it
captures it -- every Alt key claimed once, and a window title present, since a
dialog without one shows in the task switcher as a blank entry.

The menus had two mnemonic clashes when they were examined; nobody had ever
looked at the dialogs. The first run reported four in settings:

    Alt+C   "Check now"        vs "Copy the flagged ones"
    Alt+R   "Remove selected"  vs "Rescan for players"
    Alt+R   "Remove selected"  vs "Remove selected"
    Alt+X   "Export..."        vs "Export all settings..."

**Three of those were between different pages of a stack.** "Remove selected"
lives on Privacy, "Rescan for players" on Media; they cannot be on screen
together, and Qt skips hidden widgets when matching a mnemonic, so it would
never confuse them. The audit was comparing every button in the dialog rather
than every button a person can see.

Restricted to visible buttons, **nothing survives**. The dialogs are clean and
every one has a title. The defect was in the checker, and an audit that cries
wolf about pages is an audit somebody turns off -- which would have cost more
than the four false reports.

Worth recording precisely because it found nothing: a systematic check that
comes back empty is a result, and the alternative -- reporting four "fixes" to
collisions that could not happen -- would have been worse than doing nothing at
all. It is the same shape as the three deliberate decisions this pass left
alone after reading the code.

**One limitation, stated rather than discovered later**: the audit sees a
stacked dialog on whichever page is showing when it is captured, so settings is
checked on Privacy only. Auditing the rest means driving the page list, which is
worth doing when a dialog gains buttons rather than now.

### Every drop folded the tree up

Found by handing the running browser over and being told, after one gesture,
that it was too annoying to test anything else with. That is the most useful
kind of report: it stopped the session at the first thing that mattered rather
than working around it.

**A drop resets the model.** `beginResetModel` tells the view that everything it
knew is void, so it collapses every folder -- and after moving one tab between
folders the whole tree closed. One drag was possible; the second needed the tree
re-opened by hand first.

The project already knew this. The comment beside `replace_mirror` says a reset
"collapses every folder in the tree" and uses fine-grained row signals for
exactly that reason. `dropMimeData` never got the same treatment, and there are
six reset sites in the model.

**Fixed in the view rather than at the six call sites.** Which folders are open
is the *view's* state, not the model's, so it records them before a reset and
reopens them after -- by id, so it works whether the reset moved the existing
nodes or replaced them all. That covers every reset site, including ones not
written yet, and does not require teaching six operations to emit correct
`beginMoveRows` sequences, which is the kind of change that is subtly wrong for
months.

Two details that would each have made it look half-fixed:

- **The current item was being lost too**, jumping to the top of the list on
  every drop. Same mechanism, same fix, noticed only because the code to
  restore folders was already walking for ids.
- **Reopening runs until nothing more opens.** Expanding a child of a
  still-collapsed parent silently does nothing, so a single pass would have
  restored the top level and left every nested folder shut -- which looks like
  a fix that works on the simple case and fails on real trees.

Guarded by a driver check: expand a folder, perform a real drop through
`dropMimeData`, confirm it is still open. Proved by disabling the restore, at
which point it reports the folders closed.

**And the verification caught me out first.** The source was restored and the
*app* rebuilt, but the sweep then ran a `try_import` binary still built from the
broken version and reported a failure that no longer existed. That is the stale
binary the build guidelines name in as many words, met in the wild.

### An empty tree that would not say which kind of empty it was

Typing a search that matches nothing emptied the tab pane and said nothing
about it. That reads identically to a tree with no tabs in it, and to the
filter being broken -- the one thing it does not read as is "your search
matched nothing", which is what happened. It is the same defect as the
downloads list, in the surface people use most.

The two cases are worth separating rather than covering with one message:

    filtered away   somebody's own doing, and there is an obvious way out
    genuinely empty  nothing has been filed yet, and the way out is different

So the view says which: *Nothing matches that search -- clear the box above*,
or *No tabs yet -- File > New Tab, or drag one in from another browser*. It
knows both, because it can see its own row count and the source model's.

**In the view, consistent with the folder-expansion fix.** Which folders are
open and why a pane looks empty are both presentation, and putting them in the
model would mean the model knowing about searches it does not run.

Two things carried over from the downloads label rather than rediscovered.
`layoutChanged` is the signal a filter emits, so watching insertions and
removals alone would have missed the only case this exists for. And the
geometry comes from an event filter on the *viewport*: the widget's own resize
fires before the viewport settles, which is exactly how the first version of the
downloads message came out clipped into a corner.

### Three buttons that could not do anything, and did not say so

Back, Forward and Reload were enabled from the moment the window opened,
including on an empty tab where none of them did anything at all. That is the
same defect the Media button had, and the browser had already answered it there
with a status message. A navigation button has a better answer available: it can
look unavailable, which is what every other browser does and what somebody is
already reading the toolbar for.

The state is asked of the view each time rather than cached. Mirroring two
booleans that Chromium changes underneath us is exactly the kind of state that
drifts -- redirects, in-page navigations and restored sessions all move history
without anybody pressing anything. Qt WebEngine has no history-changed signal
(`QWebEngineHistory` emits nothing), so the seam grows one, derived from the two
moments history can move: a navigation committing and a load ending.

`can_go_back()` is virtual with a default of **yes**, not pure. A backend that
does not track history should keep the buttons it has always had rather than
have them switched off on a guess.

**Finding this turned up a duplicate.** The toolbar and the Go menu held three
*separate* actions apiece, wired to the same three slots -- so greying the
toolbar's Back would have left the menu's Back on, offering a route to the same
nothing. They are one action each now, created in the menu bar (which is built
first) and lent to the toolbar. The test driver found the duplicate before a
person could: it looked up "Back" by tooltip and got the menu's copy, which
never greys, so every check that expected a greyed button failed.

Two ordering bugs, both crashes, both worth the entry. `update_navigation()`
reads the stacked widget, and called from the toolbar builder it ran before the
stack existed -- a segfault in every window construction, caught by the driver
on its first run. Guarding it then made the initial state silently wrong instead,
because nothing asked again until a page opened. It is called once more at the
end of the constructor, which is the only point where everything it reads
exists.

#### One file changed, everything that depends on it rebuilt

The clean build happens once; the edit-rebuild loop happens all day. Same three
systems, warm, `-Os`, `-j2`, a real content change to one file:

    leaf   src/tree_diff.cpp   3 dependents    cmake 24s   make 19s   fmake  8s
    mid    src/policy.cpp      8 dependents    cmake 24s   make 19s   fmake  6s
    hub    src/node.h         24 dependents    cmake 55s   make 40s   fmake 28s

**fmake wins every case, by two to three times**, and its clean-build deficit
(374s against 293s) is repaid by the second edit. It compiles each translation
unit once, so a hub header costs it one recompile per affected file rather than
one per affected file *per target*, and it relinks only the binaries whose
closure contains the object.

**The archive's signature is the flat column.** Make costs the same 19s whether
the change reaches three files or eight, because the cost is not the change: it
is `ar` rewriting `libhydra_app.a`, after which all seventy binaries relink,
including the ones that never referenced the changed code. That is the case
against a static archive in one number, and it is the same property that made
the clean build look good.

Two measurement bugs were caught before these numbers were believed, both of
which had produced a *flattering* result for the tool being measured:

- **`CXXFLAGS='-Os'` as a command-line argument to Make overrides the whole
  variable**, including every `-I` and `-std` the makefile appends -- so every
  compile failed instantly and the rebuild "took 0s". A command-line assignment
  beats `+=`; the environment does not, which is where the flag belongs.
- **`touch` is not a change.** fmake keys its object cache on content, so
  touching a file correctly rebuilt nothing and reported a flat 6s -- which is
  its analysis pass, and also its floor. Measuring an incremental build against
  a tool that hashes content requires editing the file.

#### End to end, on an idle machine

The same deliverable each way -- the app plus all 70 test binaries, `-Os`,
`-j2`, clean, sequential:

    qmake + Make    app  82s    app+tests 293s    287 objects
    cmake           app  56s    app+tests 328s    432 objects
    fmake           app 157s    app+tests 374s    181 objects  (app included)

**Fewest compiles does not mean fastest.** fmake does 181 to CMake's 432 and
takes 14% longer, because its compiles are not all it does: it reads the symbol
table of every object to compute each binary's closure, and it links 71
binaries from a large candidate set. What it buys is the smallest binary of the
three -- 1,214,488 bytes stripped against 1,329,416 -- because nothing unreached
is linked at all.

Two corrections to the first attempt at this measurement, both worth keeping
because both were confidently wrong:

- **A 4.5x gap that was the machine, not the build.** The qmake app build
  measured 142s standalone, 347s during a busy run, and 73-82s idle. The first
  reading was attributed to AUTOMOC's unity translation unit; on an idle machine
  the same comparison is 82s against 56s. Load was most of it. Any timing taken
  on this machine while anything else runs is worth about as much as no timing.
- **The test tree's 840s was two faults, and the larger was ours.** Not one of
  the 38 offline suites references Qt WebEngine, and the Makefile was linking
  `Qt6WebEngineWidgets` into all of them -- the largest library on the machine,
  scanned 38 times for nothing. Giving the engine only to what reaches it is
  most of the 840s -> 293s.

### -Os everywhere except under a debugger

The three build systems disagreed about optimization by default and nobody had
said which was right: CMake's `Release` is `-O3`, qmake's release default is
`-O2`, and fmake's is `-O2 -g`. All three now build `-Os`, and `DEBUG=1` is the
only exception, because a debugger needs the code to match the source.

Expressed as the *build type* on the CMake side (`MinSizeRel`) rather than as
`-Os` patched on top of `Release`, and as a replacement rather than an addition
on the qmake side -- `QMAKE_CXXFLAGS_RELEASE -= -O2` before adding `-Os`. Two
`-O` flags on one command line leave the last one winning, which makes the
setting depend on where in the line the generator happened to put it.

### The Android port has been dead since the rename, and nothing said so

Found while going to wire the navigation decider into Android. The port does
not work at all, and has not since `android: rename the application id to
se.vibes.hydra` -- which is the most recent commit to touch `android/src`, so
nothing has been run on a device since it landed.

**JNI binds by name and by nothing else.** A `native` method `m` on class
`p.q.C` resolves to the C symbol `Java_p_q_C_m`. The rename moved the Java to
`se.vibes.hydra` and updated the class paths C++ hands to `QJniObject` -- those
are string literals containing `org/qtproject/example/hydra`, which a grep for
the old id finds. The seven JNI entry points kept their old names, because in a
function name the separator is `_` rather than `/` and the same grep does not
match them.

Not inferred. The APK built earlier this session was unpacked and its library
read:

    nm -D --defined-only libhydra_arm64-v8a.so | grep Java_
    Java_org_qtproject_example_hydra_HydraWebView_onUrlChanged
    ... and six more

against `package se.vibes.hydra;` in the same package's Java. Every native call
would have thrown `UnsatisfiedLinkError` on first use: no url reporting, no
request filtering, no script bridges, no external links, no file picker. The
System WebView backend is all seven of those.

**Nothing could have caught it.** The Java compiles without the C++ and the C++
compiles without the Java; the two are joined at runtime by string equality. It
built, packaged, signed and passed the APK content checks written earlier
today -- which verified that our Java classes were in the dex and that the
library was present, both true and neither sufficient.

So `tool/jni_check.py` compares them at rest: every `native` method's expected
symbol against the symbols `src/*.cpp` defines, in either direction. It is pure
text, needs no SDK, NDK or device, and runs with the other gates -- `make style`
and CI's cheap job, in milliseconds. It refuses to pass when it finds no native
methods at all, since a check over an empty set reports success as loudly as a
real one. Verified against the bug itself: it exits 1 on the tree as it was and
0 on the tree as it is.

The APK was rebuilt afterwards and the library's exported symbols compared to
the names the Java declares. Seven wanted, seven exported, matched exactly.

**Still unverified: that it now runs.** Symbol resolution was the defect and it
is fixed at the level the defect existed; whether the port works on a device is
a separate question needing a device, and this is not evidence about it.

### The live drivers, which nothing had run since the build system changed

CI does not run them and neither had anything else this session. Running all
thirty-five found three failures, and every one was a different kind of thing.

**The sweep was pointed at a directory that no longer exists.** `sweep.sh` had
`BIN=test/build`, which was CMake's, and CMake is gone. The directory survived
on disk with two binaries in it from before the migration -- so the sweep did
not fail. It found drivers, ran them, and would have reported a clean sweep of
two out of thirty-five, every one built before a session's worth of changes.
That is the vacuous pass in its purest form: the check ran, said nothing was
wrong, and had looked at almost nothing.

It has a floor now, compared against the source count rather than a number
maintained by hand, so "far fewer drivers than sources" stops the sweep instead
of summarising it. The zero case was already refused; the handful case was not,
which is the same lesson `.style-gate.toml` records and the CI guards repeat.

**`try_navigate` was a real regression, and mine.** Making a page's new window a
child of the tab that asked (§5.5) changed where the node lands, and the driver
asserted the old placement -- while its own message said "under the tab that
asked, where the tree shows the relationship". It had inherited the shell's
contradiction word for word. The offline suites were updated when that changed;
this one was missed because nothing runs it. It asserts the intended behaviour
now, on both counts: the folder does not grow, the asking tab does.

**`try_handoff` cannot pass offscreen**, and that is the platform rather than
the driver. It hands a url to another application through
`QDesktopServices::openUrl`, and the offscreen plugin has no desktop services to
hand it to. Checked rather than assumed: `xdg-open` on this machine fetches a
local url fine, and the driver passes five of five on the real display. It had
been reported as a failure in every offscreen sweep -- and `sweep.sh`'s own
header claims every driver passes offscreen, which was simply not true.

**`try_evolve_confirm` waits on a model that is not running** and is killed at
the five-minute timeout. project.md already recorded that its real trigger is
unexercised for exactly this reason; what it did not say is that it costs every
sweep five minutes and a failure line about the machine.

Both are named skips now, with their reasons, the way `try_watch` and
`try_extract` already were. `SWEEP_ALL` still runs them. The summary reads 19
passed, 12 report-only, 0 failed -- and a failure line in it now means
something.

## The GUI pass: every window, looked at and then measured

Nine sections follow, and they are one piece of work. The method is worth
stating once rather than eleven times, because it is the finding: **looking
found the defects, and measuring found the next ones.**

Every one of the first four was found by opening a window and reading the
picture -- an empty table with its explanation stranded underneath, a dialog
whose every paragraph was clipped, a Send button offered for a model that was
not installed, six buttons squeezed to "pen Folde". None was found by a test,
because each is a fact about pixels rather than about the widget tree, and the
suites were all green throughout.

So the pass turned into building the instrument. `try_phone` opens all thirteen
windows and asks six questions of each: can the layout shrink to a phone screen,
is every button on it, is any label cut, is a paragraph absorbing height meant
for a stretch, does anything have focus, does Tab reach the rest. It found five
more defects, including one in a dialog the "what is next" list had written off
as not worth testing.

**And three of its own checks were wrong before they were right.** The width
check compared a size against the value just assigned to it and agreed every
time. The button check asked only whether a rectangle fell inside the dialog and
passed a button reading "pen Folde". The stretch check flagged the empty-state
overlay, which is *meant* to fill its viewport. Each was caught by the same
habit that found the defects -- read what it actually reported, not what it was
supposed to -- and the tally is worth keeping: on this pass the instrument was
wrong about as often as the code.

### The media dialog's empty state, finally photographed

The capture pass has always skipped four dialogs with a line saying they need a
loaded page -- the media dialog lists what a page is playing, and the extractor
works from the requests a page made, so both are empty by construction on an
empty tab. Nobody passed a url, because the only url to hand was a real site.

The local media fixture removed that reason. Passing it to `try_look` produced
the first pictures of the media dialog and the annoyed report ever taken, and
the media dialog was the odd one out of three treatments of "nothing here":

- the tab tree and the downloads dialog centre the message in the empty area
- the annoyed report makes it the first row of its list
- the media dialog left a dialog-sized black table with the explanation
  stranded underneath it

The first is right and was already a deliberate fix -- the GUI pass began with
"two empty states that could not say which kind of empty they were". The media
dialog missed it for one reason: it could not be looked at.

It has an overlay in the viewport now, the same pattern as downloads, and the
status line stays empty in that state rather than repeating the sentence two
inches lower. `m_status` could not simply be moved: it is the dialog's status
line and also reports assembling, progress and errors.

**Three treatments turned out to be four**, and the count is what eventually
forced the fix into one place -- see *The empty state, written three times and
wrong the third* below, where the consent dialog turned out to have the same
defect and the pattern became `empty_state`.

**And then the other two, which needed something different.** The note that
used to sit here said they wanted evidence the fixture did not produce. That
was wrong: both call `choose_ai()` first and return with a status message when
no provider answers. They were blocked on a model, not on fixture content --
the same blocker as `try_evolve_confirm`. Reading the two slots settled in a
minute what guessing had got backwards.

With Ollama running they both open, and so does the reorganizer, which is
gated the same way. **Neither needs one to be measured**, which is the later
correction: a layout does not care whether the provider behind it can answer, so
`try_phone` builds all three against a closed port and photographs them. What a
running model is still needed for is watching one of them actually reply.

The fixture did need extending for the *filter* dialog to have anything to say: `filter_signals::looks_ad_shaped` refuses first-party
requests outright, so a page serving its own ad-shaped paths produces nothing.
It serves them from 127.0.0.2 now -- loopback, and a different host -- which is
the same trick `try_cookies` uses for its third-party cookie, and the dialog
lists all four.

### The extractor dialog, first look: every paragraph clipped

Photographing it for the first time showed the pane you read before sending
anything to a model, with every prose line running off the right edge behind a
horizontal scrollbar. The request table was fine; the paragraphs explaining
what the table means were not.

`NoWrap` was set deliberately, and for a real reason: the pane holds a
column-aligned request table whose columns line up only if nothing reflows. The
trade goes the other way once seen. This is the dialog whose entire purpose is
"read this before it leaves the machine", and a paragraph that has to be
scrolled sideways line by line is one nobody reads. The table rows are short
and survive wrapping; a long url folds instead of disappearing, which is the
smaller loss.

Same defect, same shape, third time this session: the certificate chooser hid
its issuer and expiry the same way, and the media dialog stranded its empty
state under an empty table. All three were found by looking and none by a test,
because each is a fact about pixels rather than about the widget tree.

### Send, offered for a model that is not there

The filter dialog's header read "Local model (Ollama, llama3 -- not installed)"
while its Send button stayed enabled. That label exists precisely because the
reorganizer used to announce a model that was not there and the first anyone
knew was a failed request after pressing Send -- so the label was the fix for
that, and the button that produces the failed request was still sitting beside
it. The rule from the GUI pass says a control that cannot work should look
unavailable.

**The question `available()` answers is not the one the button needed.** Ollama
answers its API as soon as it is serving, so `available()` is true with no
usable model installed at all -- correctly, because it is about the backend.
`ready(QString *reason)` is about the model, defaults to `available()` for a
backend with no separate notion of one, and hands back the sentence explaining
itself. One `gate_send()` helper does both halves at each of the three call
sites, so the rule is stated once and the tooltip is never left stale.

**An empty model list is an unanswered question, not a no.** `name()` already
took this care -- `m_models` is empty before the server has replied, and
reading that as "not installed" would grey out Send on a working setup, which
is the same guess dressed as a fact in the other direction. `ready()` keeps the
same guard, and a test holds it.

**The driver caught itself passing vacuously**, which is worth more than the
fix. `try_send_gate` builds the real dialog against a fake Ollama and
photographs it. Its first run showed Send greyed in *both* cases and reported a
pass -- because with no requests recorded, `suspects_for()` was empty and the
button was already disabled for having nothing to ask about. The gate under
test had not run at all. It now files two ad-shaped requests first and refuses
to continue if no suspect was recorded, so the only remaining reason for the
button to be grey is the one being tested. The two pictures differ in exactly
one pixel region.

### The empty state, written three times and wrong the third

The consent-rules dialog showed a large blank table with the sentence
explaining it -- "Nothing recorded. A banner is only listed here when it was
found, looked like consent, and offered nothing any rule matched." -- in a
status label *under* the table, in small text, reading as a footnote about the
window rather than as the answer to the question a blank table asks. The
downloads and media dialogs both centre theirs inside the empty list. Same
defect, fourth occurrence, and again found by looking rather than by a test.

**The fix was not to write it a third time.** Two of the three had grown the
same overlay label, the same viewport resize filter and the same placement
function independently, so `empty_state` now holds one copy and all three
attach to it. The count comes from the model rather than from the caller,
because a caller that must remember to call `refresh()` forgets on exactly one
path and it is always the one that empties the list.

**And it crashed, which is the part worth keeping.** `try_look` segfaulted
every run immediately after photographing that dialog. The backtrace: inside
`~QTreeWidget`, `deleteChildren()` frees the viewport and the overlay with it,
and *then* the item model emits `modelReset` -- so `refresh()` ran on a label
that had been freed one frame earlier. `QPointer` for both members turns
teardown into a no-op instead of a fault.

Two things about how it was found. The live driver caught it and the unit test
did not, because every section of that test declared the view first and the
helper second, so the helper always died first and the ordering that crashes
could not occur; the dialogs parent the helper to the dialog, where the list
goes first. The test now has that section, and it was confirmed by reverting
the `QPointer` and watching it segfault at exactly that point rather than by
reading the code and being satisfied.

**And `try_look` runs against the fixture now**, which is what made the last
part possible. Four surfaces -- the window with a page in it, the media dialog,
the annoyance report and the extractor -- were skipped on every run that did not
name a url, which was every run, so the dialogs needing a page were exactly the
ones nobody looked at. That is where two of this session's visible defects had
been hiding. A `file://` page is not enough and it is worth saying why: the
annoyance report and the extractor both key on the site host, and a file url has
none, so both correctly refuse and neither gets photographed. The fixture serves
from 127.0.0.1 and needs no network.

Still not photographed: the extractor and filter dialogs, which call
`choose_ai()` and return when no provider answers. `try_send_gate` reaches the
filter dialog by building it directly against a fake Ollama; making the *shell*
reach it would mean writing an endpoint into the user's real settings, which is
a decision rather than a driver change.

### Do the dialogs fit a phone? Two of four did not

`android_dialogs::install()` exists because the media dialog came up 1200
logical pixels wide on a 1080-pixel screen, so its list was visible and its
buttons were not -- found by tapping Play and discovering there was nothing to
tap. **The fix was never checked against the contents.** It clears the dialog's
own minimum and hands it the screen rectangle, which is the right instruction,
but a widget cannot go below its *layout's* minimum, and a horizontal row's
minimum is the sum of its children. So the fix can be exactly right and the
dialog still too wide, and nothing said so.

`try_phone` asks the question on a desktop, because it is entirely layout
arithmetic: give each dialog the instruction Android gives it, then ask what its
layout will actually accept. Two of the four measured could not shrink to 360
pixels -- downloads at 532, settings at 395.

**The driver's first check was vacuous and it was the interesting failure.** It
compared `dlg->size()` to the screen straight after `setGeometry`, which is the
value just assigned, so it agreed every time -- reporting downloads as fitting
while the number that disproved it, the layout floor, was printed on the same
line. Qt honours an assigned geometry only until something re-lays-out, and on a
device something always does. The check reads `minimumSize()` now.

**Its second check was too weak in a subtler way.** Asking only whether each
button's rectangle fell inside the dialog passed the downloads dialog, whose
"Open Folder" was on screen reading "pen Folde": six buttons squeezed to 51
pixels against an 80-pixel label. Inside the dialog and unusable are different
things, so a button narrower than its own `sizeHint` now counts as cut.

`flow_layout` is the fix for the row: at any width where the items fit it lays
out exactly as a QHBoxLayout would, so no desktop window changes shape, and
below that it uses a second line. Its `minimumSize()` is the widest single item
rather than the sum, which is the number the Android path reads. Downloads went
from a floor of 532x123 to 102x154, and Close moved to its own right-aligned row
-- deliberate, since a flow layout has no stretch to hold it at the right edge,
and separating the five things you can do from the one that leaves reads better
than it sounds.

**Settings is a named gap rather than a fix.** Two of its rows use the flow
layout now, but the binding constraints are elsewhere and each is a page:
`page_downloads` 367, `page_ai` 347, and the button box needs 373 because the
Restore label names the page it acts on -- a deliberate choice worth keeping.
That is a pass over seven pages, not a row, so `try_phone` names it with those
numbers and prints them without failing on them.

**The pages are done, and one technique did most of it.** A `wide` row gives its
control width through the layout's stretch, so the control's size *hint* only
ever decides what it will accept when there is not enough -- which means
lowering a hint costs the desktop nothing and buys the whole difference on a
phone. Three findings, each a single widget holding a page hostage:

- `page_kiosk`, 439, was one combo entry: "Geometric -- exact transform (test on
  the target GPU)". A `QComboBox` will not go below its longest item. The popup
  still lists every option in full, which is where they are read.
- `page_downloads`, 367, was `setMinimumWidth(260)` on the torrent interface
  field. It was reaching for the right thing by the wrong instrument -- it
  existed so the placeholder stays readable, but a *minimum* is a promise no
  phone screen can keep.
- `page_ai`, 347, was three radio buttons carrying their qualifier in the label,
  and a `QRadioButton` does not wrap. Their explanations were tooltips, which
  need a pointer to hover -- so on the Android build the reasoning behind the
  one setting that decides whether anything leaves the machine was readable on a
  desktop and invisible on a phone. Short labels with the explanation under
  them, which is what every other setting on these pages already does.

**And then the sidebar, which was the real one.** With the pages fixed the
window still gave 190 of 360 pixels to a permanent category list, leaving about
150 for the settings -- enough to wrap every description to two words a line and
still clip the controls off the right edge. Below 520 pixels the list becomes a
dropdown under the search box and the pages take the full width. The same shape
the main window already uses for the tab tree, and the desktop is byte-for-byte
what it was.

What is left is the button box: 373, because the Restore label names the page it
acts on, which is a deliberate choice. On a device it elides rather than
anything becoming unreachable, so it stays a named gap.

### The two dialogs the network puts in front of you, and a third check

A password prompt and a certificate chooser are not opened by a menu, so no
slot reaches them and the phone pass had never seen either -- although they are
the two where being unable to reach a button matters most, one of them being how
you say "do not send my identity". `try_phone` builds them directly now, the way
`try_chrome` does and for the same reason: a modal blocks the driver.

All three fit at 188 pixels, and the certificate dialog is worth a note about
why. It sets `setMinimumWidth(460)` on itself so its list can show issuer and
expiry, which is right on a desktop -- and `android_dialogs` clears exactly that
before assigning the screen rectangle. The driver copies that step, so it
measures what a phone gets rather than what a desktop keeps.

**The defect was in the other axis, and no check could see it.** Handed the
whole screen, the proxy prompt came up with an inch of nothing between each
sentence and its two fields against the bottom edge. Every button was on screen
and none was cut, so both existing checks passed it; it was found by looking at
the picture. The cause is that a dialog given more height than it asked for has
to put the difference somewhere, and with no list and no stretch to absorb it a
word-wrapped QLabel will, because its size policy permits it. One
`addStretch(1)` before the buttons, which changes nothing on a desktop where the
dialog is already the height of its contents.

A label taller than the text it holds is the signature, so that is the third
check: `height()` against `heightForWidth()` at the width it actually got. It
was confirmed by removing the stretch again and watching it report labels 260
pixels tall holding 17 pixels of text. Its first run failed the downloads and
consent dialogs, whose `empty_state` overlay is *meant* to fill the viewport --
the check was the thing that was wrong there, and it skips that one label by
name.

The annoyance report went the same way once it was measured: 479 wide, of which
457 was a `QDialogButtonBox` holding four buttons, so its labels squeezed past
reading on anything narrower. The three tools are a `flow_layout` now and "Just
Record It" keeps its place last, in a box of its own -- which is the bottom row
rather than the right of one, and the reading the ordering was chosen for
survives a narrow screen, which the row did not. 479 to 149.

**And the three that ask a model, which no capture pass had ever photographed.**
In the shell they open only when `choose_ai()` returns a provider, so they were
absent from every run. The question here is about layout, and a layout does not
care whether the provider behind it can answer -- built directly with one
pointed at a closed port, the same way `try_send_gate` reaches the filter
dialog. All three fit: filter 291, extractor 304, reorganizer 284. The
reorganizer's picture also shows the Send gate working in a dialog it had not
been checked in, greyed with its reason on it.

**Every dialog this browser has is measured against a phone screen now, and so
is the window.** The media dialog was the last, and it wanted a page rather than
a provider -- building it directly would have meant standing up a detector, a
player launcher, a download manager, a proxy and an MSE tap, where opening a tab
is both cheaper and the path a person takes. It comes out at 102.

The window itself is the surface anybody actually uses and everything above
appears in front of it, so leaving it unmeasured was the odd gap. It fits at
321, and its drawer -- the only way to reach a tab at that width, and never
photographed before -- takes 82% of the screen and leaves a strip of page to tap
back on. Nothing to fix, which is worth recording as a result rather than as
silence.

### The site controls had no keyboard way in

Nothing in this tree sets a tab order and nothing tested one, so what Qt does by
default -- construction order -- had never been looked at. Asking each dialog
what holds focus when it opens answered well for twelve of thirteen: the two
password prompts land on the username field, settings on its search box, the
lists on their lists.

The exception was the site controls, where **nothing had focus at all**. The
cause is one line: it sets `Qt::Popup`, and a popup does not hand focus to a
child the way a dialog does. So a panel of fifteen per-site controls opened with
no focus ring anywhere and no entry point for anybody not using a pointer. It
lands on the scope selector now, which is the right place because it decides
what everything below it applies to -- reading it first is the order the panel is
meant to be used in.

Now a check rather than a printout, and confirmed by taking the line out again
and watching it fail.

**And then whether Tab reaches the rest**, which is the same question one step
further in: a control no number of Tab presses arrives at is operable by pointer
only. Walked with a real `Qt::Key_Tab` rather than `focusNextChild()` -- which is
protected, and which would have been the wrong instrument anyway, since pressing
the key goes through the focus machinery a widget can intercept and that is
exactly where a control gets stranded. Nothing is stranded: 24 controls in
settings, 16 in the site controls, and every other window complete.

**The small numbers are the honest part.** Downloads reports two focusable
controls, not eight, because its five action buttons start disabled and a
disabled control is not in the chain -- so "reaches all 2" is a true statement
about a small question. The count of sleeping controls is printed beside it so
the number is read for what it is. Confirmed by capping the walk at two steps
and watching it name what it had not got to. The mnemonic audit in `try_look` covers the neighbouring
question and reports nothing, so this was the gap rather than the pattern.

**The floor counts dialogs, not pictures.** It counted shots first, and the
settings walk takes one per page, so a run that measured a single window seven
times would have cleared a floor of twelve. It is exact rather than comfortable
now: one below the real count lets a dialog go missing without anything saying
so, which is the failure the guard exists to prevent rather than a smaller
version of it. Checked by raising it out of reach and watching the run exit 1.
The sweep reads the "N failed" line, and this tree's own rule is that a summary
which is always wrong trains people to skip the summary; a named gap is visible
without being noise. The pages carry object names now, because the first run of
that diagnostic answered "QWidget" four times, which is the question again.

### A claim about legibility, now checked rather than asserted

`empty_state` dims its message with `setEnabled(false)` rather than by writing a
colour, and the comment beside that line says it is "so it stays legible in both
colour schemes". That is a statement about a palette this code does not own, and
it had never been measured -- the settings dialog carries a contrast check
precisely because the same assumption was wrong there once: it dimmed by writing
a colour, which froze under whichever scheme was current when the widget was
built.

The claim holds. Composited over the base it is painted on, the message stands
49 lightness levels clear in light and 124 in dark, against the floor of 25 that
`try_settings_ui` already uses.

**Two checks, and they catch different things.** Breaking it the historical way
-- a written grey instead of a role -- left the contrast at 27 in dark, above the
floor and visibly poor; what caught that was the older check that the label is
dimmed by the style rather than by a colour. Painting the disabled text in the
background colour outright took both schemes to 0 and the contrast check failed.
So neither is redundant, and the pair is worth more than either: one is about
the mechanism, the other about the result.

### The two colours this tree writes by hand

Everything else asks the palette. Two places do not: the tab tree paints an
unopened link mid-grey, and the downloads list paints the public-swarm marker
amber. Both are deliberate -- one is a shade of the ordinary text colour, the
other a warning that must not read as ordinary -- and both are frozen numbers a
colour scheme cannot move, which is the shape of the settings-description bug.

They survive both schemes: 115 lightness levels clear in light and 107 in dark.
The point of measuring is that nobody knew it. A mid-grey happens to clear a
dark background and a light one, and "happens to" is the part worth holding
still.

**Asked of the model rather than copied from it.** The first version restated
`QColor(140, 140, 140)` in the test, which tests the file against itself:
change the colour in `tab_tree_model.cpp` and the copy agrees with the old value
forever. It builds a model and reads `Qt::ForegroundRole` now, and was confirmed
by changing the colour in the source and watching the light case drop to 10.

The downloads marker is deliberately not checked, and named so the omission is a
decision rather than an oversight: reaching it needs a live download, and
asserting against a second copy of the literal would be the same self-agreement
in a different file.

## Dependencies, CI, and the chrome a browser needs

Another such run, and grouped for the same reason: the dependency list and the
build system, CI taken from nothing to green, the drivers that stopped fetching
real sites to make a point, and the chrome the browser simply did not have -- a
password prompt, a certificate chooser, find in page, zoom, a loading indicator
-- until somebody used it and noticed.

### The dependency list, written down and therefore found to be wrong

Three places name what this needs to build -- `README.md`, `debian/control`'s
`Build-Depends`, and CI's apt line -- and no two of them agreed. Writing the
list out as a table, with each package confirmed by `dpkg -S` on the file that
provides the module rather than by what looked likely, is what surfaced it.

| what for | Qt module / library | Debian package |
|---|---|---|
| shell, networking, DBus, test harness | `widgets` `network` `dbus` `Test` | `qt6-base-dev` |
| moc, rcc, uic | -- | `qt6-base-dev-tools` |
| the page bridge | `webchannel` | `qt6-webchannel-dev` |
| the extractor sandbox (`QJSEngine`) | `qml` | `qt6-declarative-dev` |
| the engine (not on Android) | `webenginewidgets` | `qt6-webengine-dev` |
| KeePassXC bridge | libsodium | `libsodium-dev` |
| pairing across a restart | libsecret-1 | `libsecret-1-dev` |
| BitTorrent | libtorrent-rasterbar | `libtorrent-rasterbar-dev` |
| Firefox session decoding | liblz4 | `liblz4-dev` |

**`Build-Depends` was missing three.** `qt6-declarative-dev` and
`qt6-webchannel-dev` are direct dependencies -- `hydra.pro` says
`QT += webchannel qml` -- and were satisfied only because `qt6-webengine-dev`
happens to depend on both. That works and is fragile in a specific way: the
Android build asks for no WebEngine at all, which is exactly where a
transitively-satisfied dependency stops being satisfied.

**`liblz4-dev` was the one that cost something.** It was in nobody's build
dependencies, so a `.deb` built in a clean chroot would have come out without
the LZ4 decoder -- silently, because a missing optional dependency is a smaller
build rather than a failure. The package is the one place that hazard has no
guard: CI checks that the *binary it builds* links all four, and the deb build
is not in CI.

Checked rather than assumed after the fix: `make deb` succeeds and
`dpkg-deb -f` shows `liblz4-1`, `libsecret-1-0`, `libsodium23` and
`libtorrent-rasterbar2.0t64` in the package's computed `Depends`, which is
`dpkg-shlibdeps` reporting what the binary actually linked.

All three lists name every direct dependency now, including the two that arrive
anyway, and each says so.

### The drivers no longer fetch a real site to make a point

`try_media`, `try_frame` and `try_mse` each carried a `dramafren.org` url as
the value used when given no argument, so a sweep fetched a live ad-serving
site three times over -- announcing the machine to it and pulling in whatever
it served that day. That is where the tracking urls in the committed example
came from.

It also made those three unrepeatable in the way that matters: what they
measured changed between runs for reasons nothing here controls, so a
difference in output was as likely to be the site's week as the code's.

`test/live/media_fixture.h` serves the shape instead -- a manifest, segments
under it, a player in an iframe, and a MediaSource that is opened and appended
to -- from a `QTcpServer` on loopback, in the same shape `try_cookies` already
used for its origins. A real url is still what you pass when a real site is the
question, which is how `try_extract` always worked.

**The default was not the only thing reaching the network.** With the fixture
in place `try_media` still fetched ten hosts: it activates the tree's first tab
on purpose, and in the committed example that entry is `doc.qt.io`, which
brings googletagmanager, amplitude and surveymonkey with it. Two separate
things had to be fixed for one symptom -- the url the driver navigates to, and
the tree it opens -- and fixing the first alone left 61 requests across 10
hosts looking like an improvement on 70 across 12.

It is 10 requests across 1 host now, and the driver still finds what it is
looking for: one item filed, the badge lit, two video-shaped urls. The other
two hook the MediaSource and find the iframe against the fixture as well.

**What the fixture cannot answer** is whether a real site's obfuscation defeats
the detector, which is the question those drivers were originally pointed at a
real site to ask. That question now needs the url typed deliberately -- which is
the right shape for it, since the answer was never repeatable anyway.

### A sweep wrote somebody's ad-tracking into the committed example

Found by `git status` after the sweep rather than by any check. Two
`fedoq.com/clicks/...` urls had been written into `sample-tree.txt` -- the
tracked example -- carrying screen size, timezone, browser version and the
referring page, as **sub-tabs** under the first entry.

Three things combined, and none of them was new on its own:

- Eleven drivers loaded the tracked `sample-tree.txt` directly, several through
  a hardcoded `/home/nabbe/src/hydra/...` that is one machine's path.
- The shell saves the tree whenever it changes -- a title arriving, a `seen=`
  stamp, a tab opening. `make run` was given a copy for exactly this after the
  example was reverted from git five times in one day; the drivers never were.
- A page's new window is a sub-tab now (§5.5), so a driver pointed at a real
  site saves what that site opened, nested under the tab it came from. What was
  a stale timestamp became a third party's tracking parameters.

`test/live/sample_tree.h` gives each driver a private copy under its own
scratch directory, and finds the example rather than being told where it is, so
the hardcoded paths are gone with it. A full sweep now leaves `git status`
empty.

**Worth noticing about the shape of it.** The example being dirtied was a known
problem with a known fix, applied in one place. Nothing was watching the other
eleven, and the change that made it serious -- sub-tabs -- had nothing to do
with either. A fix applied where a problem was noticed is not a fix applied
where the problem is.

### CI is green, and what it took to get there

Four runs. Each failure was a real defect rather than a CI problem, which is
the argument for having it: none of the three would have been found by building
on this machine, because this machine is the one they were all invisible on.

| run | failed at | what it actually was |
|---|---|---|
| 1 | Build the app | the declared Qt floor had been wrong for months |
| 2 | Run the offline suites | a suite asserting a fact about *this* machine |
| 3 | -- | green |

The third finding came between runs rather than from one: going to wire the
Android navigation decider turned up seven JNI symbols naming a package that
was renamed away, which had left the whole Android port dead. That one CI could
not have caught either, and now `make style` does.

**What the green run establishes**, taken from its own log rather than from the
tick: 237 files conform to the indentation gate, project.md says nothing twice
and names no missing file, 7 native methods resolve, Qt is the version
`hydra.pro` demands, all four optional packages are present *and* linked into
the binary, 38 suite sources produced 28 binaries, and 28 suites passed. Those
counts are asserted, not printed -- an empty file list or a collapsed suite list
fails the run.

**What it does not establish.** The live drivers are not *linked* or run: that
wants thirty-five Qt WebEngine links and a display, and neither belongs in a job
that has to finish. They are compiled, which is the half that is cheap and
catches what actually kept happening -- a change to the shell breaking a
driver's compilation, found by hand after the fact because nothing built them.
Checked by breaking one on purpose: the step exits non-zero and names the file
and line.

The APK is still not built, and running the drivers is still something this
machine does. The gap is worth naming rather than reading a green tick as
"everything is checked".

### CI, and the first thing it found was a lie in the documentation

The first run went green on the gates in eight seconds and failed the build in
ninety, with two hundred lines of `'ColorScheme' is not a member of 'Qt'`.

**The declared Qt floor was 6.4 and had not been true for months.** It was
derived once, honestly, from the menu bar's `addAction` overload -- and then two
things raised it and neither re-derived it. `theme.h` names `Qt::ColorScheme`,
which arrived in 6.5. `qtwebengine_view.cpp` includes `QWebEnginePermission`,
which arrived in 6.8 -- and **that include sits outside the
`#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)` guard around the code that uses
it**, so the guard bought nothing at all: the file could not compile on an
older Qt whether or not the guarded code was reached.

This file predicted it. The note on building against 6.11 said moving to
`QWebEnginePermission` "would lift the floor from 6.4 to 6.8". The move
happened; the floor stayed where it was.

**So the floor is 6.8, and `hydra.pro` now refuses anything older by name**
rather than letting it fail in the compiler. 6.8.2 and 6.11 are the versions
actually built against; 6.5 to 6.7 are excluded by that include rather than by
evidence, and nobody has tried them.

**CI builds in a `debian:trixie` container, not on the runner's Ubuntu.**
ubuntu-24.04 ships Qt 6.4.2, which is what found the problem and cannot build
the result. Trixie ships 6.8 and is what this machine runs, so CI now compiles
against the same Qt as the person writing the code.

**What the failure did not cost.** The two risks flagged when the workflow was
written were the Ubuntu package names and `test_credstore` needing a Secret
Service. The first was fine -- the dependency check passed, naming all four
optional packages. The second is handled in the suite itself, which reports
`skipped` with a reason when `credential_store::available()` is false. Neither
was the thing that broke, which is the ordinary experience of predicting which
part will fail.

### Every driver screenshot this session was in the wrong theme

The capture pass found it, and it invalidates how several decisions were
judged. `try_look`'s images came out light for the first five surfaces and dark
from the sixth onwards. Nothing in the driver changes the theme, and the flip
was reproducible at the same point.

**The drivers never run `main()`.** They construct `main_window` directly, so
`theme::apply()` and `theme::apply_icon_theme()` -- which `main()` calls before
anything is shown -- never ran. The window came up in Qt's default light
palette. The settings dialog's Cancel restores the stored appearance, and that
was the first time in the whole run that anything asked the desktop what it
wanted; from there on, everything was dark.

**This desktop is dark.** Asked directly:

    dbus-send --session --print-reply --dest=org.freedesktop.portal.Desktop \
      /org/freedesktop/portal/desktop org.freedesktop.portal.Settings.Read \
      string:org.freedesktop.appearance string:color-scheme
    -> variant variant uint32 1        # 1 = prefer dark

So every screenshot taken from a driver before this fix showed a browser nobody
here runs, and the locked-tab padlock -- whose legibility was argued about at
length, and whose fallback colours were chosen "because the tree's background is
light" -- had been judged entirely against a white tree that this desktop never
draws.

**The fix is two lines in each of two places**, matching what `main()` does: the
palette and the icon theme. `shell_fixture` covers the drivers that use it;
`try_look` predates the fixture and builds its own window, so it carries the
same two lines, and collapsing that duplication is worth doing when something
else brings that driver onto the fixture.

**The icon theme is the half that is easy to miss.** Applying only the palette
gave a dark window wearing the light theme's icons, and the padlock became a
dark glyph on a dark row -- less legible than the smudge that started this. With
both applied it is breeze-dark's `emblem-locked`, orange and unmistakable, which
is also what every other application on this desktop uses to mean the same
thing.

**What this says about the method.** Looking at the screenshots is what caught
the smudged padlock and the clipped certificate row; this is the same lesson one
level further out -- a picture is only evidence of what a user sees if it was
taken in the conditions a user has. The offscreen note at the top of `try_look`
already said colours and icons are not faithful under `QT_QPA_PLATFORM=offscreen`;
what nobody had noticed is that they were not faithful on the real display
either, for a different reason.

### The address bar did nothing with no tab open

Found in the same pass, from the picture of the window with an empty tree. The
address bar is enabled and inviting -- the empty state beside it says "Select a
tab from the tree" -- and typing an address and pressing return discarded it
silently, because `navigate_to_address` loads into the current view and there
was no current view.

That is the defect the GUI pass spent its time removing, in the one state
nobody had photographed. It opens a tab at the root now, which is what every
other browser does with a typed address, and says so in the status bar.

### The test tree's archive, replaced by fmake's symbol closure

`test/Makefile` built one `libhydra_app.a` and linked all seventy-odd binaries
against it. The reasoning was sound as far as it went -- an archive contributes
only the members that resolve something, so each suite still linked only what
it reached, and thirty-eight source lists never had to be written. What it cost
is what `build-and-commit.md` says an archive costs: the archive is rebuilt when
any input changes, and everything using it relinks, a flat 19s whether the
change reached three files or eight.

**The archive was standing in for a computation**, and fmake performs exactly
that one: compile, read the symbol tables with `nm`, close transitively over
undefined symbols. That is the same thing the linker does when deciding which
archive members to pull, so the answer is exact rather than an approximation
from the include graph -- which is what the earlier plan recorded here was
going to use, and would have been wrong wherever a header is implemented in a
file it does not name.

`tool/objsets.py` asks fmake once, rewrites the answer into the object names
`test/Makefile` uses, and writes `test/objsets.mk`: 79 programs, 3992
objects, 50.5 apiece. It is committed, so an ordinary build needs `make` and
nothing else; fmake is needed only to regenerate it.

**Measured, from an up-to-date tree each time**, counting offline suites that
relink after touching one file:

| touched | relinks | was |
|---|---|---|
| `src/tree_diff.cpp` | 3 of 38 | 38 |
| `src/policy.cpp` | 5 of 38 | 38 |
| `src/main_window.cpp` | **0 of 38** | 38 |

The zero is the one worth reading twice: no offline suite links the shell at
all -- only the live drivers build a real window -- and the archive was
relinking all thirty-eight of them for it anyway. The first attempt at this
measurement reported 7 and 7, because the second `make -n` was still counting
the first touch's pending relinks; a measurement taken without returning the
tree to a known state measures the previous measurement.

**It also stopped linking the test-side moc into everything.** The old rule put
`$(TMOC_OBJS)` on every link line; the closure says two programs need it.

**The generated file carries the source list it was generated from**, and the
Makefile compares that against the same glob its rules use, refusing to build
on a mismatch and naming the command that fixes it. A stale link set is not a
loud failure by nature: it either fails as an undefined symbol while linking a
binary nobody touched, or links against a set that no longer describes the
tree.

**The archive was also confusing the other build system.** fmake at the repo
root refused to decide anything, reporting `main_window::open_url` as defined
by both `src/main_window.cpp` and `test/build-make/libhydra_app.a` -- a build
artifact of one system read as source by another. That went away with the
archive.

### The build system, decided: qmake and fmake, no CMake

Three ways to build the same tree had been measured and none chosen. The
decision is two: **Makefile + qmake** as the one people run, and **fmake** as
the second opinion. CMakeLists.txt and test/CMakeLists.txt are gone.

**What each does now.** `make` runs qmake against `hydra.pro` and builds into
`build/`; `make test` drives `test/Makefile`; `make android` runs the Android
kit's own qmake and then androiddeployqt. `fmake -C src` builds the same
sources from no build file.

**qmake is re-run on every `make`, deliberately.** `hydra.pro` globs
`src/*.cpp` and qmake resolves a glob once, when it generates the Makefile --
unlike CMake's `file(GLOB CONFIGURE_DEPENDS)`, which re-checks. A new source
would otherwise stay invisible until somebody happened to re-configure, which
surfaces as a link error naming a file nobody touched. Re-running costs about a
second and is what makes globbing safe.

**The APK was the one thing CMake held alone, and it was the risk in this
change.** The kit ships `qmake` beside `qt-cmake`, so `make android` drives
that instead. It built, and the artifact was *checked rather than trusted* --
which is the failure `harmonization.md` records from beerssh, where signing
flags passed to Qt's generated Makefile were silently dropped and a debug-signed
APK came out under a message announcing a release build. What was verified:
`lib/arm64-v8a/libhydra_arm64-v8a.so` present and the only ABI, `se.vibes.hydra`
in the binary manifest, all three of our Java classes in the dex, and an
Android Debug signing certificate from `apksigner`.

**The Java check nearly passed vacuously.** Grepping `classes.dex` for
`HydraWebView` found nothing, which reads exactly like a build that dropped
`ANDROID_PACKAGE_SOURCE_DIR`. The package is multidex: the classes are in
`classes4.dex`. A control -- grepping for a Qt class that must be present --
is what separated "not there" from "not looking in the right file", and is
worth keeping in any check of a package's contents.

**What was lost with CMake**, so it is not rediscovered as a regression: the
`-DCMAKE_CXX_FLAGS` route for SANITIZE, replaced by qmake's own `CONFIG +=
sanitizer sanitize_address sanitize_undefined`, which has the advantage of
putting the flags on the link line too. And `QT_HOST_PATH`, which was a
`qt-cmake` argument the kit's qmake does not need.

**Binary sizes are not comparable across the three without saying what was
measured.** qmake's release build is 1.8 MB, fmake's is 44 MB, and CMake's
MinSizeRel was 35 MB -- the difference is debug information, not code. fmake
defaults to `-Os -g`. All three link the same five optional libraries, checked
with `ldd`.

### Where this stands, and what is deliberately unfinished

Written down because the reasoning is expensive to rebuild and cheap to record.

**The chrome pass** closed, in order: two empty states that could not say which
kind of empty they were, navigation buttons that offered what they could not do,
a window title that never named the page, no sign that a page was loading, no
way to stop one, no find-in-page, no zoom, no link target on hover, a tab whose
renderer died in silence, a link that opened nothing, a certificate failure that
read as a site being down, and a site asking for a password that nobody
answered. Each is a section below with what it cost and why the answer is the
one chosen.

**The window pass** closed after it, and is *The GUI pass* above: every dialog
this browser has, opened and read, then measured. Nine defects, five of them
found by looking at a picture and four by an instrument built because looking
does not scale. Its own three checks were wrong before they were right, which is
the part worth carrying forward -- on that pass the instrument was wrong about
as often as the code.

**Open, in rough order of how much they matter:**

- **The `src/` source list is no longer duplicated** -- the hand-written half
  went with CMake. Both remaining builds glob.
- **dramafren has not been measured** since the page-row prompt fix; every one
  of five runs timed out at the seven-minute budget, and it wants about 75
  minutes at the fifteen-minute budget on a quiet machine.
- **`try_evolve_confirm`'s real trigger** -- accepting an AI proposal -- is still
  unexercised, because the model returned no proposals within 150s.

**Two things that trip up every session here** and are worth reading before
touching either:

- The style gate counts brace depth, so a continuation line inside an
  initialiser or an argument list needs its **structural tabs first, then
  alignment spaces**. Space-only alignment reads correctly and fails the gate.
  Run `make style` *before* `git commit`, not in the same command, where a red
  result scrolls past.
- Timings taken on this machine are worth nothing unless it is idle. The same
  qmake build measured 142s standalone, 347s under load and 82s idle, and a
  four-fold difference was briefly attributed to a build-system property it had
  nothing to do with.
- **A new source file means `python3 tool/objsets.py` before `make test`.** The
  link sets are generated, so a `.cpp` that gains a dependency -- or a new file
  that some test now needs -- links against a stale set and fails with undefined
  references in a test nobody touched. It happened twice in one session, on
  `test_settings` and `test_annoyance`, and both times the error named a file
  that was perfectly correct. The staleness guard in `test/Makefile` catches a
  *new source*; it cannot catch an existing one that gained an include.
- **And the generator could not be run at all.** `src/tab_history.cpp` was the
  third occurrence, and running `objsets.py` to fix it failed on
  `FileNotFoundError: .../hydra/tests`. The rename to singular directory names
  changed one of the three places this file spelled the test directory and left
  the other two, so the generator listed a directory that no longer exists --
  while `objsets.mk`, which had been corrected by hand, went on describing the
  tree perfectly. **A correct artifact beside a generator that cannot run is
  the worst arrangement of the two**: everything builds, every check passes,
  and nothing reports it until somebody needs to regenerate. The name is
  declared once now. Four more `make -C tests` invocations in `test/Makefile`,
  `test/README.md` and this file were stale the same way.
- **The stale `/usr/bin/fmake` drops link sets silently-ish.** With the
  installed Aug-4 copy the run reported "fmake produced no link set for:
  test_bridge test_empty_state test_extloop test_probe_ui test_settings" and
  refused to write -- five programs that are in the committed file, so the
  regression was visible only because the generator refuses a partial answer.
  Regenerating needs `FMAKE=` pointed at a current build until the installed
  copy is replaced, which needs root. Note the provenance line records the
  path it was run from, and both the previous and current generations name a
  session scratch directory that no longer exists -- honest, and not
  reproducible from the path alone. The mtime is the part that carries.

### One driver that had become three, and the bug that fell out

`try_navigate` started at fifteen checks about the toolbar and reached
sixty-nine across three subjects. A failure named the driver and meant any of
moving between pages, what the window says about the page, or the tools that
act on one. Split by what each is *about*:

    try_navigate    the toolbar and history, Stop, new-window requests
    try_chrome      what the window asserts: title, failures, link target,
                    what survives a tab switch, what is left when a page goes
    try_pagetools   find and zoom -- tools acting on the page in front of you

`shell_fixture.h` holds what all three need: two local pages, a tree holding
both, the window around them, and the widgets a driver reaches for -- all found
the way a person finds them, by placeholder, tooltip or object name. Splitting
cost a preamble rather than three copies of the setup.

**The split immediately found a real defect.** `update_address` echoed the url
into the *status bar* on every navigation. That put the same string on screen
twice a few pixels apart, and worse, made every navigation wipe whatever the
status bar was saying -- the load failure, the refused certificate, the blocked
popup and the dead-renderer notice are all said there, and any of them could be
erased by the next `url_changed`. It had been that way long before those
messages existed, which is why nobody had noticed: there was nothing to erase.
The address bar says where you are; the status bar says what is happening.

**And two of my own mistakes, both worth keeping.**

The certificate suppression was a boolean flag cleared when the next load
reported progress. A load that fails outright can reach `loadFinished` without
ever reporting any, so the flag survived to silence the *next* failure instead
of its own. It is keyed on the url now rather than held as a flag: an unrelated
failure has a different host and speaks for itself, and nothing needs clearing.
Worth recording that the failing test which prompted this had its own bug --
see below -- so the fragility was found by reasoning rather than by the failure
that was blamed on it. The design is still better for needing no lifetime.

`open_tab` returned as soon as the address bar updated, which is `url_changed`
-- when a navigation *commits*, not when it finishes. Sections then ran against
a load still in flight: the progress bar was up, so "no bar once the page has
arrived" failed, and a second navigation started on top of the first swallowed
its own failure message. It waits for the bar to go now, which is what a person
waits for.

**A predicate that matched the thing it was waiting to replace.** The wait for
"could not" was satisfied by the certificate message -- *could not be trusted* --
so the loop fell straight through and reported the application broken when the
test was. Two runs of instrumentation printed nothing at all before that was
visible, because the edits kept landing in the first of two identical loops.
The lesson is the cheap one: wait for the exact sentence, not a fragment that
another message also contains.

### A site asking for a password, and nobody answering

`authenticationRequired` had no handler, so a site wanting HTTP authentication
could not be used at all: Qt reports the challenge through a callback that must
be answered *while it runs*, nothing answered it, and the request was abandoned.
The page then failed with nothing on screen to say a password had been asked
for.

**A decider, not a signal**, following `set_permission_decider`. An answer that
arrives after the callback returns is an answer to a request that has already
been dropped. Declining is still the outcome when somebody cancels -- the
difference is that it becomes a choice a person made rather than the only thing
the browser could do.

The dialog names the site, which is the one thing that has to be checked before
typing a password anywhere. The realm is the site's own label for what it is
protecting; it is often empty or machine-generated, so it is shown when it says
something and dropped when it does not, rather than printing an empty pair of
quotes.

**And it says when the connection is not encrypted.** HTTP authentication over
a plain connection puts the password on the wire in a form anything in between
can read, and only the person typing it can decide whether that is acceptable.
Said in the dialog rather than afterwards, because afterwards the password has
already gone.

Guarded by building the dialog directly and never running it: a modal blocks the
driver, and what is worth checking is what it says before anybody types --
including that the password field is masked, which is the kind of thing that is
correct until somebody refactors it.

`proxyAuthenticationRequired` and `selectClientCertificate` remain unhandled,
and are the same shape of silent dead end.

### A certificate failure that looked like a site being down

Four page-level prompts had no handler at all: certificate errors,
authentication, proxy authentication and client-certificate selection. The
first is the one that matters most here, and it is fixed; the other three are
recorded below as known gaps rather than quietly left.

**The refusal was never the problem.** Qt rejects an unhandled certificate
error, which is the right answer and is what happened before. What was missing
was any account of it. Unreported, the page simply failed -- and once the load
failure message existed, it failed as *"could not be loaded"*, which is exactly
what a site being down says. Those two want opposite responses from a person:
try again later, versus do not type anything into this.

So the message names the host and quotes Qt's own description of what was
wrong. **No click-through.** Letting somebody past a certificate error is a
security decision this browser has not made, and adding one while fixing a
wording problem would be making it in passing.

**The specific message has to win.** A refused certificate fails the load too,
so the generic failure arrives immediately afterwards and would replace "its
certificate could not be trusted" with something vaguer -- losing the only part
that told anybody what to do differently. It is suppressed for exactly that one
load, and the flag is cleared when the next load starts. The driver checks both
halves: that the specific message is shown, and that an ordinary failure after
it still speaks, because a flag that sticks would silence every failure for the
life of the window.

**All four are handled now**, each in its own commit and each recorded in the
sections above. The line that used to sit here listing `authenticationRequired`,
`proxyAuthenticationRequired` and `selectClientCertificate` as open outlived the
first of those by a commit and the other two by a session -- the same staleness
this file warns about at the top, in the file that warns about it.

### The proxy asking, and a site asking who you are (done)

The last two page-level prompts. Both were silent dead ends of the same shape as
HTTP authentication, and each turned out to be a different *kind* of question
underneath, which is why neither could reuse the existing handler as it stood.

**A proxy prompt is not a site prompt, and the box is identical.** Same title,
same two fields, same shape -- and the credentials belong to different parties.
Answering the proxy's prompt with the site's password hands that password to
whoever runs the network, and nothing in the dialog said which was asking. So
the seam grew a *separate* `proxy_authenticator` rather than reusing
`authenticator`: one callback cannot say two things, and the distinction is the
entire safety property. The dialog names the proxy, says in a line of its own
that this is the network rather than the site, and its title says so before the
window is read.

**A client certificate is an act of identification**, which makes the safe
answer the one that has to be reachable. Sending one tells the site who you are,
by name, before anything has been typed -- so nothing is selected when the
dialog opens, `Send` is disabled until something is, and `Don't send` is the
default button and the escape key's answer. That also matches what happened
before: Qt aborts a selection nothing is connected to, so "none" was already the
outcome. The change is that none is now a decision somebody made.

The certificates are flattened to plain strings (`certificate_offer`) before
they cross the seam. A chooser taking a `QSslCertificate` would put Qt
Networking's spelling of a certificate into every backend that ever implements
this, which is exactly what §19.2 exists to prevent.

**Photographing them found what the assertions could not**, for the second time
this session. Thirteen checks passed against a certificate list whose issuer and
expiry ran off the right edge behind a horizontal scrollbar -- hiding the two
facts the row exists to show, on the dialog where the whole question is whether
those facts are acceptable. The list wraps now. The assertion suite reads the
widget tree and cannot see a clipped word.

### A link that opened nothing

Nothing implemented `newWindowRequested`, and in Qt an unhandled request is not
a refusal -- it is a click that silently does nothing. So every `target="_blank"`
link and every `window.open` was a dead end, which is the failure this project
calls the worst kind, sitting underneath a Popups setting that appeared to work.

The setting was half-real, and that is what hid it. `JavascriptCanOpenWindows`
does follow the policy, so script-opened windows were genuinely blocked; a
blocked popup and an unhandled one are indistinguishable from the page, so the
half that did nothing was invisible beside the half that worked.

**Another window is a child tab.** The tree already exists to show what came
from what, and a link that spawns a window is exactly that relationship; a
second top-level window would throw it away and leave two trees to reconcile.

**A click is not a popup**, which is Chromium's rule and the right one: the
setting exists to stop pages opening windows nobody asked for, not to break
links. A user-initiated request opens and is switched to, whatever the policy
says. A script-initiated one is checked against the policy for the page that
asked, and refused *out loud* -- a blocked popup that says nothing is the same
silence this change exists to remove. Allowed script windows open in the
background, because a page that opens a window while you are reading is not
entitled to take the page away from you.

~~`openIn` is deliberately not called. Taking the url and opening it ourselves
drops the opener relationship, which a page can otherwise use to reach back into
the window that spawned it.~~ **Reversed, and see §"OAuth popups, and the opener
that was never wired" for why.** The reasoning above is sound and the protection
is real; what it left unsaid is the price, which is that no OAuth sign-in can
complete anywhere. A window the person asked for gets its opener now, and a
script-initiated one still does not.

### A tab that died and did not say so

`render_process_gone` had exactly one listener: kiosk mode, which reloads on it
so an unattended screen self-heals. Interactively, nothing listened at all -- the
page went blank, with no explanation and no hint that anything could be done
about it. That is the state every other browser fills with a message, and for
good reason: a blank area is indistinguishable from a page that simply has
nothing on it.

**Not reloaded automatically, and that is the difference from kiosk.** A page
that crashes on load crashes again on reload, so an automatic retry turns one
blank page into a loop that also eats the machine. Reload is offered instead,
and it genuinely recovers -- Qt starts a new render process for it -- so the
advice is real rather than a formula.

**The message has no timeout**, unlike almost every other thing this window
says. It describes a state the window is *still in*; one that expires after five
seconds leaves somebody in front of a blank page wondering what they missed. It
is cleared by the thing that makes it untrue: a load starting.

Driven directly by the driver, because a render process cannot be killed from
inside the test on purpose, and the wiring is one line. What is worth checking
is what gets said, that it names the site, that it survives a moment, and that
loading something clears it.

### Five pieces of chrome, four places to hook them, and two lies

The window title, the navigation buttons, the loading bar, the find count and
the link target each say something about the page in front of you, and each was
wired into the same four moments -- opening a tab, switching to another,
suspending one, deleting one -- one addition at a time.

That went wrong twice, and the second time it was already recorded here as a
risk before anything was looked for:

- **The find count outlived its page.** Search for a word, get "3 of 12", switch
  tabs: the bar went on claiming twelve matches for a page it had never
  searched.
- **The link target outlived the pointer.** Hover a link so the status bar shows
  where it goes, switch tabs: the url stayed, now describing somewhere the
  pointer is not.

Neither had anything clearing it, because nothing asked what *else* becomes
untrue when the page changes. `page_changed()` is that question asked once:
fourteen scattered refresh calls become seven, and the seven that remain are
per-signal handlers, which are event-driven rather than page-driven and belong
where they are.

Two details it needed:

- **The status bar is only cleared if this window put a link there.** Clearing
  unconditionally would wipe whatever else it was saying -- "Ready" at startup,
  or the result of the action that caused the change.
- **`clear_result()` rather than `set_result(0, 0)`**, because zero matches is
  a thing worth saying and the absence of a search is not.

**The driver's own assumption broke, which is the useful part.** Adding a second
tab so a switch could be made at all left the final section deleting one tab
while the other was still open -- so it was asserting that the chrome forgets a
page which is still there. The premise had been invisible while the tree held
exactly one tab.

### No way to see where a link goes

A browser whose whole argument is that you can see what a page is doing had no
link target on hover. That is the oldest security affordance the medium has: the
only check on link text that says one thing and points somewhere else, and the
only way to find out before committing to the click.

**The status bar, not a tooltip.** A tooltip follows the pointer and covers the
thing being pointed at. The status bar is where browsers have put this since
before tooltips existed, and it is out of the way of the page.

**Elided in the middle, and that is the whole point.** A long url truncated from
the right keeps its scheme and loses its host -- the only part that answers the
question being asked. Cutting the middle keeps both ends, so the host and the
final path segment both survive.

**An empty url clears rather than expires.** The pointer has left the link, and
a target still on screen after that is a claim about where the pointer is now.

The presentation is a method the driver can call directly, because what is worth
checking is the eliding and the clearing; synthesising a mouse move over an
offscreen page would test Qt rather than this. The engine connection is one
line -- Qt already reports the link and reports an empty string when the pointer
leaves.

### A page that would not stop loading

The progress bar said a page was arriving and there was no way to tell it not
to. A slow site could only be waited out, which is precisely the moment
somebody reaches for the toolbar.

**The Reload button becomes Stop while a load runs**, which is what every
browser does with that slot, and it works because the two are never wanted at
the same moment. One action rather than two, so the Go menu gains it as well
without a second entry that would be wrong half the time -- and `reload_page`
dispatches on which of the two the button currently is, since that is a
property of the moment rather than of the button.

`stop()` joins the seam as a virtual that does nothing by default. A backend
that cannot abandon a load then has a Stop button that would do nothing -- which
is answered by only offering one while a load is actually running, rather than
by a special case.

Switching tabs takes it back to Reload along with the bar: left as Stop, it
would offer to abandon a load belonging to a tab that is no longer in front of
you.

**The driver watches rather than samples.** Even a local file goes through
`loadStarted` and `loadFinished`, so the button is Stop for an interval far too
short to catch by looking at it afterwards. Recording every change to the action
and asserting that Stop appeared at some point catches the transition however
fast the page arrives -- and the recorded sequence is in the failure message, so
a break says what it saw instead of only that it did not see what it wanted.

### No way to make a page bigger

`set_zoom_factor` had been in the seam since kiosk mode needed it, and nothing
a person could reach ever called it. So the browser could scale a page and
offered no way to ask.

**A ladder, not a multiplier.** Repeated multiplication lands on levels nobody
chose -- 1.1 three times is 1.331 -- and the way back to 100% then depends on
the route taken rather than on where you are. The steps are Chromium's own set,
so a page zoomed here looks like the same page zoomed anywhere else, and
*Actual Size* is an absolute rather than an undo.

**The current rung is asked of the page, not remembered.** Kiosk mode sets the
factor directly, so an index held in the window would step from a level that is
no longer true. `zoom_factor()` joins the seam for this, defaulting to 1.0 for a
backend that cannot scale -- which leaves a caller stepping up from "whatever it
is" starting exactly where it would have anyway.

**Per tab, and remembered across suspension.** That is the behaviour that makes
zoom worth having: one site wants 125% permanently and the rest do not, and a
zoom that resets on every navigation gets redone forever. 100% is stored as the
*absence* of an entry rather than as an entry, or the map grows a row for every
tab ever looked at.

The driver reads the level back through the seam rather than from anything the
window believes, because a window that thinks a page is at 125% when it is not
is precisely the bug worth catching.

### No way to search the page

A browser with no find-in-page is missing the affordance people reach for
first, and this one had none: `findText` appeared nowhere in the tree.

The bar is its own widget (`find_bar`) rather than more bulk in `main_window`,
and it **knows nothing about the engine**. It emits what was typed and which
direction was asked for, and it is told how many matches there were; the one
engine-specific part, `findText` and its result, stays behind the WebViewBackend
seam. `find_text` is virtual with a default that reports no matches, so a
backend without search shows an honest empty result rather than needing a
special case.

Details that are each a decision:

- **It searches as you type**, which is what makes it feel like a search rather
  than a form. Every keystroke is a *fresh* search -- the term changed, so the
  engine restarts instead of advancing -- and only the bar knows which of those
  two just happened, which is why `fresh` is a parameter.
- **"No matches" is said out loud.** A blank label beside a term somebody just
  typed reads as the search not having run.
- **Closing it clears the term**, because that is what drops the engine's
  highlight. Hiding the bar alone leaves the page marked up for a search
  nothing on screen still refers to.
- **Icons from the style, not characters.** The toolbar already learned this:
  a phone font had no glyph for the reload character and the button drew an
  empty box.

**And it found a shortcut clash.** `Ctrl+F` was already bound to *Find in Tree*,
the sidebar filter. Two actions sharing a `QKeySequence` is an ambiguous
overload and Qt then fires neither reliably, so the first version of this broke
the tree filter as well as its own.

`Ctrl+F` now searches the page, which is what it does in every browser and
therefore what somebody arrives already expecting; the tree filter moved to
`Ctrl+Shift+F`. That was asked rather than assumed, because it takes a binding
away from somebody using it daily -- the kind of change that is cheap to make
and expensive to discover. The driver checks that exactly one action holds
`Ctrl+F`, that it is the page one, and that the tree filter still has a
shortcut of its own.

### Nothing said a page was loading

The seam had no notion of loading at all. On a slow site nothing in the window
moved, so the browser looked frozen rather than busy, and a load that failed
outright said nothing whatsoever -- for the cases Chromium handles without
drawing its own error document, the window simply kept showing the previous
page.

`load_progress` and `load_finished` join the seam beside `history_changed`, and
a backend with no notion of either just never emits them. The bar lives in the
status bar and is **only on screen while something is loading**: a bar that is
always present and empty is a permanent claim that something is happening,
while one that appears is the only thing on that bar that moves, which is what
makes it readable out of the corner of an eye.

Three details that are each a bug avoided:

- **Only the current view's progress counts.** Every view emits, and a
  background tab finishing its load would otherwise drive the bar for the page
  in front of you.
- **Switching tabs hides it.** Left showing, it would report the previous tab's
  load against the new one for as long as that took.
- **The failure names the host, not the url.** What failed is a site, and a url
  long enough to be interesting is long enough to push everything else off the
  status bar. A `file://` url has no host, so the message falls back to "That
  page could not be loaded" -- which is what the driver observes, since it fails
  a local file deliberately.

### A window that would not say what it was showing

Every window was called "Hydra" and nothing else, whatever page it held. Two of
them side by side are then indistinguishable in a task switcher, and a tab
opened five minutes ago cannot be found again from a window list -- which is
what a window list is for.

The page's own title where it has one, the host where it does not: a document
with no `<title>` is usually one where the address is the only thing
identifying it. Truncated at 70 characters, because a title is a label rather
than a document -- some pages carry a paragraph in theirs, and a window manager
handed one either elides it in the middle or pushes everything else out of the
switcher.

Driven from the same four places the navigation state is, plus `title_changed`
itself, and the tab-open path had to be added separately: it sets the current
widget and syncs the page context without going through either teardown
function, so a change made to those alone left the title stale on every tab
switch. That is the third time this session that a piece of window state needed
hooking in exactly these places, which is an argument for one function that
does all of them rather than four call sites that must each remember.

### Three build systems for the same tree, measured

hydra was the only private project that needed CMake, and the accounting for
what dropping it would cost was written from CMakeLists.txt rather than from a
trial. Two thirds of it was wrong. So the app was converted to qmake, the test
tree to plain Make, and both were measured against fmake on a copy.

**What each one had to be told.**

| | app sources | Q_OBJECT headers | Android sources | optional features | test programs |
|---|---|---|---|---|---|
| CMake | listed, 119 lines | AUTOMOC | `if(ANDROID)` | 4 x `pkg_check_modules` | 38 source lists |
| qmake | `$$files()` minus platform | `HEADERS` must be complete | one `else` branch | 4 x `packagesExist` | -- |
| Make | `$(wildcard)` minus platform | `grep -l Q_OBJECT` | `filter-out` | 4 x `pkg-config --exists` | globbed |
| fmake | nothing | nothing | nothing | 5 one-line comments | found them itself |

**What it costs to compile.** The same 70 test binaries, plus the app:

    CMake   434 objects   (68 app + 366 tests)
    Make    287 objects   (109 app + 178 tests)
    fmake   181 objects   (one tree, every TU compiled once)

CMake's test tree compiles `local_proxy.cpp` twelve times and `policy.cpp`
eleven, because each suite names its own sources and each target compiles them
again. The Make conversion builds one `libhydra_app.a` and lets the linker
choose members, which deletes both the duplication and the thirty-eight source
lists -- a static archive contributes only what resolves something, so a suite
still links only what it reaches. fmake reaches the same answer from symbol
tables with no archive at all.

**The binaries agree.** Stripped: CMake 2,045,928 bytes at `-O3`, qmake
1,828,728 and fmake 1,783,544 at `-O2`. All three link the same four optional
libraries and the same seventeen Qt modules. Every offline suite built by Make
produces the same result as the CMake-built one, including the six that fail
for want of a local helper -- identical counts, suite by suite.

**Two traps, one per system, and both are the same trap.**

`annoyed_dialog.h` and `cosmetic_filters.h` carry `Q_OBJECT` and are *absent*
from the CMakeLists source list. AUTOMOC finds them anyway, through the
same-named `.cpp`. A hand-written `HEADERS` transcribed from that list would
have inherited the omission and failed at link with an undefined vtable, which
is a confusing error a long way from its cause. Globbing the directory is what
makes the difference stop mattering -- in both qmake and Make.

fmake's directives are Doxygen comments, so `//` is not enough: `//!` is. The
first five annotations did nothing, and the only reason that was visible in
seconds rather than at link time is that fmake kept printing the diagnostic
naming the remedy. A build system that reports what it cannot see is worth more
than one that guesses.

**What is still CMake's alone: the APK.** `qt-cmake` drives androiddeployqt,
and neither the Make conversion nor fmake packages an Android app. The `.pro`
carries the Android block -- package source directory, version name and code --
but it has not been run against a kit, so it is written and unverified. beerssh
already ships a Qt 6 app and an APK from qmake, so the path exists; it has not
been walked here.

### Report-only drivers are not failures

Every sweep this session ended `failed=2 try_flicker try_settings`, and neither
had failed. Both capture screenshots and timings for a person to read; neither
prints a `N passed, M failed` line, and the sweep judged them on one. Two lines
of noise in every summary is how you learn to skip the summary, and the day one
of them breaks for real it will look exactly as it does now.

The sweep moved out of a scratch directory into `test/live/sweep.sh` (and
`make sweep`), globbing whatever drivers were built rather than naming them, and
it now distinguishes three outcomes rather than two: a result line that says
zero failures, no result line but a clean run to `done`, and anything else.

The same conflation had already cost a wrong answer once today in the retry
harness, which reported `no retry needed` for run 3 -- a retry that had fired
and then timed out. Reading the five logs directly rather than the summary they
produced is what turned a 5-run measurement back into a 1-run one.

### Deleting a tab, and the cap that quietly stopped counting

The tree learned to delete this session, and deletion was the one operation
that had nothing on the other side of it. `suspend_node` was the only place
that ever cleaned up `m_views_by_id`, the LRU and the stack -- and nothing at
all ran when a node was removed. A deleted tab's view stayed in the stack, its
id stayed in the map, and `state/<id>.blob` outlived it for good.

The leak is the boring half. `enforce_live_cap` picks a victim from the LRU and
then resolves it against the tree:

    if (node *n = m_model->node_by_id(victim))
        suspend_node(n);
    else
        break;

So the *first* deleted-but-still-live tab the cap tried to evict ended the loop
-- and not just that once. The entry stayed in both structures, so every later
call found it again and gave up again. **One deletion switched the four-view
cap off for the rest of the session.** The symptom is memory, hours later, with
nothing pointing anywhere near a delete.

Two changes: `about_to_remove(node *)` on the model, connected to a
`forget_subtree` in the shell that walks the node *and its descendants* --
deleting a folder takes live views with it -- closing each view, dropping it
from the map and the LRU, and removing its state blob. And the cap now drops an
unresolvable victim and continues instead of breaking. That path should now be
unreachable; it is belt and braces, and it is the half that was dangerous.

The blob removal is not tidiness. `unused_id` only avoids collisions with what
is *in the tree*, so an orphaned blob is unreachable by everything except a
reused id -- which is precisely the one way it could ever be read again, into
somebody else's tab.

`state_store` already had `remove()`. I did not find it because I grepped for
`bool remove` and the header is column-aligned: `bool       remove`. The
compiler caught the duplicate immediately, but the same miss in a read-only
pass would have been a confident "there is no way to delete a blob".

**The driver was made to fail before it was believed.** `try_delete` passed
11/11 first time, which is worth nothing on its own -- vacuous checks have gone
green in this project twice already. Backing both changes out and rebuilding:

      FAIL  its saved state goes with it
      FAIL  the whole subtree's views are closed (4 live)
      FAIL  six more opened after the deletions, still four live (10)

Ten live views against a cap of four. That third line is the whole reason the
section exists, and it is the one no offline test could have produced.

### ANDROID_ABI named the apk instead of choosing it

`make apk ANDROID_ABI=x86_64` produced an **arm64 apk called x86_64**. The
variable was interpolated into the output filename and nowhere else; the
architecture came from whichever kit `QT_ANDROID_ROOT` happened to point at,
and its default pointed at an arm64 kit. The Makefile said so in a comment --
"only used to name the copied apk" -- which documents the trap rather than
removing it.

**It is the emulator case that makes this expensive.** A desktop emulator is
x86_64 and a phone is arm64, so the person most likely to set the variable is
the one about to be lied to, and `adb install` then fails complaining about the
package rather than about the architecture. The session recorded above at
§"Android: it runs, and it browses" lost time to exactly that.

**ANDROID_ABI now selects the kit.** Qt's kit directories are named for the ABI
in Qt's spelling rather than Android's (`android_armv7` for `armeabi-v7a`), so
the four are mapped explicitly and an unlisted ABI is refused by name. The kit
itself is discovered under `QT_ROOT` newest-Qt-first, because the default was
`Qt/6.11.1/...` -- a version this machine does not have, so the default always
had to be overridden anyway.

**The kit is asked what it is, not read off its path.** `qmake -query QT_ARCH`
was the obvious source and was tried first: all four kits here answer
`**Unknown**`. What is reliable is the Qt6Core they ship --
`libQt6Core_arm64-v8a.so` -- which carries the ABI in Android's own spelling
and is the same string that lands in the apk's `lib/` directory. A kit that
will not say what it is now fails rather than passes.

**And the apk is opened before it is named.** The old name was the only thing
that ever asserted an architecture, which is what kept the bug invisible; `make
apk` now reads `lib/<abi>/` out of the zip and refuses to write the name when
it disagrees, carries more than one ABI, or carries no native code at all.
python3 rather than `aapt2`, so the check runs wherever the build does instead
of depending on a build-tools version the Makefile would have to choose.

Build directories are per ABI (`build-android-<abi>`) for the same reason: one
shared tree meant switching ABI reused the previous architecture's generated
Makefile and objects, which would have made the fix look like it had not
worked.

Verified against the four kits in `~/Qt/6.10.0`: each ABI resolves to its own
kit and reports it; `ANDROID_ABI=x86_64 QT_ANDROID_ROOT=.../android_arm64_v8a`
-- the original foot-gun -- exits 2 naming both sides; an unknown ABI, a
missing kit and a directory with no qmake each fail with their own message. The
apk check was exercised against synthetic zips carrying one ABI, two, and none.
**What is not verified is a real build**: this machine has the kits but no NDK
or JDK, so `make android` gets as far as the NDK check and stops.

### Logged out of corporate SSO on every restart

Reported against Teams, and specifically not against other logins, which is the
clue rather than an aside. `qtwebengine_factory.cpp` sets
`AllowPersistentCookies`, and Qt's three policies differ exactly here:
`NoPersistentCookies` keeps everything in memory, `ForcePersistentCookies`
saves session and persistent cookies to disk, and **`AllowPersistentCookies`
saves only the cookies that carry an expiry -- session cookies live in memory
and die with the process.**

Corporate SSO issues its authentication as a **session** cookie by design, so
this browser discards precisely the cookie Teams needs while keeping every
ordinary login. Other browsers appear to behave differently because "continue
where you left off" restores session cookies, which is `ForcePersistentCookies`
behaviour under another name.

**Not changed, because it is a privacy decision rather than a defect.** The
policy is per *profile* in Qt, so flipping it makes every site's session
cookies outlive the browser -- and a session cookie surviving a restart is a
deliberate weakening of something sites rely on, which sits oddly in a browser
whose posture is a per-site policy engine. Three options, put to the copyright
holder and unanswered at the time of writing: flip it globally, one line;
flip it and expose the old behaviour as a setting; or make it per-site through
the policy engine, which **cannot be done with this enum** since Qt has no
per-site form of it and would need real design.

### Saving state on a signal and on a timer, designed and then built

Asked for after the view-state work, because `closeEvent` was the only writer of
the view state and the tab blobs and a browser that is killed or crashes lost
them. The tree was the exception and this entry originally overstated it: the
debounce timer had usually just written that. The copyright holder clarified
that "kill" means SIGTERM rather than SIGKILL, which splits the problem in two
and makes most of it tractable.

**The design below was written before any of it existed, and every part of it
survived contact with the implementation** -- which is the reason to leave it
standing rather than replace it with an account of the code.

**SIGTERM, SIGINT and SIGHUP are catchable, and a handler still must not
save.** Only async-signal-safe calls are legal there, so `QSettings`, the
model and anything allocating are all out. The shape that works is Qt's own:
write a byte to a self-pipe in the handler, wake a `QSocketNotifier`, and do
the real save on the Qt thread. Getting this wrong produces a deadlock or a
half-written file at exactly the moment the system is shutting the program
down, which is the worst time to find out.

**SIGKILL and a crash cannot be caught, so a timer is the only cover** -- and
the timer is what makes atomicity mandatory rather than nice. Write to a
temporary file and rename over the target: an interrupted save that truncates
`tree.txt` is worse than a stale one, because the stale file is at least a
tree. Track dirtiness so an idle browser is not rewriting itself on every tick.

**All of it is implemented**, and the condition this entry set for building it
-- that an untested signal handler is a way to corrupt the tree rather than a
safety net -- was met before it was: see *Saving on a signal, built* and *The
blobs, measured first and then built* below. Signal handling, the atomic
writers, the view-state timer and the blob checkpoint are all in, each verified
against a real SIGKILL.

Two things the design got right and one it did not. The self-pipe shape and the
insistence on atomicity were both correct and both load-bearing -- the atomic
writers turned up three faults worse than this entry assumed, including a
`save` that could never report failure and a policy write that deleted the file
before rewriting it. What it did not anticipate is that the cost it feared was
never measured: serialising a tab's history is 10us and 815 bytes per entry, so
the timer half it treated as the expensive half is the cheap one.

### Dark desktop, light browser -- and it is the desktop, not this tree

`theme.h` already carries a three-tier detector, and on this machine all three
tiers abstain. Measured 2026-08-31:

    XDG_CURRENT_DESKTOP=TDE          Trinity, the KDE 3 fork
    QT_QPA_PLATFORMTHEME             unset -- no Qt 6 platform-theme plugin
    org.freedesktop.portal.Desktop   not provided by any .service file
    gsettings color-scheme           'default', i.e. no preference

`QStyleHints::colorScheme()` returns `Unknown`, the portal cannot be asked at
all, and with no platform theme Qt hands the program its **default light
palette** -- so the third tier reads light and is not malfunctioning, it is
reading Qt's fallback rather than the desktop. That tier was written to catch
"a dark GTK or Qt theme with no portal", and this is the case it cannot see:
there is no dark palette anywhere for it to find.

**The desktop does state its colours**, in `~/.trinity/share/config/kdeglobals`
(and `~/.kde/share/config/kdeglobals`, same format, for KDE 3):

    colorScheme=DarkBlue.kcsrc
    windowBackground=0,42,78
    windowForeground=220,220,220

**The fix does not touch `decide()`.** That function is pure over (Qt hint,
portal int, palette), and what is missing is a *source* for the middle
argument, not new logic -- a reader returning the portal's own vocabulary, 1
prefer dark, 2 prefer light, 0 no preference. Two things a naive version gets
wrong: compare the colours rather than the scheme name, since `DarkBlue.kcsrc`
happens to contain "Dark" and plenty of dark schemes do not; and abstain rather
than guess when the file cannot be read, because the errors are not symmetric
-- this file already records that a wrong light guess is merely plain while a
wrong dark guess is unreadable text.

**Implemented as tier 4, after the workspace settled the rule.**
`theme::color_scheme_from(sources)` reads `windowBackground` against
`windowForeground` under `[General]` and returns the portal's own vocabulary,
so `decide()` did not change at all -- the missing piece was a source for its
middle argument, exactly as this entry predicted. Measured against the real
file: background luminance 35.7 against foreground 220.0, which is 1, prefer
dark.

**The test carries both directions and the reason it has to.** A fixture that
only shows dark-in-dark-out cannot separate a working comparison from a
function that returns 1, so the light case is the same file with the two
colours exchanged and the scheme still named `DarkBlue.kcsrc` -- the only thing
that can tell them apart is the luminance test. A third case proves a later
`[konqueror]` section does not override `[General]`, and a fourth that an
unreadable file abstains.

**Signalled first, then implemented here**, on the copyright holder's
instruction:
the cause is the desktop, so every Qt GUI in the workspace fails identically
and a per-tree fix is the same work done many times with many results. Open
alongside it, and bigger than detection: whether a program should follow the
desktop's *hue* at all -- this scheme is dark blue rather than neutral dark,
and adopting `0,42,78` as a window colour is a different question from knowing
the desktop is dark.

### The shield was a wall of eighteen controls in enum order

It drew one row per feature in declaration order, so the panel opened as a flat
list with the camera three rows above the referer header and nothing saying any
of them were related. It has a layout table now, in the order somebody reads it
rather than the order the enum happens to declare.

**The last group is a request, and it is worth being accurate about what is in
it.** "Blocking that needs rules" holds `ads`, `popups` and `cookie_notices`,
and only two of the three earn the name on the implementation: `ads` consults a
built-in host check and then a filter list that is empty until somebody imports
one, and `cookie_notices` runs from `site_rules::defaults()` plus what has been
added since -- while `popups` is enforced outright in `main_window` and needs no
list at all. It sits there because it belongs with the other two in a reader's
mind, which is a good enough reason for a panel and not a good enough reason to
let the comment imply otherwise.

**The table is checked against the enum at construction**, every feature
exactly once, and warns where somebody will see it if not. That guard is not
hypothetical: `settings_dialog`'s equivalent table -- written the same way, by
hand, with no check -- is missing `extractor_fetch`, `clipboard_read`,
`pointer_lock` and `media_detect` today, so four controls cannot be reached
from the settings page at all. Nothing reported it, because no assertion can
fail about a row nobody wrote. That is the whole argument for the guard, and
the four missing rows are a separate defect still open.

### The window remembered nothing about itself

Reported as three symptoms -- folders opening and closing on their own, things
moving, and the tab you were on not coming back -- and they are one gap.
`closeEvent` wrote the model and the policy and stopped: no open-folder set, no
current tab, no sort selection, no geometry. `node` has no `expanded` field and
nothing anywhere held a current-tab id. The model was saved; the **view** never
was. `load_tree` then called `expandAll()`, so even a saved arrangement would
have been flattened on the way in.

`view.ini` now sits beside `policy.ini`, for the same reason that one is a file
somebody can read: an arrangement of somebody's own windows is theirs to
inspect and delete.

**Folders are recorded by id, not by row.** A row number is a position in a
tree that reorders itself -- sorting moves it, and so does any new sibling --
so a saved row restores the wrong folder as soon as the shape changes. An id is
"short, opaque, stable for the node's lifetime" by `node.h`'s own promise,
which is exactly the property needed.

**Two things that would each have made it look like it did nothing.** The
restore collapses before it expands: without that it could only ever add
folders, so a tree that started expanded would stay expanded and the setting
would appear inert. And `isExpanded` takes the index the *view* uses, so every
lookup goes through the sort proxy -- handing it a source index reads as "no
folders were open" rather than as an error.

**No file means first run, and first run keeps the old behaviour**: everything
expanded, which is the right thing to show somebody who has never arranged
anything and only wrong as an answer to somebody who has.

### `objsets.mk` cannot be regenerated, and that is why it went stale

**Every live driver has been unlinkable since the `address_line` commit.**
`moc_address_input.o` is in no object set, so the link fails with `undefined
reference to vtable for address_line`. `make test` cannot see it -- it does not
build the live drivers -- and this is the third instance of that class in this
tree.

**The regeneration is blocked by something else, measured twice.**
`tool/objsets.py` refuses while an Android build is present, which is its own
documented guard and is worked around by moving the directory aside. With it
aside it still fails:

    fmake produced no link set for: test_bridge test_empty_state test_extloop
                                    test_probe_ui test_settings

and the tool correctly refuses to write a partial file. The comfortable
explanation was that a killed concurrent run had corrupted fmake's object
cache; that was tested by clearing `.fmake` entirely and running again, and the
same five targets failed identically. So it is not the cache. All five sources
exist, and none of them references `main_window`, so it is not this session's
edits either. It is a real fault between `objsets.py` and fmake, and it is
almost certainly why the file was stale to begin with -- a generator nobody can
run stops being run.

**Resolved, and not by the explanation that was to hand.** The regeneration
succeeded on the next attempt, and the tempting reading -- that fmake had just
been updated -- does not survive being checked. `/usr/bin/fmake` is
`fmake 1.0 (399762fd)` with an mtime of 2026-08-04 and was not touched that
day. What changed is **which fmake ran**: the header of the file this replaced
records that it was generated by a scratch build of fmake HEAD dated
2026-08-25, and the successful run used the packaged 1.0.

So the failure belongs to that HEAD build rather than to the released one,
which also explains the staleness without anything further: the file was last
written by a build of fmake that no longer regenerates it, so it stopped being
regenerated and drifted until a header gained `Q_OBJECT` and the drift became a
link error. The diff is 34 additions of `moc_address_input.o` and **zero**
removals, and `try_permissions` links again -- the check worth making, since a
regenerated file that still does not link would have proved nothing.

**What fmake wants from this end** is the five target names -- `test_bridge`,
`test_empty_state`, `test_extloop`, `test_probe_ui`, `test_settings` -- and
that they produce no link set under a 2026-08-25 HEAD build from a cleared
cache while the packaged 1.0 handles them. It is a sibling project, so that is
signalled rather than fixed from here.

### `BIN=` on the sweep's command line does nothing, and starts a real sweep

**Measured by doing it.** `test/live/sweep.sh` reads
`BIN=${HYDRA_SWEEP_BIN:-test/build-make}`, so an assignment of `BIN` on the
command line is overwritten by the script's own line before it is used. It does
not error and it does not warn: it runs the full live sweep against the default
build, which is minutes of real browser drivers, when what was intended was a
scratch build of one of them. The name that works is `HYDRA_SWEEP_BIN`, and
`HYDRA_SWEEP_OUT` is its counterpart for the output directory.

What stopped it was a `timeout -s KILL 25` wrapped round the invocation on
general principle, not anything in the script -- `SWEEP_TIMEOUT` defaults to
300 seconds **per driver**, so the outer bound on a sweep nobody meant to start
is the driver count times five minutes.

**The shape is worth more than the instance**: a variable whose name is the one
the script uses internally is not an override, and assigning it looks exactly
like configuring the thing. Read where a script gets its defaults before
setting one, and prefer the prefixed name where a script offers both.

### Android SDK levels, and the one path the test device cannot reach

`android/AndroidManifest.xml` builds at **targetSdk 36, minSdk 28**, and the
handset available for testing is a Note 9 at **SDK 29**. That gap is fine for
almost everything and matters in one place.

`POST_NOTIFICATIONS` became a runtime permission at API 33. Below it the
permission is granted by installing, which is the case on the test device, so
every measurement of the playback service's notification here was taken on the
half of the world where it cannot be refused. On a phone at 33 or above a
refusal hides the notification **without** stopping the service -- the sound
keeps playing and only the label for it is missing -- and nothing in this tree
has ever exercised that path. It is not a defect and it is not verified; it is
the difference between the device we have and the one the manifest targets.

### Measuring on the handset, since nothing else can

Android is not reachable from CI -- and since 2026-08-14 nothing is -- so a
physical phone is the only instrument for anything the emulator cannot show.
The method is written down because every part of it was arrived at the
expensive way, and a session that has to re-derive it spends an hour before it
measures anything.

**Read one bit, not the screen.** The phone in use is the copyright holder's
daily handset and its screen shows their mail, their maps and their banking, so
screenshots are not an available instrument here. Almost everything wanted can
be had as a single bit from `dumpsys` instead. To find where the address bar
is, tap a candidate and ask whether a text field took focus:

    adb shell input tap 500 170
    adb shell dumpsys input_method | grep -m1 mInputShown

`mInputShown=true` says the tap landed in an input. That located the bar in
four tries on a 1080x2220 override resolution without looking at the display.

**A dozing phone reads exactly like a regression.** Time was lost to an app
that appeared to have stopped responding and had not -- the screen had gone to
sleep, partly from repeated `force-stop`s. Check the phone is awake before
believing anything it does:

    adb shell dumpsys power | grep -m1 mWakefulness=     # expect Awake
    adb shell svc power stayon usb

**`adb shell input text` does not decode anything.** Writing `%3F` for a
question mark types the three characters, so a url with a query string arrives
mangled and the address bar sends it to a search engine -- which looks like the
address bar being broken. Prefer a url with no query string for a fixture.

**Fixtures that work, and the ones that wasted time:**

| | |
|---|---|
| `https://www.w3schools.com/html/mov_bbb.mp4` | ~10s, video **with** audio |
| `https://ice1.somafm.com/groovesalad-128-mp3` | endless audio, for anything needing more than ten seconds |
| a `test-videos.co.uk` jellyfish clip | **silent** -- video only, so an audio measurement reads as failure |
| `commondatastorage.googleapis.com/gtv-videos-bucket/...` | 403 now |

The mp4 autoplays only because the profile under test permits autoplay; a
profile with it blocked needs a tap on the video first, and the difference is
invisible in the measurement.

**An app op is a control that needs no rebuild.** A foreground service can be
refused without touching the code, which is what made the background-audio
control a same-build comparison rather than two builds:

    adb shell cmd appops set se.vibes.hydra START_FOREGROUND deny
    ... measure ...
    adb shell cmd appops set se.vibes.hydra START_FOREGROUND allow

The refusal surfaces as `SecurityException: foreground not allowed as per app
op`, so the log says plainly that the service did not start -- which is what
turns "these two runs differ" into "they differ by exactly this". Restore it
afterwards: it is the holder's phone, and a denied op left behind is a setting
they did not choose.

### `site_extractor.cpp` is the workspace's lexer fixture, by accident

**Do not reformat this file without knowing what it is used for.** It is the
only file in this workspace where two candidate implementations of the style
gate's C lexer visibly disagree, and it decided both of the 2026-08-30 lexer
questions: it chose the correct restore for a closing brace that had been
discarding the braceless bodies open around it, and it rejected the wrong fix
for a lambda's opening brace eating one it does not own.

**Why this file and not another**, measured rather than assumed: it carries 21
braceless `if`/`for`/`while` bodies, 4 lambdas with brace bodies, and 39
aligned continuations spelled tabs-then-spaces, in 612 lines. Both faults turn
on brace bookkeeping across exactly that combination, and nothing else here
holds all three densely enough for the two implementations to diverge. It
conforms under the current gate.

That makes it a discriminating fixture in the sense `evidence.md` means:
agreement between two implementations is evidence only where a case exists that
would separate them, and this is that case. Reflowing it -- adding braces to
the braceless bodies, or converting the aligned continuations -- would destroy
the property silently, because the file would still conform and nothing would
report a loss. The next person to tidy it will not know, which is why this is
written down rather than left in a session.

**The lexer fix itself is `fd6ba14`**, and the lambda fault is recorded as open
rather than fixed: the obvious patch trades it for a different false finding.

### CI has verified nothing here since 2026-08-14

**The runner is blocked on account billing, and a blocked run is coloured
exactly like a failed one.** GitHub reports `failure`, `gh run list` renders it
identically to a real failure, and the annotation is only visible one level in:
*The job was not started because recent account payments have failed or your
spending limit needs to be increased*. Nothing was built.

**The method, so this can be re-taken when somebody doubts it**, since the fact
expires the moment the billing is settled:

    gh run view <id> --json jobs --jq '[.jobs[].steps|length]|add'

Zero means no step ran. Measured 2026-08-31 by binary-searching the run list on
that number:

    last executed:  31765400314  2026-08-14T02:59:27Z  success  22 steps
    first blocked:  31789557779  2026-08-14T09:47:40Z            0 steps

No run sits between them, so this tree's last executing run was green -- it has
no phase of genuine failures to be confused with the block, which a sibling
does. **113 commits have gone to master since**, verified by `make style` and
the suite here and by nothing else.

**Duration screens, `steps: 0` decides.** A blocked run finishes in 3 to 6
seconds where this build's genuine failures took 95 and 389, so a short run is
worth a second look -- but that gap is measured over two failures in a history
that has never contained a fast one, and malformed workflow YAML, an
unauthenticated checkout or a missing action reference each fail in seconds
with steps attempted. Reading the duration alone would file a real defect as a
billing block, which is the direction that sends somebody away from something
true.

**What is specifically unexercised**: `.github/workflows/ci.yml` builds from
`git archive HEAD`, which is the only thing that checks the vendored submodule
path from clean, because the working tree has yt-dlp already in place. That
property has not been tested since 2026-08-14 and cannot be tested here.

The billing is account-level -- it splits on repository visibility, private
metering Actions minutes where public does not -- so it is not fixable from
this tree and is with the copyright holder.

### The licence, removed on the holder's instruction

**This tree declares no licence, deliberately.** Instructed by the copyright
holder on 2026-08-31: develop the browser under no licence until it is
complete, and settle the terms then. It carried GPL-3.0-or-later until that
day. Under copyright law an absent licence grants nothing, so nothing built
here is distributable as it stands -- which is the intended state while a
project is being written, not a gap in it.

**Do not restore it and do not add another.** Not to satisfy a lint warning,
not to match a sibling project, and not because a dependency's terms look like
they want company. A published grant cannot be taken back from anyone who has
relied on it, so a wrong one is not something that gets fixed later; an absent
one leaves every option open. The decision belongs to the copyright holder and
to nobody else.

**Nothing in the dependencies was forcing the old one**, which is the
measurement that preceded the instruction. Every Qt module this program uses
offers LGPL-3.0-only -- read from the SPDX line in the headers actually
compiled against, `qapplication.h`, `qwebengineview.h`, `qwebchannel.h`,
`qqmlengine.h`, `qdbusconnection.h` and `qprinter.h`, all six identical -- and
the binary links 17 Qt libraries dynamically, which is what LGPL relinking
asks for. The optional pkg-config dependencies are ISC (libsodium), BSD-3
(libtorrent-rasterbar) and LGPL-2.1+ (libsecret); liblz4 looks copyleft to a
whole-file grep because its catch-all stanza is GPL-2+, and is not, because
`lib/*` -- the shared library, as against the CLI in `programs/` -- has its
own BSD-2 stanza. yt-dlp and ffmpeg are subprocesses rather than links.

Three things would change that answer and are worth knowing before they are
chosen: a GPL-only Qt module (Qt Charts and Qt Virtual Keyboard are GPL-3-only
in the open-source offering, and this program uses neither), a static Qt build
-- plausible on Android, where the APK ships Qt as shared objects today -- and
redistributing Qt WebEngine itself, which bundles Chromium and carries its
third-party obligations. The holder has said a commercial Qt licence can be
bought if it is ever required.

**How it was removed.** The 179 SPDX header lines came out mechanically, under
a tool that refuses to write unless the file it wrote, with the grant line put
back at the index it came from, is byte-identical to the file it read -- proved
against the disk rather than against memory, since reassembling in memory what
was just removed cannot fail. Its first version did exactly that and was
tautological. The tool carries its own control, classifying a correct removal
and an over-eager one before touching a real file and exiting 2 if either comes
out wrong, because a clean report from a blind check reads like a clean report
from a working one. `git diff --numstat` then said 179 files at one deletion
and zero insertions each, independently of anything the tool claimed.

Seven files were edited by hand and carry no such proof, so they are named
rather than tucked behind the number: `LICENSE` (removed), `debian/copyright`,
`README.md`, the About box in `src/main_window.cpp`, two claims in
`doc/architecture.md` that reasoned from this program being GPL-3-or-later, and
one in this file.

### Audio stopped when the browser was backgrounded

Reported as "the audio stops when the browser isn't in the foreground", and it
was two problems wearing one symptom. Both are fixed, and the order in which
they were found is the point.

**A foreground service was the obvious answer and was not the answer.** The
first measurement said what it was not: Android's audio service showed hydra's
player going from `state:started` to `state:stopped` seconds after HOME, while
the process stayed alive at the same pid and the window log said
`destroySurfaces: appStopped=true`. Nothing had been killed, so this was not the
out-of-memory problem a foreground service is usually reached for.

The service was built anyway, because an app in the background is not entitled
to sustained playback without one, and with it running -- `mFgServiceShown=true`
against the `hydra.playback` channel, the notification on screen -- a video's
audio still stopped two seconds after backgrounding. **An audio-only stream on
the same build played on indefinitely.** That pair is the diagnosis: Chromium
suspends a video whose window has become invisible, and an audio element has
nothing to suspend. So the service is necessary and was never sufficient, and
`Player.onWindowVisibilityChanged` in `HydraWebView.java` is the second half --
it withholds the invisibility while, and only while, a foreground service is
held.

**Gated on the service on purpose.** Ungated, that override is a browser that
never stops decoding because it happens to be on a page with a video, which is
somebody's battery. Gated, the browser keeps playing exactly when it has told
the system it is playing, with the notification saying so on screen throughout.
The withheld visibility is handed over the moment the service goes down, or the
engine would believe its window was still on screen -- the same battery cost
arriving by the back door.

**One instrument cost an hour and is worth naming.**
`dumpsys activity services se.vibes.hydra` lists the Chromium sandbox service
and not this one, so a package-scoped grep reported the service absent while it
was running and its notification was on screen. An unscoped
`dumpsys activity services | grep PlaybackService` finds it. The probe was
validated the only way that works -- run it while the service is definitely up
and confirm it says so -- before its silence was read as an answer.

**Measured with a control that discriminates**, same build, same page, on the
Note 9:

| | watcher | service | +2s background | +4s |
|---|---|---|---|---|
| app op `START_FOREGROUND` allow | reports playing | started | `started` | `started` |
| app op `START_FOREGROUND` deny | reports playing | `refused` | `stopped` | `stopped` |

Denying the app op leaves everything else identical and makes the service throw
`SecurityException: foreground not allowed as per app op`, so the only
thing that varies is whether the service is held.

**The first attempt at that control was vacuous and said so in its own log.**
The two halves disagreed as hoped, but the log showed the watcher had never
reported playing in the control run at all -- so the difference could have been
the script rather than the app op. The cause was a real defect: the watcher only
sampled on media events, and a cached video autoplays before the script is
injected, since injection is at `onPageStarted` and not at document start. Its
`playing` event had already been and gone. The watcher samples immediately on
injection now, and twice more on a short timer.

**What tells the service to run is Android's observation, not the page's
claim** -- but by way of the page, which is the compromise this needed.
`AudioManager.registerAudioPlaybackCallback` is the device's own answer and
anonymizes what it reports about players the app does not own, so it can say
"something on this device is playing" and not "this tab is playing". The
injected watcher asks the document how many media elements are actually
playing, recomputed rather than counted: a play/pause counter drifts when an
element is removed mid-play and its `ended` never comes, and it ends up holding
a notification over a silent browser for ever. It does not see media inside a
cross-origin iframe, which keeps the old behaviour there.

`PlaybackService.java` is tab-indented while the three Java files beside it are
space-indented. The global rule is tabs and Java is not in `.style-gate.toml`'s
`indent_suffixes`, so nothing has ever gated these files; converting the other
three is a deliberate pass rather than something to do inside a feature.

## Five defects in the tab tree, reported as "it is unreliable"

Reported from use, in two sentences: sometimes a tab does not update its text,
sometimes it does not open the correct page. Both turned out to be one defect,
and looking for it found four more. Worth recording as a group because the
report was vague in a way that was *earned* — three of the five only misbehave
in one of the directions you might do the thing, which is exactly how a pile of
specific bugs reads as general unreliability.

### One defect wore both reported symptoms

`add_tab` makes a row with no address on purpose — that is what a new tab is —
and `open_node` refused any node whose url was empty, so nothing was ever shown
for it. Nothing said so. The row appeared in the tree, and the stack went on
showing the previous tab.

From there both symptoms follow. `current_view()` derives from the stack rather
than from the tree, so it kept answering with the tab *before* the new one.
`new_tab` focuses the address bar — an empty tab is a question about where to
go — and `navigate_to_address` loaded whatever was typed into that stale answer.
So the new row never loaded a page and therefore never changed the title
`add_tab` gave it, **and** the address typed for it opened somewhere else.

The guard is about being a *folder* now, which is the thing that genuinely has
no page; an empty tab opens `about:blank`. That had a tail: Chromium reports the
address as the title of the blank page, correct of Chromium and useless on a
row, so the tree read `about:blank` until you went somewhere. `set_page_title`
refuses that one name.

### Dropping a row into its own folder put it one place too low

`dropMimeData` was handed a row counted against the list *before* the move, then
removed the node from that same list — which shifts every later sibling down one
— and inserted at the number it was given. One place too far.

Only downwards: removing from below the insertion point changes nothing above
it, and a drop past the last row appends either way. **Three of the four
directions were correct**, which is why this never got reported as an off-by-one.
The case that reads as corruption rather than as a bug is the no-op: dropping a
row back into its own gap moved it. The gesture that means "leave this where it
is" was the one that disturbed it.

### Sibling `order` could collide, and nothing checked

A new node took `parent->children.size()` as its `order` — one past the highest,
true only while nothing has *left* the list. A delete and a drag-out each leave a
gap in the numbering without leaving one in the count, so the next node added
collided with a sibling still sitting there. **Three siblings were measured
holding order 2.**

Two rows with the same order are not in the wrong order, they have *no* order:
`lessThan` reports them equal. Being honest about the damage — the visible
symptom was **not** reproduced. The proxy showed the list in list order before
and after a title arrived, twice. What is established is the collision and its
two real consumers: tree-order sorting has no defined answer for the tied rows,
and `tree_diff` reads a stale `order` as a reorder nobody made. It does not reach
the outline file, which writes children in list order and renumbers on load.

The fix is a `renumber` at every point the list changes. The part worth keeping
is the *other* half: `tree_invariants` now checks that a child's recorded order
equals its position, so `holds()` — which `test_model` runs after every section —
catches any future mutation that forgets. It immediately caught a case reading
had missed, in a test that already existed: a cross-folder move renumbered the
target and left the source parent stale.

`test/tree_gen.h` and `test_invariants`'s private copy of the same generator
both had to be fixed first: they appended without numbering, so every generated
node recorded order 0 and ten thousand siblings all claimed position zero. **The
checker could not be given the rule until the fixtures obeyed it** — a fixture
building a shape no code path can produce was hiding the invariant that says so.

**The two generators are now one** (`test/tree_gen.h`, used by
`test_invariants` and `test_tree_scale`). They had the same three parameters,
the same node ids, and the same docstring arguing for one generator rather than
a pile of fixtures — an argument both copies made while being two of them, and
neither said the other existed. That is what the duplication cost here: the
`order` fix had to be made twice, and the second copy was found only because
fixing the first left its suite still failing.

Nothing was lost collapsing them. The local copy took a `node_type` where the
shared one has `leaf` and `folder`, and titled nodes by their bare id where the
shared one writes `Tab t0_1`; no assertion in the file reads a title, and every
violation it provokes is reported by id. The proof it is the same generator is
the suite's own arithmetic, unchanged and still passing: `build(3, 4, 2)` gives
`2 + 3 + 12` nodes at depth 4, and the 10,208-node scale case round-trips
through the file with the same shape. 20 checks before, 20 after.

### F2 with nothing selected edited the invisible root

`node_for_index` answers the *root* for an invalid index, because
`rowCount(QModelIndex())` has to mean "how many top-level rows". Correct there,
and a trap for a caller holding a view index: `if (node *n = ...)` gets a
non-null answer it cannot tell from a real row. F2 with no row selected opened a
properties dialog for a node nobody can see, where OK sent the typing nowhere.
The view resolves rows through one helper that refuses the root now.

### Opening a tab did not highlight it

Nothing set the current row when a tab opened — only `new_folder` did. So the
row that was showing and the row that looked selected were different rows. Not
merely cosmetic: `selected_parent()` reads the highlight to decide where a *new*
tab is filed, so a tab opened by any route but a click filed the next one under
the wrong parent. `open_node` calls `tab_tree_view::show_node` now, which expands
ancestors outermost-first — the ordering `reopen_folders` had already learned,
because expanding into a folder that is still shut does nothing.

### How far this is verified

`test_model` covers the reorder arithmetic in both directions, at the end, into
its own gap, across parents, and two rows at once; the order collisions by both
routes; and what a page may call a tab. The first version of the reorder section
shared one folder across its cases, so case 1's wrong answer became case 2's
starting position and three cases failed together — one fault reported three
times, with nothing to say which was independently broken. Each case builds its
own list now.

`try_navigate` drives the reported defect through the real window: New Tab from
the menu, the tree's current row, an address typed, the page arriving in the new
tab, and **the previous tab still on the page it was on** — which is the half
that would have failed before. `try_menus`, `try_lock`, `try_rename` and
`try_delete` were re-run because `show_node` changes selection under them; all
four pass.

Offline: 31 suites, 0 failures. Not run: the ten drivers needing a network, a
device or a model.

## The suite failed for a second account, and blamed the code

`make test` reported 22 failures across four suites on a checkout owned by
another user: `test_tree` "it saves", "the folder comes back"; `test_extractor`
"saves"; `test_settings` "a custom command resolves to an executable";
`test_seam` "the job completes". Every one of those names code that is correct.
All four pass unchanged with a `TMPDIR` the runner owns.

Seven suites build a scratch path out of `QDir::tempPath()` and a fixed name --
`hydra-tree-test`, `hydra-model-test`, `hydra-state-test`, `hydra-asm-test`,
`hydra-bundle-test`, `hydra-settings-test`, `hydra-extractors.json` -- and
`test_seam` writes `clip.mp4` into the root of it by that bare name. A
predictable name in a directory shared with every other account on the machine
is two faults, not one.

**It reads as a test failure.** `/tmp/hydra-tree-test` had been created on
14 August by the account that owns the tree, mode `drwxrwxr-x` and group
`funk`; a second uid cannot write into it, `save` returns false, and the
assertion that reports it is about trees. The hour goes into
`tree_outline.cpp`.

**And it destroys somebody else's file.** `test_seam` removes and recreates
`$TMPDIR/clip.mp4` unconditionally, which under a shared `/tmp` is not this
suite's file to remove. That half never announces itself at all.

**The correspondence is exact, both directions, which is what establishes the
cause rather than merely fitting it.** The four suites that failed are the four
that leave their scratch behind -- `hydra-tree-test`, `hydra-extractors.json`,
`hydra-settings-test`, `clip.mp4`, all present in `/tmp` owned by the first
account. The four that passed -- `test_model`, `test_state`, `test_assembler`,
`test_bundle` -- are the four that clean up after themselves, so there was
nothing of anybody else's for them to meet. 4 of 4 and 0 of 4; no suite is on
the wrong side of it.

The fix is `TMPDIR` in `TEST_ENV`, pointed at `$(TESTS_DIR)/tmp`, because
`QDir::tempPath()` honours it and no suite had to change. Under the build
directory rather than a per-uid name in `/tmp` so that `clean` covers it:
twelve stale `/tmp/hydra-*` directories had accumulated here since 14 August,
which is what a name nobody removes does. Absolute, because a suite is free to
`chdir` and a relative `TMPDIR` would follow it.

Verified by where the bytes went: after the change `test/build-make/tmp/` holds
exactly those four, and `/tmp/clip.mp4` still carries its 13:16 timestamp from
the run before -- untouched by a suite that had rewritten it every time until
now. Offline: 31 suites, 0 failures.

**This is the fourth of the same shape in a day**, and the other three are in
`claude-guidelines`' record: a tool taking an operation's refusal, or another
account's ownership, as a fact about the thing it was inspecting. None of the
four was reachable from a single account. The configuration was the
instrument -- a second uid working in trees owned by the first -- and these had
been sitting here for as long as nobody ran them that way. Worth saying because
the instrument is cheap and nothing else in the tree had found them.

`test/live/sweep.sh` has the same shape in its `OUT=${HYDRA_SWEEP_OUT:-/tmp/hydra-sweep}`
and is deliberately left: it is an output directory meant to outlive `clean`,
and it already takes an override.

**The sentence that used to close this paragraph was wrong, and wrong because
the search behind it looked in the wrong place.** It said the other
`/tmp/hydra-*` directories on this machine came from ad-hoc commands rather
than from anything committed. They came from the live drivers. The grep that
produced the claim was written against `test/try_*.cpp`, and the drivers are in
`test/live/`, so it matched nothing and the empty result was read as an answer
— the precise failure this whole section is about, committed while writing it.

Corrected by grepping the right directory: **thirty fixed `/tmp/hydra-*` paths
across the drivers.** Twenty-nine can be redirected — most read
`HYDRA_TEST_OUT` themselves, the rest inherit it through `shell::fixture`, and
`try_evolve_confirm` uses `HYDRA_TEST_CONFIG` for its settings root. `sweep.sh`
sets `HYDRA_TEST_OUT` per driver, so a sweep is contained; running one directly,
which is what `test/README.md` tells a reader to do, is not.

`try_send_gate` was the one that could not be redirected at all and now can.

## Saving on a signal, built -- and the writers made atomic first

The design was recorded in the section above and deliberately not implemented,
on the grounds that an untested signal handler is a way to corrupt the tree
rather than a safety net. Built now, with the test that was the condition.

### What a signalled exit used to lose

`closeEvent` is what suspends every live view into a blob, writes the tree,
writes the policy and writes the view state. A window that is closed runs it;
a process that is signalled does not -- so a logout, a shutdown, a Ctrl-C in
the terminal it was started from, or a session manager reaping the application
all skipped the entire list.

The tree was the least of it, and the entry above overstated that half: the
debounce timer (`save_tree_soon` at 1500ms) had usually just written it, so at
worst a second and a half of structure was lost. **The view state and the tab
blobs had no second writer at all.** Which folders were open, which tab was in
front, where the window sat, the sort order, and every live tab's navigation
history -- all of it existed only in `closeEvent`, and a signal threw the lot
away. That is the loss the report was actually about.

`save_everything()` is that list, split out of `closeEvent`, which now calls
it. It suspends the live views, so it is an ending rather than a checkpoint,
and the comment says so: nothing that means to carry on running may call it.

### The handler cannot do the saving, and that is the whole shape

Only async-signal-safe calls are legal in a handler -- no `QSettings`, no
model, nothing that allocates. `shutdown_signals` therefore writes one byte to
a self-pipe and returns; a `QSocketNotifier` wakes the event loop and the real
save happens on the Qt thread under no restrictions. Qt's own documented
recipe. The byte is the signal number, so the receiver can say which arrived.

Three details that are easy to leave out and were not:

- **`errno` is saved and restored across the handler.** A signal can land
  between a failing syscall and the code that reads `errno` to decide what to
  do about it. A handler that leaves the wrong value there turns a retry into
  an error return several frames away, with nothing near the fault to suggest
  a signal was involved.
- **`SA_RESETHAND`, so the second signal is the default action.** If the save
  wedges, the next Ctrl-C or the shutdown sequence's SIGKILL ends the process
  the way it would have without any of this. A shutdown handler that can make
  a program unkillable is worse than none.
- **The notifier is disabled before the emission, and only the first byte is
  reported.** The receiver's job is to save and quit, which may delete the
  window that owns the object; a second emission would run over a torn-down
  window.

SIGQUIT is deliberately not taken: its default action is a core dump, and
somebody sending it has asked for one rather than for a tidy exit.

### The writers had to be atomic first, and two of them were worse than assumed

The timer half of the design cannot be caught -- SIGKILL and a crash are not
deliverable -- so the cover there is that a write interrupted anywhere leaves
the previous file rather than half of the new one. Three writers were on the
path and all three were wrong in a different way:

- **`tree_outline::save` truncated and then wrote**, so the window between
  emptying the file and finishing the last line was a window the process could
  die in. What that leaves is a tree file that *parses*, with the tabs from the
  top of the tree in it and none of the rest -- a loss that reads as a
  successful load.
- **It also could not fail.** It returned `true` unconditionally after a
  successful `open`. `QTextStream` buffers, so every byte landed after the last
  statement of the function: the stream flushed in its destructor, the file
  closed in its own, and a full disk was reported to nobody. `commit()` is the
  first thing in that function that can say whether the bytes arrived.
- **`policy_engine::save` deleted the file before writing it.** `QFile::remove`
  then `QSettings::sync`, which is a window with *no policy file at all* --
  a process killed inside it comes back with every site on its defaults. The
  intent was to drop keys the previous save wrote and this one does not, which
  `clear()` expresses without the hole: the erasure is queued inside the
  QSettings object and `sync()` writes the whole result through one rename.
  It also stops lying to the QSettings cache, which keys a shared `QConfFile`
  on the path and was left describing a file that was not there.
- **`state_store::save` truncated too**, and it matters more there than for the
  outline: a blob is opaque, so a half-written one is not recognisably damaged.
  WebEngine is handed whatever the file says and the failure surfaces as a tab
  that restores wrong.

### How far this is verified, and what is argued rather than measured

`test_shutdown` drives the real handler with a real `raise()`: that nothing is
emitted before the event loop runs -- which is the assertion separating this
design from one that saves in the handler -- that the event loop then delivers
it once, carrying the right number, that the disposition is back to `SIG_DFL`
afterwards, and that two signals produce one emission. Arming, refusing a
second instance, and putting the dispositions back on destruction are each
checked against `sigaction` rather than against the object's own account of
itself.

It does **not** send a second signal to prove the second one kills. That would
kill the suite, which is the point; the disposition is inspected instead, which
answers the same question exactly.

`test_tree` and `test_state` pin what the atomic writers can be held to
offline: a save reports success, leaves no temporary beside the target, keeps
the permissions the file had -- the write creates a new inode, so a rewrite
could otherwise publish a 0600 file to the group -- and a save that cannot
write reports failure and leaves the previous contents whole, with nothing
half-written in the directory.

**That the rename itself is atomic is not tested and will not be.** It is
`rename(2)`'s guarantee; a test here could only demonstrate the kernel keeping
a promise it makes to everybody, and this tree already declines to write tests
whose subject is Qt or the platform. What is worth saying plainly is where the
line falls: the mechanism and its housekeeping are measured, the atomicity is
inherited.

**The permissions check failed first, and the code was right.** It compared
`QFile::permissions()` against the `ReadOwner | WriteOwner` that had been set,
which is not what comes back: on Unix Qt reports the Owner bits *and* the User
bits for the same three permissions, so two flags in read back as four
(`0x6600`, not `0x6000`). Settled by measuring rather than by reasoning about
it -- a five-line probe against Qt6Core showed `-rw-------` surviving the
commit, so QSaveFile does carry the mode across the new inode. The check now
reads the mode back and compares that, and additionally asserts the file is
not group- or world-readable so it is testing something rather than comparing
a value with itself. This is the tree's own rule paying out: a gate that
disagrees with code you believe correct is the first suspect.

### Measured against the real browser, not only the fixture

The suite exercises `shutdown_signals` in isolation; what it cannot show is
that the wiring reaches the files. So the built binary was run offscreen
against a scratch tree with `XDG_DATA_HOME` pointed away from the real
profile, and sent a real SIGTERM:

    --- before the signal ---
    view.ini: absent
    policy.ini: absent
    --- after the signal ---
    exit status: 0
    process is gone
    view.ini: WRITTEN
    policy.ini: WRITTEN

**The before/after is the control, and it needs no second build to be one.**
Those two files have exactly two writers between them, `closeEvent` and the
new signal path, and no window was closed. Their absence at the first check is
therefore not a coincidence of timing -- nothing in the program writes either
of them while it runs -- so their presence at the second is the signal handler
and can be nothing else. The tree file was re-read afterwards and still parses,
with its nine lines.

Offline: 33 suites, 0 failures.

### The guard that refused to run its own remedy

Adding a source made the whole test tree unbuildable, which is the guard in
`test/Makefile` doing its job -- `objsets.mk` names the tree it was generated
from and refuses a build against a stale one. But `$(error)` is evaluated while
the makefile is read, before make looks at a single target, so it fired for
`make -C test objsets` as well: the command that fixes the condition, and the
command its own message names.

    Makefile:165: objsets.mk was generated from a different set of sources
    Makefile:166:   only in the tree: test_shutdown.cpp
    Makefile:168: *** run `make -C test objsets` to regenerate it.  Stop.

The way through was `python3 ../tool/objsets.py`, which is the target's recipe
and is written down nowhere a reader would look. The check is now skipped when
`objsets` is among the goals. A guard whose remedy it refuses to run leaves the
reader with two problems, and the second one is invisible.

The Android-build refusal recorded above is still in force and still correct;
`build-android-arm64-v8a` was parked under `build/` for the regeneration and
put back, which is the documented workaround.

## The timer half, and a question that had already been answered

Left on the list one commit earlier as wanting "deciding rather than typing":
the view state had no periodic writer, so a crash still lost which folders
were open, which tab was in front, where the window sat and the sort order.
The choice recorded there was between marking the view dirty through new
connections and a slower unconditional tick.

**That was a wrongly-deferred question, and this tree had already decided
it.** `save_tree_soon` is the same shape with a different source, and the
comment above its connection says so outright -- *"One place to persist,
however the change was made -- a drag, a rename, a new folder."* A signal per
change point, a single-shot timer, one flush. There was nothing to decide;
the answer was fifteen lines above the code that needed it.

Worth recording because the failure mode is the one `working-practice.md`
names as invisible: a deferral looks exactly like diligence, and nobody comes
back to take a decision that was already taken somewhere else. The search that
would have caught it is the one that file describes -- not the name, the
shape: something else in this tree choosing between the same two answers,
whatever it happens to be called.

### Two timers, not one

`save_view_soon()` starts `m_view_timer`, and the deliberate part is that it is
not `m_save_timer`. `flush_tree` writes the whole outline and every tab's
history; hanging the view state off it would rewrite a ten-thousand-node file
every time somebody dragged a window edge, and dragging an edge is by a wide
margin the most frequent thing on the list of what changes the view.

2500ms against the tree's 1500ms, because what feeds it is a gesture rather
than an edit. A resize or a drag through the tree emits continuously and stops
when the hand stops, so the interval wants to be past the end of the gesture
rather than inside it.

Four sources, none of which the model can see -- which is why
`structure_changed` never covered them: `expanded` and `collapsed` on the
tree, `currentChanged` on its selection model, `currentIndexChanged` on the
sort box, and both `resizeEvent` and `moveEvent`. **Both** of the last two,
because `saveGeometry` records position as well as size: keeping only the
resize would remember a window dragged to another corner as still being in the
old one, which reads less like a missing feature than like the restore being
broken.

The selection-model connection is made after `setModel`, which is what creates
that model. Above it the pointer is null and Qt's connect on one is a runtime
warning rather than a compile error -- the kind that is only ever read by
somebody already looking for it.

### Measured against SIGKILL, which is the case that made this necessary

The signal handler is no help here by construction, so the demonstration is
the exit it cannot catch. The built binary, offscreen, against a scratch tree
with `XDG_DATA_HOME` pointed away from the real profile:

    --- 1s in: before the 2500ms debounce could have fired ---
    view.ini: absent
    --- 8s in: the debounce has had every chance, and nothing was signalled ---
    view.ini: WRITTEN while the process is still running
    --- idle for 6s more ---
    unchanged: the timer is single-shot, not a tick
    --- SIGKILL: uncatchable, so nothing saves on the way out ---
    exit status: 137
    view.ini: SURVIVED
    tree.txt lines: 9  (intact)
    stray temporaries beside them: none

Four properties, and each line is one of them. **Absent at one second** is what
makes it a debounce rather than an eager write. **Present at eight** with
nothing signalled and no window closed is the timer, and can be nothing else:
until this change `view.ini` had exactly two writers, `closeEvent` and the
signal path, and this run performed neither. **Unchanged after six idle
seconds** is the design's own condition -- an idle browser must not rewrite
itself on every tick -- and is the assertion a periodic writer would fail.
**Surviving a 137** is the whole point.

No second build was needed for the control, for the same reason as the SIGTERM
measurement above: the file has no writer that runs while the program is up,
so its absence at the first check is not a coincidence of timing.

### What is left, and it is only the blobs

A crash now keeps the tree to within a second and a half and the view state to
within two and a half. What it still loses is the live tabs' navigation
history, since a blob is only written when a view is suspended. That is
deliberately not built: serialising every live WebEngine view on a timer is
real work against a loss only a crash produces, and it wants a measurement of
what that costs on a window full of tabs before anybody commits to it. It
stays on the list as the one remaining piece.

## The Android launcher icon was small, outlined and on a white plate

Reported from a phone, against Firefox and the other browsers sitting beside
it: Hydra's icon is smaller than theirs, carries more outline, and sits on
white. **Three complaints, one cause, and the cause is a file that was not
there.**

`android/res/` held only `mipmap-*/ic_launcher.png` — a *legacy* icon. Every
Android from 8 (API 26) onwards puts a legacy icon through its legacy
treatment: shrink the bitmap, drop it on a white plate, mask the plate to
whatever shape the launcher uses. So the drawing was scaled down inside
somebody else's white circle, which is the "small" and the "white" directly —
and the "outline" follows from the white, because the artwork's own heavy dark
linework had a white ground to contrast against instead of the dark it was
drawn for. Firefox and the rest ship adaptive icons and fill their slots.

**minSdk is 28 and adaptive icons arrived at 26.** So the new
`mipmap-anydpi-v26/ic_launcher.xml` is not a progressive enhancement, it is the
icon on every device the app can be installed on, with two releases to spare,
and the PNGs are a default-configuration fallback nothing will ever pick.

*Corrected twice, and the second correction is the interesting one.*

The commit that introduced this section said minSdk 26. That was wrong: the
package declares 28, from `aapt2 dump badging`, which is the source that cannot
go stale. The number was quoted from `tool/android.mk`'s comment instead of
from the artifact the comment itself tells the reader to check, six lines
above.

**The reason given for that correction was then wrong in turn, and it took
another session to see it.** It read the fragment's `ANDROID_API ?= 26` as
stale because the build generates `qtMinSdkVersion=28` — treating the two as
one quantity that ought to agree. They are not. `ANDROID_API` is the NDK level
**dependencies** are cross-compiled against; `qtMinSdkVersion` is what the
**app** declares, written by Qt per build. **This tree is the proof they are
independent**: hydra sets `ANDROID_API` nowhere, takes the fragment default of
26, and ships an app declaring 28.

Nor is `qtMinSdkVersion` a property of the kit, which is what would have made
"track what Qt declares" a coherent instruction. Measured across four adopters'
own generated builds, all on Qt 6.12.0:

| hydra | fuzzypickles | beerssh | bbq-predictor |
|---|---|---|---|
| 28 | 28 | 26 | 26 |

So raising the default to 28 would cross-compile dependencies at 28 for two
trees whose apps declare 26 — precisely the failure the paragraph above it
describes, arriving as the fix for it. **The value stays 26; only the
justification changed.** Closed as `claude-guidelines` 147e8af, with the
corrected fragment synced here as 27797ce.

Nothing above depended on the number: 28 and 26 are both at or above the 26
adaptive icons arrived in, so the icon conclusion holds either way, and
`build_icons.py` already cites the package rather than the fragment. What the
episode cost was a wrong reason published in a commit message, which cannot be
edited — this is the record that supersedes it.

**The shape worth keeping**: a comment that states a fact *and* a justification
can be wrong in either half, and the halves fail differently. The number was
checkable against an artifact in one command. The justification was not
checkable at all from inside one tree — it took four adopters disagreeing to
show that the quantity it named was the wrong one.

### The geometry, and the number it did not settle

The adaptive canvas is 108dp, the outer 18dp on every edge is always cropped,
and the launcher's mask is applied inside the central 72dp that remains. The
drawing is a rounded blob rather than a true circle — `hydra-master.png` is the
artwork squashed 9% into a square, which `build_icons.py` has always said — so
a *circular* mask cuts the tops of the three heads and the outer edge of the
wave once the art is large enough. Rendered at 62, 64, 66, 68 and 70dp under
circle, squircle and rounded-square masks: **clipping starts at 68.**

That measurement is correct and is still the reason the art is not drawn at
72dp. **The conclusion drawn from it was wrong**, and this section used to
carry it: that 66dp — the largest that survives the circular mask — was
therefore the right size, and that a plate taken from the drawing's own darkest
ink answered the complaint about outlines.

Both were rendered, compared against each other, and shipped. The phone
reported the result as smaller than before. What the comparison could not show
is in the section below, which supersedes the reasoning here rather than
extending it: the size a person perceives is set by the plate, not by the
drawing, and every candidate had been judged on a single light backdrop.

No `<monochrome>` layer, which is unaffected by any of that. The themed icons
of API 33 want a flat single-colour silhouette, and reducing this drawing to
one is redrawing it rather than generating it; absent, a themed launcher falls
back to the two layers.
### It shipped, and the phone said it was smaller

Installed on the handset, and reported back in three words: the icon became
smaller. It had. **Everything above this line about sizing and about the plate
was wrong, and wrong in a way that no amount of rendering masks here would have
caught, because the mistake was about what a person sees rather than about
geometry.**

The correction came from measuring the icons it sits beside instead of
reasoning about the spec. Chrome and Samsung Internet were pulled off the
device and their adaptive foregrounds unpacked; the drawn extent of each within
its own 108dp canvas:

| | drawn extent |
|---|---|
| Chrome | 52dp |
| Samsung Internet | 48dp |
| Hydra, first attempt | 66dp |

**The one that was reported as smaller is the largest of the three.** So the
drawing was never the problem, and "make the drawing bigger" — the whole of the
reasoning above — was answering a question nobody had asked.

**What a person sees the size of is the plate, not the drawing.** Chrome's
white and Samsung's blue fill the mask edge to edge, so each reads as a full
disc with a logo inside it, and the logo being 48dp costs nothing at all. The
first attempt put a near-black plate behind a drawing whose own outer rim *is*
dark linework, so on a dark home screen the icon had no visible boundary
anywhere: what read as the icon shrank to wherever the bright ink began, which
is a good deal smaller than the drawing and much smaller than 66dp.

The white plate that was removed was ugly, and it *bounded the icon*. Taking it
away is what made it smaller — so the third complaint and the first were not
independent, and satisfying "darker, not white" the obvious way is what
violated "bigger". Nothing in the earlier section noticed that they were in
tension.

**The rendering that would have caught it takes one more variable.** Every
candidate above was rendered on a light grey sheet, where a near-black plate
has an obvious edge. Rendered on a dark backdrop as well — which is what a home
screen usually is — the first attempt loses its boundary and the failure is
immediate and plain. One axis, and it was the axis the complaint was about.

**Settled by the copyright holder from a rendering of five candidates on both
backdrops**, since it is their phone and their artwork: the drawing at **62dp**
on **`#4B2E83`**, a violet from the artwork's own mid-tones. That is a clear
ring of colour on every mask shape, no clipping under any of them, a plate that
holds an edge on a dark wallpaper, and a drawing still a fifth larger than
Chrome's. It is the shape Chrome and Samsung both use, arrived at by measuring
them rather than by copying them.

**The general form, which is the part worth keeping.** A rendering answers the
question it was set up to ask and silently passes on every question it was not.
Six background candidates were compared against each other and all six were
compared on one backdrop, so the sheet could rank them and could not report
that the whole family was wrong. The check that would have worked was not a
better rendering of this icon but the cheap external one: unpack what the
neighbours ship and measure it, before deciding what the number should be.

### Generated, not hand-placed

It is in `icon/build_icons.py`, which already owns every other size from the
same master. A second generator is a second thing to forget, and until now
nobody could regenerate the Android set from the artwork at all.

**That the Android PNGs did not come from this script is measurable, and it is
how the risk of rewriting them was settled.** Regenerating the desktop set
reproduced `hydra-16` through `hydra-512` byte for byte — same code, same
library — while the five `ic_launcher.png` came out different. Composited over
a flat ground, the largest visible difference is **2 of 255 at mdpi and 4 at
xxxhdpi**: the same drawing, differing only by encoder rounding from whatever
produced them originally. The 254 that a raw channel comparison reports is
noise in the RGB of fully transparent pixels, where the values mean nothing.
They are rewritten rather than left, because the alternative is a generator
that owns every icon in the tree except five it cannot explain.

### Verified by the packager, not by looking at it

Looking at renders settles the size and the colour. It cannot settle whether
Android will use the file at all, and that is the part the whole change turns
on. `aapt2` was given the real `android/res/` and the real manifest with the
version placeholders filled the way androiddeployqt fills them:

    application-icon-160:'res/mipmap-anydpi-v26/ic_launcher.xml'
    application-icon-240:'res/mipmap-anydpi-v26/ic_launcher.xml'
    application-icon-320:'res/mipmap-anydpi-v26/ic_launcher.xml'
    application-icon-480:'res/mipmap-anydpi-v26/ic_launcher.xml'

At every density the application icon resolves to the adaptive XML and not to
the PNG, which is the platform's own resolver answering the question rather
than this file asserting it. The compiled resource table carries
`color/ic_launcher_background` as `#ff1b0a28`, and the stored XML tree shows
`<background>` pointing at that resource and `<foreground>` at
`mipmap/ic_launcher_foreground`.

**Not verified: how it looks on a real launcher.** Nothing here was installed
on a device, and the renders are this tree's own arithmetic about a mask rather
than Android drawing one. The thing that would close it is the phone the report
came from.

## The blobs, measured first and then built

The last of the three things a crash took. A tab's navigation history reached
disk only when its view was suspended, which happens on the way out, so a
crash returned every live tab to its current url with nothing behind it.

It was left unbuilt with a stated reason: the cost was unmeasured, and
serialising every live WebEngine view on a timer is real work repeated against
a loss only a crash produces. So the cost was measured before anything was
written.

### What one save_state() costs

`qtwebengine_view::save_state()` is `QDataStream << *m_view->history()` and
nothing else, so it can be timed without the shell, without the project's
sources, and without a driver — which also avoided regenerating `objsets.mk`
in a tree another session was editing at the time. Sixty lines against Qt
WebEngine directly, offscreen, on this machine:

| history depth | 1 | 4 | 12 | 30 entries |
|---|---|---|---|---|
| per view | 21us | 80us | 131us | 346us |
| blob | 778 | 3220 | 9756 | 24624 bytes |

About **10us and 815 bytes per history entry** over a small fixed cost, near
enough linear across a thirty-fold range. The worst arrangement anybody is
likely to have — twenty tabs each thirty pages deep — is **6.9ms and 492kB**,
which is under one frame at 60Hz and only after a load has settled.

So the question that had blocked it is answered, and answered generously: it
is affordable by a wide margin. **The measurement is in the code**, on
`m_blob_timer`, so the next person to wonder does not have to build the
harness again.

### What was built

`flush_blobs()` writes the history of every tab that has navigated since it
last ran, on a 5000ms debounce — the longest of the three timers, for two
reasons pulling the same way. It is the most expensive writer, and it is the
only one whose loss window is bounded by a crash alone, since every ordinary
exit goes through `save_everything`. What feeds it is also a discrete event
rather than a gesture: a page load, of which a redirect chain delivers
several, and five seconds coalesces the burst.

**Only the tabs that moved**, which is the whole difference between this and
`save_everything`. That one serialises every live view because it is about to
destroy them all; this runs while the browser is in use, and rewriting twenty
unchanged blobs because one tab followed a link is work with nothing to show
for it. `m_blobs_dirty` holds node ids rather than views, because a view can
be suspended between the navigation and the timer firing — and suspending
writes the blob itself, so a stale id is found to have no live view and
dropped rather than being an error.

The trigger is `load_finished` on **every** view, not only the current one. A
background tab finishing a load has moved its history exactly as much as the
one in front. The existing connection updated the chrome and was correctly
guarded on being current; the record is not.

### Verified against the exit that cannot be caught

    --- 4s in: the 5000ms debounce has not fired yet ---
      blobs on disk: 0
    --- 12s in: it has, and nothing has exited ---
      blobs on disk: 1
        a1.blob  1102 bytes
    --- SIGKILL: uncatchable, nothing saves on the way out ---
      exit status: 137
      blobs SURVIVED: 1

Zero at four seconds is what makes it a debounce rather than an eager write.
One at twelve, with no window closed and no signal sent, is the checkpoint and
can be nothing else — before this, a blob had exactly one writer and it was
`suspend_node`. Surviving a 137 is the point. No stray temporaries, which is
the atomic writer underneath doing its job.

**The first version of that probe measured nothing and said so**, which is
worth recording because it looked like a failure of the feature. It passed a
`file://` url as the argument, and reported no blobs — correctly, because
`load_tree()` is what creates the state store and a url argument never reaches
it. There was nowhere for a blob to go. The fix was to hand the browser a tree
with one tab and a `view.ini` naming it current, which is how the application
opens a tab on its own account.

Offline: 33 suites, 0 failures. The three timers now stand at 1500ms for the
tree, 2500ms for the view state and 5000ms for the blobs.

## `hydra file:///page.html` builds a directory named after the url

Found while writing the probe above, and it is not a test artifact: the
browser created `file:/tmp/claude-1001/.../scratchpad/blobrun/` **under the
working directory**, complete with a `state/` directory and a `view.ini`. The
repository already contained a `file:` directory from 26 August with the same
shape underneath it, from a session running as another uid — so this has
happened at least twice and neither time was noticed.

**The cause is a decision that is written down and deliberate.** `main.cpp`
classifies argv[1], and takes only `http` and `https` as a page to open:

    // A path is not a url and `file:` is not treated as one either:
    // `hydra ./tree.txt` has always meant the tree, and this must not
    // quietly change what that does.

So a `file:` url falls through to `tree_path`. `load_tree()` then derives the
state directory from it with `QFileInfo(path).absolutePath()`, which for
`file:///tmp/x/page.html` is the *relative* path `file:/tmp/x`, and
`state_store`'s constructor calls `mkpath` on it. The url has become a
directory tree rooted wherever the browser happened to be started.

**Two separable things, and only one of them is a design question.**

The littering is a defect on any reading. Whatever argv[1] turns out to mean,
deriving a directory from it and creating it unconditionally is wrong: a tree
path naming a directory that does not exist is a typo or a url, and silently
building it is what turned both incidents into junk in a git repository rather
than an error message.

Whether `file:` should open as a page is the design question, and it belongs
to the copyright holder because the current answer is deliberate. The cost of
the present behaviour is larger than it looks: the desktop entry is
`Exec=hydra %U` and claims `text/html`, and `%U` means urls — so a file
manager handing over `file:///home/me/doc.html` gets a browser that cannot
open a local html file, comes up empty, and leaves a directory behind. The
distinction that would preserve the recorded intent exactly is the scheme
rather than the path: `hydra ./tree.txt` has no scheme and would be untouched,
while a literal `file:` prefix is a url and always was one.

Not changed here. Recorded, with the option, its cost, and whose decision it
is.

### The url no longer becomes a directory

`load_tree` refuses a path whose directory does not already exist, and says so.
Everything the window persists is derived from that directory — the state
store, the view state, the policy, the filters — and `state_store` creates its
own with `mkpath`, which builds every missing component. So an argument that
was never a path became a directory tree rooted wherever the browser was
started. Nothing is assigned before the refusal returns, so every writer stays
guarded by the `isEmpty()` checks it already had and the refusal leaves no
trace at all.

**A refused argument must not cost the session everything it saves**, which is
the second half and was nearly missed. With only the refusal in place the
browser comes up working and persists nothing — no tree, no view state, no tab
histories — with one line on stderr as the only sign. Silently saving nothing
is a worse failure than the litter it replaced. `main.cpp` falls back to the
personal tree in app data, loudly: the same file the same command with no
argument would have opened.

Measured from a scratch directory standing in for the repository:

    --- starting: hydra 'file:///.../page.html', cwd = .../cwd ---
    --- what appeared in the working directory ---
      nothing: the working directory is untouched
    --- did the browser still get a usable tree? ---
      yes: .../data/Hydra/tree.txt (9 lines)
    --- what it said ---
      tree: file:///.../page.html is not in an existing directory
      (.../cwd/file:/.../litterrun); refusing to create one, because an
      argument that is not a path is how a url became a directory tree twice
    --- after a clean exit, working directory again ---
      still nothing

The message prints the derived directory, which is the part worth keeping: the
absurdity of `.../cwd/file:/tmp/...` is the diagnosis, and a reader who sees it
does not have to work out what happened.

**The design question is untouched.** Whether a `file:` url should be opened as
a page rather than read as a tree reverses a deliberate recorded decision and
is still the copyright holder's. What is fixed here is the half that was wrong
on any answer to it.

The twenty live drivers all call `load_tree`, and all of them were checked:
every one reaches it through `scratch_dir`, `inert_sample_tree`,
`single_tab_tree` or its own `out`, and every one of those calls `mkpath` on
the directory first. The refusal cannot fire for them.

## CI came back, and the first thing it found was three tests wrong about root

CI had verified nothing since 2026-08-14 and the reason was recorded as
account billing, not fixable from any tree. That cleared at some point on
2026-08-31: runs before 19:29 that day exit in four seconds having executed
nothing, and runs after it take five and a half minutes and execute
everything. Nobody in this tree noticed, because nobody was looking at a thing
that had been broken for a fortnight.

What it found, on the first real runs in seventeen days, was **three
assertions that were wrong about the environment rather than about the code**,
all of them green on every developer machine:

    FAIL test_state   a save it cannot write reports failure
    FAIL test_state   and the blob that was there is still whole
    FAIL test_tree    a save onto a file it cannot write says so
    FAIL test_tree    and the tree that was there is still there, whole
    FAIL test_theme   the system icon directories are searchable (:/icons)

**The first four are root.** The build job runs in a `debian:trixie`
container, which is uid 0, and for root the permission bits are advice: a 0400
file opens for writing without complaint. Those four assertions make a file
read-only and require the save to fail — a premise that is simply false there,
so the save succeeded and the checks failed against correct code. They are
this session's own, added with the atomic writers.

**The fifth is an empty list.** `test_theme` asserted that seeding keeps the
system's icon directories searchable, by looping over
`QStandardPaths::locateAll(GenericDataLocation, "icons")` and setting a flag.
The container has no icon theme installed, so that list is empty, and a loop
over an empty list can only leave the flag false. On a machine with no system
icon directories there is nothing to keep searchable and the assertion has
nothing to say.

### Fixed on both sides, and the pair is the point

The suites now skip those checks when `geteuid()` is 0, or when there are no
icon directories to find, and print a line saying so. A check that cannot fail
is worse than one that says it did not run.

**And CI runs the suite as an ordinary user**, which is the better half: it
makes the checks run rather than excusing them. `useradd`, `chown` the tree the
root build produced, and `su ci -c "make test"`. Skipping under root is then
the belt to that braces, for anyone who runs the suite as root by hand.

Doing only the skip would have left CI green having quietly disabled a class of
assertion — which is the vacuous pass the workflow file's own header says every
job in it must refuse. Doing only the user change would have left the suite
wrong for anyone running it as root.

Not done: installing an icon theme in the container, which would make the fifth
check run rather than skip. It is one word in the apt list, but that list is
deliberately "every direct dependency named" and an icon theme is a test
dependency rather than a build one, so it is left for whoever owns that
decision. CI will print the skip line, which is how anybody notices.

Local: 33 suites, 0 failures, and the same assertion counts as before the
guards — `test_state` 48, `test_theme` 44, `test_tree` 37 — so nothing was
removed for a normal user.

## One field with two meanings, found by the lens the last bug suggested

`working-practice.md` says to derive the next lens from the last bug rather
than from a list decided in advance. The last bug in `main_window` was
`load_finished`: the chrome was updated only for the current view, correctly,
and the *record* was not updated at all, which is the shape it names as its
highest-yield — a value with two consumers where only some are wired.

Applied to every per-view signal in `main_window`, there are eleven, ten of
them guarded on `view == current_view()`. Nine of the guards are right: back
and forward buttons, the address field, the progress bar, the find bar, the
status tip for a hovered link — all of those are the chrome of the tab being
looked at and mean nothing for a tab that is not. `url_changed` is the
interesting one, and it is already half-unguarded: `apply_policy` runs for
every view and only the chrome is behind the test.

**What the lens actually found is one field below all of that.** Nothing
writes a browsed address back to the node. The title does follow the page --
`set_page_title` exists for exactly that -- and the url does not, so the two
can disagree.

Measured 2026-09-01, with a page that redirects itself after a second and a
half so the debounced flush lands after the navigation:

    - [a1] open | SECOND | file:///.../first.html | created=... | seen=...

One row, two pages: the title from the second, the url from the first. The
tree file is human-readable and hand-editable by design, which is the whole
reason it is a text outline, so a row that is internally inconsistent is worth
more than a stale field would be.

**The obvious repair is wrong, and measuring stopped it.** Making the url
follow the page, as the title already does, would break the tab lock. The pin
is stored in the same field: `set_locked` does `n->url = pin_url`, because a
pin has to survive a suspend, a restart and a reopen from the outline, all of
which throw away the live view that knew which page was showing. A url that
followed navigation would overwrite the pin on the next page load and the lock
would fail silently, which is the worst way for a pin to fail.

So the field carries two meanings — where the tab was created, and, once
locked, the address it is pinned to — and they coexist only because nothing
updates it. The inconsistency is not a defect in either meaning; it is the
cost of having both in one place.

**Nothing asserted the pin, so that is what changed here.** `test_model`
covered the lock's drag refusal, its flags, its copy behaviour and the
reorganizer, and said nothing about where the pinned address goes. It does
now: the pin is written to `url`, and it survives unlocking. The comment
beside it records the measurement and says plainly that the obvious repair
must fail loudly rather than quietly.

**What is left is a decision and it is the copyright holder's**, because
separating the two meanings — a `pin` field distinct from `url` — is a change
to the tree file's format, and every existing tree would have to be read by
both. The options, with their costs:

- **Leave it.** No format change, and rows stay internally inconsistent.
  Cheapest, and the inconsistency has been there all along without anybody
  reporting it.
- **Give the pin its own field.** Rows become consistent and `url` can follow
  the page like the title. Costs a format change, a migration read, and a
  careful look at everything that reads `n->url` — the properties editor, the
  outline writer, `open_node`, the mirror comparison.
- **Stop the title following too.** Consistent the other way, and much worse:
  the title is the row's label, and a tab that never changed its label as it
  browsed would be unusable.

Worth saying that this matters less than it did a day ago. A live tab's real
position is now checkpointed into its blob every five seconds, so the url is
the fallback for a tab whose blob is missing rather than the only record of
where it was.

## The full driver sweep, which nothing else runs

CI compiles the live drivers and never links them; `make test` does not build
them at all. So after three behavioural changes to `main_window` this session —
`save_everything` split out of `closeEvent`, three debounce timers, and
`load_tree` refusing a directory that does not exist — nothing had exercised
the shell. Two drivers had been run by hand. The rest had not.

All 39 linked with no errors, which is itself worth having: a link is the one
thing CI cannot tell you about, and a stale `objsets.mk` once left every driver
undefined at link time with `make test` unable to see it.

    drivers: passed=21  report-only=8  failed=1

**The timers broke nothing, which was the specific risk.** They now fire during
every driver run, writing view state and history blobs that were not there
before, so any driver asserting on the contents of the state directory could
have seen files it did not expect. `try_forget` (32), `try_cookies` (12) and
`try_delete` (11) all touch that directory and all pass. `try_phone` (76),
`try_settings_ui` (89) and `try_chrome` (48) are the broadest and pass too.

### The one failure was the same shape as everything else today

`try_import` reported three failures — two mirrors coexisting, the tree beside
them, and an imported tab carrying its history. None is a defect in this
browser. The driver imports tabs from Firefox and Chromium sessions on the
machine, and **there are none here for either account**: `/home/funk/.mozilla`
exists but has no `firefox` under it, and neither `.config/chromium` nor
`.config/google-chrome` exists at all. So the three assertions were reporting
the absence of somebody else's browser as a fault in this one.

Not a second-uid problem this time — it fails for the tree's owner too, and has
presumably been failing for as long as this machine has had no Firefox. Nobody
saw it because the sweep is not routine.

**The file already knew the answer and had not applied it to itself.** It
`note`s a skip in three places — *"no mirror: this machine may have no Firefox
session"* — and asserts unconditionally in three others, for the same
condition. One of the unguarded ones carried a comment defending the choice:

    // Said out loud rather than skipped quietly: a section that finds
    // nothing to test and prints nothing reads exactly like one that
    // passed.

That objection is right and `note` is what satisfies it. The complaint is
against *silence*, not against skipping, and `note` is neither silent nor a
claim that something is broken. So the three now skip with a line saying what
was not tested, and the assertions stay inside the `else` — a machine with both
browsers still runs all of them.

**`sweep.sh`'s own header is the argument, and it was written about a different
symptom:** judging report-only drivers on a result line they never print
reported them as failures in every sweep, which is *"noise that trains you to
skip two lines of the summary, and the day one of them breaks for real it will
look exactly the same as it does now."* A driver that fails because the machine
has no Firefox is that same noise arriving by another route, and the principle
was already decided.

`try_import` is 25 passed, 0 failed, with two skip lines. No assertion was
dropped: the counts of what actually ran are unchanged.

## KeePassXC: the socket path, settled before the session that can test it

Reported broken, and to be fixed from the account that has a working KeePassXC
set up. What follows is the part that could be established without one, so that
session starts after this work rather than at it.

`try_keepass` scored `1 passed, 0 failed` in the sweep, which is the shape of a
driver that skipped everything: it needs a running KeePassXC with browser
integration, and this account has none. Its own header has said since it was
written that the bridge was *"wired since step 8 and never once run, which in
this project is the same sentence as probably broken"*.

### What the other end actually does, traced rather than assumed

KeePassXC 2.7.10 is installed here, which is enough to interrogate the protocol
end without any database. `keepassxc-proxy`, fed one native-messaging frame
under `strace -e trace=file`, probes in this order:

    <runtime>/org.keepassxc.KeePassXC.BrowserServer
    <runtime>/app/org.keepassxc.KeePassXC/org.keepassxc.KeePassXC.BrowserServer

where `<runtime>` is Qt's `QStandardPaths::RuntimeLocation` — not the
`XDG_RUNTIME_DIR` variable, and not `/tmp`.

**So the socket path is probably not what is broken on the working set up.**
With a valid `XDG_RUNTIME_DIR`, which is what a desktop session has, Qt's
runtime location *is* that variable, and hydra's path was already KeePassXC's
first choice. That rules out the most obvious suspect before anybody spends an
evening on it.

### Two configurations where they did diverge, and both are now fixed

- **No `XDG_RUNTIME_DIR`.** KeePassXC lands on Qt's fallback,
  `$TMPDIR/runtime-$USER`; hydra answered a bare `/tmp`. Different directories,
  so the socket could never be found. This is the configuration this account
  runs in — every launch prints *"XDG_RUNTIME_DIR not set, defaulting to
  /tmp/runtime-claude"* — which is why the bridge cannot work here at all.
- **`XDG_RUNTIME_DIR` set but not 0700.** Qt validates the directory's owner
  and mode and falls back when it fails; reading the variable directly does
  not. So a wrongly-moded runtime directory sends KeePassXC to the fallback and
  hydra to the variable.

`socket_path()` asks `QStandardPaths` now, which is the same function KeePassXC
reaches through, so both cases agree by construction rather than by matching
two implementations by hand.

**The second one was found by tripping over it.** The first trace of the
"variable set" case reported the fallback path, which made no sense until the
scratch directory turned out to be 0775 from `mkdir`. A measurement that
disagrees with the thing being measured is the instrument's fault first — and
here the instrument's fault *was* the finding.

`test_autofill` pins it, asserted against `QStandardPaths` rather than a
literal path: a literal would be the test guessing at the same thing the code
guesses at, two copies of one assumption agreeing with each other and with
nothing else.

### What is left, and it needs the other account

Not the path. Everything after it: transport framing, the key exchange, and
association. `try_keepass` runs the socket, handshake, key exchange and the
not-associated path unattended, and gates `associate()` behind
`HYDRA_KEEPASS_INTERACTIVE=1` because pairing shows a dialog a human must
confirm — by design, since a browser that could grant itself vault access would
be the defect.

    HYDRA_KEEPASS_INTERACTIVE=1 QT_QPA_PLATFORM=offscreen \
      ./test/build-make/try_keepass

The driver's notes describe pointing it at a throwaway KeePassXC with its own
config and database, which is worth preferring over the real vault even from
the account that owns one.

Unverified and deliberately not guessed at: the `app/org.keepassxc.KeePassXC/`
variant above is Flatpak's layout. Matching it needs two candidates and a rule
for choosing between them, and there is no Flatpak KeePassXC here to check
against. If the working set up is a Flatpak, that is the first thing to try and
the path is not ruled out after all.

### Two more checks that could not fail, found by looking for the shape

The lens that produced the root-permission fix, the icon-directory fix and
`try_import` is *a check that cannot fail*, and by the fourth instance it is
worth searching for deliberately rather than waiting to trip over.

The search is mechanical: a loop over a collection that sets a flag, followed
by an assertion on the flag. When the collection is empty the loop cannot set
it either way, so the assertion reports whatever the initialiser said. Eleven
sites across the suites and drivers match that shape; nine are loops over
fixture data the test built itself and cannot be empty. Two were real.

**`test_settings`: the escape hatch was in the initialiser.**

    bool ok = p3.selected().isEmpty();
    for (const player_entry &e : p3.players())
        if (e.id == p3.selected() && e.installed) ok = true;
    check(ok, "and what it falls back to is actually installed");

`selected()` is empty precisely when no media player is installed, so on such a
machine `ok` starts true and the check passes while its own message claims an
installation was verified. **That machine is CI** — the workflow installs no
media player at all, so this has been green there and testing nothing. It does
real work here, where mpv, mplayer and smplayer are present, which is why it
never looked wrong.

The correction is the one already applied twice: the accommodation belongs in a
printed skip, not in an initialiser. A reader of the CI log now sees *"no media
player installed, so there is nothing to fall back to; not checked"*.

**`test_settings`: a negative assertion with no subject.**

    bool leaked = false;
    for (const QString &k : probe.allKeys())
        if (probe.value(k).toString().contains(secret)) leaked = true;
    check(!leaked, "and never written to the settings store");

This one is worse than vacuous, and worth stating precisely: `leaked` can only
stay false if the loop runs, so an empty key list passes exactly as loudly as a
clean one — and an empty key list is what a save that silently did nothing
leaves behind. **The check would be greenest in the one case that should alarm
anybody**, namely that the write it is auditing never happened.

Fixed by asserting the subject exists before searching it: the store has keys
(13 here). That is `evidence.md`'s rule about mechanical changes applied to a
test rather than to a tool — state what must hold, check it, and refuse to
report on an empty population.

**The generalisation, which is the useful part.** A negative assertion — *this
did not happen*, *nothing leaked*, *no entry is present* — is satisfied by an
absent population and by a correct one alike, and the two are indistinguishable
from the result line. Every one of them wants a companion check that the
population is not empty. The positive assertions in the same files do not need
this, because a loop that never runs cannot make them pass.

132 assertions in `test_settings`, up from 131: the leak check gained its
precondition and the fallback check kept its meaning.

### Running the lens as a search, and the one it caught next door

The previous pass fixed two checks that could not fail and wrote down the
generalisation: *a negative assertion is satisfied by an absent population and
by a correct one alike, and the two are indistinguishable from the result
line.* Written down, that is a search rather than an observation.

Run over the suites, `check(!<haystack>.contains(...))` matches 24 sites.
Fifteen have a companion assertion on the same haystack a few lines away, which
proves the population exists; nine do not. Of those nine, four are the regex
being coarse — `test_autofill` asserts `create.value(...)` three times before
asking what `create` does *not* contain, and `test_theme`'s `useless` list is
expected to be empty and prints "none" when it is.

Five were real, and they share a shape worth naming: **read something, then
assert what is not in it.** An unopened file, a failed call and a correct
result are one and the same to `contains`.

- `test_settings` — the audit that a stored secret never reaches the settings
  file on disk. `f.open()` was unchecked, so a file that would not open read as
  empty and the audit passed. **This is two lines below the leak check the
  previous pass fixed**, which is the part worth admitting: the shape was
  named, the instance beside it was not looked at, and the fix stopped one line
  short of the more important half. A secret-leak audit is the last assertion
  that should be allowed to pass by default.
- `test_model` — the outline round trip, asserting no `locked=0` marker is
  written. An empty read contains no marker either.
- `test_seam` — the resumed download, asserting the first sixteen bytes are the
  server's rather than the stale ones. A read that returned nothing contains no
  stale byte, and would report the file correct precisely when it could not be
  examined.
- `test_extractor`, twice — the evidence payload, asserting a long address and a
  served content type never reach it. An empty payload contains neither.

Each gained one assertion that the thing being searched exists, with its size
in the message so the log says what was searched rather than that something was.

**And a variant the first search did not look for.** In the same section:

    for (const QString &row : with.split('\n')) {
        if (cols.size() < 4) continue;
        check(cols.last().startsWith("http"), ...);
        break;
    }

A `check` inside a loop that may not run asserts nothing at all, and reports it
no more loudly than a section that passed — there is not even a green line to
be suspicious of. A payload with no four-column row leaves the section silent.
`examined` is now set in the loop and asserted after it.

So the family has three members, and only the first is obvious: a flag set in a
loop and checked after, a negative assertion over a haystack that may be empty,
and a check inside a loop that may not execute. All three report success by
doing nothing.

Counts: `test_settings` 133, `test_model` 228, `test_seam` 75, `test_extractor`
152 — each up by the assertions that pin the population. 33 suites, 0 failures.

### The third member, searched for rather than stumbled on

The previous entry named three ways a check reports success by doing nothing
and had systematically searched for two of them. Naming a family and searching
two thirds of it is the shape this session keeps finding in other people's
work, so: `check(` lexically inside a `for` or `while` body, across every
offline suite. **Twenty loops.**

**Seventeen are sound and it is worth saying why**, because the result is
mostly a compliment to the code rather than a list of faults. Most iterate a
literal array written two lines above — `pages`, `rows`, `fetched`,
`assembled`, a `QStringList{...}` — or a fixed count, and cannot be empty.
`test_settings`'s native-player loop already prints *"(no native-stream player
installed to check)"* when it finds none. `test_model`'s second loop asserts
`check(loose != nullptr, "there is a tab outside that folder to move")` before
using what it found. Two more belong to suites that need a corpus or a network
and never run here at all.

**One was real, and it is the largest silent block found this session.**

    node *mine = nullptr;
    for (node *c : m.root()->children)
        if (c->mirror.isEmpty() && c->is_folder()) { mine = c; break; }
    if (mine && keeper) {
        ... eight checks ...
    }

Eight assertions — that a mirrored tab can be dragged into the tree, that it
stops being the other browser's, that its id is re-minted out of the mirror's
namespace so it cannot collide, that its history survives — all conditional on
a search that nothing verified. A loop that finds nothing leaves `mine` null,
the block is skipped whole, and the section reports *nothing at all*: not a
failure, not a skip, not even a green line to be suspicious of.

`keeper` was already safe, and by the right mechanism: the assertion two lines
up pins the mirror to exactly one child, so `first()` has something to return.
That is the difference between the two — one was proven in passing by a check
written for another purpose, and the other was not proven at all.

**And the file already knew.** Ten lines further down, the same section does it
correctly. So this was never a missing idea, only a missing line — which is
also why it survived: nothing about the code looks wrong, and the failure it
guards against produces no output to notice.

`test_model` 229, up from 228. 33 suites, 0 failures.

**The family is now closed**: flag set in a loop and checked after; negative
assertion over a haystack that may be empty; check inside a loop that may not
run. Searched mechanically, 55 sites triaged, 8 real, all fixed. What the
search cost was worth paying once — the shapes are simple enough to grep for,
and none of the eight would have been found by reading, because every one of
them looks correct.
## Two searches that found the code correct

Both are negative results and both are recorded, because this session's whole
theme is that a search which found nothing must be distinguishable from one
that was never run. Neither changed a line; the value is that nobody needs to
run them again.

### The vacuous-success lens, pointed at the software

The three shapes that produced eight fixes in the suites were pointed at
`src/`, where the same fault would be far worse — a function that reports
success without doing the work is a silent data loss rather than a weak test.

**Unchecked `open()`: none.** Every file open in the software checks its
result. The unchecked one that started this whole line of enquiry was in a
test, auditing whether a secret reached the disk.

**A `bool` that cannot fail: none.** Two candidates matched the search and both
are correct on reading — `flow_layout::hasHeightForWidth` returning `true` is
what that Qt override is for, and `torrent_download_source::available()` sits
inside `#ifdef HYDRA_HAVE_LIBTORRENT` with an `#else` returning `false` that a
regex cannot see.

So `tree_outline::save` returning `true` unconditionally — found this morning,
and the reason the lens existed — was **the exception in the software rather
than the pattern**. The vacuous-success problem lived in the tests and the
tooling, which is worth knowing precisely because it is the opposite of what
the first instance suggested.

### Android autofill: the code-level preconditions, all four met

§19's remaining gap was recorded as *"Autofill on Android is the system
service's job rather than this browser's… what is not established is that
filling works"*, blocked on an emulator with no autofill service. That is now
narrower on both sides.

**The device can test it.** The handset reports
`com.google.android.gms/.autofill.service.AutofillService` configured, with
keepass2android and Samsung Pass also installed. The emulator's excuse is gone.

**And nothing in this browser prevents it**, which is checkable without a device
and was not checked before:

- The WebView is constructed with the **Activity** context — `new Player(a)`,
  with a comment saying the activity is handed in rather than fetched. A
  WebView built on the application context does not participate in autofill,
  and this is the commonest way an app breaks it without meaning to.
- It is added with `addContentView`, so it is a real View in the activity's
  content hierarchy where the autofill framework traverses. Not a texture, not
  an offscreen surface.
- Nothing disables it. `autofill` does not appear anywhere in `android/` or in
  the `android_*` sources, so no `IMPORTANT_FOR_AUTOFILL_NO` is set on the view
  or inherited deliberately.
- minSdk 28 is above the 26 autofill arrived in.

So the claim in §19 is now supported structurally rather than assumed, and what
remains open is only whether a fill actually happens — which no amount of
reading can settle.

**The measurement that would settle it needs no credentials and no
screenshot**, which matters because this handset is the copyright holder's
daily phone and its screen is not an available instrument: open a local html
login form, tap the username field, and read `adb shell dumpsys autofill` for
whether a session was started against the WebView's view structure. That is the
single-bit method the handset notes already prescribe, and it answers the real
question — whether the WebView exposes its fields to the framework — without
touching a stored password.

Not run here. The phone reported `mWakefulness=Dozing`, and this file records
that a dozing phone reads exactly like a regression, so any result taken now
would be untrustworthy.

## The live model suite, run for the first time

`test_live_model` is in `NEEDS_MORE` and had never run here. The reason
recorded for skipping items 1 to 4 was "needs a network, a device or a model",
and that was **assumed rather than checked** — the device was verified for the
autofill item, and the model never was. Ollama is serving `qwen2.5-coder:14b`
on this machine and has been all along. So the suite ran.

**And the instructions for running it were wrong.** `test/README.md` told the
reader to run `./test/build/test_live_model`, a directory that has not existed
since the migration from CMake, which this file records elsewhere. Nine
commands in that README named it, and `project.md` named it twice more — once
describing which build directories survive a session and once saying where a
failing suite writes its log. Eleven paths, every one of them a reader's first
step, all pointing at nothing. The document gate did not catch them: it checks
that project.md names no missing *file*, and these are directories.

### What eight runs say, and what they do not

Eight runs against the built-in synthetic evidence set, gate verdict recorded
each time: **four accepted, four rejected.**

That number is about the synthetic set and does not transfer, and the README
says so plainly in the paragraph above the command — real segments arrive
disguised as `.woff2` web fonts while the synthetic ones arrive as `.ts`, so
the two disagree about the thing being detected. Item 1's figures for dramafren
and kisskh came from real captures, and `evidence/` is not in this checkout.
So this is not a comparable score and must not be read as one.

**What does transfer is the shape of the failures**, because it is about
regular expressions rather than about the corpus.

A rejected run wrote:

    const hlsPattern = /master\.txt/;
    const manifestPattern = /\.(m3u8|mpd)$/;

and the gate answered *"the script found nothing"*. Both clauses miss, for two
different reasons, and the second is general:

- `master\.txt` cannot match `cf-master.1774687168.txt`. The version digits sit
  between the word and the extension, and the model wrote the pattern as though
  they did not exist.
- `\.(m3u8|mpd)$` is anchored to the end of the string, so **any query string
  defeats it**. Real manifest urls carry tokens — the accepted runs picked
  `...cf-master.1774687168.txt?k=UCpS63&kx=17` — so an end-anchored extension
  fallback fails on precisely the urls it exists to catch.

That second point bears directly on the extractor-loop item, which records
that kisskh scored
2 of 5 "purely on an `.m3u8` fallback". If the fallback the model writes is
end-anchored, it works only where the url has no query string, and its
successes are a property of the site rather than of the loop.

**And writing both clauses is not sufficient.** Item 1 proposes asking the
model for a note-driven match and an extension fallback in the same script.
Three of the eight runs did produce both, unprompted — and one of those three
was still rejected. So "both clauses present" and "accepted" are not the same
property, and a change that only increases the first would move a number that
is not the one being measured.

Not attempted here: the real-capture runs the extractor-loop item is actually
about. They need
`evidence/`, which is not in this checkout, and a site visit.

### Two more writers with the fault the atomicity pass was for

The atomicity work earlier in this session made `tree_outline::save`,
`state_store::save` and `policy_engine::save` survive an interrupted write,
because the debounce timers mean those files are now written while the browser
is running. It scoped itself to "the persistent-state writers that `closeEvent`
and `flush_tree` touch" and named those three.

**`load_tree` derives five persisted paths, not three.** The other two are
`filters-ai.txt` and the learned extractors, and both had the identical fault.
`site-rules.ini` and `annoyances.ini` are safe by a different route: they go
through `QSettings`, which writes via `QSaveFile` internally.

`filter_list::save` was `tree_outline::save` character for character before the
fix — truncate, a buffered `QTextStream`, and an unconditional `return true` so
that a full disk reported success. `site_extractor::save` is the same shape
with the write's return value discarded rather than a stream's.

Neither is small data. The filter list is the rules this browser and its model
have authored, kept in a separate file from any imported EasyList *precisely*
because they cannot be re-fetched; the extractors are what it has learned about
sites it has visited. A half-written filter list parses and is a smaller set of
rules; a half-written `extractors.json` does not parse and is none of them.

**What made this findable is that the earlier fix wrote down its own scope.**
The entry said which three writers it covered, so the question "which writers
are there" had an answer to be compared against — five. A fix that had not
named its boundary would have left nothing to notice.

That is the second time today the same thing happened in the same shape: the
`test_settings` leak audit was fixed and the identical fault two lines below it
was not, and both were found by re-reading the fix rather than the code. A fix
that names what it covered is checkable; one that does not is only trustworthy.

**And it happened a third time in the fix itself.** The three writers made
atomic this morning each gained assertions pinning the property — reports
success, leaves no temporary, a save that cannot write leaves the previous
contents — and these two gained none. `test_settings` covers the filter list
now and `test_extractor` the learned extractors, both in suites that already
linked the writer so neither needed an `objsets` regeneration. Both carry the
`geteuid() == 0` skip the earlier ones do, for the reason CI made plain.

The extractor's version asserts something the outline's cannot: that the file
still *parses* afterwards. A half-written outline is a smaller tree, and a
half-written `extractors.json` is a syntax error, so "the set that was there is
intact" is checked by loading it rather than by comparing bytes.

**A fourth time, and this one was the original commit's.** `policy_engine::save`
was changed this morning too — from removing the file before rewriting it to
clearing the QSettings object — and nothing pinned that either. It is the
change with the most to lose: `QFile::remove` was not only a hole, it was also
**how stale keys were dropped**, since `setValue` alone adds and overwrites but
never deletes. Swapping the mechanism kept the hole shut and could quietly have
lost the behaviour the hole was paying for, and the failure would be a rule the
user deleted returning at the next launch.

`test_bundle` checks it now: two site rules saved, one dropped by writing a
policy that never had it, and the dropped one asserted gone from the reloaded
file while the other stays. It passes, so the swap kept the behaviour — but
that was worth establishing rather than assuming, and until now it rested on
nothing.

**Five writers were changed this session and all five are pinned**: the outline
in `test_tree`, the blobs in `test_state`, the policy in `test_bundle`, the
filter list in `test_settings`, the extractors in `test_extractor`. The count
is the point — it is the number that made three of the four gaps visible.

### The gate's blind spot, met in passing

Reindenting the new scope in `filter_list.cpp` left the loop's closing brace one
tab short, and `make style-source` passed. It says so itself, in the line it
prints on success: *"indentation except under-indentation, which is not
checked"*. The converter it compares against never adds indentation, so a line
with too few tabs is invisible to it.

Recorded because it is the first time that documented gap has actually let
something through here, and because the message was what made it obvious —
the gate saying what it does not check is what turned a green run into a reason
to look. It was found by reading the diff, which is the only instrument that
covers it.

### Four drivers still fetched a real site, and the entry that fixed three said how

*The drivers no longer fetch a real site to make a point* records fixing
`try_media`, `try_frame` and `try_mse`, and names the part that is easy to miss:
**two things had to change for one symptom, the url the driver navigates to and
the tree it opens.** Four drivers had only the first half.

Found by reading a report nobody had read. The sweep classes eight drivers as
report-only — they capture and time rather than assert — and judges them on
"ran to the end". Running one and actually looking at its output, `try_flicker`
printed the Amplitude logger's warnings and a Simple Analytics line, which no
local page produces.

The mechanism is the one that entry describes. `inert_sample_tree` marks every
row unopened so nothing loads on startup, which is what "inert" means; a driver
that then activates a row loads whatever that row points at, and in the
committed example the first one is `doc.qt.io`. `try_flicker`, `try_capture` and
`try_downloads` all did that, and `try_look` wrote its own tree with a real
`doc.qt.io` in one row while using non-resolving `example.test` in the row below
— inconsistent with its own practice rather than with a rule elsewhere.

All four open `shell::local_page_tree()` now, a shared helper next to
`single_tab_tree` so the fixture is written once rather than three times.
`try_look`'s one real url matches the neighbour it sits beside. Re-run: zero
tracker lines from any of the four, all still producing their captures —
`try_capture` 5, `try_downloads` 14, `try_flicker` 11 grabs.

It also makes `try_flicker` mean something. What it reports is how the shell
paints in the moments after a tab opens, and while the page came off the network
those timings moved with a remote site's week rather than with this code —
unrepeatable in exactly the way that entry gives as its second reason.

### The grep that looked in the wrong place, and the claim it produced

Fixing the suites' shared `/tmp` paths this morning ended with a sentence saying
the other `/tmp/hydra-*` directories came from ad-hoc commands rather than from
anything committed. **That was false, and false in this section's own way:** the
grep behind it was written against `test/try_*.cpp` while the drivers live in
`test/live/`, so it matched nothing, and the empty result was read as an answer.

Grepping the right directory: **thirty fixed `/tmp/hydra-*` paths across the
drivers**, which is where every one of those directories came from.
Twenty-nine can be redirected — most read `HYDRA_TEST_OUT`, the rest inherit it
through `shell::fixture`, and `try_evolve_confirm` uses `HYDRA_TEST_CONFIG` for
its settings root, which a three-line lookback missed and a fourth line found.
`sweep.sh` sets `HYDRA_TEST_OUT` per driver, so a sweep is contained; running a
driver directly, which is what `test/README.md` tells a reader to do, is not.

`try_send_gate` was the one that could not be redirected at all. It now can.

The lesson is not "grep more carefully". It is that **an empty result and a
verified absence look identical**, which is the same sentence as the one about
gates over empty file lists, and it applies to the person writing the search as
much as to the tool. A search that returns nothing should be made to prove it
looked — here, by naming a file it must have found.

## getUserMedia on Android: three things, none of them sufficient alone

Asked for so that Teams web meetings can work. What was missing on Android was
not one thing with a switch, and that is the part worth recording: a page
calling `getUserMedia` was refused three separate times over, and fixing any one
of them would have changed nothing observable.

- **The manifest declared no capture permission.** Only `INTERNET`,
  `POST_NOTIFICATIONS` and the two foreground-service ones.
- **The `WebChromeClient` did not override `onPermissionRequest`.** Its only
  override was `onShowFileChooser`, so Android's default ran, and the default
  denies. This is the one with no diagnostic: the page sees a rejected promise
  and nothing anywhere says why.
- **Nothing asked for the runtime grant.** No `requestPermissions`, no
  `checkSelfPermission`, which is mandatory from API 23 against a minSdk of 28.

All three are in now, and the desktop needed none of them: `policy` has had
`camera` and `microphone` all along and `qtwebengine_view` maps
`MediaAudioCapture` and `MediaVideoCapture` onto them, so a call there was
already answerable. Android had the same `permission_decider` set on every
view and never once asked it.

### Two refusals, in the right order, and one deliberate inversion

`android_view::request_capture` puts the site policy first — the same engine
the desktop asks, so the shield governs Android identically — and the operating
system's grant second. Either is final. A user who denied this application the
camera has said something no per-site rule may override, and a site the shield
blocks does not get to raise an OS dialog to argue about it.

The OS questions are asked one after the other rather than together, because
each is a dialog and two at once is a stack of them in front of somebody trying
to join a call.

**And it refuses when nothing is listening, which is the opposite of
`allow_navigation` directly above it.** That one returns *true* for a view whose
shell never set a decider, on the recorded grounds that a refusal nobody asked
for is a browser that will not browse. Both are right. The safe direction is not
a property of the pattern; it is a property of what is being asked for, and a
missing decider must not hand a page the camera.

### The asynchronous part, which is why nothing returns a value

Every other JNI entry point in that file goes through `on_qt_thread`, which is
a `BlockingQueuedConnection` — it holds the calling binder thread until the Qt
thread answers. That is right for a question with an immediate answer and wrong
here, because the answer may wait on a dialog: holding a binder thread while one
is on screen is how an application stops responding.

So `requestCapture` posts and returns. Java parks the `PermissionRequest` under
a token, C++ answers by calling `onCaptureDecision` back, and the request is
granted or denied then. A `PermissionRequest` may be answered later, which is
what makes the design possible at all. Every early return in `request_capture`
goes through one `answer` lambda, because a request that is never answered
leaves the page's promise pending for ever — indistinguishable, to a user, from
a camera that is slow to start.

Granting names the resources the page asked for rather than everything it might
have, so a request for audio alone cannot come back holding the camera because
the policy happened to allow both.

### What is verified, and what is not

Verified against the artifact rather than the build log: `aapt2 dump badging`
reports `CAMERA`, `RECORD_AUDIO` and `MODIFY_AUDIO_SETTINGS` in the package, and
both hardware features as **`uses-feature-not-required`**. That last one is not
cosmetic — declaring `CAMERA` makes Android imply a *required* camera feature,
which would quietly make the browser uninstallable on a device without one, and
`required="false"` is what prevents it.

`make jni` reports 12 native methods, every one resolvable, which is what
catches a JNI signature that does not match its Java declaration. The arm64
build is clean; the desktop build does not compile these files at all, so it
could not have caught a fault here.

**Not verified: that a Teams meeting connects.** Nothing here has met a real
call. Three necessary conditions were missing and are now present; whether they
are sufficient is a different claim and needs an account, a meeting and the
handset. Two known risks sit beyond them and are unaffected by this work:
screen sharing is still refused on both platforms — `DesktopVideoCapture` falls
to the deny branch and there is no source picker — and the session-cookie policy
recorded against Teams by name still discards the SSO cookie on exit.

## Wanted: an indicator for the AI batch jobs

Asked for 2026-09-01, and recorded rather than built: the browser has to work
before the AI features get harder, and this is the AI side. Written down now
because a requirement that lives only in a conversation is one nobody can act
on later.

**What was asked for**, in the holder's terms:

- a **tiny display** showing progress and a log for AI batch jobs;
- which can **expand**, rarely, to ask a question;
- **stop** and **kill** buttons;
- a **slider that reduces CPU load and memory use dynamically** — while a job
  is running, not only for the next one.

### What already exists to build it on, rather than beside

Nothing runs AI work as a *job* today, and the provider interface reports no
progress at all. But two shapes in the tree already answer parts of this, and
the useful move is to copy them rather than invent:

- **`download_manager` and `downloads_dialog`** are the nearest thing to a job
  model: a queue, per-job state with a `terminal()` predicate, progress, and a
  dialog listing them. An AI batch queue is the same shape with a different
  worker.
- **`m_confirm_action`** — the toolbar's "Still working?" item — is already the
  rare-question pattern: hidden until there is something to ask, shown when
  there is, and answered in one gesture. Whatever asks the batch's questions
  should behave the way that already does rather than inventing a second idiom
  for the same thing.

The obvious home for the tiny display is the status bar, which already carries
transient state; expanding it to a panel is the same move `downloads_dialog`
makes from its own indicator.

### Answered, 2026-09-01

- **Stop and kill differ, and both stay.** Stop lets the item in flight finish
  and then halts the queue; kill abandons it immediately, including the request
  already sent to the model.
- **The slider does whatever can be done**, up to and including loading a
  cheaper model when that is what lowering memory takes. Marked a late-stage
  feature: throttling our own concurrency and pacing is the near-term part, and
  swapping the model underneath a running batch is not.
- **A batch is persisted only far enough to be restarted**, and only where work
  would otherwise be lost. Restarting the job, not resuming mid-item -- which
  is the cheaper promise to keep and the one a reader of the log can verify.
- **Everything about a batch belongs in one place.** The questions go in the
  panel with the progress, the log and the controls, rather than borrowing the
  toolbar. The reason given is mobile: the UI there is non-standard and hard
  enough to find without one feature's state being split across two places.

  Note for whoever builds it: the toolbar's existing "Still working?" item is
  **not** this. It asks whether newly applied filter rules broke the page in
  front of you, which is a different question about a different thing, and the
  two should not be merged because they happen to both come from the AI side.

Not started. The permissions work comes first, on the holder's instruction:
the browser has to be a browser before the harder half is worth building.

## `ask`: the third answer, and the four defaults that now use it

The shield had two answers and needed three. `allow` and `block` are both
decisions taken in advance, by somebody who has not met the page — which is the
right shape for JavaScript and images and cookies, and the wrong shape for a
camera. There was nowhere for "put the question to the person in front of the
screen" to live, so camera and microphone were globally blocked, which is the
correct thing to do in the absence of an answer and was never meant to *be* the
answer.

**What that cost is recorded two sections above.** The `NotAllowedError` that
looked like broken `getUserMedia` support on Android was the shield working
exactly as configured: `camera=block`, refusing in silence. The page was told;
the person was not. A video call Hydra had deliberately stopped and a video call
that was simply broken were indistinguishable from the only side anybody was
looking at.

### The shape of the change

`setting::ask = 3` went in earlier, along with the three corrections it forced —
`is_allowed` reading `== allow` instead of `!= block`, `global_default` failing
closed on `unset`, and the engine's private copy of the word list deleted in
favour of `policy::setting_from_word`. That copy knew "allow" and "block" only,
so a stored `ask` read back as `unset`: the setting would have been accepted,
written, and quietly undone on the next start.

What this section adds is everything that makes `ask` reach a person.

**The decider is asynchronous.** It was
`std::function<bool(const QUrl &, policy::feature)>` and the comment above it
said, accurately, "synchronously… no UI and no waiting". That was the whole
limitation: a `bool` cannot say *"I am going to put a dialog on the screen and
tell you afterwards"*. It is now
`std::function<void(const QUrl &, policy::feature, permission_answer)>`, and the
answer callback may fire before the call returns — which is what the common case
does, since a policy that already says allow or block answers without touching
the screen. Every call site therefore captures by value rather than relying on a
scope that may be gone.

Four call sites moved:

- **Desktop, 6.8 and later.** `QWebEnginePermission` is a value carrying its own
  target, so the settle lambda captures it and can answer whenever it likes.
- **Desktop, the pre-6.8 branch.** This one has to call back into a page this
  object owns, and an answer that waits for a person can easily outlive the tab
  it was asked about — so the page is held in a `QPointer` and checked at answer
  time. That branch still cannot be compiled against the Qt in this tree, and is
  written to the same standard anyway.
- **Android capture.** The two policy questions were `if (video && !decider(...))`
  and `if (audio && !decider(...))`, which only worked while the decider answered
  from a table. They are a chain now, in front of the OS-permission chain that
  was already there.
- **The shell**, which is where the dialog actually lives.

### The prompt

`permission_dialog`, in the idiom of `auth_dialog` beside it. It names the site,
says what is about to happen in the words of the thing rather than the words of
the setting — "wants to use your camera", not "Camera" — and adds a second
sentence where the plain name understates it ("It can start and stop on its own
while the page is open").

Three decisions in it are deliberate and each is the less convenient option:

- **Closing the dialog blocks.** Escape, the window button, any dismissal — all
  refusals, and by construction rather than by remembering: `m_granted` is false
  and only the Allow button sets it. A prompt that treats "go away" as "ask me
  again shortly" is how a page nags somebody into agreeing.
- **Block is the default button and holds focus.** A stray Return as the dialog
  appears lands on the refusal. A wrongly refused camera is a page that does not
  work and a person who tries again; a wrongly granted one is a camera that is on.
- **"Remember" is unchecked.** Checked-by-default is what other browsers do and
  it works there because their prompt is a bar at the top of a window nobody is
  looking at. Here the answer is given under interruption by someone who wants
  the interruption gone, and the fastest way to dismiss a dialog is the button,
  not the checkbox above it. A grant that outlives the moment has to be asked for.

The nagging that would otherwise cause is handled better elsewhere: the shell
keeps `m_session_permissions`, keyed host-and-feature, for this run only. A page
calling `getUserMedia` in a retry loop is answered from there and nobody sees a
second dialog. Nothing is written down, so it is gone when the browser is —
which is exactly what lets the checkbox default to unchecked without condemning
anyone to answering forever.

When the box *is* ticked, the answer goes into the site rules and the policy file
is saved immediately rather than on the debounce. An answer to a permission
prompt is the kind of decision somebody would be furious to give twice because
the process was killed in between.

### One table, because two would have drifted

The prompt's sentence and the settings page's "may this feature be set to ask?"
are the same fact. They are one field: `ask_phrase` in `policy.cpp`'s feature
table, non-null on six rows, with `can_ask(f)` defined as `ask_phrase(f) != nullptr`.

The alternative was a list of promptable features in the UI, which is a second
copy — and the failure mode of the copy going stale is either a feature offered
as `ask` that no prompt has words for, or words written for a feature the combo
box will not let anybody choose. A test asserts the two halves agree for every
feature, and that no feature *defaults* to `ask` without a prompt able to answer
it, which is the invariant that stops a capability being permanently and
invisibly denied.

The twelve rows with no phrase say `nullptr` explicitly. Leaving the member off
compiles and value-initialises correctly, and cost twelve
`-Wmissing-field-initializers` in a build that carries none — a warning worth
keeping, since it is the one that notices a field added to this struct and
forgotten in every row below.

### Both settings surfaces were quietly destroying `ask`

Neither could represent it, and neither failed loudly:

- **`site_policy_dialog`** had three combo entries; `ask` fell into the
  `default:` arm and drew as "Default". Touching that row wrote it back as
  `unset`. A site the person had chosen to be asked about silently became a site
  on the global default. Its "Default (allowed)" label would also have called an
  `ask` default *allowed*, which is the one word of the three that is actively
  wrong.
- **`settings_dialog`** had two entries and `setCurrentIndex(cur == block ? 1 : 0)`,
  so an `ask` default rendered as **Allow** — and its save path reads the combo,
  so opening the settings page and pressing OK without touching anything would
  have widened the setting. Its exception list had the same two-way ternary and
  printed `ask` as "block".

Both now carry the third state. The per-site dialog greys "Ask" where
`can_ask` is false rather than omitting it, because its index *is* its meaning
and a per-row entry count would make `setting_to_index` row-dependent; the
settings page omits it instead, because it carries the setting in the item's
data and a two-entry row and a three-entry row both read back correctly.

### The defaults that moved, and the two that did not

Camera, microphone, location and pointer lock now default to `ask`. Nothing is
granted by this: the question is put to the person it belongs to, once per site,
refused by default if the dialog is dismissed.

**Notifications stay blocked, and not out of caution.** No
`QWebEngineProfile::setNotificationPresenter` is installed anywhere in this tree
— `grep` finds neither that nor `QWebEngineNotification`. A granted notification
permission would resolve the page's promise and then silently drop every
notification it sends. Prompting for that is asking somebody to grant a
capability the browser cannot honour, which is worse than refusing it. **This is
a real gap and the default moves when a presenter exists, not before.**

**Clipboard reading stays blocked for the same class of reason.** The engine
gates it behind `JavascriptCanAccessClipboard` and `JavascriptCanPaste`, neither
of which this project enables, so the permission request never arrives at all.
`ask` there would be a setting offering a prompt that cannot fire.

**Pointer lock asks, where the plan said allow-plus-a-transient-notice.** The
notice is UI that does not exist, and `allow` without it is a silent grant — the
precise failure this whole change exists to remove, pointed the other way. `ask`
until there is something to show.

### What is verified

The desktop build is clean and warning-free, and the full offline suite passes.
`test_settings` gained 18 checks in three sections: the four defaults and the two
that deliberately did not move, the strictness of `is_allowed` against `ask`, the
one-table invariant above, and a save/load round-trip proving `ask` survives the
file as both a global default and a site rule.

`try_permissions` now covers the prompt end to end and passes 19 of 19 offscreen.
Two cases were added, and the driver had to be repaired to take them.

- **A dialog really appears, and pressing Allow reaches the engine as a grant.**
  Geolocation set to `ask`, the button pressed by object name, and the decider's
  own log shows `GRANTED` — the answer made the round trip out to a widget and
  back into Chromium, which is the thing the asynchronous decider exists to make
  possible and the thing a bool could not do.
- **Dismissing refuses**, and the page sees `NotAllowedError` for it.
- **The run-scoped memory works.** Geolocation is left on `ask` for the second
  case and produces no second dialog; the prompt count rises by exactly one, for
  the microphone. That count is the assertion that a page cannot nag.
- **Remember unticked leaves the site rule alone**: `effective_setting` is still
  `ask` after the grant.

The buttons carry object names for this. An unnamed button in a modal dialog is
a decision no test can ever make, which would have left the whole path resting on
somebody having clicked it by hand once.

### Two things the driver was wrong about, found by running it

**It asserted on a camera this machine does not have.** There is no `/dev/video*`
here, so the page fails at device enumeration and Chromium never requests the
camera permission at all — the decider is never consulted and the page reports
`NotFoundError`. Four checks failed on that, and had been failing before any of
this work: nothing in the output distinguished "no camera on this box" from "the
permission plumbing is broken", which is the more expensive of the two to
misread. The camera checks are now conditional on `camera_reachable`, derived
from the decider's own log rather than by looking for a device — the question is
not "is there a webcam" but "does a camera request reach our code here" — and a
`--   skipped:` line says so when it does not.

The same fact bit the new case: it was written against the camera, where it would
have passed trivially and tested nothing, since no request means no dialog to
dismiss. It uses the microphone, which this machine does ask about —
`NotReadableError` is a device that exists and will not open, as against
`NotFoundError` for one that is not there.

**And case 1 could no longer be called "the defaults".** It ran without setting
anything, and three of its four features now default to `ask` — so it would have
put four modal dialogs on the screen and hung the sweep, unattended, with nothing
to say why. It sets `block` explicitly and keeps testing what it was written to
test. The prompt-answering watcher is armed for every case rather than only the
two that expect one, and the cases that expect none assert the count is zero, so
an unexpected dialog fails a check instead of hanging a sweep.

### A build bug this surfaced: moc objects tracked no headers

`test/Makefile` compiled its two moc object rules without `-MMD -MP`, alone among
the rules around them. A moc object therefore recorded nothing about the headers
it had read, and `moc_qtwebengine_view` reads `web_view_backend.h` transitively
through `qtwebengine_view.h`. When the decider's signature changed there, nothing
knew to rebuild it, and the stale object kept a vtable entry for a function that
no longer existed.

It surfaced as an undefined reference, which is the lucky outcome. A signature
change that stayed link-compatible would have produced an object disagreeing with
the rest of the program about a virtual call, with no error at all. Both rules
now emit dependencies.

**Also worth recording: the first check of that build reported success and was
worthless.** `make -C test try_permissions` is not a target — the real one is
`build-make/try_permissions` — and make said "No rule to make target" while the
grep looking only for `error:` found nothing and printed "compile done". A gate
over an output that cannot contain what it is looking for passes exactly as
loudly as a real one.

### The phone half, as far as it can be taken without the phone

The handset was not plugged in -- `lsusb` shows no device and adb has none -- so
the device check could not be run at all. Two thirds of the Android question turn
out not to need it.

**Sizing needs no change, and that is a property of `android_dialogs` rather
than luck.** It is an application-wide event filter on `QDialog`, not a list of
dialogs, so the prompt was adapted the moment it existed. The rule it applies --
a dialog fills the available screen, minimum size cleared -- is what stops a
desktop layout putting its buttons off the right edge.

**And it is measured at phone geometry, with pictures.** `try_phone` grew two
cases, camera and microphone, chosen because between them they cover every
optional paragraph the dialog can grow: the camera carries the capability
sentence on an encrypted origin, the microphone carries the not-encrypted warning
instead. 88 of 88 at 360x640 -- both shrink to the width (floors 281 and 270),
open with something focused, Tab reaches all three controls, no button is off
screen, no label is cut, and no paragraph is absorbing spare height. That last
one is the check that exists because the proxy prompt once came up with an inch
of nothing between every line, and it was found by looking at the picture rather
than by any assertion.

Looked at here too, for the same reason. Both read correctly and wrap correctly.
The spare height goes between the checkbox and the buttons, which puts them at
the bottom edge where a thumb is -- the same `addStretch(1)` `auth_dialog` needed.

**One thing the picture shows that no check asserts: Allow sits left of Block.**
That falls out of the button roles, and it is the reverse of what a browser
usually does. Left as it is, deliberately: the position a mis-tap lands on is the
refusal, which is the direction that costs a repeated tap rather than a camera.
It is worth revisiting if it turns out to annoy anybody in practice, and it is
the kind of thing only use will settle.

**And the compositing question is already answered -- by this project, some time
ago.** The paragraph that stood here said it was the one thing still unknown and
that camera and microphone on Android were therefore *worse* than they had been.
That was wrong, and wrong in the direction of having reasoned about the platform
instead of reading the file.

`android_view`'s event filter says it outright: *"Any dialog at all, modal or not:
while one is up the native view has to be out of the way, because it is drawn over
everything Qt renders."* The hazard was found on a phone -- tapping "Media (1)"
and watching nothing happen, because the button depressed behind a WebView
composited above Qt's surface -- and the fix counts visible top-level `QDialog`s
and hides the native view while any is up. `permission_dialog` is a `QDialog`, so
it was covered the moment it existed, exactly as `android_dialogs` covers its
size.

Worth noting *why* that fix is generic, because it is the reason this one is free:
the first attempt used `QEvent::WindowBlocked`, which only a **modal** dialog
sends, and the downloads dialog is shown rather than exec'd -- so the page drew
through the middle of it. Counting dialogs needs no assumption about modality, and
so needs no update per dialog.

**What is left for the handset is confirmation, not investigation.** Both
mechanisms that would have to work are generic, both are documented as generic,
and both were written against a defect found on the device. That is a good reason
to expect it to work and not a substitute for having seen it. The specific thing
to look for is a permission prompt raised over a live page rather than during a
load -- `auth_dialog` was observed on the phone, but HTTP authentication happens
before a page paints, so it does not exercise the same moment.


## Notifications, which were granted and then thrown away

The permission audit found this and could not fix it in the same breath, so the
default stayed at `block` with a note. This is the note discharged.

**Chromium treats a missing notification presenter as success.** A page calls
`new Notification(...)`, the promise resolves, and the notification goes
nowhere -- no error, nothing in a log, and the settings page showing the
capability as allowed the whole time. Nothing in this tree installed one:
`grep` found neither `setNotificationPresenter` nor `QWebEngineNotification`.
So a site could be granted notifications, believe it had them, and put nothing
on anybody's screen.

`qtwebengine_notifications` is the presenter, over freedesktop's
`org.freedesktop.Notifications` -- the same bus this project already talks to
for the colour scheme. The name puts it in the `qtwebengine_*` family that
`hydra.pro` drops from the Android build, which is correct rather than
convenient: there is no such service on a phone.

### The default is raised by whoever can deliver, not by the policy engine

`policy_engine` cannot see whether a notification will arrive, and must not
guess. So it holds `block` as a floor, and `main()` lifts it to `ask` when
`qtwebengine_notifications::install` succeeds -- which happens **before**
`main_window` reads `policy.ini`, so anything saved, including a deliberate
block, overwrites it. It raises a default; it does not overrule a decision.

The Android build has no presenter and so leaves the floor exactly where it is.

### Three mistakes in it, all found by running it

**The service was called to check whether it was there, and that was wrong
twice.** The first version sent `GetServerInformation` and waited, on the
argument that a registered name is not the same as something that will answer.
The argument is true; the trade is not. It is a blocking call on the startup
path, so a wedged notification daemon would hold the browser's launch for the
timeout -- and a blocking call cannot be answered by a service in the same
thread, which is exactly how a test stands one up. `try_notify` failed at that
line before it could test anything.

What makes the weaker check sufficient is that the strong one already happens
later and cannot be skipped: `present()` checks the `Notify` reply, so a name
nothing answers on costs one notification rather than a browser that will not
start. Activatable names count too -- a daemon started on demand is not
registered until something sends to it.

**`Notify` was called synchronously, which would have handed the GUI thread to
the desktop's notification daemon**, once per notification. It is an async call
with a watcher now. That was the second thing the test could not get past, and
it presented as the presenter silently not working -- which is precisely what a
failed notification is indistinguishable from, and why the error is now printed
under `HYDRA_NOTIFY_DEBUG`.

**And the third was the test's own.** The stub exported its slots under
`local.fake_notifier`, which is what a class name gets by default, so the call
came back `UnknownInterface`. One `Q_CLASSINFO("D-Bus Interface", ...)`. Worth
recording because the diagnosis came from the debug line added for the previous
mistake: *"No such interface 'org.freedesktop.Notifications' at object path
'/org/freedesktop/Notifications'"* named the fault exactly, in a place where the
observable behaviour was otherwise a notification nobody saw.

### What the stub proves

`try_notify` stands up a stub `org.freedesktop.Notifications` on a private bus
and drives a real page through the shell. 11 of 11: the presenter installs, the
page is granted permission, exactly one `Notify` arrives, and it carries the
page's title as the summary, its body as the body, `Hydra` as the application,
the `desktop-entry` hint, and the `default` action that a web notification means
by being clicked. The page is told `show` -- and only after the service has taken
it, because saying so on a call that then failed is the one lie that matters
here. Then `ActionInvoked` reaches the page as `onclick` and `NotificationClosed`
as `onclose`.

The stub is the point rather than a compromise: what is under test is the
arguments and the round trip, not anybody's daemon. It runs under
`dbus-run-session`, which also stops it taking a name a real daemon owns -- and
it refuses rather than taking it if one does.

**The site's own icon is deliberately not sent.** `icon()` gives a `QImage` and
the hint that carries one is a marshalled `(iiibiiay)`; that is a page-supplied
image travelling into a system service, and it is not worth writing that path
for decoration when the title already names the site.

### A fourth mistake, found by reading the API rather than by running it

**A page closing its own notification never reached the service.**
`QWebEngineNotification::closed()` is emitted when the page calls `close()` -- a
chat marking a message read, a countdown that has finished -- and nothing was
listening, so the notification stayed on the desktop after the page had withdrawn
it. The opposite direction, the service telling us, was handled from the first
version; this is the same fact travelling outward, and it was missing because
only one direction was obvious.

`withdraw()` sends `CloseNotification` and drops the entry. The ordering in
`on_closed` had to change with it: the notification is taken out of the map
*before* the page is told, because `close()` can emit `closed()`, whose handler
would otherwise send a `CloseNotification` back to the service that has just
finished saying the notification is gone.

`try_notify` grew a second notification for it, one the page takes back itself,
and the stub's ids had to stop being fixed: with one id for every notification
the close assertion was incapable of failing, since any withdrawal at all
matched. 14 of 14 now, and the last check names both ids -- closed 4244, kept
4243 -- so it says which one went rather than that one did.

### Two more build-system gaps, in the same place as the last one

`test/Makefile` scanned `live/*.cpp` for `Q_OBJECT` and had no rule to moc one:
the `.moc` rules covered `src/` and the test root, and the object rules below
them have a `live/` variant that the `.moc` rules never got. The intent was
there and the rule was missing, so the first driver to want a `Q_OBJECT` in
`live/` stopped with "no rule to make target build-make/try_notify.moc". Added.

That is the second dependency gap this session in the same file, after the moc
objects that recorded no headers. Both were invisible until something new
crossed them.

## The sweep reported a two-week-old result as a current one

Worth its own section, because the failure mode is the one this project keeps
paying for and it appeared in the tool that exists to catch it.

The sweep writes each driver's output to `$OUT/<driver>.log` and then greps that
file for a `passed,` line. `OUT` defaulted to `/tmp/hydra-sweep` -- a fixed path
in a shared directory -- and on this machine it already existed, owned by another
account. Every redirect failed with "Permission denied", every driver was
reported as "no result line and did not finish", and one was not:
`try_permissions` came back **"7 passed, 4 failed"**, in exactly the format a
real result arrives in, listing the four camera checks that had been made
conditional hours earlier. The file it read was from **14 August**.

So the sweep did not fail to measure. It measured something else and said nothing
about the substitution -- which is worse, because a run that produces no answer
gets looked at and a run that produces a plausible one does not.

Two changes, and the second is the one that matters:

- **`OUT` is per-uid.** This is the second time a fixed name in `/tmp` has cost
  something here; the offline suite's temporary paths met another uid's
  directories earlier in the same session and produced 22 failures that had
  nothing to do with the code.
- **The log is truncated before the run, and the driver's row stops if it cannot
  be.** That is what makes reading a stale file impossible rather than unlikely.
  A path that cannot be written is now a `FAIL` saying so, instead of a `grep`
  over whatever happens to be there.

**The real sweep, once it could write:** 24 passed, 8 report-only, 0 failed.
`try_permissions` 19 of 19, `try_notify` 14 of 14, `try_phone` 88 of 88,
`try_settings_ui` 89 of 89. The `try_permissions` skip was also removed from the
sweep's own list, since the driver now decides about the camera itself.

## Screen sharing, which was refused with no way to say otherwise

The last of the three permission follow-ups, and the one that needed more than
a setting. `DesktopVideoCapture` fell to the same `default:` arm that clipboard
reading and pointer lock used to, so a Teams or Meet call could be joined and
the Present button did nothing at all -- silently, which is how it went
unnoticed.

### Two questions, and they are different in kind

**Granting the permission is not choosing what to share, and the two must not be
collapsed.** "May this site present" is a decision about a *site*, and the
prompt offers to remember it. "Send *that* window, now" is a decision about this
moment, and nothing about it is stored. A meeting allowed to present last week
has not been allowed to present whatever happens to be open today.

So the order is: `screen_share` goes through the shield exactly like the camera,
and only if that says yes does `screen_picker` ask what. That is also why this
one arrived later than the other two capabilities that got features in the same
place -- for them, "yes" was a sufficient answer.

`getDisplayMedia` arrives on `QWebEnginePage::desktopMediaRequested` rather than
through `permissionRequested`, and Qt splits it out for the same reason: a
yes/no cannot carry a surface. An unanswered request leaves the page's promise
pending rather than rejected, so **every path that is not a choice calls
`cancel()`** -- no chooser, a refusal, an empty picker, a row that vanished
while the dialog was open. A page left waiting reads to somebody trying to
present as the browser having frozen, not as having refused.

### The picker is engine-neutral, which is what makes it testable

Qt hands over two `QAbstractListModel`s and takes back an index into one. Those
are Core types, so `screen_picker` never mentions the engine and can be built
against `QStringListModel` fakes. A picker that could only be exercised inside a
real `getDisplayMedia` would not be exercised at all -- there is no compositor
here that will hand out a screen capture.

Its three decisions, each the less convenient one:

- **Share is dead until something is picked.** A picker whose button works
  before a choice has been made will eventually send the wrong surface to a
  meeting, and nothing about the dialog would have warned anybody.
- **The two lists are one choice**, so selecting in either clears the other.
  Allowing both to hold a selection means the dialog has to guess, and the guess
  picks a surface to broadcast.
- **`chosen_row()` is the answer on every path**, cleared on `rejected` rather
  than on the Cancel button -- Escape and the window's close button never touch
  a button. Making the row itself the answer means the caller has no result code
  to remember to check.

It says what is at stake once, because people know it in the abstract and forget
it while joining a meeting: everything on what you pick is sent, including
anything that appears on it later.

### What running it found

**A guard caught the feature missing from a dialog.** `site_policy_dialog`
asserts that its hand-written layout covers every policy feature exactly once,
and it does -- `try_phone` died on "the shield layout does not cover every policy
feature exactly once" the first time the driver ran. That assertion is the
reason the omission cost a minute rather than a release.

**`settings_dialog` has the same kind of table and no such guard**, and it was
already missing four features -- which its sibling's comment says in as many
words. Three of them are now added under Permissions: `screen_share`, and
`pointer_lock` and `clipboard_read`, which mattered less while they were
permanently blocked and matter now that pointer lock *asks*. A capability that
asks and cannot be configured from the settings page is one whose prompt
somebody will want to stop and cannot. The remaining two, `extractor_fetch` and
`media_detect`, are not permissions and are left where they were.

**And the picture showed two things no assertion did.** A long window title
scrolled sideways instead of eliding, so reading it on a 360-pixel screen meant
dragging a scrollbar; and after a rejection the dialog still showed a
highlighted row beside a live Share button while `chosen_row()` was -1 -- an
object contradicting itself, harmless while the shell builds a fresh picker per
request and wrong for whoever reuses one next.

### What the fakes prove, and what they cannot

`test_settings` is at 165, with the new feature round-tripping through its
machine name, carrying a label and a sentence, allowing per-site without
allowing everywhere, and writing and reading back through a rule line -- the two
places a nineteenth feature silently falls out of.

`try_phone` is at 101. The picker fits 360x640 with a layout floor of 188,
opens focused, reaches all four controls by Tab, cuts no label and stretches no
paragraph -- and its selection logic is checked there too, because the offline
suites run on a `QCoreApplication` and cannot build a widget at all.

**Not verified: a real `getDisplayMedia`.** Nothing here has met a compositor
that will hand out a screen. What is proven is every part up to the engine call
-- the shield is consulted, the picker is shown, the answer is turned into a
`selectScreen` or `selectWindow` on the right model, and every other path
cancels.

## The prompt, seen on the handset

Confirmed on 2026-09-01 on the SM-F926B, Android 15, over a **live page** -- the
one moment `auth_dialog` never exercised, since HTTP authentication happens
before a page paints.

The method, because it is reusable and nothing about it needs a screenshot. A
page served from this machine through `adb reverse tcp:8099 tcp:8099`, so the
phone fetches `http://127.0.0.1:8099/` -- a loopback address, which Chromium
treats as a secure origin, which `getUserMedia` requires. The page reports each
step back to the server by `fetch`, and asks for the camera **four seconds after
`load`**, so that the request provably arrives at a page that has already
painted. The server log is the transcript:

    load   = painted
    asking = now
    camera = stream-1

The gap between the second and third lines is the prompt: the promise stayed
pending, which nothing but a waiting decider can cause. The third line is the
answer -- a real camera, one track, opened and stopped by the page immediately.

**The confirmation that it was visible came from the copyright holder, not from
an instrument, and the instruments were misleading.** `uiautomator dump` reads
the accessibility tree, and Qt's Android bridge does expose the whole shell
through it -- before a page was open it listed every menu, button and the
address bar with exact bounds, which is how the address bar was found and driven
without a screenshot. But with a page open the dump returned only the WebView's
own content, no shell and no dialog, and the reasonable-looking conclusion from
that is that the dialog was composited underneath. It was not. It was on screen
and it was answered by hand while the dump said otherwise.

Worth writing down for whoever reaches for the same tool: **the accessibility
dump shows whichever surface is on top, and a Qt dialog over a native WebView is
not something it reports.** It is an excellent instrument for driving the shell
and a useless one for deciding whether a dialog is visible.

### It is Hydra's dialog, not Android's, and that was looked at and kept

The first remark on seeing it was that it is not an Android system prompt. It is
not, and cannot be. There are two gates and they answer different questions: the
OS asks once whether *this application* may use the camera -- already granted on
that handset, which is why it did not appear -- and Hydra asks per *site*, which
is a question Android's permission says nothing about. Every browser does this
with its own UI for the same reason.

**What was genuinely open is whether it should look native, and the answer is
no.** Put to the copyright holder with three options -- leave it, restyle the Qt
dialog to look Material on Android, or drive a real Java `AlertDialog` through
JNI -- and it was left as it is. So a later session finding a desktop-looking
dialog on a phone should know it has been seen and kept: it matches every other
Hydra dialog there (authentication, certificates, downloads), it is already sized
to the screen by `android_dialogs`, it refuses on dismissal and it defaults to
Block. Reopening it needs a new reason, not a fresh opinion about how it looks.

### What that settles, and what it does not

Settled: a permission prompt raised over a live page appears, is answerable, and
its answer reaches the engine -- which is the last thing the Android permission
work was waiting on. The two mechanisms it depends on, `android_dialogs`'s
sizing and `android_view`'s hiding of the native view while any `QDialog` is up,
both did their job without a line written for this dialog.

Not settled by this run: the microphone, which was left at `block` on the device
so that exactly one prompt could appear and the pending promise would mean one
thing. And `getDisplayMedia`, which Android has no answer for at all.

### The upgrade problem this exposed, which is not about Android

**A saved `policy.ini` pins every default that was current when it was written**,
and the file on the handset said `camera=block` -- from a build that predates
`ask`. Loading it correctly beat the new default, so the prompt could not fire
until the file was edited. That is the rule working as designed: a stored setting
is a decision and must win.

The trouble is that it is not a decision. `policy_engine::save` writes **every**
feature explicitly, defaults included, so the file cannot distinguish "the person
chose block" from "block was the default the day this file was written". Anyone
who has run any previous build therefore has every future default change silently
pinned to the old value, for ever, with nothing to say so.

This was found by having to edit the phone's file to make the test run at all --
which is the same discovery a user would make by never seeing a prompt. It is not
fixed here and it is not obviously the policy engine's to fix alone: the options
are to write only settings that differ from the default (which changes the file
format and loses the record of what was current), to record a format version and
migrate on read, or to distinguish an explicit choice from an inherited one in
the file itself. It is a decision about a file people are told they can hand-edit,
and so the copyright holder's.

## The camera prompt was the wrong question, and is gone

Corrected on instruction, the same day it was built, and worth recording as a
mistake rather than quietly editing away.

**The ask was for the capability dialog, not a per-site one.** The original
request was *"code to throw up the capability activation dialogs for camera/mic
-- that is why I didn't edit them for the app"*: the application's Android
permissions had been left ungranted deliberately, so that Hydra would ask for
them. That code was written and works. What was built on top of it -- a per-site
`ask` prompt in front of it -- answers a different question, and the answer to
that one already exists:

> Per site we already have the shield menu.

Which is right, and is the part that should have been obvious. A menu somebody
opens on purpose already carries the per-site decision. A prompt raising the same
question as an interruption, mid-call, is not a second layer of safety; it is the
same question asked in the worse place.

So `camera` and `microphone` default to `allow` again. That does **not** mean any
site may have them: on Android the operating system's own permission is still in
front, which is the consent that belongs in front of a person here, and on either
platform a site is blocked from the shield menu. `setting::ask` stays in the model
and both settings surfaces offer it, so restoring a prompt is one word in a file
-- it is simply not the default, and not the shape of the answer.

**How the mistake happened is worth keeping.** The audit found a real defect: a
blocked camera reached the page as `NotAllowedError` and reached the person as
nothing. That finding was correct. The repair chosen for it was not -- it treated
"nobody was told" as "nobody was asked", and reached for a prompt, when what the
silence actually hid was the shield refusing before the operating system was ever
consulted. Fixing the visibility of a refusal and inventing a new place to ask for
consent are different jobs, and only the first was needed.

**Geolocation still asks**, and that is not an oversight: nothing carries the
question for it the way the OS permission carries camera and microphone.

### And the desktop loses a gate, which is the cost

On Android the OS permission stands where the prompt used to. On the desktop
there is no such thing, so a page that asks for the camera now gets it with
nothing in front of the person. That is a real loosening and is recorded as one
rather than left to be discovered. Setting `camera=ask` in `policy.ini` restores
a prompt there for anybody who wants it; making that the desktop default again is
a decision, not a fix.

### Proved on the handset, and the upgrade problem proved itself first

The run that confirmed it also demonstrated, unprompted, the file problem
recorded in *The prompt, seen on the handset*.

First attempt with the new build: `load = painted`, `asking = now`, and
`camera = NotAllowedError` **in the same tenth of a second**. No dialog at all.
The phone's saved `policy.ini` still said `camera=block`, written by a build that
predates any of this, and it correctly beat the new `allow` default -- so the
shield refused before Android was ever asked. Exactly the failure a person
upgrading would meet, found by meeting it.

With that one line changed to `allow` and the application's `CAMERA` grant
revoked, the second run reads:

    load   = painted
    asking = now            <- 3.8 seconds
    camera = stream-1

and the permission afterwards reads:

    android.permission.CAMERA: granted=true, flags=[ USER_SET|... ]

`USER_SET` was not there before and appears only when a person answers the system
dialog. That flag, the gap, and the revocation together are the evidence that
**Android's own capability dialog appeared and was answered** -- with no per-site
prompt in front of it. Which is what was asked for in the first place.

## A deadlock in ordinary navigation, and two things it uncovered

The ANR that ended the geolocation run was not about geolocation, and the trace
said so exactly. Two stacks, each waiting for the other:

**Android's UI thread**

    WebView shouldOverrideUrlLoading
      -> HydraWebView$3$2.shouldOverrideUrlLoading
        -> Java_se_vibes_hydra_HydraWebView_takeExternalUrl   (JNI)
          -> QMetaObject::invokeMethodImpl
            -> QLatch::waitInternal                  <- waiting for the Qt thread

**The Qt thread**

    QWidgetRepaintManager::paintAndFlush
      -> QBackingStore::flush
        -> QRhiBackingStore::flush -> QRhi::create
          -> QOpenGLContext::makeCurrent
            -> QWaitCondition::wait                  <- waiting for the UI thread

`Subject: Input dispatching timed out ... Waited 10010ms for MotionEvent`.

**The comment on `on_qt_thread` claimed this could not happen**: *"No deadlock:
the Qt thread never blocks waiting on a binder thread."* Wrong twice. Some of
those calls arrive on the **UI** thread rather than a binder thread -- the file
says so three lines further down -- and the Qt thread does block on the UI
thread, not for a binder call but for a surface, inside ordinary repainting. It
is a race, which is why a dozen navigations had already worked that afternoon.

The neighbouring `chooseFile` had the whole answer written above it since it was
added: *"Posted rather than waited on: the picker Qt is about to show needs that
same UI thread, so blocking here would deadlock the two against each other."*
The same reasoning was one function away from the bug for weeks.

### The fix is to not ask, where the question does not need asking

`take_external_url` was doing two things: deciding whether the url is the shell's
(`renders_as_page`, a pure function of the scheme) and then handing it over. Only
the second needs the Qt thread. So it is split -- `claims_external_url` answers on
whatever thread asks, `hand_to_external` is posted -- and `takeExternalUrl` never
blocks at all.

Where the question genuinely needs the Qt thread's state -- `allowNavigation`
reads the tab tree, the bridge calls reach QObjects living there -- `on_qt_thread`
now posts and waits **with a deadline**, and each caller has a documented default
if it expires. `allowNavigation` defaults to allowing, which is what the decider
itself already documents for a view with nobody listening: a refusal nobody asked
for is a browser that will not browse. The result is held by `shared_ptr`, because
a lambda that times out may still run and must not write into a stack frame that
has gone.

**A deadline is a mitigation and the split is the fix.** No
`BlockingQueuedConnection` remains in the file.

Verified on the handset: six navigations in a row, six page loads, no hang.

### Menus rendered underneath the page

Reported from the phone while the above was being tested: *"on mobile the drop
down menus go underneath the web browser content and cannot be seen"*.

Same architectural fault as the dialog case, one window type along. The event
filter that hides the native WebView tested `qobject_cast<QDialog *>` in both its
guard and its counting loop -- and a `QMenu` is not a dialog, it is a `Qt::Popup`.
So every toolbar menu and every combo-box drop-down opened behind a view that is
composited above everything Qt renders.

The two tests are now one predicate, `covers_the_page`, which is the point: the
guard and the loop having their own copy of "what counts" is exactly how a window
type came to be missing from both. Tooltips are included -- a tooltip behind the
page is the same defect in miniature, and there is no reason to leave one type
out and find it again from a phone.

Verified on the handset: with a page open, opening the File menu now puts eight
menu items on screen and no page content behind them.

### The drawer button is leftmost

Asked for, and the reason given was other phone apps that use the pattern.

It was fourth, after Back, Forward and Reload, which put the one control that
reveals *where you are* in the middle of the three that move you around. It is
first now. Verified on the device by the accessibility tree, which reads the
toolbar left to right: the button is at x=5 and Back has moved to x=74.

## Teams refused the phone, and the user agent is why again

Reported from the handset: `teams.microsoft.com` answers **"your browser isn't
supported"**.

This is the second time a site has turned this browser away over what it calls
itself, and the first time is written up above -- a Swedish bank, refusing the
`QtWebEngine/` token and a Chrome version years old. The desktop was fixed then,
in `user_agent::corrected`, applied from `qtwebengine_factory`.

**Android never got it.** Nothing in the port calls
`WebSettings.setUserAgentString`, so the System WebView sends its own default,
which names itself three times:

    Mozilla/5.0 (Linux; Android 15; SM-F926B Build/AP3A.240905.015.A2; wv)
    AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0
    Chrome/131.0.6778.200 Mobile Safari/537.36

- **`wv`** is Android's documented marker for "this is a WebView embedded in an
  application, not a browser". Sites read it that way on purpose.
- **`Version/4.0`** is the other half of the same signal: a number frozen since
  the stock Android browser, which no Chrome has sent since.
- **`Build/...`** is the device's build fingerprint. Real Chrome on Android does
  not send it, so it marks the string as not-Chrome *and* identifies the handset
  more precisely than any site needs.

Corrected, the same three rules plus the existing version replacement give
exactly what Chrome on Android sends:

    Mozilla/5.0 (Linux; Android 15; SM-F926B) AppleWebKit/537.36
    (KHTML, like Gecko) Chrome/140.0.0.0 Mobile Safari/537.36

### One function, called from Java

Every new rule is a **removal**, so a desktop string passes through untouched
and the idempotency the factory relies on -- it derives from whatever the
profile reports, which may already have been through here -- still holds. The
existing tests did not move.

The correction is not reimplemented in Java. `HydraWebView` declares a native
`correctedUserAgent(String)` and calls it with what the WebView offers; the JNI
side hands it straight to `user_agent::corrected`. A second copy of these rules
in Java would drift from the tested one, which is the failure this file keeps
recording under other names.

**It is also the one native entry point that needs no thread hop.** The
transformation is a pure function of the string -- no view, no shell, no Qt
object -- so it answers on whatever thread Java asks from, while every other
entry point either posts or waits with a deadline. Worth saying explicitly so
the next person does not wrap it in `on_qt_thread` for symmetry and reintroduce
the deadlock recorded above.

### Measured on the handset: the fix works, and it was not the cause

Both halves were put on the phone and measured, and then Teams was asked again.

**The string is right.** With the corrected build installed, a page served over
`adb reverse` reports the header and `navigator.userAgent` as

    Mozilla/5.0 (Linux; Android 15; SM-F926B) AppleWebKit/537.36
    (KHTML, like Gecko) Chrome/140.0.0.0 Mobile Safari/537.36

-- no `wv`, no `Version/4.0`, no `Build/`, and the platform and model intact.
Exactly what Chrome on Android sends.

**The client hints still name the WebView**, as predicted:

    sec-ch-ua: "Chromium";v="152", "Not?A_Brand";v="24", "Android WebView";v="152"

So a shim was added -- `user_agent::client_hints_shim`, a main-world script that
turns the `Android WebView` brand into `Google Chrome` -- and it works: the page
then reads `Chromium 152 | Not?A_Brand 24 | Google Chrome 152`. The header is
untouched, because the network stack writes it before any script runs.

**Teams refused anyway**, with both fixes in place: *"Your browser version isn't
supported. Quickest solution? Download Microsoft Teams"*.

### And the actual reason, which is neither

Asked directly, from a machine with no phone involved, with two user agents and
nothing else different:

    our mobile UA -> https://teams.microsoft.com/v2/unsupported-browser#isMobile=true
    desktop UA    -> https://teams.microsoft.com/v2/

**`#isMobile=true`.** It is a server-side redirect keyed on the `Mobile` token in
the user-agent string, and it has nothing to do with WebViews, client hints or
this browser. Chrome on Android gets the same page. Teams' web app does not serve
mobile browsers at all -- Microsoft wants the native app -- and no amount of
looking more like Chrome will change that, because looking exactly like Chrome on
Android is precisely what earns the redirect.

**Which makes the missing feature a different one: "request desktop site".** Every
mobile browser has it, this one does not, and it is the only thing that will open
Teams on a phone. It is also the general answer to a class of sites rather than a
special case for one.

**Both fixes stay, and neither is validated by an observed failure.** The string
correction is right on its own terms -- `wv` and `Version/4.0` are documented as
"not a browser", the desktop precedent is exactly this shape, and dropping
`Build/` removes a device fingerprint no site needs. The client-hints shim is the
same argument one channel along, and is the weaker of the two: nothing measured
here was fixed by it, and it is a spoof rather than a correction. It is written
down as such so that removing it later needs no archaeology.

### What was verified along the way

Verified offline: seven new checks in `test_settings` over a real WebView
string -- the three markers gone, the platform and model kept, the result shaped
like Chrome on Android, no doubled space or orphaned bracket, and idempotent.
`make jni` reports 13 native methods, every one resolvable. Both builds are
clean.

Verified offline before the phone came back: seven checks in `test_settings`
over a real WebView string, `make jni` at 13 native methods all resolvable, and
both builds clean. What the device then added is in the section above.

## Request desktop site, and Teams opens

The section above ends with the diagnosis: Teams refuses on `isMobile`, from the
server, keyed on the `Mobile` token, and Chrome on Android gets the same page.
This is the feature that answers it, and it answers a class of sites rather than
that one.

**Per tab, checkable, in the View menu, and forgotten when the tab goes.** Every
mobile browser draws the line there, and the reason is worth stating: this is a
deliberate lie, not a correction. The corrected user-agent string says something
true about what the engine is; `desktop_form` says the machine is an X11 Linux
desktop, which it is not. That is a thing to do for one page, chosen each time by
the person who wants that page, and remembering it would quietly turn a decision
into a standing claim.

`user_agent::desktop_form` is two rules -- swap the platform, drop `Mobile ` --
and the second is where the first draft broke.

**A user agent has two parenthesised groups.** The platform, and
`(KHTML, like Gecko)`. A pattern replace rewrote both and produced
`AppleWebKit/537.36 (X11; Linux x86_64)` in the middle of the string, which is
not a user agent any browser has ever sent. Replacing by index -- first `(` to
the next `)` -- is what it does now, and there is a check asserting the second
group survives.

**It was caught by running the transformation over a real string before building
it**, in six lines of Python that took less time than the build would have. Worth
recording as a habit rather than an anecdote: the two user-agent transformations
in this file were both modelled that way, and both were wrong the first time.

### The seam, and where the toggle lives

`web_view_backend::set_desktop_site` / `desktop_site`, with a base implementation
that ignores the setter and answers false. That is the honest desktop answer --
its user agent already says X11 Linux, so there is nothing to request -- and the
menu entry disables itself there rather than being a control that silently does
nothing.

The Android side sets the WebView's user agent and reloads. The reload is not
tidiness: the string is read when a page is fetched, so without it the toggle
appears to do nothing until the next navigation, which is the shape of bug that
gets reported as "the setting is broken".

The state is read back from the view on every action refresh rather than
remembered from the last click, because the toggle is per tab and switching tabs
has to show that tab's answer.

### Measured on the handset

With the toggle on, the local page reports

    Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko)
    Chrome/140.0.0.0 Safari/537.36

-- platform swapped, `Mobile` gone, second bracket group intact.

`teams.microsoft.com` then **loads**: no unsupported page, no "Download Microsoft
Teams", and the Microsoft sign-in appears, which is the web app's entry point.
Fifteen page nodes where there had been one.

### It reloads what was asked for, not what was arrived at

The first version reloaded the current url, and that made the feature useless in
exactly the case it exists for. By the time anybody reaches for "request desktop
site" the redirect has already happened and the tab is sitting on
`/v2/unsupported-browser` -- so the reload fetched the refusal again and nothing
appeared to change. A toggle that then requires retyping the address is doing
half the job.

Android's WebView answers this directly: `getOriginalUrl()` is the url as
requested, before redirects. Fetching that instead is one condition --

    String original = w.getOriginalUrl();
    if (original != null && !original.isEmpty() && !original.equals(w.getUrl()))
        w.loadUrl(original);
    else
        w.reload();

-- falling back to a plain reload where there was no redirect, where the WebView
has no original to give, or where the two are the same.

Verified as the sequence a person actually performs: navigate to
`teams.microsoft.com`, be refused, open View, tick Request Desktop Site, and the
Microsoft sign-in appears. No retyping, no second navigation.

**Not established: that a Teams meeting works.** Sign-in was not attempted -- it
is the copyright holder's account. What is established is that the door opens,
which is the thing that was shut.

## Back closed the browser, and menus had no way out

Reported from the phone: *"menus like shield can't be closed without pressing
back that closes hydra"*, and separately that tapping outside a menu should
dismiss it, *"like the tab menu where this doesn't work"*.

**Nothing in this project handled the Back button at all.** No `Qt::Key_Back`,
no `keyPressEvent` for it, nothing. Qt's default for an unhandled Back is to
finish the activity, so the only button Android users reach for reflexively
closed the whole browser -- from anywhere, including with a menu open, which is
how it was found. It is also why a browser's Back never went back: the history
button in the toolbar worked, the hardware one quit.

`main_window::handle_back` answers it in the order every mobile browser uses,
each step the least destructive thing still available:

1. a `Qt::Popup` -- any menu, any combo drop-down -- is closed;
2. a modal dialog is **rejected**, because Back is a way out and not a way of
   agreeing, which is the answer every dialog here already treats as safe;
3. the tab drawer is closed;
4. the page goes back in history;
5. and only then is it left unconsumed, so Android does what it does at the root
   of a browser.

**Installed on the application, not on the window.** A popup takes the keyboard
grab while it is up, so a filter on `main_window` would never have seen the one
press that matters most.

### Tapping beside the drawer

The menus were half the report and the drawer was the other half. A `QMenu` gets
outside-dismissal from Qt for nothing -- `Qt::Popup` grabs the mouse and closes
on a press outside -- but the tab drawer is an ordinary widget slid into place by
an animation, so nothing dismissed it and the only way out was the button it came
from.

A press that lands on this window and not on the drawer now closes it, and is
consumed. That is the bargain a scrim makes everywhere else: while the drawer is
open the rest of the window is a way of closing it rather than a set of controls,
so the first tap closes and does not also press what was underneath.

### A note on measuring this, because it wasted an hour

The scripted harness -- `adb shell input tap` against coordinates read from
`uiautomator dump` -- stopped working partway through, and did so *silently*:
taps landed nowhere, dumps came back stale or empty, and the reasonable reading
of that was "the build broke navigation". An A/B against the previous apk showed
the same nothing, which should have been the clue and was instead read as the
regression being older.

It was neither. The copyright holder tested by hand and reported camera and
microphone working in both modes, on the build the harness said could not load a
page at all. **The instrument had failed, not the program**, and every conclusion
drawn from it in that window was worthless.

Qt's accessibility bridge is the weak point -- `Qt A11Y: Could not run
accessibility call in object context, no valid surface` appears in logcat around
the failures -- and it is the same instrument this file already warns about for a
different reason above. Two lessons kept rather than one: a dump that returns
fewer nodes than the last one is suspect before the code is, and a hand test from
somebody looking at the screen outranks any of this.

## The standing answer: request desktop site as a site rule

The View menu's toggle is per tab and forgotten, which is right for a one-off
and wrong for a site that will *never* serve a phone. Teams is that site --
it redirects a mobile user agent server-side, on every visit -- and re-ticking a
menu item each time is not an answer. Asked for, and added.

`desktop_site` is the twentieth policy feature. Off by default, because it is a
deliberate lie about the machine rather than a correction, and not one to tell on
somebody's behalf. It sits in the shield's **Content** group rather than with the
capabilities: it changes what the site is *told* about the browser, which decides
what it serves, and that is a content question rather than a permission.

### Applied on arrival, and why it costs a round trip

It is not part of `view_settings`. That struct carries things the engine can be
told at any moment; this one changes the user agent, which is read when a page is
*fetched*, so it can only take effect on a load.

So `apply_policy` sets it when a view arrives somewhere, and the first visit to
such a site loads as a phone, is noticed, and reloads. One extra round trip, then
the page that was wanted. That is the honest cost of a switch that has to be
decided before the request it affects, and the alternative -- deciding during
`allowNavigation` -- cannot work: the UI thread is blocked inside that call
waiting on the Qt thread, so anything posted from there runs *after* the load has
already started.

**The reload does not loop**, because `set_desktop_site` returns early when the
value has not changed: after the second load the view already agrees with the
policy. The View menu's toggle writes the same state through the same path, so a
site rule and a one-off cannot disagree about what a view is currently doing.

### The guard earned itself again

`site_policy_dialog` asserts its hand-written layout covers every policy feature
exactly once, and `try_phone` refuses to run when it does not. That is the second
feature in a day whose omission from that panel cost a minute instead of a
release. `settings_dialog` still has no such guard and still needs its row added
by hand, which is recorded above and remains true.

Six checks in `test_settings`, `try_phone` at 101, both gates clean. Built and
signed; not yet installed, because the handset went off adb again before it could
be.

## What is next (in order)

Rewritten after a session that closed most of what used to be on it. What is
listed here is open; what closed is recorded in the sections above rather than
carried along as amendments to a list item.

1. **The permissions work is built and wants a real device for the last two
   claims.** `ask` exists, the decider is asynchronous, the prompt appears and
   both answers reach the engine; notifications are presented; screen sharing
   has a feature, a picker and a wired-up engine signal. The sections above
   record each. Two things cannot be checked from here.

   The handset half is **done** — see *The prompt, seen on the handset*. A prompt
   raised over a live page appears, is answerable, and its answer reaches the
   engine; a real camera opened and stopped. What that run exposed instead is an
   upgrade problem in the policy file, recorded in that section and left as the
   copyright holder's: a saved `policy.ini` pins every default that was current
   when it was written, so no future default change reaches anybody who has run
   a previous build.

   **And meet a real `getDisplayMedia`.** Everything up to the engine call is
   proven against fakes -- the shield is consulted, the picker shown, the answer
   turned into a `selectScreen` or `selectWindow` on the right model, and every
   other path cancels -- but nothing here has a compositor that will hand out a
   screen capture, so the last hop is unmeasured. A meeting on the desktop is
   the test.

   Two capabilities stay blocked on purpose and are not outstanding work.
   Clipboard reading is gated by the engine behind `JavascriptCanAccessClipboard`
   and `JavascriptCanPaste`, which this project leaves off, so the request never
   arrives; its default moves if and when those do. Notifications stay blocked on
   Android, which has no service to present with.

2. **The loop works on a disguised manifest; make it work on a noisy capture.**
   Three runs in five on dramafren now return `url.includes('cf-master')` — a
   stable fragment, no tokens, the master manifest on a site with no `.m3u8`
   anywhere. That is the case the whole content-type tier exists for and the
   first output here that would survive a second visit. See the three-arrangement
   section above; the short version is that the note had to be printed *away
   from the rows*, and prose never moved the number in three attempts while
   layout moved it twice.

   **kisskh is 1 of 5 now, and the accept is the real manifest** -- see the
   section above. Its manifest is in the payload and annotated, so the runs
   measure the model. The difference from
   dramafren is noise: one useful note among eighty-nine requests of Google and
   Firebase furniture, against three in a row on a clean media host. Every
   failure was refused by a different layer — two wrote this visit's tokens into
   the script, two picked an image, one picked an analytics beacon — so there
   are no false accepts to chase, only a hit rate.

   **Corrected by the section above, and the correction matters.** kisskh's
   payload does carry its manifest, annotated, and the probe budget is not the
   problem — so "spend more probe budget" is off this list.

   **Asking the model for both clauses is not on this list either, because the
   commit that wrote this paragraph also did it.** `7515847` changed the prompt
   and added these lines together: rule 3 has read *"Write two tests, not one"*
   since 4 August, naming the note-driven match first and the manifest-extension
   fallback second, and the paragraph beneath it already tells the model that a
   master playlist is routinely disguised with an innocuous extension **and a
   query string**. The reasoning stands — kisskh scored 2 of 5 under the older
   arrangement purely on an `.m3u8` fallback, and the legend that won dramafren
   3 of 5 stopped it writing that clause at all, so one script has to hold both
   — but it is a description of what the prompt says, not of work outstanding.
   It has sat here as a plan for a month.

   **What the first measurement of it shows is that the instruction is not
   reliably obeyed.** Eight runs on the synthetic set — see *The live model suite*
   above, and note that the set does not transfer — produced four accepts, and
   of the three runs whose script was kept, two wrote both clauses and one wrote
   only the note-driven match. So the prompt asks for two tests and gets two
   about two thirds of the time. That is the gap now, and it is a compliance
   question rather than an instruction one: the sentence is already there.

   One observation too small to be a finding, recorded so it is not
   rediscovered: of those three kept scripts the only one with an end-anchored
   `\.(m3u8|mpd)$` fallback was the only one rejected, and its sibling clause
   `/master\.txt/` could not match `cf-master.1774687168.txt` either. n is 3.

   The retry is measured but not yet answered: of five runs, three never
   answered inside fifteen minutes on a loaded machine, one retry came back
   still refused and one timed out. That is a one-run result wearing a five-run
   coat, and it wants an idle machine — **not a model, which was never the
   blocker.** Ollama serves `qwen2.5-coder:14b` here and has throughout, so the
   part of this item deferred for want of one never needed to be;
   `test_live_model` runs, and the eight runs in *The live model suite, run for
   the first time* above are the first this tree has taken.

   What those runs cannot supply is the real captures this item is about:
   `evidence/` is not in this checkout and the synthetic set does not transfer.
   What they did supply transfers, because it is about regular expressions
   rather than about the corpus — an end-anchored `\.(m3u8|mpd)$` fallback is
   defeated by a query string, which is what a real manifest url carries. So
   the 2 of 5 kisskh scored "purely on an `.m3u8` fallback" is a property of
   that site's urls rather than of the loop, and the plan above to have the
   model write both clauses needs the fallback unanchored to be worth
   anything.

   Site 2 is kisskh (`kisskh.co`, player CDN `kisscloud.online`), and it *is*
   recorded — `evidence/README.md` names the site, the episode and the exact
   stream to be found. The line that used to sit here saying otherwise was
   stale.

3. **Nothing has needed the helper tier's DOM half, and two captures is two.**
   The decision is made and recorded -- the permission is gone and the design
   stays, in arch §11.5.1 and in the section above. What keeps this on the list
   is that "nothing has needed it" rests on two measured sites, both of which
   had the stream in the request log. A third site that computes its address in
   page JS would reopen it, and there is no way to know without meeting one.

4. **An indicator for the AI batch jobs.** Recorded, not designed — see
   *Wanted: an indicator for the AI batch jobs* above for the requirement and
   the four questions that have to be answered before any of it is built. It
   comes after the browser is a browser, on the holder's instruction.

5. **Android's remaining gap is the platform's autofill, and only the runtime
   half is now unverified.** The four code-level preconditions are checked and
   met — see *Two searches that found the code correct* above — and the test
   handset has an autofill service configured, so the emulator's excuse is
   gone. What is left is one measurement, needing no credentials and no
   screenshot: open a local html login form, tap the username field, and read
   `adb shell dumpsys autofill` for whether a session started against the
   WebView's view structure. Wake the phone first; a dozing one reads as a
   regression.

   §19's list is otherwise done — System WebView, drawer, request filter, script
   bridges, external links, file picker, player handoff, downloads that a file
   manager can see. Autofill on Android is the system service's job rather than
   this browser's, and the menu no longer offers a KeePassXC pairing that cannot
   exist there. The line that used to close this item — that the claim "needs a
   device that does" have an autofill service — is answered: there is one, and
   the browser's side of the arrangement is verified. Only the fill itself is
   open.

6. **What is left untested now needs a network or a device.** The sweep through
   never-tested files is finished — see the sections above; four of nine were
   wrong. The line that used to sit here said the remaining dialogs were covered
   only incidentally and that a unit test for one would be testing Qt. That is
   no longer the state: `try_phone` opens all thirteen windows and measures each
   for width, button reachability, cut labels, stretched paragraphs, opening
   focus and Tab coverage — and it found five real defects doing it, including
   one in `site_policy_dialog`, which was on that list as not worth testing.

   What remains genuinely out of reach here is the WebEngine backend and the
   thin adapters around it, which need a page rather than a fixture, and are
   driven through the shell by the live drivers instead.

7. **Whether a `file:` url should open as a page is still open, and is the
   copyright holder's.** The littering half is fixed — see *The url no longer
   becomes a directory* above — so what remains is only the question the fix
   deliberately did not answer: `main.cpp` classifies `file:` as a tree path,
   on the recorded grounds that `hydra ./tree.txt` has always meant the tree.
   The cost of leaving it is that the desktop entry claims `text/html` and
   passes `%U`, so a file manager handing over `file:///home/me/doc.html` gets
   a browser that says it cannot use the argument and opens the personal tree
   instead. The distinction that would preserve the recorded intent exactly is
   the scheme rather than the path: `./tree.txt` has none, `file:` always did.

8. **CI skips one check for want of an icon theme.** `test_theme`'s
   system-icon-directory assertion has nothing to look at in the
   `debian:trixie` container and says so rather than failing. Installing
   `hicolor-icon-theme` would make it run; that is one word in a dependency
   list which is deliberately "every direct dependency named", and an icon
   theme is a test dependency rather than a build one. Left for whoever owns
   that list.

9. **A node's url means two things at once, and rows can be internally
   inconsistent.** See *One field with two meanings* above: the title follows
   the page and the url does not, so a row can carry a title from one page and
   a url from another — measured. The obvious repair breaks the tab lock,
   which stores its pin in the same field. Separating them is a tree-file
   format change and so the copyright holder's; the three options and their
   costs are in that section. The pin is now asserted in `test_model`, so the
   repair fails loudly rather than quietly.

10. **KeePassXC support is broken and needs the account that has a working
   set up.** See *KeePassXC: the socket path, settled before the session that
   can test it* above. The socket path is fixed and ruled out for an ordinary
   desktop; what is untested is everything after it — framing, key exchange,
   association — because the bridge has never met a real KeePassXC. Run
   `HYDRA_KEEPASS_INTERACTIVE=1 QT_QPA_PLATFORM=offscreen
   ./test/build-make/try_keepass` from that account, against a throwaway
   KeePassXC rather than the real vault. If that set up is a Flatpak, try the
   `app/org.keepassxc.KeePassXC/` socket variant first — it is the one thing
   about the path still unverified.

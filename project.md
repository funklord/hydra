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
`tests/build/` are in-tree and do survive, so the desktop suite runs without a
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
`~/.claude/tools/commit-msg` is a convention change to raise rather than make in
passing. It will refuse the commit if one of those five is ever amended.

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
drivers are report-only and `tests/README.md` now names them. Worse,
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

## The icon## The icon## The icon

`icons/` holds the app icon and `icons/build_icons.py` regenerates every size
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

All seven sizes are compiled in through `icons/hydra.qrc` and added to a single
`QIcon` in `main.cpp`, so Qt chooses per use instead of rescaling one image;
adding only the large one would quietly discard the tuned small ones.
`packaging/install-icons.sh` lays the same files into a `hicolor` theme with
the desktop entry.

**Judge small icons at their real size.** `tests/` has nothing for this because
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

**The Makefile is a wrapper over CMake, not a build system.** It exists because
this tree was the odd one out: beerssh and fuzzypickles' `gui/` subtree both
present `make` / `make test` / `make android` with `DEBUG=1` and `SANITIZE=1`,
and hydra presented two different cmake invocations plus a per-binary test run
you had to know to prefix with `QT_QPA_PLATFORM=offscreen`. Now all three look
the same from outside. CMake underneath is unchanged and can still be driven
directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2       # a number, always: see the warning above
```

**A migration to qmake was considered and deferred rather than rejected.** The
argument for it is real — a Makefile is easier to read than `CMakeLists.txt`,
and beerssh ships a Qt 6 app *and* an APK from qmake today. What is keeping
CMake is dependency discovery and the target count, and the Makefile's own
header lists the three things a later migration has to solve so they are not
rediscovered: 54 executables of which 21 are globbed so adding one costs
nothing; `find_package(LibtorrentRasterbar)` disambiguating rasterbar's library
from rakshasa's identically-named one, which pkg-config alone cannot; and the
host pkg-config answering cheerfully for an Android cross build, which once
reported libsodium found and failed at link looking like a toolchain fault.

Requires Qt 6 with **Widgets** and **WebEngineWidgets** (Arch: `qt6-base
qt6-webengine`; Debian/Ubuntu: `qt6-base-dev qt6-webengine-dev`), CMake ≥ 3.19,
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
runtime.

### The cookie filter, half proven

`tests/live/try_cookies` drives it the way the interceptor was driven, and for
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

`tests/live/try_permissions` drives geolocation, camera, microphone and
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
| Consent banners | `consent_blocker.{h,cpp}`, `site_rules.{h,cpp}`, `consent_dialog.{h,cpp}` | answers "accept cookies?" dialogs; rules as data, shareable later |
| Anti-adblock notice | `antiadblock_watch.{h,cpp}` | says so when a page is checking for a blocker, and names the lever |
| Shared rule store | `site_rules.{h,cpp}` | consent-banner and detector rules as one file, with provenance; the unit a future exchange would move |

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
wrong nonce and a wrong key all pass. ~~What remains unexercised is everything
above the crypto — the socket handshake, association, and `get-logins` have
never run, because KeePassXC itself is not installed.~~ **All of it runs now.**
KeePassXC is installed, and the handshake, association, a stored pairing
restored and accepted, and `get-logins` against a real vault are measured — see
"The KeePassXC bridge finally met KeePassXC" and "A pairing that survives a
restart". Also not done from §13: the key icon and entry-picker UI (a single
match fills automatically; multiple matches are deliberately left alone rather
than guessed at), `set-login` on new-credential submit, `generate-password`,
storing the association key encrypted at rest via Secret Service — it is in
memory only, so pairing does not survive a restart — and the optional
direct-`.kdbx` fallback, which §13.4 recommends against anyway.

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
again the fix is invisible. Seven checks (`tests/live/try_adblock_fix`), driven
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

**Verified through the real shell, 34 checks** (`tests/live/try_consent`),
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

**15 checks** (`tests/live/try_settings_ui`): the list and stack exist and are
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
`tests/live/try_subframe` is the reproduction and the proof, offline and
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

The measurement item 1 existed for. Everything before it was dramafren, where
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

## Android: it builds, and there is an APK

Not a port — a **placeholder behind the seam**, which is a smaller thing that
proves a larger one. The seam has claimed since step 3.5 that adding a platform
is "one new backend pair plus a different two lines in `main()`". That claim is
now measured.

**The core is genuinely platform-neutral.** Fifty-one translation units compiled
for `arm64-v8a` with **no errors** and no changes. The only link failure was the
three `qtwebengine_factory` symbols `main()` names — exactly the file the design
says should be the only one that knows.

`android_view` is honest about what it is: it renders a message saying the web
view is not written yet and shows the address it was asked to open. A stub that
displayed a blank page would be indistinguishable from a real backend that is
broken, and this project has lost enough time to things that look like they
work. `set_script_bridge` does nothing on purpose — on Android that becomes
`addJavascriptInterface`, and pretending to register a bridge that cannot
deliver would make every script that waits for one hang rather than fail.

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

### The key, which §13.2 asked for as an icon and gets as a word

It appears when a page has a login form, and **before the gate runs** rather than
after — which is the whole design. A key that showed up only when a fill
succeeded would be absent from exactly the pages where someone needs to know
why nothing happened. Four states, each a different thing to do next: hidden,
`Key` (this page has a form), `Key ✓` (filled, click to do it again) and
`Key ✕` with the refusal in its tooltip. Clicking re-runs the whole gate rather
than re-opening a cached answer, because caching the answer means holding
credentials past the fill that asked for them.

**A word, not an icon**, and that is a deliberate deviation from §13.2's
wording: this toolbar is made of text actions — Media, Shield — and the icon set
is the app's own mark at seven sizes, not a symbol library. Inventing one glyph
for one affordance would make it the odd one out.

**The tooltip is where the reason lives**, not the status bar. A status message
is gone in six seconds and the empty form is still sitting there; this project
has already recorded a defect where a message was written into a label something
else overwrote.

`try_autofill` drives it through the real shell, **8 checks, no KeePassXC
needed** — autofill is HTTPS-only by default, so a login form served over plain
http is refused for a reason the shell knows on its own, which makes the whole
chain observable without a vault or a pairing dialog. Two defects in the driver
before it worked, both this project's own recurring shapes: it took the first
`QLineEdit` it found, which is the sidebar's *search* box, so every navigation
filtered the tree instead of loading anything; and it never activated a tree
node, so there was no view to navigate at all. Both failed by blaming the
feature.

**Still not built from §13.2:** `set-login` on new-credential submit, and
`generate-password`.

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

Failing suites now write their full output to `tests/build/failed/<t>.log`. The
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

For the runs that do need a model, `tests/live/measure.sh` runs them at `nice 19`
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

Both captures, five runs each, through `tests/live/measure.sh` at `nice 19` on a
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

That last one is the general lesson. **A driver is only a picture of the
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
bug.** CMake copies `sample-tree.txt` next to it, so the beside-the-binary branch
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

The repository never had this problem and still does not: `tests/live/shoot.sh`
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
(`tests/tree_gen.h`) parameterised on the three numbers that matter: how many
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

So `tools/jni_check.py` compares them at rest: every `native` method's expected
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
`BIN=tests/build`, which was CMake's, and CMake is gone. The directory survived
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

**And then the other two, which needed something different.** The note that
used to sit here said they wanted evidence the fixture did not produce. That
was wrong: both call `choose_ai()` first and return with a status message when
no provider answers. They were blocked on a model, not on fixture content --
the same blocker as `try_evolve_confirm`. Reading the two slots settled in a
minute what guessing had got backwards.

With Ollama running they both open, and so does the reorganizer, which is
gated the same way. The fixture did need extending for the *filter* dialog to
have anything to say: `filter_signals::looks_ad_shaped` refuses first-party
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

`tests/live/media_fixture.h` serves the shape instead -- a manifest, segments
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

`tests/live/sample_tree.h` gives each driver a private copy under its own
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

`tests/Makefile` built one `libhydra_app.a` and linked all seventy-odd binaries
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

`tools/objsets.py` asks fmake once, rewrites the answer into the object names
`tests/Makefile` uses, and writes `tests/objsets.mk`: 73 programs, 3655
objects, 50 apiece. It is committed, so an ordinary build needs `make` and
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
by both `src/main_window.cpp` and `tests/build-make/libhydra_app.a` -- a build
artifact of one system read as source by another. That went away with the
archive.

### The build system, decided: qmake and fmake, no CMake

Three ways to build the same tree had been measured and none chosen. The
decision is two: **Makefile + qmake** as the one people run, and **fmake** as
the second opinion. CMakeLists.txt and tests/CMakeLists.txt are gone.

**What each does now.** `make` runs qmake against `hydra.pro` and builds into
`build/`; `make test` drives `tests/Makefile`; `make android` runs the Android
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

**The GUI pass** closed, in order: two empty states that could not say which
kind of empty they were, navigation buttons that offered what they could not do,
a window title that never named the page, no sign that a page was loading, no
way to stop one, no find-in-page, no zoom, no link target on hover, a tab whose
renderer died in silence, a link that opened nothing, a certificate failure that
read as a site being down, and a site asking for a password that nobody
answered. Each is a section above with what it cost and why the answer is the
one chosen.

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

`openIn` is deliberately not called. Taking the url and opening it ourselves
drops the opener relationship, which a page can otherwise use to reach back into
the window that spawned it.

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

The sweep moved out of a scratch directory into `tests/live/sweep.sh` (and
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

## What is next (in order)

Rewritten after a session that closed most of what used to be on it. What is
listed here is open; what closed is recorded in the sections above rather than
carried along as amendments to a list item.

1. **The loop works on a disguised manifest; make it work on a noisy capture.**
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
   problem — so "spend more probe budget" is off this list. What is on it: ask
   the model for *both* a note-driven match and a manifest-extension fallback in
   the same script. kisskh scored 2 of 5 under the older arrangement purely on
   an `.m3u8` fallback, and the legend that won dramafren 3 of 5 stopped it
   writing that clause at all. One script can hold both.

   The retry is measured but not yet answered: of five runs, three never
   answered inside fifteen minutes on a loaded machine, one retry came back
   still refused and one timed out. That is a one-run result wearing a five-run
   coat, and it wants an idle machine.

   Site 2 is kisskh (`kisskh.co`, player CDN `kisscloud.online`), and it *is*
   recorded — `evidence/README.md` names the site, the episode and the exact
   stream to be found. The line that used to sit here saying otherwise was
   stale.

2. **Nothing has needed the helper tier's DOM half, and two captures is two.**
   The decision is made and recorded -- the permission is gone and the design
   stays, in arch §11.5.1 and in the section above. What keeps this on the list
   is that "nothing has needed it" rests on two measured sites, both of which
   had the stream in the request log. A third site that computes its address in
   page JS would reopen it, and there is no way to know without meeting one.

3. **Android's remaining gap is the platform's autofill, and it is unverified.**
   §19's list is otherwise done — System WebView, drawer, request filter, script
   bridges, external links, file picker, player handoff, downloads that a file
   manager can see. Autofill on Android is the system service's job rather than
   this browser's, and the menu no longer offers a KeePassXC pairing that cannot
   exist there. What is *not* established is that filling works: the emulator has
   no autofill service configured, so that claim needs a device that does.

4. **What is left untested now needs a window or a network.** The sweep through
   never-tested files is finished — see the sections above; four of nine were
   wrong. The remainder are dialogs (`media_dialog`, `filter_dialog`,
   `reorganize_dialog`, `site_policy_dialog`), thin adapters (`capture_source`,
   `qtwebengine_interceptor`), and the WebEngine backend, all of which the live
   drivers already drive through the shell. A unit test for any of them would be
   testing Qt.

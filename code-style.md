<!-- The three rules and their detail are copied from
     ~/.claude/guidelines/code-style.md -- the source. Keep in sync; fix
     drift the moment you notice it. -->

# code-style.md

Code style for this project. Applies to everything the project writes --
the Qt Widgets app (`src/`), the Android glue, the packaging and the build
files.

**The global source**, `~/.claude/guidelines/code-style.md`, applies to
every private project and sits above both this file and `project.md`. Where
either disagrees with it, that is **drift to fix, not a local override**. A
genuine divergence needs a technical reason and is raised rather than
decided in passing -- and when a conflict actually comes up, stop and ask
instead of picking a winner.

`third_party/` keeps its upstream style and is exempt. So do generated
sources: the `hydra_seed.qrc` and `sample-tree.txt` copy the build writes
into the build directory, and anything `moc` or `androiddeployqt` produces.

## The three rules

1. **`snake_case`, not `camelCase`,** for identifiers this project defines.
2. **Tabs for indentation, spaces for alignment.**
3. **Lowercase filenames,** unless a tool demands otherwise.

Everything below is these three rules in detail, plus the exceptions that
are already settled. An exception not listed here is not yet settled: raise
it rather than deciding in passing.

## 1. Naming

`snake_case` for functions, variables, class and struct names, and fields.

This holds **even though the whole app is Qt C++**. Qt's own API is
`camelCase` and you call it exactly as it is (`setParent`, `addWidget`) --
that is not a violation, it is the API's name. But names *you* introduce
stay `snake_case`, and do not let the surrounding convention pull your own
names across. Qt signals and slots are ordinary members and follow the same
rule.

- **C++ member variables keep an `m_` prefix** but are otherwise
  `snake_case`: `m_accept`, `m_active`.
- Prefer the plain descriptive name over the redundant one. Name the thing,
  not its category: `plan`, not `plan_struct` or `plan_result`. No
  `I`-for-interface prefix, no `Abstract` prefix.
- **No abbreviations that are not already vocabulary.** `observed`, not
  `obs`; `interface`, not `iff`. This matters most wherever an internal
  name escapes into something you cannot rename later -- a wire format, a
  config key, a CLI output, an on-disk path. In this project that includes
  the tree file's keys (`created=`, `seen=`, `named=`) and the extractor
  payload's column headings, both of which are read back by later versions.
- **One word per concept, everywhere.** The same word in the type name, the
  file path, the setting and the documentation. A synonym introduced for
  variety reads as a second concept. Use the component names `project.md`
  already establishes rather than inventing synonyms, and update
  `project.md` if one genuinely needs renaming.

### Prefixes, and visibility

Prefixes exist to keep this project's symbols from colliding with a
library's. So they follow **visibility**, and the choice is judgement
rather than a mechanical rule:

- **Anything with more than small visibility carries the project prefix** --
  the public API, and anything a linker or importer outside its own module
  can reach.
- **Module-private symbols are left unprefixed**, precisely so that the
  absence of a prefix reads as "this does not leave the module."

**No project prefix on type names here.** `tab_tree_model`,
`consent_blocker`, `ai_provider` -- not `hyd_tab_tree_model`. This is the
discretion the source allows rather than a divergence from it: this is a
leaf application with nothing linking against it. If any of this is ever
extracted into a library, the prefix rule applies to what is extracted.

The middle case decides itself on link safety, not on taste. A symbol that
is internal by intent but still reaches the linker -- cross-file within a
library, not `static`, not part of the API -- is *not* private for this
purpose. Prefix it. **A deliberate parallel copy of a function in two
libraries needs a distinct name**, not the same name in both on the
assumption that nothing will ever link both sides; that assumption fails
later, at a call site that changed nothing, and names files you did not
touch. This project now builds its sources into a static library that both
the app and every test driver link, so two definitions of one name are
reachable in a way they were not when each driver compiled its own copy.

Where a language enforces its own scheme, accept it rather than fight it:

- **Python** -- a leading underscore (`_name`) is the language's private
  marker and stands in for "unprefixed" above. This project's Python is
  `icons/build_icons.py` and `tests/live/serve.py`.
- **Rust** -- not used here. **Debian package spellings** are kebab-case
  where `dpkg` requires it, and read back with their own spelling; the
  package is `hydra` and the directory is not named differently from it.

## 2. Indentation and alignment

Indent structural nesting with **tab** characters, one tab per level. When
lining up tokens *within* a line -- parameters under an open paren, a
constructor's member-initialiser list, a block comment's `*` column, an
aligned trailing comment -- use **spaces** after the indent tabs.

Alignment is expressed relative to the shared leading tabs, so it survives
at any tab width. No tab width is prescribed; the viewer decides.

**Never mix tabs and spaces within the indent itself.** Tabs come first and
spaces come after; the reverse, or an alternation, is what breaks at a
different tab width -- and in Python it is a syntax error.

`switch` bodies get a full tab too: a `case`/`default` body is one level
deeper than its brace nesting, since C++ labels open no brace.

The whole of `src/` is already tab-indented. Keep it that way.

### Settled exceptions

Divergence needs a technical reason. These are already accepted and need no
discussion:

- **Makefile recipe lines** -- `make` requires a literal tab. Compliant by
  construction.
- **YAML** -- the spec forbids tabs for indentation outright. Use spaces.
- **Markdown** -- list continuation and code fences are space-indented by
  specification. Exempt, which covers `project.md`, this file and
  everything in `docs/`.
- **Debian packaging files** -- exempt, and the two halves are exempt for
  different reasons. `debian/changelog` has a fixed layout that a tab is
  not part of: `dpkg-parsechangelog` calls a tab-indented change line
  "unrecognized" and loses the trailer outright if a tab precedes `--`. A
  deb822 continuation in `control` or `copyright` is the opposite case --
  `deb822(5)` allows a leading SPACE *or* TAB and dpkg round-trips either,
  but that leading whitespace is field syntax rather than indentation, so
  the rule has nothing to say about it and everything past it is
  alignment. Both measured against dpkg rather than read off the manual.
- **Vendored and generated sources** -- exempt, per the header above.

Python deserves a note, because PEP 8 prefers spaces and the tension looks
worse than it is: the language's only hard rule is that indentation must
not be *ambiguous*, and tabs-then-spaces is unambiguous at every tab width.
Continuation lines inside brackets are not indentation-significant at all.
Never a space *before* a tab in leading whitespace -- that is the case that
raises `TabError`.

Anything else that seems to need spaces: raise it, get it settled, and add
it here.

## 3. Filenames

Lowercase, `snake_case`, for everything this project names itself --
sources, headers, docs. So `tab_tree_model.cpp`, not `TabTreeModel.cpp`.
`src/` is already consistent.

Settled exceptions:

- **Names a tool will not accept lowercased** -- `CMakeLists.txt`,
  `AndroidManifest.xml`, `Makefile`.
- **Root files with an established convention** -- `README.md`, `LICENSE`.
- **Package-system spellings** -- kebab-case where Debian requires it.

## Formatters

A formatter is allowed **only if it can be configured to honour the three
rules completely**. Configuration gaps are disqualifying, not something to
work around: a formatter that gets indentation right and alignment wrong
will rewrite the tree on somebody's next save.

The decision is per tool, per project, and it is a real evaluation. If a
tool can be made to comply, use it and commit the config with a comment
saying which setting is load-bearing and what happens without it. If it
cannot, do not run it -- **not even ad hoc on a single file**. If no
existing tool fits and the rule is worth mechanising, write our own; a
checker that only gates indentation is worth more than a formatter that
reflows everything.

Record the decision **and the finding that produced it**, so a verdict does
not get re-litigated and a tool that improves later can be reconsidered.

Naming and filename rules are review items, not automated ones.

### clang-format is deliberately NOT used

There is no `.clang-format` here, and adding one would be a mistake rather
than an oversight. The reasoning was established in the sibling
`fuzzypickles` and `beerssh` trees the hard way:

- With no config present, `clang-format` falls back to LLVM defaults, which
  are **spaces**. Running it silently converts tab-indented sources -- this
  has actually happened over there, reverting committed files.
- A config does not fix it. Continuation parameters sit one column past the
  open paren, which clang-format cannot express
  (`AlignAfterOpenBracket` aligns *to* the paren), so any config would
  reflow nearly every multi-line signature in the tree.

**Do not run it, not even ad hoc on a single file.**

What would change the answer: an `AlignAfterOpenBracket` mode that offsets
from the paren rather than aligning to it, together with tab indentation
that survives a missing config.

This project runs the shared gate: `make style`, which is
`tools/style_gate.py`, copied verbatim from `~/.claude/tools/style_gate.py`.
`.style-gate.toml` says which files here it applies to, and the floor it
carries makes it fail rather than pass when that file list collapses.

## 4. GUI toolkit

**Qt Widgets. Not QML, not QtQuick** -- a project-level rule that predates
the global one and agrees with it. Qt WebEngine is the content layer; the
chrome around it is Widgets. Deviating needs a very good reason, raised
before the code is written rather than defended after it.

## Precedence

Three layers, and they are not equals:

1. **The global guidelines** (`~/.claude/CLAUDE.md` and the files it
   imports) -- the source, and they win.
2. **The project's `project.md`** -- project-specific design and conventions.
3. **This file** -- the copy.

A project copy that disagrees with the source is **drift, not an
override**: fix it. A project that genuinely needs to diverge needs a
technical reason, and that is a decision to raise with the user -- not one
to make while working on something else.

**When a conflict between layers actually comes up, stop and ask.** Do not
silently pick a winner, even the global one.

This precedence rule lives here and in the global guidelines only. It does
not belong in `project.md`.

## Keeping this copy in sync

This file copies the source at `~/.claude/guidelines/code-style.md`. Below
the copied rules it adds only what is genuinely this project's own: its
exempt paths, its clang-format verdict, its `m_` and no-type-prefix
decisions, its Qt Widgets rule.

**"Do not diverge" means semantically identical, not byte-identical.** What
must match is every rule and every exception, in substance.

**Re-read the source before reconciling this file**, rather than working
from what was loaded at the start of a session -- a copy reconciled against
a stale source is drift being written rather than fixed.

## ASCII in source

Source and comments are ASCII. Write `--` where prose would use an em dash,
and "section" for a section sign.

This governs the text the repository writes about itself, not the data the
software handles. Documentation may use typographic punctuation; so may
user-facing text in UI software, and anything that genuinely requires
Unicode.

Where a project needs the rule enforced, `ascii_only` in `.style-gate.toml`
turns it on. In Python it enforces exactly that shape -- ASCII outside
string literals, Unicode allowed inside them -- because the gate reads the
file with `tokenize`. Other languages get a whole-file byte check, having no
tokenizer there, and so does a Python file that will not tokenise: a file
nobody can parse is not a file that has been cleared.

It was the whole file for everyone until a project that prints two status
ticks had to switch the check off to keep them, which switched it off for
its comments as well, and an em dash arrived in one. **An exception wider
than its reason is how a rule stops being enforced.**

**This project is the same shape, and further along it.** `ascii_only` is
off here, and the reason is real: the tree is C++, the gate has no C++
tokenizer, and a whole-file byte check cannot tell a glyph in a button label
from an em dash in a comment. The glyphs are genuine output -- the toolbar's
`☰`, the media dialog's `▶ Watch` and `⬇ Download`. But the exception taken
for them covers every comment in the tree, and counting says what that
bought. Of 1114 non-ASCII characters in the 233 C and C++ files the gate
reads, 260 are inside string literals and are output; the other **854 are in
comments, across 163 files** -- 437 em dashes and 369 section signs, which
are the two characters this rule names by example.

Nothing catches those today. Closing it needs either the comments spelled
back to ASCII first or a C++ tokenizer in the gate, and each is its own
piece of work rather than something to do in passing. Recorded in
`project.md` with the measurement.

## The commit-msg hook

The commit-msg hook is `tools/hooks/commit-msg`, installed with `make hooks`.
It rejects generator attribution and a subject over 75 columns. It lives in
the tree rather than only in `.git/hooks` so that it is reviewable and
survives a clone; the copy that runs is installed from it.

Two things it deliberately does not reject. The directory `.claude` and the
file `CLAUDE.md` are names, so a message may say where the shared tooling
comes from -- the ban is on crediting a generator, and neither spelling is
one. And it ignores what git is about to discard: comment lines, and the
diff that `git commit -v` puts below the scissors line. Reading those
refused commits over text that never reaches the message -- the hook's own
diff contains its own pattern list, so it rejected every commit that
edited it.

## See also

- **`~/.claude/guidelines/code-style.md`** -- the source this file copies.
- **`project.md`** -- what exists, what is next, and why. It wins over this
  file where the two disagree.
- **`../fuzzypickles/code-style.md`**, **`../beerssh/code-style.md`** -- the
  sibling Qt Widgets trees, which carry the same rules and the same copies
  of `style_gate.py` and the commit-msg hook.

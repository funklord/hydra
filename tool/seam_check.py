#!/usr/bin/env python3
"""The web-view seam, checked rather than asserted.

Architecture doc sec 19.2: the shell talks to a page through
`web_view_backend` and knows nothing about Qt WebEngine. Only the
`src/qtwebengine_*` files may name the engine's types or include its headers.

**Why this is worth a gate rather than a rule.** `hydra.pro` drops
`src/qtwebengine_*.cpp` and `.h` from the Android build, so a stray
`#include <QWebEngineProfile>` anywhere else compiles cleanly on a desktop
and breaks a build nobody on this machine can run. That is the shape
`evidence.md` calls a gate being weakest where it matters: the violation
gets written on the machine that cannot detect it. It held when this was
written -- zero engine includes and zero `QWebEngine*` uses outside the
backend -- and holding today is exactly what a check is for.

**Prose is not a dependency.** Every non-backend file that mentions the
engine does so in a comment, and several must: the seam header explains what
it is a seam from. So comments and string literals are stripped before
looking, which is the difference between this and the one-line grep that
reports fourteen files and means nothing.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# The files allowed to know. A name rather than a pattern would go stale the
# first time a fifth backend file appeared; the build system selects these by
# the same prefix, so the two cannot drift apart.
ALLOWED_PREFIX = "qtwebengine_"

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"](Qt?WebEngine[^>"]*)[>"]', re.M)
TYPE = re.compile(r'\bQWebEngine\w*')


def strip_comments_and_strings(text):
	"""Code only. Block comments, line comments and string literals go.

	Character by character rather than by regex, because a `//` inside a
	string literal and a quote inside a comment each defeat the obvious
	pattern, and this file's own doc comment is full of both.
	"""
	out = []
	i, n = 0, len(text)
	while i < n:
		c = text[i]
		nxt = text[i + 1] if i + 1 < n else ""
		if c == "/" and nxt == "/":
			while i < n and text[i] != "\n":
				i += 1
		elif c == "/" and nxt == "*":
			i += 2
			while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
				i += 1
			i += 2
		elif c in "\"'":
			quote = c
			i += 1
			while i < n and text[i] != quote:
				i += 2 if text[i] == "\\" else 1
			i += 1
			out.append(" ")
		else:
			out.append(c)
			i += 1
	return "".join(out)


def faults_in(name, text):
	"""Every way this file names the engine, as (line, what) pairs."""
	code = strip_comments_and_strings(text)
	found = []
	for m in INCLUDE.finditer(code):
		found.append((code[:m.start()].count("\n") + 1,
		               "includes %s" % m.group(1)))
	for m in TYPE.finditer(code):
		line = code[:m.start()].count("\n") + 1
		if any(l == line and w.startswith("includes") for l, w in found):
			continue
		found.append((line, "names %s" % m.group(0)))
	return [(name, l, w) for l, w in found]


def self_test():
	"""Samples broken in each way this checker claims to catch.

	**A detector over an all-clean corpus is a green light with no
	demonstrated ability to be anything else**, and this corpus is clean --
	that is the point of the gate. So the controls run first and their
	verdicts are compared; a checker that misclassifies one of them reports
	nothing, because a result from it would mean nothing.
	"""
	bad = [
		# **Lower-case header path on purpose.** The obvious sample,
		# `#include <QWebEngineProfile>`, is matched by the TYPE pattern as
		# well -- so with the include half deliberately blinded the control
		# still passed and reported a clean tree. Measured, not reasoned: the
		# sabotage was run and rc stayed 0. A control has to be able to fail
		# the way the thing it controls for fails, and this one names a header
		# no type pattern can match.
		("an include", '#include <QtWebEngineCore/qwebengineprofile.h>\n'
		                'int f() { return 1; }\n'),
		("a type", 'void g(QWebEngineProfile *p);\n'),
		("a type in a call", 'auto *v = new QWebEngineView(this);\n'),
	]
	good = [
		("a comment naming it",
		  '// QWebEngineProfile is what the backend uses.\nint f() { return 1; }\n'),
		("a block comment",
		  '/* includes <QWebEngineProfile> in the backend */\nint f() { return 1; }\n'),
		("a string literal",
		  'const char *s = "QWebEngineProfile";\n'),
		("a comment holding a quote",
		  "// the engine's own QWebEngineView lives behind the seam\nint f() { return 2; }\n"),
	]
	wrong = []
	for what, text in bad:
		if not faults_in("<control>", text):
			wrong.append("missed %s" % what)
	for what, text in good:
		if faults_in("<control>", text):
			wrong.append("flagged %s" % what)
	return wrong


def main():
	wrong = self_test()
	if wrong:
		print("seam-check: CONTROL FAILED -- %s; no result below means "
		       "anything" % "; ".join(wrong), file=sys.stderr)
		return 2

	files = sorted(p for p in SRC.iterdir()
	                if p.suffix in (".cpp", ".h")
	                and not p.name.startswith(ALLOWED_PREFIX))
	if not files:
		print("seam-check: no files to check, which is not a pass",
		       file=sys.stderr)
		return 2

	faults = []
	for p in files:
		faults += faults_in(p.name, p.read_text(encoding="utf-8"))

	if faults:
		print("seam-check: the engine is named outside src/%s*:"
		      % ALLOWED_PREFIX, file=sys.stderr)
		for name, line, what in faults:
			print("  src/%s:%d: %s" % (name, line, what), file=sys.stderr)
		print("  The shell talks to a page through web_view_backend "
		       "(architecture doc sec 19.2), and hydra.pro drops the "
		       "qtwebengine_ files on Android -- so this compiles here and "
		       "breaks the build that cannot be run here.", file=sys.stderr)
		return 1

	print("seam-check: %d file(s) outside src/%s* and none names the engine"
	      % (len(files), ALLOWED_PREFIX))
	return 0


if __name__ == "__main__":
	sys.exit(main())

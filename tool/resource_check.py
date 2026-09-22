#!/usr/bin/env python3
"""Every `:/` path the code asks for, against what the `.qrc` files publish.

**This is a bug that already happened here, and nothing prevented it.** The
comment at the top of `icon/hydra.qrc` records it: the prefix was `/icons`
while `main.cpp` asked for `:/icon/`, so every `addFile` found nothing and the
application had no icon at all. `QIcon::addFile` reports a missing resource by
returning quietly, which is why it was noticed by looking rather than by
anything failing.

The same silence covers the whole class. `QFile(":/missing")` opens nothing
and says so only if somebody reads the status; a `QIcon` with no file in it
draws nothing; and a resource path is a *string*, so no compiler and no
linker has an opinion about it. The build's `rcc` step catches a qrc naming a
file that is not on disk -- the opposite direction -- and nothing at all
catches code naming a path no qrc publishes.

**Patterns are checked by their fixed prefix**, because that is what the real
fault looked like. `QStringLiteral(":/ui/%1.svg").arg(name)` cannot be
resolved without knowing every `name`, but `/ui/` either is a published
prefix or it is not, and in the `/icons` incident it was not.

**A control runs before any file is read**, because every result below is a
silence: a checker that has stopped resolving anything reports a clean tree in
exactly the words of a real pass. It fabricates a published set and two
references, one that must resolve and one that must not, and refuses to report
on the tree if either comes out wrong.
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# Directories whose `.qrc` files are not this project's to check.
SKIP_DIRS = {"build", "third_party", "attic", "test"}

# Prefixes published by Qt rather than by this tree. Each needs a reason: an
# allowlist without one is how a gate gets switched off by instalments.
FOREIGN = {
	# Qt WebChannel compiles its own JavaScript API into the library and
	# publishes it under this prefix. `qtwebengine_view.cpp` reads it to
	# inject the bridge, and no qrc here can or should publish it.
	"/qtwebchannel/": "published by Qt WebChannel",
}


def strip_comments(text):
	"""Comments out, string literals kept -- the paths live in the strings."""
	text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
	return re.sub(r"//[^\n]*", "", text)


def published(root):
	"""[(qrc, resource path, file on disk)] for every <file> in every qrc."""
	out = []
	for qrc in sorted(root.rglob("*.qrc")):
		if any(part in SKIP_DIRS for part in qrc.relative_to(root).parts[:-1]):
			continue
		tree = ET.parse(qrc)
		for qres in tree.getroot().findall("qresource"):
			prefix = qres.get("prefix", "/")
			if not prefix.startswith("/"):
				prefix = "/" + prefix
			for node in qres.findall("file"):
				name = (node.text or "").strip()
				alias = node.get("alias") or name
				out.append((qrc, prefix.rstrip("/") + "/" + alias,
							qrc.parent / name))
	return out


def references(src):
	"""{path: [files]} for every ":/..." literal, patterns included."""
	found = {}
	for f in sorted(src.glob("*.cpp")) + sorted(src.glob("*.h")):
		text = strip_comments(f.read_text(encoding="utf-8"))
		for m in re.finditer(r'"(:/[^"]*)"', text):
			path = m.group(1)[1:]
			# `"://"` is a url's scheme separator, not a resource root. A
			# resource path never has an empty first component.
			if path.startswith("//"):
				continue
			found.setdefault(path, []).append(f.name)
	return found


def unresolved(refs, names):
	"""[(path, why)] for each reference nothing publishes."""
	bad = []
	for path in sorted(refs):
		if any(path.startswith(p) for p in FOREIGN):
			continue
		if "%" in path:
			stem = path.split("%")[0]
			if not any(n.startswith(stem) for n in names):
				bad.append((path, "no published path starts with " + stem))
		elif path not in names:
			bad.append((path, "no qrc publishes it"))
	return bad


def control():
	"""Fabricated, and run before the tree is read. Returns a reason or None."""
	names = {"/icon/hydra-16.png", "/ui/back.svg", "/sample-tree.txt"}
	good = {"/sample-tree.txt": ["a.cpp"], "/ui/%1.svg": ["b.cpp"]}
	bad = {"/icons/hydra-%1.png": ["c.cpp"], "/nope.txt": ["d.cpp"]}
	if unresolved(good, names):
		return "a reference that resolves was reported as dangling"
	missed = {p for p, _ in unresolved(bad, names)}
	if missed != set(bad):
		return "a dangling reference was not reported: " + str(set(bad) - missed)
	return None


def main():
	failed = control()
	if failed:
		print("resource-check: the control failed -- %s." % failed,
              file=sys.stderr)
		print("                No result below means anything.", file=sys.stderr)
		return 2

	pub = published(ROOT)
	if not pub:
		print("resource-check: no .qrc file published anything; a sweep over "
              "an empty set passes for free.", file=sys.stderr)
		return 2

	names = {name for _, name, _ in pub}
	gone = [(q, n, d) for q, n, d in pub if not d.is_file()]
	refs = references(SRC)
	bad = unresolved(refs, names)

	for qrc, name, disk in gone:
		print("%s: publishes %s, but %s is not there"
              % (qrc.relative_to(ROOT), name, disk.relative_to(ROOT)),
              file=sys.stderr)
	for path, why in bad:
		print("%s: asks for \":%s\" and %s"
              % (", ".join(sorted(set(refs[path]))), path, why), file=sys.stderr)

	if gone or bad:
		print("resource-check: %d dangling reference(s), %d missing file(s)"
              % (len(bad), len(gone)), file=sys.stderr)
		return 1

	print("resource-check: %d resource path(s) published, %d referenced in "
          "src/, every one resolves" % (len(pub), len(refs)))
	return 0


if __name__ == "__main__":
	sys.exit(main())

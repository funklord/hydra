#!/usr/bin/env python3
"""Every architecture-section number cited from the code, against the document.

`working-practice.md` makes the design document authoritative over the code,
and this tree takes that seriously: 397 citations of 51 distinct section
numbers, from source comments and from test headers. A number is a **stable
anchor**, which is the whole reason to cite one -- and nothing was checking
that the anchors exist.

The failure is the one this workspace keeps meeting. A renumbered or deleted
section does not break a build; it leaves several hundred comments pointing
somewhere else, and a comment that sends a reader to the wrong place is worse
than one that sends them nowhere, because they arrive and believe it.

**Two anchor shapes, because the document has two.** Numbered headings are the
usual one. Section 12 alone writes its parts as bold numbered paragraphs --
`**1. Signal collection.**` -- rather than `### 12.1`, and eleven files cite
those as `§12.1` through `§12.5`. Both are read here. That the document is
inconsistent with itself is a real observation and is reported in the summary
rather than quietly absorbed: making section 12 look like sections 11 and 13
is the copyright holder's to decide, and is not something a checker should do
by refusing to read what is there.

**A citation naming another document is not this one's.** `RFC 8216 sec
4.3.2.2` is the HLS byte-range rule and has nothing to do with the
architecture. The test is what stands immediately before the number, not a
list of exceptions.

**A control runs before either file set is read.** Every finding is a number
that is absent, and a checker that has stopped resolving reports a clean tree
in the words of a real pass.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "doc" / "architecture.md"
SOURCES = ("src", "test", "test/live")

HEADING = re.compile(r"^#+\s+(\d+(?:\.\d+)*)\.?\s", re.M)
SECTION = re.compile(r"^##\s+(\d+)\.\s")
STEP = re.compile(r"^\*\*(\d+)\.\s")
# A citation, with whatever stands in front of it on the same line.
CITE = re.compile(r"([^\n]{0,24}?)(?:sec|§)\s?(\d+(?:\.\d+)*)")
# ... and what makes it somebody else's document.
FOREIGN = re.compile(r"(RFC\s*\d+|ISO\s*\d+|IETF)\s*$", re.I)


def anchors(text):
	"""Every number the document can be pointed at."""
	found = set(HEADING.findall(text))
	current = None
	for line in text.split("\n"):
		m = SECTION.match(line)
		if m:
			current = m.group(1)
		step = STEP.match(line)
		if step and current:
			found.add("%s.%s" % (current, step.group(1)))
	return found


def citations(root):
	"""{number: {file}} for citations that mean the architecture document."""
	out = {}
	for where in SOURCES:
		d = root / where
		if not d.is_dir():
			continue
		for f in sorted(d.glob("*.cpp")) + sorted(d.glob("*.h")):
			text = f.read_text(encoding="utf-8")
			for before, number in CITE.findall(text):
				if FOREIGN.search(before):
					continue
				out.setdefault(number, set()).add(str(f.relative_to(root)))
	return out


def unresolved(cited, have):
	return sorted((n for n in cited if n not in have),
	               key=lambda x: [int(p) for p in x.split(".")])


def control():
	have = {"11", "11.5", "12", "12.3"}
	if unresolved({"11.5": {"a"}, "12.3": {"b"}}, have):
		return "a citation that resolves was reported as missing"
	missed = set(unresolved({"11.5": {"a"}, "99.1": {"b"}}, have))
	if missed != {"99.1"}:
		return "a missing citation was not reported: %r" % (missed,)
	if not FOREIGN.search("An omitted offset is not zero. RFC 8216 "):
		return "an RFC citation was not recognised as another document's"
	if FOREIGN.search("the picker writes into the AI list, "):
		return "an ordinary sentence was taken for another document's"
	return None


def main():
	failed = control()
	if failed:
		print("doc-check: the control failed -- %s." % failed, file=sys.stderr)
		print("           No result below means anything.", file=sys.stderr)
		return 2

	if not DOC.is_file():
		print("doc-check: %s is not there; a check against a document that "
		       "does not exist passes for free." % DOC.relative_to(ROOT),
		       file=sys.stderr)
		return 2

	text = DOC.read_text(encoding="utf-8")
	have = anchors(text)
	cited = citations(ROOT)

	for what, got in (("section anchors", have), ("citations", cited)):
		if not got:
			print("doc-check: found no %s at all; the pattern has stopped "
			       "matching and the comparison is vacuous." % what,
			       file=sys.stderr)
			return 2

	missing = unresolved(cited, have)
	for number in missing:
		print("%s: cites §%s and doc/architecture.md has no such section"
		       % (", ".join(sorted(cited[number])), number), file=sys.stderr)
	if missing:
		print("doc-check: %d citation(s) point at nothing" % len(missing),
		       file=sys.stderr)
		return 1

	files = set()
	for where in cited.values():
		files |= where
	stepped = sorted(n for n in have if "." in n and n.split(".")[0] == "12")
	print("doc-check: %d section(s) cited from %d file(s), every one "
	       "resolves%s"
	       % (len(cited), len(files),
	          "; §12's parts are bold numbered paragraphs rather than "
	          "headings, unlike every other section" if stepped else ""))
	return 0


if __name__ == "__main__":
	sys.exit(main())

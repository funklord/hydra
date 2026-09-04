#!/usr/bin/env python3
"""Public methods declared in src/*.h that nothing in src/ or test/ calls.

An interface is only as wired as its least-used method, and a method that
exists, compiles and is obviously correct attracts no suspicion. A signal with
no listener at least looks unfinished; a complete-looking interface looks done.
Reading the declarations finds nothing, because the declarations are exactly
what is not wrong -- so this reads the callers instead.

**Run by hand, and deliberately not a gate.** It reports two names today that
are correct as they are, so wiring it into `make style` would need an ignore
list, and a gate carrying an ignore list has been switched off by instalments.
A one-off sweep is read once by the person who ran it, where over-reporting
costs a look and buys the errors you can catch.

    python3 tool/dangling.py .              the tree as it is
    python3 tool/dangling.py /path/to/old   an older tree, as a control

**The control is the second form.** A probe whose failure mode is silence has
to be shown capable of speaking: run it against a commit from before a known
dangling method was wired and check that it names it. Against 8b9949a it
reports five, three of which were fixed in the commit that added this file.

Bounded: a fixed file list, no subprocesses, no loop that can grow. It has had
three bugs of its own, all of which made it quieter or slower rather than
wrong-and-loud -- a file reread inside a per-line loop, a per-name rescan of
every file, and a `^` without `re.M` that made a set of class names come back
all but empty, which reads exactly like a header with no classes in it.
"""
import os, re, sys, glob, collections

root = sys.argv[1] if len(sys.argv) > 1 else "."
hdrs = sorted(glob.glob(os.path.join(root, "src", "*.h")))
srcs = sorted(glob.glob(os.path.join(root, "src", "*.cpp")) +
              glob.glob(os.path.join(root, "src", "*.h")) +
              glob.glob(os.path.join(root, "test", "*.cpp")) +
              glob.glob(os.path.join(root, "test", "live", "*.cpp")) +
              glob.glob(os.path.join(root, "test", "live", "*.h")))

# name -> where declared
decl = {}
everywhere = {}
DECL = re.compile(r'^\t(?:[A-Za-z_][\w:<>,\s\*&]*?[\s\*&])([a-z_][a-z0-9_]*)\s*\(')
# re.M, or `^` matches only the very start of the file and this set
# comes back all but empty -- which reads exactly like a header with no
# classes in it.
CLASS = re.compile(r'^\s*(?:class|struct)\s+([a-z_][a-z0-9_]*)\b', re.M)
for h in hdrs:
	access = "private"
	text = open(h, encoding="utf-8", errors="replace").read()
	# Read once per header, not once per declaration: the first version of
	# this line reread the file inside the loop and the probe stopped
	# terminating in any useful time.
	classes = set(CLASS.findall(text))
	for i, line in enumerate(text.splitlines(), 1):
		st = line.strip()
		if st in ("public:", "private:", "protected:",
		           "public slots:", "private slots:", "protected slots:",
		           "signals:", "public Q_SLOTS:"):
			access = st.rstrip(":")
			continue
		# **Every section feeds the ambiguity map, only public ones are
		# reported.** `find_bar::dismissed` is a SIGNAL, and it is what hid
		# `consent_blocker::dismissed` -- an accessor with no caller whose
		# comment names two consumers it does not have. A map built from
		# public methods alone could not see the collision that caused the
		# miss.
		if m0 := DECL.match(line.rstrip("\n")):
			everywhere.setdefault(m0.group(1), set()).add(
			    "%s:%d" % (os.path.basename(h), i))
		if access not in ("public", "public slots"):
			continue
		if "operator" in st or st.startswith("//") or st.startswith("*"):
			continue
		m = DECL.match(line.rstrip("\n"))
		if not m:
			continue
		name = m.group(1)
		# A constructor is not a method with callers to find, and this project
		# names classes in snake_case -- so `android_factory(request_filter *)`
		# matches the declaration pattern exactly. Reported once as a finding
		# before anybody read the line it pointed at.
		if name in classes:
			continue
		# constructors/destructors and Qt machinery
		if name in ("qt_metacall", "qt_metacast", "metaObject", "tr"):
			continue
		if re.search(r'\b(override|final)\b', st):
			continue          # a virtual: the base's callers are the callers
		decl.setdefault(name, []).append("%s:%d" % (os.path.basename(h), i))

# count uses, excluding the declaration lines themselves
#
# **One pass per file, not one per name.** The first version ran two regex
# scans for each of 400 names over each of 90 files -- 72,000 scans of text up
# to a couple of hundred kilobytes -- and stopped finishing inside two
# minutes. Collect every call-shaped and qualified identifier once, then look
# the names up in the counter.
CALL = re.compile(r'(?<![\w:])([a-z_][a-z0-9_]*)\s*\(')
QUAL = re.compile(r'::([a-z_][a-z0-9_]*)\b')
use = collections.Counter()
for f in srcs:
	text = open(f, encoding="utf-8", errors="replace").read()
	use.update(CALL.findall(text))
	use.update(QUAL.findall(text))

# **A name declared by more than one class cannot be counted this way**, and
# the failure is silent and in the comforting direction: `annoyance_log::all`
# and `consent_blocker::dismissed` both have no caller anywhere, and both were
# invisible here because `site_rules::all` and `find_bar::dismissed` exist and
# are used. The count is per NAME, so a busy namesake hides an unused method.
#
# Resolving it properly needs to know the type of the object at each call
# site, which needs a parser. What this does instead is refuse to answer:
# a name declared in more than one place is reported as ambiguous rather than
# counted, so a miss becomes a question instead of a silence. Both instances
# above were found by hand, after the probe had reported a clean sweep twice.
def shared(name):
	return len(everywhere.get(name, ())) > 1

ambiguous = [(n, sorted(everywhere.get(n, ())))
              for n in sorted(decl) if shared(n)]
dangling = []
for name, where in sorted(decl.items()):
	if shared(name):
		continue
	# the declaration site itself matched once as "name(" -- discount it
	if use[name] - len(where) <= 0:
		dangling.append((name, where))

print("declared public: %d names" % len(decl))
print("no caller found: %d" % len(dangling))
for name, where in dangling:
	print("  %-34s %s" % (name, ", ".join(where)))
# Ordered by how little the name is used anywhere, because that is where an
# unused member is most likely to be hiding: a name with two declarations and
# three uses has at most one spare, while one with forty is busy in both
# classes and says nothing. The count is the whole point of the ordering -- it
# is not a verdict, and every row here needs a person.
print("declared by more than one class, so not counted: %d" % len(ambiguous))
for name, where in sorted(ambiguous, key=lambda r: use[r[0]]):
	print("  %-30s uses=%-4d %s" % (name, use[name], ", ".join(where)))

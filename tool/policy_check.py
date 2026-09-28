#!/usr/bin/env python3
"""Every policy feature, against something that enforces it.

The shield and Settings offer a per-site answer for each feature in
`policy.h`. A feature nobody enforces is a control that accepts an answer
and does nothing with it -- the shape this tree has already met twice, and
both times it was invisible from the UI side because the control looked
exactly like the ones that work.

`desktop_site` on the desktop and `screen_share` on Android are the two
where doing nothing is correct, and both are greyed with the reason on them.
That is a per-platform absence; what this checks is the other thing, that
something outside the policy and UI layer reads the feature at all.

**The alias is the trap, and it is why this file exists rather than a
one-line grep.** Several sites write `F::autoplay`, `F` being a local alias
for `policy::feature`, and a pattern looking for `feature::autoplay` reports
two wired features as unenforced. The first version of this sweep did
exactly that. So the control below asserts that a feature reachable *only*
through the alias is still found -- a check whose pattern has quietly
narrowed reports a clean tree in the words of a real pass.
"""

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# Where the feature is defined, decided and drawn. Enforcement is everywhere
# else, and that is the whole question: a feature named only in these files
# is one the UI offers and nothing acts on.
LAYER = {
	"policy.cpp", "policy.h",
	"policy_engine.cpp", "policy_engine.h",
	"settings_dialog.cpp", "settings_dialog.h",
	"site_policy_dialog.cpp", "site_policy_dialog.h",
	"permission_dialog.cpp", "permission_dialog.h",
}


def pattern(name):
	return re.compile(r"\b(?:policy::feature|feature|F)::%s\b" % re.escape(name))


def features(text):
	r"""The enumerators, including any written with an initialiser.

	**This read 19 of 20 for as long as it existed.** The pattern was
	`^\t(\w+),` and the first enumerator is `javascript = 0,` -- so the one
	feature every page depends on was the one the gate never checked, and the
	count it printed said 19 beside an enum of 20 with nothing comparing them.
	"""
	body = re.search(r"enum class feature\s*:[^{]*\{(.*?)\}", text, re.S)
	if not body:
		return []
	found = re.findall(r"^\t(\w+)\s*(?:=[^,]*)?,", body.group(1), re.M)
	return [m for m in found if m != "count"]


def table_names(text):
	"""The same features counted a second way: the descriptor table in
	policy.cpp, which carries one row per feature and is indexed by the enum.

	**Only the number is comparable, deliberately.** These are the machine
	names -- the JSON keys rules persist under -- so they are camelCase and
	some are not the enumerator's word at all (`media_detect` is
	`autoDetectMedia` on disk). What the two readings share is how many
	features there are, which is the quantity that went wrong: one reading of
	one file cannot tell a narrowed pattern from a shortened enum, because
	both come back as a smaller number that still looks like a result.
	"""
	return re.findall(r'^\t\{ "(\w+)",', text, re.M)


def enforcers(name, sources):
	return sorted(f for f, t in sources.items()
	               if f not in LAYER and pattern(name).search(t))


def control():
	"""A feature reachable only through the alias must still be found."""
	fake = {"elsewhere.cpp": "if (m_policy->is_allowed(F::autoplay, host))"}
	if not enforcers("autoplay", fake):
		return "a feature written as F::autoplay was not found"
	if enforcers("autoplay", {"policy.cpp": "feature::autoplay"}):
		return "a use inside the policy layer was counted as enforcement"
	if enforcers("camera", fake):
		return "a different feature's name matched"
	# The enumerator forms this has to survive. The second one is the form the
	# first feature is written in, and missing it is what this file was doing.
	sample = ("enum class feature : int {\n"
	           "\tjavascript = 0,\n\tcookies,\n\tcount\n};\n")
	if features(sample) != ["javascript", "cookies"]:
		return ("an enumerator written with an initialiser was not read (%s)"
		         % features(sample))
	return None


def main():
	failed = control()
	if failed:
		print("policy-check: the control failed -- %s." % failed, file=sys.stderr)
		print("              No result below means anything.", file=sys.stderr)
		return 2

	head = SRC / "policy.h"
	if not head.is_file():
		print("policy-check: %s is not there; a sweep over a file that does "
		       "not exist passes for free." % head.relative_to(ROOT),
		       file=sys.stderr)
		return 2

	names = features(head.read_text(encoding="utf-8"))
	if not names:
		print("policy-check: read no features from the enum at all; the "
		       "pattern has stopped matching and every check below is "
		       "vacuous.", file=sys.stderr)
		return 2

	body = SRC / "policy.cpp"
	if not body.is_file():
		print("policy-check: %s is not there, so the second reading of the "
		       "feature list cannot be taken." % body.relative_to(ROOT),
		       file=sys.stderr)
		return 2
	table = table_names(body.read_text(encoding="utf-8"))
	if len(names) != len(table):
		print("policy-check: the enum and policy.cpp's table disagree about "
		       "how many features there are, so one of the two readings is "
		       "wrong:", file=sys.stderr)
		print("              enum  (%d): %s" % (len(names), ", ".join(names)),
		       file=sys.stderr)
		print("              table (%d): %s" % (len(table), ", ".join(table)),
		       file=sys.stderr)
		return 2

	sources = {}
	for f in sorted(os.listdir(SRC)):
		if f.endswith((".cpp", ".h")):
			sources[f] = (SRC / f).read_text(encoding="utf-8")

	missing = [n for n in names if not enforcers(n, sources)]
	for n in missing:
		print("src/policy.h: %s is offered in the shield and named nowhere "
		       "outside the policy and UI layer, so nothing acts on the "
		       "answer" % n, file=sys.stderr)
	if missing:
		print("policy-check: %d feature(s) with no enforcement" % len(missing),
		       file=sys.stderr)
		return 1

	print("policy-check: %d feature(s), the enum and policy.cpp's table "
	       "agreeing on the number, every one read outside the policy and UI "
	       "layer" % len(names))
	return 0


if __name__ == "__main__":
	sys.exit(main())

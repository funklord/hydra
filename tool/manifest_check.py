#!/usr/bin/env python3
"""The Android manifest, against what the rest of the tree assumes about it.

**A whole suite rests on one attribute here and nothing reads it.**
`test/test_rotation.cpp` opens by explaining that
`android:configChanges` declares `orientation|screenSize|screenLayout|...`,
so Android RESIZES the window on a rotation instead of destroying and
recreating the activity -- "there is no save, no restore, and nothing is
serialised, because nothing is torn down". Every one of that file's checks is
about the resize path. Delete `orientation` from the manifest and Android
recreates the activity: tabs torn down, state serialised or lost, and the
suite passes exactly as before, because it resizes a desktop window and never
opens the manifest.

That is the shape `evidence.md` calls a gate being weakest where it matters --
the machine that would notice is the phone, and the phone is not where the
edit gets made.

The other three are cheaper and the same kind: a name in XML that must match
a name somewhere else, where a typo is not a build error but an app that will
not start, or a `make android-install` that installs and then cannot find
what to launch.

**A control runs before the manifest is read.** Every finding here is the
absence of a string, and a checker that has stopped looking reports a clean
manifest in the words of a real pass.
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "android" / "AndroidManifest.xml"
JAVA = ROOT / "android" / "src"
ANDROID_NS = "{http://schemas.android.com/apk/res/android}"

# What the rotation design needs, and why each one. A config change NOT listed
# here is not a finding: the manifest declares more than this and may declare
# more still, and a gate that pinned the whole string would fail for an
# addition somebody made on purpose.
NEEDED_CONFIG = {
	"orientation": "turning the phone",
	"screenSize": "the window's size in dp changing with it",
	"screenLayout": "a foldable opening, which changes the size class",
	"smallestScreenSize": "the same fold, by the other measure Android reports",
}


def make_var(text, name):
	"""`NAME = value` from a Makefile, first definition wins."""
	m = re.search(r"^%s\s*[:?]?=\s*(\S+)" % re.escape(name), text, re.M)
	return m.group(1) if m else None


def config_changes(root):
	for activity in root.iter("activity"):
		value = activity.get(ANDROID_NS + "configChanges")
		if value:
			return set(value.split("|"))
	return set()


def control():
	"""Fabricated, before anything is read. Returns a reason or None."""
	declared = {"orientation", "screenSize", "screenLayout", "smallestScreenSize"}
	if [k for k in NEEDED_CONFIG if k not in declared]:
		return "a complete set was reported as missing something"
	if "orientation" in (declared - {"orientation"}):
		return "set arithmetic is not doing what this file thinks"
	missing = [k for k in NEEDED_CONFIG if k not in (declared - {"orientation"})]
	if missing != ["orientation"]:
		return "a missing config change was not reported: %r" % (missing,)
	return None


def main():
	failed = control()
	if failed:
		print("manifest-check: the control failed -- %s." % failed,
		       file=sys.stderr)
		print("                No result below means anything.", file=sys.stderr)
		return 2

	if not MANIFEST.is_file():
		print("manifest-check: %s is not there; a check over a file that does "
		       "not exist passes for free." % MANIFEST.relative_to(ROOT),
		       file=sys.stderr)
		return 2

	root = ET.parse(MANIFEST).getroot()
	makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
	problems = []

	# 1. The rotation premise.
	declared = config_changes(root)
	if not declared:
		problems.append("no activity declares android:configChanges at all; "
		                 "every rotation recreates the activity")
	for key, why in sorted(NEEDED_CONFIG.items()):
		if key not in declared:
			problems.append("android:configChanges does not list %s (%s), so "
			                 "Android recreates the activity rather than "
			                 "resizing it -- test_rotation tests the resize"
			                 % (key, why))

	# 2. Every class the manifest names must be there to start.
	named = set()
	for el in root.iter():
		value = el.get(ANDROID_NS + "name") or ""
		if value.startswith("se.vibes.hydra."):
			named.add(value)
	for cls in sorted(named):
		path = JAVA.joinpath(*cls.split(".")).with_suffix(".java")
		if not path.is_file():
			problems.append("the manifest names %s and %s is not there"
			                 % (cls, path.relative_to(ROOT)))

	# 3. The library Qt loads is the one the build produces.
	lib = None
	for meta in root.iter("meta-data"):
		if meta.get(ANDROID_NS + "name") == "android.app.lib_name":
			lib = meta.get(ANDROID_NS + "value")
	target = make_var(makefile, "TARGET")
	if lib != target:
		problems.append("android.app.lib_name is %r and the Makefile's TARGET "
		                 "is %r; Qt loads the first and the build produces the "
		                 "second" % (lib, target))

	# 4. The id adb installs, launches and uninstalls by.
	package = root.get("package")
	app_id = make_var(makefile, "APP_ID")
	activity = make_var(makefile, "ANDROID_ACTIVITY")
	if package != app_id:
		problems.append("the manifest package is %r and APP_ID is %r; every "
		                 "adb target in tool/android.mk uses APP_ID"
		                 % (package, app_id))
	if activity and activity not in named:
		problems.append("ANDROID_ACTIVITY is %r and the manifest names no such "
		                 "class; android-run would start nothing" % activity)

	for p in problems:
		print("android/AndroidManifest.xml: " + p, file=sys.stderr)
	if problems:
		print("manifest-check: %d problem(s)" % len(problems), file=sys.stderr)
		return 1

	print("manifest-check: %d config change(s) declared including the %d the "
	       "rotation design needs, %d class(es) named and present, lib_name and "
	       "package match the build"
	       % (len(declared), len(NEEDED_CONFIG), len(named)))
	return 0


if __name__ == "__main__":
	sys.exit(main())

#!/usr/bin/env python3
"""Every native method a Java class declares has a C++ symbol JNI can find.

JNI binds a `native` method to a C function by *name*: a method `m` on class
`p.q.C` resolves to `Java_p_q_C_m`, and nothing checks the two agree until the
first call throws `UnsatisfiedLinkError` on a device.

This tree shipped that mistake. The application id was renamed from
`org.qtproject.example.hydra` to `se.vibes.hydra`; the Java moved, and so did
the class paths C++ passes to `QJniObject` -- those are string literals
containing `org/qtproject/example/hydra`, which a grep for the old id finds.
The seven JNI entry points did not move, because in a function name the
separator is `_` rather than `/` and the same grep does not match. The result
built, packaged, installed and signed, and every native call in the WebView
backend would have failed: no url reporting, no request filtering, no script
bridges, no external links, no file picker.

Nothing caught it because nothing could. The Java compiles without the C++,
the C++ compiles without the Java, and the two are joined at runtime by string
equality. This is that equality, checked at rest.

Text only -- no Android SDK, no NDK, no device -- so it runs in seconds
anywhere, which is what makes it worth running every time.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JAVA = os.path.join(ROOT, "android", "src")
CPP = os.path.join(ROOT, "src")

RE_PACKAGE = re.compile(r"^\s*package\s+([\w.]+)\s*;", re.M)
RE_NATIVE = re.compile(r"\bnative\s+[\w.<>\[\]]+\s+(\w+)\s*\(", re.M)
RE_SYMBOL = re.compile(r"^(Java_\w+)\s*\(", re.M)


def java_natives():
	"""{expected symbol: 'file: package.Class.method'} for every native method."""
	wanted = {}
	if not os.path.isdir(JAVA):
		return wanted
	for base, _, names in os.walk(JAVA):
		for name in names:
			if not name.endswith(".java"):
				continue
			path = os.path.join(base, name)
			with open(path) as fh:
				text = fh.read()
			pkg = RE_PACKAGE.search(text)
			if not pkg:
				continue
			cls = name[:-5]
			for method in RE_NATIVE.findall(text):
				# JNI's short form. An overloaded native method needs the long
				# form with the argument signature appended; none here are
				# overloaded, and the check below reports it if that changes.
				symbol = "Java_%s_%s_%s" % (pkg.group(1).replace(".", "_"),
				                             cls, method)
				where = "%s: %s.%s.%s" % (os.path.relpath(path, ROOT),
				                           pkg.group(1), cls, method)
				if symbol in wanted:
					print("two native methods want one symbol: %s" % symbol)
					print("    %s" % wanted[symbol])
					print("    %s" % where)
					return None
				wanted[symbol] = where
	return wanted


def cpp_symbols():
	"""{symbol: file} for every JNI entry point defined in src/."""
	found = {}
	for name in sorted(os.listdir(CPP)):
		if not name.endswith(".cpp"):
			continue
		path = os.path.join(CPP, name)
		with open(path) as fh:
			for symbol in RE_SYMBOL.findall(fh.read()):
				found[symbol] = name
	return found


def main():
	wanted = java_natives()
	if wanted is None:
		return 1
	found = cpp_symbols()

	# **A check that inspected nothing passes as loudly as one that passed.**
	# If the Java tree moves or the regex stops matching, this would report
	# success over an empty set, which is the shape of failure the whole file
	# is about.
	if not wanted:
		print("jni-check: found no native methods to check at all.")
		print("  android/src is missing, or nothing in it declares one.")
		return 1

	missing = sorted(s for s in wanted if s not in found)
	orphan = sorted(s for s in found if s not in wanted)

	for s in missing:
		print("MISSING  %s" % s)
		print("         declared by %s" % wanted[s])
		print("         JNI would throw UnsatisfiedLinkError on the first call")
	for s in orphan:
		print("ORPHAN   %s" % s)
		print("         defined in src/%s, and no Java native method wants it" % found[s])

	if missing or orphan:
		print()
		print("%d native method(s) checked, %d unmatched"
		      % (len(wanted), len(missing) + len(orphan)))
		return 1

	print("jni-check: %d native method(s), every one resolvable" % len(wanted))
	return 0


if __name__ == "__main__":
	sys.exit(main())

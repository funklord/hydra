#!/usr/bin/env python3
"""The dependency lists that must agree, and what it costs when they do not.

Three files name the same optional dependencies and none of them reads the
others: `hydra.pro` for the application, `test/Makefile` for the suites, and
`debian/control` for the package. Every one of the three failures is silent.

**A package missing from `debian/control`** is not a build failure. The build
asks `packagesExist()` and quietly leaves the feature out, so what ships is a
browser with no BitTorrent, or no keyring, or no `libsodium`, and the build
log says so in a line nobody reads. The developer's machine has everything
installed, which is precisely why the person who would notice is the one who
cannot.

**A macro missing from `test/Makefile`** is worse in a different way: the
suites then compile a *different program* from the one that ships, and they
pass. This tree has already paid for that shape -- `theme.h` defines a stub
`QDBusVariant` when `HYDRA_HAVE_DBUS` is absent, and a translation unit
reaching it without the macro while the real QtDBus header is also reachable
has the class twice.

**A package in `hydra.pro` and not in `test/Makefile`** is the same thing one
step earlier: the feature exists in the application and no suite can see it.

**The Debian name is derived, not mapped.** `libfoo` needs `libfoo-dev`, and a
table here would be a fourth list to keep in step. Qt's modules are the
exception and are left alone: `QT += webenginewidgets` becomes
`qt6-webengine-dev` by no rule this file could state, and inventing one would
be a guess wearing a check's clothes.

**A control runs before any file is read.** Every finding is a name that is
not there, and a checker that has stopped comparing reports agreement in the
words of a real pass.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def pro_packages(text):
	"""pkg-config names the application build asks for."""
	return set(re.findall(r"^\s*packagesExist\(([\w.+-]+)\)", text, re.M)) | \
	       set(re.findall(r"^\s*PKGCONFIG\s*\+=\s*([\w.+-]+)", text, re.M))


def pro_macros(text):
	return set(re.findall(r"DEFINES\s*\+=\s*(HYDRA_HAVE_\w+)", text))


def test_packages(text):
	m = re.search(r"OPT_PKGS\s*:?=\s*\$\(strip\s*\$\(foreach\s+\w+,([^,]+),",
	               text)
	return set(m.group(1).split()) if m else set()


def test_macros(text):
	return set(re.findall(r"-D(HYDRA_HAVE_\w+)", text))


def build_depends(text):
	m = re.search(r"^Build-Depends:(.*?)(?=^\S)", text, re.M | re.S)
	if not m:
		return set()
	names = set()
	for part in m.group(1).split(","):
		part = part.strip().split()[0] if part.strip() else ""
		if part:
			names.add(part)
	return names


def compare(app_pkgs, app_macros, suite_pkgs, suite_macros, deb):
	"""[(what, why)] -- pure, so the control can drive it."""
	out = []
	for p in sorted(app_pkgs - suite_pkgs):
		out.append((p, "hydra.pro asks for it and test/Makefile's OPT_PKGS "
		                "does not, so no suite compiles the feature"))
	for p in sorted(suite_pkgs - app_pkgs):
		out.append((p, "test/Makefile asks for it and hydra.pro does not, so "
		                "the suites compile something the browser has not"))
	for m in sorted(app_macros - suite_macros):
		out.append((m, "hydra.pro defines it and test/Makefile does not, so "
		                "the suites compile a different program"))
	for m in sorted(suite_macros - app_macros):
		out.append((m, "test/Makefile defines it and hydra.pro does not"))
	for p in sorted(app_pkgs):
		if p + "-dev" not in deb and p not in deb:
			out.append((p, "no %s-dev in debian/control Build-Depends, so a "
			                "package build leaves the feature out in silence"
			                % p))
	return out


def control():
	app = {"libsodium", "liblz4"}
	deb = {"libsodium-dev", "liblz4-dev"}
	if compare(app, {"HYDRA_HAVE_LZ4"}, app, {"HYDRA_HAVE_LZ4"}, deb):
		return "agreeing lists were reported as disagreeing"
	seen = {w for w, _ in compare(app, {"HYDRA_HAVE_LZ4"}, {"libsodium"},
	                               set(), deb - {"liblz4-dev"})}
	want = {"liblz4", "HYDRA_HAVE_LZ4"}
	if seen != want:
		return "a real disagreement was not reported: missed %r" % (want - seen,)
	return None


def main():
	failed = control()
	if failed:
		print("deps-check: the control failed -- %s." % failed, file=sys.stderr)
		print("            No result below means anything.", file=sys.stderr)
		return 2

	files = {
		"hydra.pro": ROOT / "hydra.pro",
		"test/Makefile": ROOT / "test" / "Makefile",
		"debian/control": ROOT / "debian" / "control",
	}
	for name, path in files.items():
		if not path.is_file():
			print("deps-check: %s is not there; a comparison with a file that "
			       "does not exist passes for free." % name, file=sys.stderr)
			return 2

	pro = files["hydra.pro"].read_text(encoding="utf-8")
	tst = files["test/Makefile"].read_text(encoding="utf-8")
	deb = files["debian/control"].read_text(encoding="utf-8")

	app_pkgs, suite_pkgs = pro_packages(pro), test_packages(tst)
	app_macros, suite_macros = pro_macros(pro), test_macros(tst)
	depends = build_depends(deb)

	# Each list is computed, so an empty one compares clean against anything.
	for what, got in (("hydra.pro packages", app_pkgs),
	                   ("test/Makefile packages", suite_pkgs),
	                   ("hydra.pro macros", app_macros),
	                   ("test/Makefile macros", suite_macros),
	                   ("Build-Depends", depends)):
		if not got:
			print("deps-check: read no %s at all; the pattern has stopped "
			       "matching and every comparison below is vacuous." % what,
			       file=sys.stderr)
			return 2

	problems = compare(app_pkgs, app_macros, suite_pkgs, suite_macros, depends)
	for what, why in problems:
		print("%s: %s" % (what, why), file=sys.stderr)
	if problems:
		print("deps-check: %d disagreement(s)" % len(problems), file=sys.stderr)
		return 1

	print("deps-check: %d optional package(s) and %d feature macro(s), the "
	       "same in hydra.pro and test/Makefile, each with a -dev in "
	       "Build-Depends" % (len(app_pkgs), len(app_macros)))
	return 0


if __name__ == "__main__":
	sys.exit(main())

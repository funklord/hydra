#!/usr/bin/env python3
"""The desktop entry, against the things it names.

A `.desktop` file is a set of claims about a program, written in a file the
program never reads. Every one of them fails the way the qrc prefix failed:
quietly, on somebody else's machine, as an absence.

    Exec=             a binary name; wrong, and the menu entry does nothing
    Icon=             an icon name; wrong, and the entry has no icon
    StartupWMClass=   the X11 class; wrong, and the window is a second entry
                      in the taskbar rather than the running one
    setDesktopFileName  the Wayland side of the same identity, in main.cpp

**The spec is somebody else's to check.** `desktop-file-validate` owns the
grammar, the registered categories and the required keys, and reimplementing
any of that here would be a second opinion that goes stale. It is run when it
is present and its absence is reported rather than passed over -- a validator
that is not installed is not a file that validates.

What this file adds is the half no external tool can know: whether the names
in the entry are the names this tree actually produces.

**A control runs before anything is read**, because every finding is a
mismatch between two strings and a checker comparing nothing reports agreement
in the words of a real pass.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DESKTOP = ROOT / "packaging" / "hydra.desktop"
INSTALL = ROOT / "packaging" / "install-icons.sh"
MAIN = ROOT / "src" / "main.cpp"


def entries(text):
	"""The [Desktop Entry] group as a dict. Later keys win, as the spec says."""
	out, in_group = {}, False
	for line in text.splitlines():
		line = line.strip()
		if line.startswith("["):
			in_group = line == "[Desktop Entry]"
			continue
		if not in_group or not line or line.startswith("#") or "=" not in line:
			continue
		key, value = line.split("=", 1)
		out[key.strip()] = value.strip()
	return out


def make_var(text, name):
	m = re.search(r"^%s\s*[:?]?=\s*(\S+)" % re.escape(name), text, re.M)
	return m.group(1) if m else None


def installed_icon_name(text):
	"""The basename install-icons.sh copies each size to, without .png."""
	m = re.search(r'cp\s+"[^"]*"\s+"\$dir/([\w.-]+)\.png"', text)
	return m.group(1) if m else None


def icon_sizes(text):
	m = re.search(r"^\s*for size in ([\d ]+); do", text, re.M)
	return [int(x) for x in m.group(1).split()] if m else []


def compare(desktop, target, icon_name, desktop_name):
	"""[(key, why)] -- pure, so the control can drive it."""
	out = []
	exec_line = desktop.get("Exec", "")
	binary = exec_line.split()[0] if exec_line.split() else ""
	if binary != target:
		out.append(("Exec", "starts %r and `make install` installs %r"
		                     % (binary, target)))
	if desktop.get("Icon") != icon_name:
		out.append(("Icon", "names %r and install-icons.sh installs %r.png"
		                     % (desktop.get("Icon"), icon_name)))
	wm = desktop.get("StartupWMClass")
	if wm is not None and wm != target:
		out.append(("StartupWMClass", "is %r and the binary is %r; a window "
		                               "whose class does not match starts a "
		                               "second taskbar entry" % (wm, target)))
	if desktop_name is not None and desktop_name != DESKTOP.stem:
		out.append(("setDesktopFileName", "main.cpp says %r and the entry is "
		                                   "%s.desktop" % (desktop_name,
		                                                    DESKTOP.stem)))
	return out


def control():
	good = {"Exec": "hydra %U", "Icon": "hydra", "StartupWMClass": "hydra"}
	if compare(good, "hydra", "hydra", "hydra"):
		return "an entry that agrees was reported as disagreeing"
	bad = dict(good, Icon="hydra-icon")
	seen = {k for k, _ in compare(bad, "hydra2", "hydra", "other")}
	want = {"Exec", "Icon", "StartupWMClass", "setDesktopFileName"}
	if seen != want:
		return "a real mismatch was not reported: missed %r" % (want - seen,)
	return None


def main():
	failed = control()
	if failed:
		print("desktop-check: the control failed -- %s." % failed,
		       file=sys.stderr)
		print("               No result below means anything.", file=sys.stderr)
		return 2

	for path in (DESKTOP, INSTALL, MAIN, ROOT / "Makefile"):
		if not path.is_file():
			print("desktop-check: %s is not there; a check over a file that "
			       "does not exist passes for free." % path.relative_to(ROOT),
			       file=sys.stderr)
			return 2

	desktop = entries(DESKTOP.read_text(encoding="utf-8"))
	install = INSTALL.read_text(encoding="utf-8")
	main_cpp = MAIN.read_text(encoding="utf-8")
	target = make_var((ROOT / "Makefile").read_text(encoding="utf-8"), "TARGET")

	if not desktop:
		print("desktop-check: read no keys from [Desktop Entry]; the parser "
		       "has stopped matching and every comparison is vacuous.",
		       file=sys.stderr)
		return 2

	icon_name = installed_icon_name(install)
	sizes = icon_sizes(install)
	m = re.search(r'setDesktopFileName\(QStringLiteral\("([^"]+)"\)\)', main_cpp)
	desktop_name = m.group(1) if m else None

	problems = compare(desktop, target, icon_name, desktop_name)
	if desktop_name is None:
		problems.append(("setDesktopFileName", "main.cpp never calls it, so a "
		                  "desktop has no way to associate the window with "
		                  "this entry"))
	if not sizes:
		problems.append(("install-icons.sh", "no size list could be read from "
		                  "it, so the icons below were not checked"))
	for size in sizes:
		png = ROOT / "icon" / ("hydra-%d.png" % size)
		if not png.is_file():
			problems.append(("install-icons.sh", "installs %dx%d and %s is not "
			                  "there" % (size, size, png.relative_to(ROOT))))

	validator = shutil.which("desktop-file-validate")
	if validator:
		run = subprocess.run([validator, str(DESKTOP)], capture_output=True,
		                      text=True)
		if run.returncode != 0:
			for line in (run.stdout + run.stderr).splitlines():
				problems.append(("desktop-file-validate", line.strip()))
	else:
		print("desktop-check: desktop-file-validate is not installed, so the "
		       "spec half was not checked here.", file=sys.stderr)

	for key, why in problems:
		print("packaging/hydra.desktop: %s %s" % (key, why), file=sys.stderr)
	if problems:
		print("desktop-check: %d problem(s)" % len(problems), file=sys.stderr)
		return 1

	print("desktop-check: Exec, Icon, StartupWMClass and setDesktopFileName "
	       "all name %r, %d icon size(s) present%s"
	       % (target, len(sizes),
	          ", desktop-file-validate clean" if validator else ""))
	return 0


if __name__ == "__main__":
	sys.exit(main())

#!/usr/bin/env python3
# The Android checks that need no phone, run on an emulator it starts and
# stops itself.
#
#     tool/emulator_checks.py <apk> [avd] [port]
#
# Build the apk for the emulator's ABI first:
#
#     make android ANDROID_ABI=x86_64
#
# Why it exists: project.md, "Test on the emulator; the phone is for what
# needs one". These are the checks 2026-10-29 asked the holder's phone for,
# none of which depends on hardware:
#
#   1. a youtube page cannot see hydra: no `hydraNative`, no hydra globals
#      or elements, and every patched function reads as native;
#   2. `ad_status.js` is answered with the stand-in, not refused, since a
#      refused one is what raised YouTube's "Ad blockers" wall;
#   3. an Annoyed report filed from the Tools menu records what was refused,
#      the page's tells -- one planted, so an empty list cannot pass -- and
#      the patches that ran.
#
# What makes it stop: every adb call has a timeout, every wait is bounded,
# and the emulator -- started in its own session -- is killed in `finally`,
# by `adb emu kill` and then by its process group if that did not take.
# `store` is refused: the screenshot target wipes it every run. An AVD that
# is already running is refused rather than shared.
import json, os, re, signal, subprocess, sys, time
import xml.etree.ElementTree as et

SDK = os.environ.get("ANDROID_SDK_ROOT", os.path.expanduser("~/Android/Sdk"))
ADB = os.path.join(SDK, "platform-tools", "adb")
EMULATOR = os.path.join(SDK, "emulator", "emulator")
HERE = os.path.dirname(os.path.abspath(__file__))
APP = "se.vibes.hydra"

if len(sys.argv) < 2:
	print(__doc__ or "usage: tool/emulator_checks.py <apk> [avd] [port]")
	sys.exit(2)
APK = sys.argv[1]
AVD = sys.argv[2] if len(sys.argv) > 2 else "Resizable_Experimental_API_33"
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 5590
SERIAL = f"emulator-{PORT}"

failures = []

def check(ok, what):
	print(("  ok    " if ok else "  FAIL  ") + what)
	if not ok:
		failures.append(what)

def adb(*a, timeout=60):
	r = subprocess.run([ADB, "-s", SERIAL, *a], capture_output=True, text=True,
	                   timeout=timeout)
	return r.stdout

def wait_for(what, test, limit, step=3):
	end = time.monotonic() + limit
	while time.monotonic() < end:
		try:
			if test():
				return True
		except subprocess.TimeoutExpired:
			pass
		time.sleep(step)
	print(f"  gave up waiting for {what} after {limit}s")
	return False

def probe(expression=None):
	cmd = [sys.executable, os.path.join(HERE, "webview_probe.py"), ADB, SERIAL, str(PORT + 4000)]
	if expression:
		cmd.append(expression)
	return subprocess.run(cmd, capture_output=True, text=True, timeout=120).stdout

def tap(label, limit=30):
	# Taps the node whose description or text starts with `label`.
	def found():
		adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
		tree = et.fromstring(adb("shell", "cat", "/sdcard/ui.xml") or "<x/>")
		for n in tree.iter("node"):
			name = n.get("content-desc") or n.get("text") or ""
			if name.startswith(label):
				x1, y1, x2, y2 = map(int, re.findall(r"\d+", n.get("bounds")))
				adb("shell", "input", "tap", str((x1 + x2) // 2), str((y1 + y2) // 2))
				return True
		return False
	return wait_for(f"'{label}' on screen", found, limit)

def reports():
	text = adb("shell", "run-as", APP, "cat", "files/annoyances.ini")
	out = {}
	for line in text.splitlines():
		m = re.match(r"(\d+)\\(\w+)=(.*)", line)
		if m:
			out.setdefault(int(m.group(1)), {})[m.group(2)] = m.group(3)
	return out

def main():
	if AVD == "store":
		print("refused: `store` is the screenshot target's, wiped every run")
		return 2
	ps = subprocess.run(["ps", "-eo", "args"], capture_output=True, text=True).stdout
	if re.search(rf"qemu-system\S* .*-avd {re.escape(AVD)}( |$)", ps, re.M):
		print(f"refused: {AVD} is already running, and an AVD runs once at a time")
		return 2
	if not os.path.isfile(APK):
		print(f"no apk at {APK}")
		return 2

	print(f"starting {AVD} on {SERIAL}")
	emu = subprocess.Popen([EMULATOR, "-avd", AVD, "-port", str(PORT), "-no-window",
	                        "-no-audio", "-no-boot-anim", "-no-snapshot",
	                        "-gpu", "swiftshader_indirect"],
	                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
	                       start_new_session=True)
	try:
		if not wait_for("boot", lambda: adb("shell", "getprop", "sys.boot_completed",
		                                    timeout=10).strip() == "1", 300):
			return 1
		print(adb("install", "-r", "-g", APK, timeout=300).strip().splitlines()[-1:])
		adb("shell", "am", "start", "-a", "android.intent.action.VIEW",
		    "-d", "https://m.youtube.com/", "-n", f"{APP}/.HydraActivity")

		print("\n== a youtube page cannot see hydra ==")
		seen = {}
		last = [""]
		def loaded():
			out = last[0] = probe()
			if out.startswith("{"):
				seen.update(json.loads(out))
				return "m.youtube.com" in seen.get("url", "")
			return False
		if not wait_for("m.youtube.com in a debuggable WebView", loaded, 180, step=5):
			print("  the probe last said: " + last[0].strip()[:600])
			failures.append("youtube never loaded")
			return 1
		time.sleep(10)   # let the player's scripts run before asking again
		loaded()
		print(json.dumps(seen, indent=2))
		check(seen.get("hydraNative") == "undefined", "no hydraNative")
		check(not [g for g in seen.get("named_globals", []) if re.search("hydra", g, re.I)],
		      "no global names hydra")
		check(seen.get("named_elements") == 0, "no element names hydra")
		leaks = {k: v for k, v in seen.get("functions", {}).items()
		         if v not in ("native", "undefined")}
		check(not leaks, f"every patched function reads as native {leaks or ''}")
		check(seen.get("wall_text") == "absent", "no \"Ad blockers\" wall")

		print("\n== ad_status.js is answered, not refused ==")
		# The stand-in comes from the subscribed lists, which a fresh install
		# fetches a minute after the window opens; a page made before they
		# arrived has none. So wait for them, then load the page again.
		def lists():
			index = json.loads(adb("shell", "run-as", APP, "cat",
			                       "files/filters-subscribed.json") or "[]")
			return index and all(s.get("rules", 0) > 0 for s in index if s.get("enabled"))
		check(wait_for("the subscribed lists", lists, 240, step=5), "the subscribed lists fetched")
		probe("location.reload(); 'reloading'")
		time.sleep(15)
		body = probe("fetch('https://static.doubleclick.net/instream/ad_status.js')"
		             ".then(function (r) { return r.text(); })"
		             ".catch(function (e) { return 'refused: ' + e; })").strip()
		print("  " + body)
		check("google_ad_status = 1" in body, "the stand-in came back")

		print("\n== an Annoyed report records what ran ==")
		before = set(reports())
		# A clean page has no tells, and an empty list cannot tell a probe
		# that found nothing from one that never ran. So plant one -- a global
		# named for this browser, which the probe looks for -- and require the
		# report to name it.
		probe("window.hydra_planted_tell = 1; 'planted'")
		ok = tap("Tools") and tap("Something Got Through Here") and tap("Just Record It")
		check(ok, "Tools, Something Got Through Here, Just Record It")
		got = {}
		def filed():
			got.update({k: v for k, v in reports().items() if k not in before})
			return bool(got)
		wait_for("the report on disk", filed, 30)
		report = got[max(got)] if got else {}
		for k in ("host", "observed", "refused", "tells", "patches"):
			print(f"  {k}: {report.get(k, '(none)')[:300]}")
		check(report.get("host") == "m.youtube.com", "filed for m.youtube.com")
		check(bool(report.get("patches")), "patches recorded")
		check("global hydra_planted_tell" in report.get("tells", ""),
		      "the page probe ran: the planted tell is in the report")
		check("ad_status.js" not in report.get("refused", "") or
		      "answered with" in report.get("refused", ""),
		      "ad_status.js shown as answered, if listed")
	finally:
		try:
			adb("emu", "kill", timeout=20)
		except subprocess.TimeoutExpired:
			pass
		try:
			emu.wait(60)
		except subprocess.TimeoutExpired:
			os.killpg(emu.pid, signal.SIGKILL)
			emu.wait(20)
		print(f"\nemulator stopped (exit {emu.returncode})")

	print(f"\n{len(failures)} failed" + (": " + "; ".join(failures) if failures else ""))
	return 1 if failures else 0

sys.exit(main())

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
#      the patches that ran;
#   4. a held finger on a link opens the long-press menu (`link_menu.h`),
#      and "Open in Background Tab" puts the link in the tree;
#   5. a link the page answers with a file is downloaded, with the WebView's
#      cookie -- from a server this script runs, reached through
#      `adb reverse`, so what the download sent can be read. The file has
#      to arrive on the device: the WebView fetches it itself while
#      navigating, cookie and all, and then drops it unless something
#      listens, so a request at the server proves nothing (measured: a build
#      without the listener passed a check that only read the server);
#   6. File > Print reaches the system's print dialog, Save as PDF writes a
#      PDF, and the window hears that it finished;
#   7. View > Reader View shows an article without the page around it;
#   8. typing in the address bar offers the tree's pages in a list inside the
#      window, and the app survives it. `input text` sends key events, not
#      the keyboard's own input, so this may not reproduce the crash a popup
#      window caused there (project.md, "The address bar suggests from the
#      tree"); the phone was the test of that;
#   9. Find on Page counts what is on the page: built for Android long ago
#      and never once run on a device until this check.
#  10. the address bar, typed by tapping the keyboard's own keys
#      (2026-10-30): a tap opens the editor across the window; the
#      keyboard's Go key and the editor's Go button; a word still being
#      composed counted as typed; a tab suggested from a composing word; a
#      tap on the page closes the editor; and a tagged link on the
#      clipboard offered as Paste and go and Paste, cleaned of its tags;
#  11. the lists against real sites (2026-10-30): a tracker's parameter is
#      taken off an opened address and off a page's own navigation; a page
#      a list says not to visit (7zip.es, in the Badware list) is refused
#      with a warning whose way through works and a forged one does not;
#      and dailymail's procedural rule hides a planted sponsored puff by a
#      marker, leaves its twin, and shows it again once its text changes.
#
# What makes it stop: every adb call has a timeout, every wait is bounded,
# and the emulator -- started in its own session -- is killed in `finally`,
# by `adb emu kill` and then by its process group if that did not take.
# `store` is refused: the screenshot target wipes it every run. An AVD that
# is already running is refused rather than shared.
import http.server, json, os, re, signal, subprocess, sys, threading, time
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

def tap(label, limit=30, exact=False):
	# Taps the node whose description or text starts with `label` (is it,
	# with `exact`), or whose resource id is `label` -- the system's own
	# dialogs name their controls.
	def found():
		adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
		tree = et.fromstring(adb("shell", "cat", "/sdcard/ui.xml") or "<x/>")
		for n in tree.iter("node"):
			name = n.get("content-desc") or n.get("text") or ""
			# Not on screen is not there: a closed drawer's rows are in the
			# tree with empty bounds, and tapping one tapped the corner.
			if n.get("bounds") in (None, "[0,0][0,0]"):
				continue
			if (name == label if exact else name.startswith(label)) or \
			   n.get("resource-id") == label:
				x1, y1, x2, y2 = map(int, re.findall(r"\d+", n.get("bounds")))
				adb("shell", "input", "tap", str((x1 + x2) // 2), str((y1 + y2) // 2))
				return True
		return False
	if wait_for(f"'{label}' on screen", found, limit):
		return True
	# What was there instead, which is the first thing anybody asks.
	tree = et.fromstring(adb("shell", "cat", "/sdcard/ui.xml") or "<x/>")
	shown = sorted({n.get("content-desc") or n.get("text") for n in tree.iter("node")} - {"", None})
	print("  on screen instead: " + ", ".join(shown)[:600])
	return False

def reports():
	text = adb("shell", "run-as", APP, "cat", "files/annoyances.ini")
	out = {}
	for line in text.splitlines():
		m = re.match(r"(\d+)\\(\w+)=(.*)", line)
		if m:
			out.setdefault(int(m.group(1)), {})[m.group(2)] = m.group(3)
	return out

class files(http.server.BaseHTTPRequestHandler):
	# A page that sets a cookie and is one big link, and the file it links to,
	# sent as an attachment. Every request is kept, so the check can read what
	# the download sent.
	seen = []
	name = "held.bin"

	def do_GET(self):
		files.seen.append((self.path, dict(self.headers)))
		if self.path.startswith("/article"):
			para = ("<p>Rivers across the northern provinces rose to their highest level "
			        "in a decade, after three days of rain that forecasters had warned "
			        "about since the weekend, and the town opened its school to families.</p>")
			body = ("<!doctype html><meta name=viewport content='width=device-width'>"
			        "<title>Rivers rise</title><aside class=sidebar><h3>MOST READ LIST</h3>"
			        "<a href=/a>Another story</a></aside><article><h1>Rivers rise</h1>"
			        + para * 6 + "</article>").encode()
			self.send_response(200)
			self.send_header("Content-Type", "text/html")
		elif self.path.startswith("/story"):
			# A page that keeps its article from the person and sends Google's
			# crawler all of it, as try_scriptlets serves on the desktop.
			whole = "Googlebot" in self.headers.get("User-Agent", "")
			rest = ("<div class=paywalled>" + "<p>The rest of the story, a paragraph "
			        "only a crawler is sent.</p>" * 12 + "</div>") if whole else \
			       "<p>Subscribe to read the rest of this story, today only.</p>"
			body = ("<!doctype html><meta name=viewport content='width=device-width'>"
			        "<title>A story</title><script type='application/ld+json'>"
			        '{"@type":"NewsArticle","isAccessibleForFree":false,"hasPart":'
			        '{"isAccessibleForFree":false,"cssSelector":".paywalled"}}</script>'
			        "<style>.paywalled{display:none}</style>"
			        "<p>The opening of the story, which everybody is sent in full.</p>"
			        + rest).encode()
			self.send_response(200)
			self.send_header("Content-Type", "text/html")
		elif self.path.startswith("/file"):
			body = b"hydra download check\n"
			self.send_response(200)
			self.send_header("Content-Type", "application/octet-stream")
			self.send_header("Content-Disposition",
			                 f'attachment; filename="{files.name}"')
		else:
			body = ("<!doctype html><meta name=viewport content='width=device-width'>"
			        f"<a href='/file/{files.name}' style='position:fixed;inset:0;"
			        "display:block;background:#9cf;font-size:40px'>DOWNLOAD</a>").encode()
			self.send_response(200)
			self.send_header("Content-Type", "text/html")
			self.send_header("Set-Cookie", "hydra_check=present; Path=/")
		self.send_header("Content-Length", str(len(body)))
		self.end_headers()
		self.wfile.write(body)

	def log_message(self, *args):
		pass

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

		print("\n== a held finger on a link offers the long-press menu ==")
		# A link of our own, large and on top, so where to press is known
		# rather than read off somebody's layout.
		target = "https://example.org/held-" + str(int(time.time()))
		where = json.loads(probe(
		  "(function () { var a = document.createElement('a'); a.href = '" + target + "';"
		  " a.textContent = 'HOLD'; a.style.cssText = 'position:fixed;top:40%;left:10%;"
		  "width:80%;height:120px;z-index:2147483647;background:#fc0;font-size:40px;"
		  "display:block'; document.body.appendChild(a); var r = a.getBoundingClientRect();"
		  " return JSON.stringify({x: r.left + r.width / 2, y: r.top + r.height / 2,"
		  " dpr: window.devicePixelRatio}); })()").strip() or "{}")
		adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
		tree = et.fromstring(adb("shell", "cat", "/sdcard/ui.xml") or "<x/>")
		view = next((n for n in tree.iter("node")
		             if n.get("class") == "android.webkit.WebView"), None)
		if view is None or "x" not in where:
			check(False, "found the page and the link on screen")
		else:
			left, top = map(int, re.findall(r"\d+", view.get("bounds"))[:2])
			x = int(left + where["x"] * where["dpr"])
			y = int(top + where["y"] * where["dpr"])
			adb("shell", "input", "swipe", str(x), str(y), str(x), str(y), "1500")
			adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
			shown = adb("shell", "cat", "/sdcard/ui.xml")
			for label in ("Open in New Tab", "Open in Background Tab",
			              "Copy Link Address", "Save Link"):
				check(label in shown, f"the menu offers {label}")
			tap("Open in Background Tab", limit=10)
			def in_tree():
				return target in adb("shell", "run-as", APP, "cat", "files/tree.txt")
			check(wait_for("the link in the tree", in_tree, 20),
			      "Open in Background Tab put the link in the tree")

		print("\n== a link the page answers with a file is downloaded ==")
		files.name = f"held-{int(time.time())}.bin"
		server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), files)
		port = server.server_address[1]
		threading.Thread(target=server.serve_forever, daemon=True).start()
		try:
			adb("reverse", f"tcp:{port}", f"tcp:{port}")
			adb("shell", "am", "start", "-a", "android.intent.action.VIEW",
			    "-d", f"http://127.0.0.1:{port}/page", "-n", f"{APP}/.HydraActivity")
			check(wait_for("the page from this script's server",
			               lambda: any(p == "/page" for p, _ in files.seen), 30),
			      "the page loaded")
			time.sleep(3)
			adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
			tree = et.fromstring(adb("shell", "cat", "/sdcard/ui.xml") or "<x/>")
			views = [n for n in tree.iter("node") if n.get("class") == "android.webkit.WebView"]
			if views:
				x1, y1, x2, y2 = map(int, re.findall(r"\d+", views[0].get("bounds")))
				adb("shell", "input", "tap", str((x1 + x2) // 2), str((y1 + y2) // 2))
			fetched = lambda: [h for p, h in files.seen if p.startswith("/file")]
			landed = lambda: files.name in adb(
			  "shell", "ls", f"/sdcard/Android/data/{APP}/files/Download/", "/sdcard/Download/")
			check(wait_for("the file on the device", landed, 40),
			      f"tapping the link saved {files.name} on the device")
			# The WebView's own request is first; the download's is the last.
			sent = fetched()[-1] if len(fetched()) > 1 else {}
			check("hydra_check=present" in sent.get("Cookie", ""),
			      f"the download sent the page's cookie ({sent.get('Cookie', 'none')}, "
			      f"{len(fetched())} request(s) for the file)")

			print("\n== File > Print saves a PDF through the system's dialog ==")
			def pdfs():
				return set(re.findall(r"[^\n]*\.pdf", adb("shell", "ls", "/sdcard/Download/")))
			before_pdfs = pdfs()
			# **From a fresh process**, because of the emulator rather than for
			# tidiness: once any Qt dialog has been shown and closed, coming
			# back from another activity -- the print dialog is one -- kills
			# the app in the emulator's eglSwapBuffers. A phone does not
			# (project.md, "Coming back after a dialog kills the app -- on the
			# emulator only"). The Annoyed report and the
			# Downloads list have both been up by now.
			adb("shell", "am", "force-stop", APP)
			adb("shell", "am", "start", "-a", "android.intent.action.VIEW",
			    "-d", f"http://127.0.0.1:{port}/page", "-n", f"{APP}/.HydraActivity")
			time.sleep(10)
			# And the restart is a check of its own: a finished download comes
			# back from the history at every start, and must not be copied into
			# the shared folder again (it was, once per launch).
			stem = files.name[:-len(".bin")]
			copies = [n for n in adb("shell", "ls", "-1", "/sdcard/Download/").splitlines()
			          if n.startswith(stem)]
			check(len(copies) == 1,
			      f"a restart did not publish the download again ({len(copies)} in Download)")
			spool = "com.android.printspooler:id/"
			ok = (tap("File") and tap("Print") and tap(spool + "destination_spinner") and
			      tap("Save as PDF") and tap(spool + "print_button") and tap("SAVE"))
			check(ok, "File, Print..., Save as PDF, the PDF button, SAVE")
			def heard():
				adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
				return "Printed." in adb("shell", "cat", "/sdcard/ui.xml")
			check(wait_for("the window to hear the job finish", heard, 15, step=1),
			      "the window said \"Printed.\"")
			made = set()
			def written():
				made.update(pdfs() - before_pdfs)
				return bool(made)
			check(wait_for("a PDF in Download", written, 20), f"a PDF was written {sorted(made)}")

			print("\n== View > Reader View shows the article alone ==")
			adb("shell", "am", "start", "-a", "android.intent.action.VIEW",
			    "-d", f"http://127.0.0.1:{port}/article", "-n", f"{APP}/.HydraActivity")
			time.sleep(8)
			check(tap("View") and tap("Reader View"), "View, Reader View")
			def reading():
				adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
				shown = adb("shell", "cat", "/sdcard/ui.xml")
				return "min read" in shown and "highest level in a decade" in shown
			check(wait_for("the reader document", reading, 20, step=2),
			      "the article in reader mode, with its reading time")
			check("MOST READ LIST" not in adb("shell", "cat", "/sdcard/ui.xml"),
			      "and without the page's sidebar")

			print("\n== typing in the address bar offers the tree's pages ==")
			# From a fresh process: after any menu has closed, the browser's own
			# controls leave the accessibility tree, so the bar cannot be found
			# by name (project.md, "Deferred: after a menu closes, the
			# browser's controls leave the accessibility tree").
			adb("shell", "am", "force-stop", APP)
			adb("shell", "am", "start", "-a", "android.intent.action.VIEW",
			    "-d", f"http://127.0.0.1:{port}/article", "-n", f"{APP}/.HydraActivity")
			time.sleep(10)
			ok = tap("Address", limit=10)
			# The tap opens the address editor, a moment later: typed into
			# before it has focus, the text goes nowhere.
			wait_for("the address editor", lambda: "Editing the address" in (
			  adb("shell", "uiautomator", "dump", "/sdcard/ui.xml") and
			  adb("shell", "cat", "/sdcard/ui.xml")), 10, step=1)
			adb("shell", "input", "text", "127.0.0")
			def listed():
				adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
				shown = adb("shell", "cat", "/sdcard/ui.xml")
				return "127.0.0.1" in shown and "Suggestions" in shown
			check(ok and wait_for("the suggestions", listed, 15, step=2),
			      "the suggestions list, inside the window")
			check(adb("shell", "pidof", APP).strip() != "", "and the app is still running")

			print("\n== Find on Page counts what is on the page ==")
			# Fresh again, and opened with Ctrl+F rather than the Edit menu: after a
			# menu closes the window's own controls leave the accessibility tree,
			# and the count is one of them.
			adb("shell", "am", "force-stop", APP)
			adb("shell", "am", "start", "-a", "android.intent.action.VIEW",
			    "-d", f"http://127.0.0.1:{port}/article", "-n", f"{APP}/.HydraActivity")
			time.sleep(10)
			adb("shell", "input", "keycombination", "KEYCODE_CTRL_LEFT", "KEYCODE_F")
			time.sleep(2)
			adb("shell", "input", "text", "rain")
			def counted():
				adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
				return re.search(r"\b[1-6] of 6\b", adb("shell", "cat", "/sdcard/ui.xml")) is not None
			check(wait_for("the find count", counted, 15, step=2),
			      "\"rain\" found six times, once in each paragraph")

			def open_url(url, wait=12):
				adb("shell", "am", "start", "-a", "android.intent.action.VIEW",
				    "-d", f"'{url}'", "-n", f"{APP}/.HydraActivity")
				time.sleep(wait)

			# **The address bar, used the way a finger uses it** (2026-10-30,
			# after the holder could not paste a meeting link). Typed by
			# tapping the keyboard's own keys: `adb input text` sends hardware
			# key events, which the keyboard composes differently, and it
			# scrambled text that real taps did not -- so it tested the wrong
			# thing. The key positions are Gboard's letter layout on this
			# AVD's 1080x2340 screen.
			print("\n== the address bar, typed by tapping keys ==")
			keys = {}
			for letters, y, xs in (("qwertyuiop", 1655, [58, 165, 273, 378, 486, 593, 700, 807, 915, 1021]),
			                       ("asdfghjkl", 1810, [111, 219, 325, 433, 539, 646, 753, 860, 968]),
			                       ("zxcvbnm", 1966, [219, 325, 433, 539, 646, 753, 860])):
				for c, x in zip(letters, xs):
					keys[c] = (x, y)
			keys["."] = (860, 2120)
			def type_keys(text):
				# The keyboard first: after a navigation it has gone away, and
				# coming back on a slow emulator takes longer than a fixed
				# wait -- taps made before it is up land on the page, nothing
				# is typed, and every check after fails for that one reason.
				wait_for("the keyboard", lambda: "mInputShown=true" in
				         adb("shell", "dumpsys", "input_method"), 15, step=1)
				time.sleep(0.5)
				for c in text:
					adb("shell", "input", "tap", str(keys[c][0]), str(keys[c][1]))
					time.sleep(0.15)
			def bar():
				adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
				tree = et.fromstring(adb("shell", "cat", "/sdcard/ui.xml") or "<x/>")
				for n in tree.iter("node"):
					if (n.get("content-desc") or "").startswith("Address"):
						x1, y1, x2, y2 = map(int, re.findall(r"\d+", n.get("bounds")))
						return (x1 + x2) // 2, (y1 + y2) // 2
				return None
			def on_screen(label):
				adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
				tree = et.fromstring(adb("shell", "cat", "/sdcard/ui.xml") or "<x/>")
				return any((n.get("content-desc") or n.get("text") or "").startswith(label)
				           and n.get("bounds") not in (None, "[0,0][0,0]")
				           for n in tree.iter("node"))
			def tap_bar():
				at = bar()
				if at:
					adb("shell", "input", "tap", str(at[0]), str(at[1]))
				# Until the editor it opens is there: on a loaded machine a
				# fixed pause was too short, and keys tapped before it had
				# focus went nowhere -- every check after failed for that.
				wait_for("the address editor", lambda: on_screen("Editing the address"), 15, step=1)
				return at is not None
			def href():
				return probe("location.href").strip()
			def tap_row(index, rows):
				# The suggestion list is in the accessibility tree with its
				# bounds, but its rows are not: Qt's Android bridge never
				# announces rows put into the list after it was first read
				# (2026-10-30; accessibility is deferred). So a row is tapped
				# by place -- rows are equal in height -- and each check
				# below is judged by where the tap leads, not by its label.
				def found():
					adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
					tree = et.fromstring(adb("shell", "cat", "/sdcard/ui.xml") or "<x/>")
					for n in tree.iter("node"):
						if (n.get("resource-id") or "").endswith("address_suggestions") and \
						   n.get("bounds") not in (None, "[0,0][0,0]"):
							x1, y1, x2, y2 = map(int, re.findall(r"\d+", n.get("bounds")))
							y = y1 + (y2 - y1) * (2 * index + 1) // (2 * rows)
							adb("shell", "input", "tap", str((x1 + x2) // 2), str(y))
							return True
					return False
				return wait_for("the suggestions", found, 10, step=1)

			adb("shell", "am", "force-stop", APP)
			open_url("https://example.com/", wait=15)
			check(tap_bar(), "found the address bar")
			# Tapping it opens the editor across the window (`address_editor`).
			check(on_screen("Editing the address"), "tapping the bar opens the editor")
			type_keys("example.net")
			adb("shell", "input", "tap", "993", "2120")   # the keyboard's Go key
			time.sleep(8)
			check(href() == "http://example.net/", f"the keyboard's Go key goes there ({href()})")

			tap_bar()
			type_keys("wikipedia")
			# Still composing: Go must not search for an empty field.
			check(tap("Go to the address being edited", limit=5), "the editor's Go button")
			time.sleep(8)
			got = href()
			check("q=wikipedia" in got, f"Go with a word still composing searches for it ({got})")

			tap_bar()
			type_keys("hacker")
			time.sleep(2)
			check(tap_row(0, 1),
			      "a tab's name, still composing, is suggested; tapping it goes there")
			time.sleep(8)
			check(href() == "https://news.ycombinator.com/", f"to the tab's address ({href()})")

			tap_bar()
			adb("shell", "input", "keyevent", "KEYCODE_BACK")    # the keyboard away
			adb("shell", "input", "tap", "540", "900")           # the page's picture
			time.sleep(3)
			check(href() == "https://news.ycombinator.com/",
			      "a tap on the page closes the editor and the page is back")

			# Paste: the system's own bubble cannot be tapped, so the editor
			# offers the clipboard as rows of its own -- cleaned of tracking
			# tags. The page copies, the way a person copies from a page.
			tagged = "https://example.org/story?utm_campaign=x&fbclid=IwAR9z&id=5"
			clean = "https://example.org/story?id=5"
			adb("shell", "input", "tap", "540", "900")
			time.sleep(2)
			copied = probe("(function(){var t=document.createElement('textarea');"
			               f"t.value='{tagged}';document.body.appendChild(t);t.focus();"
			               "t.select();var ok=document.execCommand('copy');t.remove();"
			               "return 'copied '+ok})()").strip()
			check(copied == "copied true", f"a tagged link copied from a page ({copied})")
			tap_bar()
			time.sleep(2)
			check(tap_row(0, 2),
			      "the editor offers it as Paste and go, the tracking tags already gone")
			time.sleep(8)
			check(href() == clean, f"and goes to the clean address ({href()})")
			open_url("https://example.com/", wait=12)
			tap_bar()
			time.sleep(2)
			check(tap_row(1, 2), "Paste, on its own")
			check(tap("Go to the address being edited", limit=5), "and Go")
			time.sleep(8)
			check(href() == clean, f"put the clean address in the editor to go to ({href()})")

			print("\n== a paywalled page is shown as a crawler is sent it ==")
			# hydra's own fetch, from the phone: through `adb reverse` like the
			# page, and over the OpenSSL the package carries for https.
			open_url(f"http://127.0.0.1:{port}/story", wait=15)
			text = ""
			for _ in range(10):
				text = probe("document.body.innerText").strip()
				if "only a crawler" in text:
					break
				time.sleep(2)
			check(text.count("only a crawler is sent") == 12 and "Subscribe" not in text,
			      f"the tab shows the crawler's copy, all twelve paragraphs ({len(text)} chars)")
			asked = sum(1 for p, _ in files.seen if p == "/story")
			check(asked >= 7, f"the page, and once per client ({asked} requests)")

			print("\n== a magnet link from another app is downloaded by BitTorrent ==")
			# libtorrent is embedded on Android as on the desktop
			# (tool/build-libtorrent-android.sh). A seeder on this machine,
			# built from tool/torrent_seeder.cpp against the host's libtorrent,
			# serves 4 MiB of known bytes through `adb reverse`, and the magnet
			# names it as its peer -- no tracker, no DHT, no internet. The
			# magnet arrives the way a tap in another app sends it.
			import shutil, tempfile
			seeder_src = os.path.join(HERE, "torrent_seeder.cpp")
			flags = subprocess.run(["pkg-config", "--cflags", "--libs", "libtorrent-rasterbar"],
			                       capture_output=True, text=True)
			if flags.returncode != 0 or not shutil.which("g++"):
				print("  --    not checked: no libtorrent-rasterbar or g++ on this machine to seed with")
			else:
				with tempfile.TemporaryDirectory() as seed_dir:
					seeder = os.path.join(seed_dir, "torrent_seeder")
					built = subprocess.run(["g++", "-Os", "-std=c++17", seeder_src, "-o", seeder]
					                       + flags.stdout.split(), capture_output=True, text=True,
					                       timeout=300)
					check(built.returncode == 0, "the seeder builds: " + built.stderr[-200:])
					seed_port = 18744
					adb("reverse", f"tcp:{seed_port}", f"tcp:{seed_port}")
					done_file = f"/sdcard/Android/data/{APP}/files/Download/hydra-check.bin"
					adb("shell", "rm", "-f", done_file)
					# 256 KiB/s: sixteen seconds for the 4 MiB, long enough to
					# leave the app while it is still moving.
					proc = subprocess.Popen([seeder, seed_dir, str(seed_port), "180", "262144"],
					                        stdout=subprocess.PIPE, text=True)
					try:
						magnet = proc.stdout.readline().strip()
						check(magnet.startswith("magnet:?xt="), f"the seeder offers {magnet[:60]}")
						adb("shell", "am", "start", "-a", "android.intent.action.VIEW",
						    "-d", f"'{magnet}'", "-n", f"{APP}/.HydraActivity")
						# The consent BitTorrent asks for, once a session.
						check(tap("Yes", limit=40, exact=True),
						      "it is taken as a download, and asks consent for BitTorrent")
						# **Then the app leaves the screen**, as it does on a
						# phone most of the time a download takes. The transfer
						# service holds the process in the foreground while it
						# moves (`TransferService`).
						def serving():
							return "TransferService" in adb("shell", "dumpsys", "activity",
							                                 "services", APP)
						check(wait_for("the transfer service", serving, 15, step=1),
						      "a foreground service holds the download while it moves")
						adb("shell", "input", "keyevent", "KEYCODE_HOME")
						def landed():
							return "4194304" in adb("shell", "stat", "-c", "%s", done_file)
						got = wait_for("the torrent's file", landed, 90, step=3)
						check(got, "and it finishes with the browser in the background")
						check(wait_for("the service to stop", lambda: not serving(), 15, step=1),
						      "the service goes when nothing is moving")
						same = False
						if got:
							local = os.path.join(seed_dir, "got.bin")
							adb("pull", done_file, local)
							with open(local, "rb") as a, open(os.path.join(seed_dir, "hydra-check.bin"), "rb") as b:
								same = a.read() == b.read()
						check(got and same, "the 4 MiB arrived, byte for byte what was seeded")
					finally:
						proc.terminate()
						proc.wait(timeout=10)
						adb("reverse", "--remove", f"tcp:{seed_port}")

			# **Checks of the lists against real sites**, last: each moves the
			# tab somewhere else, and the lists have been in force since the
			# ad_status check above.

			print("\n== a tracker's parameter leaves the address ==")
			open_url("https://example.com/?fbclid=abc&id=7")
			href = probe("location.href").strip()
			check(href == "https://example.com/?id=7",
			      f"opened from outside: fbclid taken off ({href})")
			probe("(function(){location.href='https://example.com/?gclid=zz&q=2';return 'nav'})()")
			time.sleep(8)
			href = probe("location.href").strip()
			check(href == "https://example.com/?q=2",
			      f"navigated to by the page: gclid taken off ({href})")

			print("\n== a page a list says not to visit is refused, with a way through ==")
			# 7zip.es is a fake download site in uBlock's Badware list, and
			# answers over valid TLS, which vlc.de does not on this emulator.
			open_url("https://7zip.es/")
			shown = json.loads(probe("JSON.stringify({t: document.title, h: location.host,"
			                         " w: (document.querySelector('a.through') || {}).href || ''})")
			                   .strip() or "{}")
			check(shown.get("t", "").startswith("Blocked:") and shown.get("h") == "7zip.es",
			      f"the warning in its place ({shown.get('t')})")
			check("hydra-proceed=" in shown.get("w", ""), "with a way through")
			probe("(function(){location.href='https://7zip.es/?hydra-proceed=forged';return 'nav'})()")
			time.sleep(8)
			title = probe("document.title").strip()
			check(title.startswith("Blocked:"), f"a token the warning did not make opens nothing ({title})")
			probe("(function(){document.querySelector('a.through').click();return 'clicked'})()")
			def through():
				t = probe("document.title + ' | ' + location.href").strip()
				return t and not t.startswith("Blocked:") and "7zip.es" in t \
				   and "hydra-proceed" not in t
			check(wait_for("the page past the warning", through, 30, step=3),
			      "the warning's own way through opens the site, token left off")

			print("\n== a list's procedural rule hides by what an element says ==")
			# dailymail's own rule, `.pufftext:has-text(/SPONSORED/):upward(2)`,
			# on two puffs planted alike but for their text -- the site's layout
			# changes, the rule's subject is what is under test.
			open_url("https://www.dailymail.com/home/index.html", wait=20)
			probe("(function(){var mk=function(id,t){var o=document.createElement('div');o.id=id;"
			      "o.innerHTML='<div><div class=\"pufftext\"><span>'+t+'</span></div></div>';"
			      "document.body.appendChild(o);};mk('hc-sp','SPONSORED');mk('hc-no','Story');"
			      "return 'planted'})()")
			time.sleep(3)
			state = probe("JSON.stringify({sp:getComputedStyle(document.getElementById('hc-sp')).display,"
			              "no:getComputedStyle(document.getElementById('hc-no')).display,"
			              "inline:document.getElementById('hc-sp').getAttribute('style')})").strip()
			check('"sp":"none"' in state and '"no":"block"' in state and '"inline":null' in state,
			      f"the sponsored one hidden by a marker, its twin shown ({state})")
			probe("(function(){document.querySelector('#hc-sp span').textContent='Story';return 'flip'})()")
			time.sleep(3)
			state = probe("getComputedStyle(document.getElementById('hc-sp')).display").strip()
			check(state == "block", f"and shown again once it says something else ({state})")
		finally:
			adb("reverse", "--remove", f"tcp:{port}")
			# The AVD keeps its data between runs, so what this run wrote
			# into it is taken out again, by name.
			for name in [files.name] + sorted(made if "made" in locals() else []):
				# Quoted: adb joins its arguments into one shell line, and a
				# PDF is named after its page, spaces and all.
				adb("shell", "rm", "-f", f"'/sdcard/Download/{name}'",
				    f"'/sdcard/Android/data/{APP}/files/Download/{name}'")
			server.shutdown()
			server.server_close()
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

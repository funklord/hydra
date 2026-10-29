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
#   7. View > Reader View shows an article without the page around it.
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

def tap(label, limit=30):
	# Taps the node whose description or text starts with `label`, or whose
	# resource id is `label` -- the system's own dialogs name their controls.
	def found():
		adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
		tree = et.fromstring(adb("shell", "cat", "/sdcard/ui.xml") or "<x/>")
		for n in tree.iter("node"):
			name = n.get("content-desc") or n.get("text") or ""
			if name.startswith(label) or n.get("resource-id") == label:
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

#!/usr/bin/env python3
# What a page on the phone can see of hydra, asked from inside the page.
#
#     tool/webview_probe.py <adb> <device serial> <local port> [expression]
#
# With an expression, evaluates that instead of the probe -- awaiting it if
# it is a promise -- and prints what it returns: `tool/emulator_checks.py`
# asks the page for `ad_status.js` this way.
#
# Opens nothing itself: load the page in hydra first. It forwards the
# WebView's DevTools socket over adb -- which a debuggable build enables
# and a release build does not -- picks the youtube.com page, and evaluates
# a probe there: which patched functions give their source away, which
# globals or elements name this browser, and what YouTube's own state says
# about ads and its "Ad blockers are not allowed" wall. It reads nothing of
# the person's: no cookies, no storage, no account.
#
# Why it exists: project.md, "On Android every page can see the bridge, and
# call it" -- the measurement behind that entry, so it can be taken again.
import asyncio, json, subprocess, sys, urllib.request

ADB, SERIAL, PORT = sys.argv[1], sys.argv[2], int(sys.argv[3])
EXPRESSION = sys.argv[4] if len(sys.argv) > 4 else None

PROBE = r"""
(function () {
	var out = {};
	var src = function (f) {
		try {
			if (typeof f !== 'function') return typeof f;
			var t = String(f);
			if (t.indexOf('[native code]') >= 0) return 'native';
			return 'SOURCE: ' + t.replace(/\s+/g, ' ').slice(0, 140);
		} catch (e) { return 'throws'; }
	};
	var X = window.XMLHttpRequest && XMLHttpRequest.prototype;
	var R = window.Response && Response.prototype;
	out.functions = {
		'fetch': src(window.fetch), 'JSON.parse': src(JSON.parse),
		'JSON.stringify': src(JSON.stringify),
		'XHR.open': src(X && X.open), 'XHR.send': src(X && X.send),
		'Response.json': src(R && R.json), 'setTimeout': src(window.setTimeout),
		'setInterval': src(window.setInterval),
		'addEventListener': src(EventTarget.prototype.addEventListener),
		'MediaSource.addSourceBuffer': src(window.MediaSource && MediaSource.prototype.addSourceBuffer),
		'SourceBuffer.appendBuffer': src(window.SourceBuffer && SourceBuffer.prototype.appendBuffer),
		'Function.toString': src(Function.prototype.toString),
		'Notification.requestPermission': src(window.Notification && Notification.requestPermission),
		'permissions.query': src(navigator.permissions && navigator.permissions.query),
		'enumerateDevices': src(navigator.mediaDevices && navigator.mediaDevices.enumerateDevices)
	};
	out.own_shims = (navigator.permissions && Object.prototype.hasOwnProperty.call(navigator.permissions, 'query') ? 'query ' : '') +
	                (navigator.mediaDevices && Object.prototype.hasOwnProperty.call(navigator.mediaDevices, 'enumerateDevices') ? 'enumerateDevices' : '') || 'none';
	out.named_elements = Array.prototype.filter.call(document.querySelectorAll('[id],[class]'),
		function (e) { return /hydra/i.test(e.id + ' ' + e.className); }).length;
	try {
		var d = Object.getOwnPropertyDescriptor(Navigator.prototype, 'userAgentData') ||
		        Object.getOwnPropertyDescriptor(navigator, 'userAgentData');
		out.userAgentData_getter = d && d.get ? src(d.get) : (d ? 'value' : 'none');
		out.userAgentData_own = Object.getOwnPropertyNames(navigator).join(',') || 'none';
	} catch (e) { out.userAgentData_getter = 'throws'; }
	out.named_globals = Object.getOwnPropertyNames(window).filter(function (k) {
		return /hydra|^qt$|webchannel|^__/i.test(k);
	});
	out.hydraNative = typeof window.hydraNative;
	try { out.hydraNative_methods = window.hydraNative ? Object.keys(window.hydraNative).join(',') : ''; } catch (e) {}
	try { out.user_agent = ytcfg.data_.INNERTUBE_CONTEXT.client.userAgent; } catch (e) { out.user_agent = 'unreadable'; }
	try {
		var p = window.ytInitialPlayerResponse || {};
		out.initial_ad_placements = p.adPlacements === undefined ? 'undefined' : 'present';
	} catch (e) {}
	out.styles = document.querySelectorAll('style').length;
	out.wall_text = (document.body && document.body.innerText || '')
		.indexOf('Ad blockers') >= 0 ? 'present' : 'absent';
	out.url = location.href;
	return JSON.stringify(out);
})()
"""

def adb(*a):
	return subprocess.run([ADB, "-s", SERIAL, *a], capture_output=True, text=True,
	                      timeout=60).stdout

async def main():
	import websockets
	sockets = [l.split()[-1].lstrip("@") for l in adb("shell", "cat", "/proc/net/unix").splitlines()
	           if "webview_devtools_remote" in l]
	if not sockets:
		print("no WebView DevTools socket: is the build debuggable and hydra running?")
		return 1
	pid = adb("shell", "pidof", "se.vibes.hydra").strip()
	adb("forward", f"tcp:{PORT}", f"localabstract:webview_devtools_remote_{pid}")
	try:
		pages = json.load(urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json", timeout=40))
		# Several WebViews can hold a youtube page (a tab left open, one in
		# the background): the one on screen is the one being asked about.
		shown = lambda p: json.loads(p.get("description") or "{}").get("visible", False)
		tube = [p for p in pages if p.get("type") == "page" and "youtube.com" in p.get("url", "")]
		page = next((p for p in tube if shown(p)), tube[0] if tube else None)
		# An expression is a question about the page in front, whatever it is.
		if EXPRESSION:
			page = next((p for p in pages if p.get("type") == "page" and shown(p)), page)
		if not page:
			print("no page to ask:", [p.get("url") for p in pages])
			return 1
		async with websockets.connect(page["webSocketDebuggerUrl"], max_size=None) as ws:
			await ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
			                          "params": {"expression": EXPRESSION or PROBE,
			                                     "awaitPromise": True, "returnByValue": True}}))
			while True:
				m = json.loads(await asyncio.wait_for(ws.recv(), 20))
				if m.get("id") == 1:
					break
		val = m.get("result", {}).get("result", {}).get("value")
		if EXPRESSION:
			print(val if val is not None else json.dumps(m)[:2000])
		else:
			print(json.dumps(json.loads(val), indent=2) if val else json.dumps(m)[:2000])
	finally:
		adb("forward", "--remove", f"tcp:{PORT}")
	return 0

sys.exit(asyncio.run(main()))

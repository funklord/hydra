#include "scriptlets.h"

#include "site_rules.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// **The catalog, and the whole of it.** Each name is uBlock Origin's, so a
// rule written for the ecosystem resolves here; each implementation is this
// project's own and is meant to be read.
//
// Written as ES5 on purpose: this runs in the page's world at document
// creation, before anything has established what the page supports, and a
// syntax error there would take the whole injection with it.
const char *k_catalog = R"JS(
var C = {};

// json-prune: remove named properties from anything the page parses as JSON.
//
// This is the one that reaches a video ad. A player asks its own backend for a
// configuration document and reads an array of ad placements out of it; remove
// the array before the player sees it and there is nothing to play. The ads
// come from the same hosts as the video, so no network rule can see them.
//
// `paths` is a space-separated list of dotted property paths. `needle`, when
// given, is a list of paths that must ALL be present for anything to be
// removed -- which is how a rule avoids pruning every document the page reads
// and breaking the ones it did not mean.
//
// No `*` in a path. uBlock supports a wildcard there; this does not, and a
// rule using one simply finds nothing to remove rather than matching more than
// it was asked to.
// **The pruning rule, in one place.** Two scriptlets prune -- `json-prune`
// by patching the parser, `json-prune-fetch-response` by replacing a body --
// and the path walking is the same work. Returns null when the rule named
// nothing to remove, so a caller can decline to patch anything at all.
var pruner = function (paths, needle) {
	var want = String(paths || '').split(/\s+/).filter(Boolean);
	var must = String(needle || '').split(/\s+/).filter(Boolean);
	if (!want.length) return null;
	var reach = function (o, p, cut) {
		var parts = String(p).split('.');
		var last = parts.pop();
		var cur = o;
		for (var i = 0; i < parts.length; i++) {
			if (cur === null || typeof cur !== 'object') return false;
			if (!(parts[i] in cur)) return false;
			cur = cur[parts[i]];
		}
		if (cur === null || typeof cur !== 'object') return false;
		if (!(last in cur)) return false;
		if (cut) delete cur[last];
		return true;
	};
	return function (o) {
		if (o === null || typeof o !== 'object') return o;
		for (var i = 0; i < must.length; i++)
			if (!reach(o, must[i], false)) return o;
		for (var j = 0; j < want.length; j++) reach(o, want[j], true);
		return o;
	};
};

C['json-prune'] = function (paths, needle) {
	var prune = pruner(paths, needle);
	if (!prune) return;
	var real = JSON.parse;
	JSON.parse = function () { return prune(real.apply(this, arguments)); };
	// And the fetch path, which is how a modern player actually asks.
	if (window.Response && window.Response.prototype &&
	     window.Response.prototype.json) {
		var realJson = window.Response.prototype.json;
		window.Response.prototype.json = function () {
			return realJson.apply(this, arguments).then(prune);
		};
	}
};

// json-prune-fetch-response: prune the body of a matching fetch, whichever way
// the page then reads it.
//
// **What this does that `json-prune` cannot**, which is the reason it has its
// own name rather than being a wider version of that one. `json-prune`
// patches the parser and `Response.prototype.json`, so it covers a page that
// parses text or asks a response for JSON -- and it covers *every* such read
// on the page, guarded only by the needle. This one is scoped to a url, and
// it replaces the body itself, so a player that reads `.text()` and parses
// with something of its own sees pruned bytes too.
//
// **A body can only be read once**, which decides the shape: once `text()`
// has been called the original response is spent, so even the pass-through
// path has to hand back a new one. Returning `res` after reading it would
// give the page a response it cannot read.
// **Wrapping fetch so a matching body passes through a transform**, shared by
// the two scriptlets that rewrite one. A body can only be read once, which
// decides the shape: once `text()` has been called the original response is
// spent, so even the unchanged path hands back a new one. Returning `res`
// after reading it gives the page a response it cannot read.
var filter_fetch = function (wanted, transform) {
	if (typeof window.fetch !== 'function') return;
	var real = window.fetch;
	var like = function (res, text) {
		// The original's status and headers, so the page's own checks still
		// pass. Without a `Response` constructor -- which not every frame has
		// -- hand back something that answers the reads a caller makes.
		try {
			return new window.Response(text, {
				status: res.status, statusText: res.statusText,
				headers: res.headers
			});
		} catch (e) {}
		return {
			ok: res.ok, status: res.status, statusText: res.statusText,
			url: res.url, headers: res.headers,
			text: function () { return window.Promise.resolve(text); },
			json: function () {
				return window.Promise.resolve(JSON.parse(text));
			},
			clone: function () { return this; }
		};
	};
	window.fetch = function (input) {
		var url = '';
		try {
			url = (typeof input === 'string') ? input
			      : (input && input.url) ? input.url : '';
		} catch (e) {}
		var answer = real.apply(this, arguments);
		if (!url || !wanted(url)) return answer;
		if (!answer || typeof answer.then !== 'function') return answer;
		return answer.then(function (res) {
			if (!res || typeof res.text !== 'function') return res;
			return res.text().then(function (body) {
				var out;
				try { out = transform(body); } catch (e) { out = body; }
				return like(res, typeof out === 'string' ? out : body);
			});
		});
	};
};

// json-prune-fetch-response: prune the body of a matching fetch, whichever way
// the page then reads it.
//
// **What this does that `json-prune` cannot**, which is the reason it has its
// own name rather than being a wider version of that one. `json-prune`
// patches the parser and `Response.prototype.json`, so it covers a page that
// parses text or asks a response for JSON -- and it covers *every* such read
// on the page, guarded only by the needle. This one is scoped to a url, and
// it replaces the body itself, so a player that reads `.text()` and parses
// with something of its own sees pruned bytes too.
C['json-prune-fetch-response'] = function (paths, needle, match) {
	var prune = pruner(paths, needle);
	if (!prune) return;
	filter_fetch(matcher(match), function (body) {
		// Not JSON is not an error: the body comes through unchanged.
		return JSON.stringify(prune(JSON.parse(body)));
	});
};

// json-prune-xhr-response: the same for XMLHttpRequest.
//
// **The reads are redirected before the request is sent**, and that is the
// whole of what makes this reliable. The obvious implementation waits for
// `load` and rewrites the body then -- and a page that registered its own
// handler before calling `send` has already read the original by the time
// ours runs, because listeners fire in the order they were added. So this
// defines `responseText` and `response` on the instance at `send` time:
// whenever the page reads, from whichever listener, it reads through these.
//
// The real accessors are captured from the prototype first, so the pruned
// value is computed from exactly what the engine would have returned.
// **Wrapping XMLHttpRequest so a matching response passes through a
// transform**, shared by the two scriptlets that rewrite one.
//
// `on_text` rewrites the body as text. `on_parsed` is given the object the
// engine has already built, for `responseType === 'json'`, where
// `responseText` is not readable at all -- the spec makes it throw. The two
// are separate rather than one derived from the other so that a caller which
// can transform an object directly is not made to serialise it and parse it
// back.
var filter_xhr = function (wanted, on_text, on_parsed) {
	var XHR = window.XMLHttpRequest;
	if (typeof XHR !== 'function' || !XHR.prototype) return;

	var describe = function (name) {
		try { return Object.getOwnPropertyDescriptor(XHR.prototype, name); }
		catch (e) { return null; }
	};
	var text_of = describe('responseText');
	var resp_of = describe('response');
	if (!text_of || typeof text_of.get !== 'function') return;

	var open = XHR.prototype.open;
	var send = XHR.prototype.send;
	if (typeof open !== 'function' || typeof send !== 'function') return;

	XHR.prototype.open = function (method, url) {
		try { this.__hydra_url = String(url == null ? '' : url); } catch (e) {}
		return open.apply(this, arguments);
	};

	XHR.prototype.send = function () {
		var xhr = this;
		var url = '';
		try { url = String(xhr.__hydra_url || ''); } catch (e) {}
		if (!url || !wanted(url)) return send.apply(this, arguments);

		// Cached on the raw body, so repeated reads neither re-parse nor
		// disagree with each other -- a page that reads `responseText` twice
		// must see the same thing both times.
		var raw_seen = null, done = null;
		var new_text = function () {
			var raw;
			try { raw = text_of.get.call(xhr); } catch (e) { return undefined; }
			// **Before the body is complete there is nothing to transform.** A
			// progressive read at readyState 3 is a fragment, and handing
			// back a fragment unchanged is correct: what the page gets is
			// what it would have got.
			if (xhr.readyState !== 4 || typeof raw !== 'string') return raw;
			if (raw === raw_seen) return done;
			raw_seen = raw;
			try { done = on_text(raw); } catch (e) { done = raw; }
			if (typeof done !== 'string') done = raw;
			return done;
		};
		try {
			Object.defineProperty(xhr, 'responseText', {
				get: new_text, configurable: true
			});
			Object.defineProperty(xhr, 'response', {
				get: function () {
					var kind = '';
					try { kind = String(xhr.responseType || ''); } catch (e) {}
					if (kind === '' || kind === 'text') return new_text();
					var real;
					try {
						real = (resp_of && typeof resp_of.get === 'function')
						        ? resp_of.get.call(xhr) : undefined;
					} catch (e) { return undefined; }
					// **Only the shapes this can read are touched.** For
					// `json` the engine has already parsed it; an
					// arraybuffer, a blob or a document is handed over as it
					// is, because replacing what cannot be read would hand
					// the page something it did not ask for.
					if (kind !== 'json' || xhr.readyState !== 4) return real;
					try { return on_parsed(real); } catch (e) { return real; }
				},
				configurable: true
			});
		} catch (e) { /* a page that pinned them keeps them */ }
		return send.apply(this, arguments);
	};
};

C['json-prune-xhr-response'] = function (paths, needle, match) {
	var prune = pruner(paths, needle);
	if (!prune) return;
	filter_xhr(matcher(match), function (raw) {
		// Not JSON is not an error: the body comes through unchanged.
		return JSON.stringify(prune(JSON.parse(raw)));
	}, function (real) {
		// Already parsed, so prune the object rather than serialising it and
		// reading it back.
		return prune(real);
	});
};

// trusted-replace-xhr-response: the same rewrite as the fetch one, on the
// other transport. uBlock's YouTube rules use both.
//
// **The `json` responseType is the one case where this cannot see the bytes.**
// `responseText` throws for that type, so what the search runs against is the
// object re-serialised by `JSON.stringify` -- the same values, not the same
// text. A pattern that depends on the server's spacing or key order will not
// match there, and will on every other responseType. Stated rather than
// worked around: the alternative is a second transport read, and a rule whose
// pattern needs the original bytes can say so by matching a url that is not
// fetched as `json`.
C['trusted-replace-xhr-response'] = function (search, replacement, match) {
	var rewrite = rewriter(search, replacement);
	filter_xhr(matcher(match), rewrite, function (real) {
		try { return JSON.parse(rewrite(JSON.stringify(real))); }
		catch (e) { return real; }
	});
};

// set-constant: pin a page global to a value it then cannot change.
//
// **Only the vocabulary below, and that is narrower than uBlock's on
// purpose.** uBlock lets a rule supply an arbitrary string; here a rule may
// ask for a boolean, null, undefined, a number, an empty string or a function
// that does nothing -- the values a flag is read as. An arbitrary string would
// be the one place a filter list could put content of its own choosing into a
// page's globals, and the gain does not pay for it.
C['set-constant'] = function (path, raw) {
	if (!path) return;
	var value;
	var word = String(raw);
	if (word === 'true') value = true;
	else if (word === 'false') value = false;
	else if (word === 'null') value = null;
	else if (word === 'undefined') value = undefined;
	else if (word === 'noopFunc') value = function () {};
	else if (word === 'trueFunc') value = function () { return true; };
	else if (word === 'falseFunc') value = function () { return false; };
	else if (word === '' || word === 'emptyStr') value = '';
	else if (/^-?[0-9]+(\.[0-9]+)?$/.test(word)) value = Number(word);
	else return;
	trap_chain(path, pin(value));
};

// --- shared helpers for the rest of the catalog ---------------------------

// A needle: a plain substring, or `/re/flags`. An empty needle or `*` matches
// everything, which is what a rule with no argument means.
//
// A bad regex matches nothing rather than throwing: a filter list is not a
// place to learn that a pattern was mistyped, and a scriptlet that throws on
// load takes the rest of the catalog's calls with it.
var matcher = function (raw) {
	var s = String(raw == null ? '' : raw);
	if (s === '' || s === '*') return function () { return true; };
	if (s.length > 2 && s.charAt(0) === '/') {
		var end = s.lastIndexOf('/');
		if (end > 0) {
			try {
				var re = new RegExp(s.slice(1, end), s.slice(end + 1));
				return function (t) { return re.test(String(t)); };
			} catch (e) { return function () { return false; }; }
		}
	}
	return function (t) { return String(t).indexOf(s) >= 0; };
};

// The object a dotted path names and the last key in it, building the path as
// it goes. Null when the path runs into something that cannot hold a property.
var owner_of = function (path) {
	var parts = String(path || '').split('.');
	var last = parts.pop();
	if (!last) return null;
	var o = window;
	for (var i = 0; i < parts.length; i++) {
		if (o[parts[i]] === undefined || o[parts[i]] === null) o[parts[i]] = {};
		o = o[parts[i]];
		if (typeof o !== 'object' && typeof o !== 'function') return null;
	}
	return { o: o, k: last };
};

// **Every link of a dotted path guarded, so the leaf survives a replaced
// parent.** `owner_of` finds the object that holds the last key as it is
// now, and a page that then assigns a whole new object to a parent leaves a
// guard defined there behind on the old one. That is the first YouTube page
// load exactly: `set, ytInitialPlayerResponse.adPlacements, undefined` was
// defined on a placeholder, and the page's own
// `var ytInitialPlayerResponse = {...}` replaced it, ads and all.
//
// So each link is an accessor that holds what is assigned to it and applies
// the rest of the path to it, which is uBlock's answer. A link that cannot
// be redefined -- already non-configurable -- is still walked as it is now,
// and simply cannot follow a replacement.
//
// **A second rule on the same parent calls the first one's setter, rather
// than replacing it.** YouTube's list sets `playerAds`, `adPlacements` and
// `adSlots` under the same global, and three accessors that each overwrote
// the last would leave only one of them following a replacement.
//
// Absent parents are still created, as `owner_of` does, so a rule naming
// `cfg.ads` reads back on a page that has not made `cfg` yet.
var trap_chain = function (path, leaf) {
	var parts = String(path || '').split('.');
	if (!parts[parts.length - 1]) return;
	var holds = function (v) {
		return v !== null && (typeof v === 'object' || typeof v === 'function');
	};
	var walk = function (owner, i) {
		var k = parts[i];
		if (i === parts.length - 1) { leaf(owner, k); return; }
		var cur;
		try { cur = owner[k]; } catch (e) { return; }
		if (cur === undefined || cur === null) {
			cur = {};
			try { owner[k] = cur; } catch (e) { return; }
			try { cur = owner[k]; } catch (e) { return; }
		}
		if (!holds(cur)) return;
		var prev = null;
		try { prev = Object.getOwnPropertyDescriptor(owner, k); } catch (e) {}
		var held = cur;
		try {
			Object.defineProperty(owner, k, {
				configurable: true,
				enumerable: prev ? !!prev.enumerable : true,
				get: function () {
					return (prev && prev.get) ? prev.get.call(this) : held;
				},
				set: function (v) {
					if (prev && prev.set) {
						prev.set.call(this, v);
						v = prev.get ? prev.get.call(this) : v;
					}
					held = v;
					if (holds(v)) walk(v, i + 1);
				}
			});
		} catch (e) {}
		walk(cur, i + 1);
	};
	walk(window, 0);
};

// The leaf both setters define: reads answer the chosen value and writes are
// dropped. Non-configurable, so the page cannot define it back.
var pin = function (v) {
	return function (o, k) {
		try {
			Object.defineProperty(o, k, {
				get: function () { return v; },
				set: function () {},
				configurable: false
			});
		} catch (e) { /* already non-configurable: the page wins */ }
	};
};

// A value from the vocabulary `set-constant` uses, or undefined for anything
// else. One copy, because two scriptlets take a value and a second list of
// words is a second thing to be wrong.
var vocabulary = function (raw, allow_remove) {
	var w = String(raw);
	if (allow_remove && w === '$remove$') return { remove: true };
	if (w === 'true') return { v: true };
	if (w === 'false') return { v: false };
	if (w === 'null') return { v: null };
	if (w === 'undefined') return { v: undefined };
	if (w === 'noopFunc') return { v: function () {} };
	if (w === 'trueFunc') return { v: function () { return true; } };
	if (w === 'falseFunc') return { v: function () { return false; } };
	if (w === '' || w === 'emptyStr') return { v: '' };
	if (/^-?[0-9]+(\.[0-9]+)?$/.test(w)) return { v: Number(w) };
	return null;
};

// abort-on-property-read: reading it throws, which stops the script doing the
// reading and nothing else. The usual answer to a page that checks whether an
// ad object exists before deciding to complain.
C['abort-on-property-read'] = function (path) {
	var at = owner_of(path);
	if (!at) return;
	var stop = function () { throw new ReferenceError('hydra: ' + path); };
	try {
		Object.defineProperty(at.o, at.k, {
			get: stop, set: function () {}, configurable: false
		});
	} catch (e) {}
};

// abort-on-property-write: reading is fine, writing throws. For a page that
// installs its own detector onto a global.
C['abort-on-property-write'] = function (path) {
	var at = owner_of(path);
	if (!at) return;
	var held;
	try {
		Object.defineProperty(at.o, at.k, {
			get: function () { return held; },
			set: function () { throw new ReferenceError('hydra: ' + path); },
			configurable: false
		});
	} catch (e) {}
};

// abort-current-script: reading the property throws, but only from a script
// whose own text matches the needle. Narrower than aborting every reader,
// which is the point -- the page's own code reads the same globals.
C['abort-current-script'] = function (path, needle) {
	var at = owner_of(path);
	if (!at) return;
	var hit = matcher(needle);
	var held = at.o[at.k];
	try {
		Object.defineProperty(at.o, at.k, {
			get: function () {
				var el = null;
				try { el = document.currentScript; } catch (e) {}
				var text = (el && el.textContent) ? el.textContent : '';
				if (text && hit(text))
					throw new ReferenceError('hydra: ' + path);
				return held;
			},
			set: function (v) { held = v; },
			configurable: false
		});
	} catch (e) {}
};

// prevent-setTimeout / prevent-setInterval: drop a timer whose callback's own
// source matches, optionally only at one delay. The timer still returns an id,
// because a page that stores it and clears it later must not break.
var prevent_timer = function (which) {
	return function (needle, delay) {
		var real = window[which];
		if (typeof real !== 'function') return;
		var hit = matcher(needle);
		var want = (delay === undefined || delay === null ||
		             String(delay) === '') ? null : Number(delay);
		var fake = 0;
		window[which] = function (fn, ms) {
			var source = '';
			try { source = String(fn); } catch (e) {}
			var delay_matches = (want === null) || (Number(ms) === want);
			if (source && hit(source) && delay_matches)
				return ++fake;
			return real.apply(this, arguments);
		};
	};
};
C['prevent-setTimeout']  = prevent_timer('setTimeout');
C['prevent-setInterval'] = prevent_timer('setInterval');

// no-fetch-if: a matching request is answered empty rather than sent. Matched
// on the url, which is the common form; uBlock also takes key:value pairs on
// the request, and a rule using those finds nothing to match here.
C['no-fetch-if'] = function (needle) {
	if (typeof window.fetch !== 'function') return;
	var hit = matcher(needle);
	var real = window.fetch;
	window.fetch = function (input) {
		var url = '';
		try {
			url = (typeof input === 'string') ? input
			      : (input && input.url) ? input.url : '';
		} catch (e) {}
		if (url && hit(url)) {
			var empty = null;
			try { empty = new window.Response('', { status: 200 }); }
			catch (e) { empty = { ok: true, status: 200,
			                       text: function () { return ''; },
			                       json: function () { return null; } }; }
			try { return window.Promise.resolve(empty); } catch (e) {}
		}
		return real.apply(this, arguments);
	};
};

// nowebrtc: a peer connection is not something an ad needs, and it is a
// route to an address this browser is otherwise careful about.
C['nowebrtc'] = function () {
	var stub = function () {
		return {
			createOffer: function () {}, setLocalDescription: function () {},
			setRemoteDescription: function () {}, addIceCandidate: function () {},
			close: function () {}, addEventListener: function () {},
			createDataChannel: function () { return { close: function () {} }; }
		};
	};
	for (var i = 0; i < 3; i++) {
		var n = ['RTCPeerConnection', 'webkitRTCPeerConnection',
		          'mozRTCPeerConnection'][i];
		if (typeof window[n] === 'undefined') continue;
		try { window[n] = stub; } catch (e) {}
	}
};

// prevent-window-open: a matching popup is refused and the caller is handed a
// stub, because code that opens a window usually touches what comes back.
C['prevent-window-open'] = function (needle) {
	if (typeof window.open !== 'function') return;
	var hit = matcher(needle);
	var real = window.open;
	window.open = function (url) {
		if (hit(String(url == null ? '' : url))) {
			return {
				closed: false, close: function () { this.closed = true; },
				focus: function () {}, blur: function () {},
				document: { write: function () {}, close: function () {} }
			};
		}
		return real.apply(this, arguments);
	};
};

// set-local-storage-item: a flag a page reads back from storage. The same
// vocabulary as set-constant, plus `$remove$` to delete one.
// remove-attr / remove-class: take an attribute or a class off the elements a
// selector names. The two differ in one line, so they are one function.
//
// **With no selector given, the names are the selector.** `remove-attr, href`
// means the elements carrying `href`, which is `[href]`; `remove-class, ad`
// means `.ad`. That is uBlock's default and it is also the only sensible one:
// a rule that named no elements would have to mean all of them.
//
// **The observer is bounded unless the rule asks to stay.** A
// MutationObserver that queries the document on every mutation for the life
// of the page is a cost paid on every page the rule matches, and most rules
// want the elements gone as the page builds rather than policed for ever.
// Sixty-four passes or ten seconds, whichever comes first; `stay` in the
// third argument is uBlock's way of asking for the other thing.
// **Sweep now, at DOMContentLoaded, and on mutation under one bound.**
// Shared by the families that rewrite the DOM, so the cost of watching a page
// is written down once: sixty-four passes or ten seconds, whichever comes
// first, unless a rule says `stay`.
//
// Most rules want the elements gone as the page builds rather than policed
// for ever, and an observer that queries the document on every mutation is a
// cost paid on every page the rule matches.
var watch_dom = function (sweep, stay, want_attrs) {
	sweep();
	try {
		document.addEventListener('DOMContentLoaded', sweep, true);
	} catch (e) {}
	if (!window.MutationObserver || !document.documentElement) return;
	var passes = 0;
	var obs = new window.MutationObserver(function () {
		if (!stay && ++passes > 64) { obs.disconnect(); return; }
		sweep();
	});
	try {
		obs.observe(document.documentElement, {
			childList: true, subtree: true, attributes: !!want_attrs
		});
	} catch (e) { return; }
	if (!stay && typeof window.setTimeout === 'function')
		window.setTimeout(function () { obs.disconnect(); }, 10000);
};

// **A RegExp, dropping flags the engine will not take.** `s` (dotAll) is
// ES2018 and `u` later still; an engine without one throws from the
// constructor, and a caller that treats that as "no pattern" turns a whole
// scriptlet inert rather than slightly less precise.
//
// Measured: QJSEngine refuses `s`, so the node-text family compiled nothing
// and cleared every node it was pointed at -- the `includes` test silently
// skipped, because a null pattern reads as "no test asked for". The engine in
// the browser takes all of these; this is for the one the suite runs in and
// for whatever ships next.
//
// Dropped one at a time, most recent first, so the loss is the smallest the
// engine will accept.
var compile_re = function (body, flags) {
	var want = String(flags == null ? '' : flags);
	var order = ['v', 'u', 'd', 's', 'y', 'm', 'i', 'g'];
	for (;;) {
		try { return new RegExp(body, want); } catch (e) {}
		var dropped = false;
		for (var i = 0; i < order.length; i++) {
			if (want.indexOf(order[i]) < 0) continue;
			want = want.split(order[i]).join('');
			dropped = true;
			break;
		}
		if (!dropped) return null;
	}
};

// A `/re/` or a plain string, as a RegExp. `anchor` makes a plain string match
// the whole subject, which is what uBlock does for a node name -- otherwise
// `script` would match `noscript` too.
var re_of = function (raw, flags, anchor) {
	var t = String(raw == null ? '' : raw);
	if (t.length > 2 && t.charAt(0) === '/') {
		var end = t.lastIndexOf('/');
		if (end > 0)
			return compile_re(t.slice(1, end), t.slice(end + 1) || flags);
	}
	var quoted = t.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
	return compile_re(anchor ? '^' + quoted + '$' : quoted, flags);
};

var remove_dom = function (kind) {
	return function (names, selector, behaviour) {
		var list = String(names || '').split(/\s+/).filter(Boolean);
		if (!list.length) return;
		var sel = String(selector || '').replace(/^\s+|\s+$/g, '');
		if (!sel) {
			var parts = [];
			for (var i = 0; i < list.length; i++)
				parts.push(kind === 'attr' ? '[' + list[i] + ']'
				                            : '.' + list[i]);
			sel = parts.join(',');
		}
		var stay = String(behaviour || '').indexOf('stay') >= 0;
		var sweep = function () {
			var found;
			// An invalid selector is a mistyped rule, not an emergency: it
			// finds nothing and the page carries on.
			try { found = document.querySelectorAll(sel); }
			catch (e) { return; }
			for (var i = 0; i < found.length; i++) {
				for (var j = 0; j < list.length; j++) {
					try {
						if (kind === 'attr') found[i].removeAttribute(list[j]);
						else if (found[i].classList)
							found[i].classList.remove(list[j]);
					} catch (e) {}
				}
			}
		};
		watch_dom(sweep, stay, kind === 'attr');
	};
};
C['remove-attr']  = remove_dom('attr');
C['remove-class'] = remove_dom('class');

// --- the trusted class -----------------------------------------------------
//
// **These run only for a list the person marked trusted**, which is enforced
// in `read` and again in `source_for`. What they have in common is that their
// power is not "stop the page doing something" but "do something on the
// page's behalf", with a value the rule chooses: a cookie, an arbitrary
// global, a replaced response body. A rule in that class is a small program.

// trusted-set-constant: set-constant without the vocabulary. A string, or
// JSON where the value looks like it.
C['trusted-set-constant'] = function (path, value) {
	if (!path) return;
	var chosen = vocabulary(value, false);
	var v;
	if (chosen) {
		v = chosen.v;
	} else {
		var raw = String(value == null ? '' : value);
		v = raw;
		if (raw.charAt(0) === '{' || raw.charAt(0) === '[') {
			try { v = JSON.parse(raw); } catch (e) { v = raw; }
		}
	}
	trap_chain(path, pin(v));
};

// trusted-set-local-storage-item: the same without the vocabulary.
C['trusted-set-local-storage-item'] = function (key, value) {
	if (!key) return;
	var raw = String(value == null ? '' : value);
	try {
		if (raw === '$remove$') window.localStorage.removeItem(String(key));
		else window.localStorage.setItem(String(key), raw);
	} catch (e) {}
};

// trusted-set-cookie: write one, for a site whose own banner will not take an
// answer any other way.
//
// **Both halves are encoded**, and that is not tidiness: a value carrying `;`
// would otherwise add attributes of its own -- a path, a domain, a longer
// expiry than the rule asked for -- which is a filter list writing a cookie
// it did not say it was writing.
C['trusted-set-cookie'] = function (name, value, days, path) {
	if (!name) return;
	// The write is `write_cookie`, shared with the untrusted `set-cookie`:
	// one place where the encoding and the path check live, so the two
	// spellings cannot come to disagree about what a value may contain.
	write_cookie(name, value, days, path);
};

// **The search-and-replace one list asks for, shared by the two scriptlets
// that rewrite a body.** The search may be a substring or `/re/`.
//
// **The replacement means what `String.replace` means, and only on the regex
// path.** A list writing `/(a)(b)/` and `$2$1` means the groups, which is what
// the rules in this family are written against -- so a regex search keeps
// those semantics rather than inventing quieter ones. A substring search has
// no groups to reference and is applied with split and join, which both takes
// every occurrence rather than the first and leaves a `$` in the replacement
// as the character it is. The asymmetry is the search's, not a choice: there
// is nothing for `$1` to mean without a pattern.
var rewriter = function (search, replacement) {
	var s = String(search == null ? '' : search);
	var rep = String(replacement == null ? '' : replacement);
	return function (body) {
		if (s === '' || s === '*') return rep;
		if (s.length > 2 && s.charAt(0) === '/') {
			var end = s.lastIndexOf('/');
			if (end > 0) {
				var flags = s.slice(end + 1);
				if (flags.indexOf('g') < 0) flags += 'g';
				var re = new RegExp(s.slice(1, end), flags);
				return String(body).replace(re, rep);
			}
		}
		// Split and join rather than `replace`, which would take only the
		// first and would read `$&` in the replacement as a back-reference.
		return String(body).split(s).join(rep);
	};
};

C['trusted-replace-fetch-response'] = function (search, replacement, match) {
	filter_fetch(matcher(match), rewriter(search, replacement));
};

// prevent-addEventListener: refuse a listener whose type AND handler both
// match. uBlock's rule is `matchesBoth`, not either, and that is what keeps a
// rule aimed at one handler from taking every listener of that type with it.
//
// **Patched on `EventTarget.prototype` rather than on window and document**,
// because a listener added to an element has to be reachable too and there is
// one place all three go through.
//
// The handler is matched against its source text, which is what a list means
// by the second argument: `aeld, load, adsbygoogle` names the function that
// mentions it.
C['prevent-addEventListener'] = function (type, pattern) {
	var ET = window.EventTarget;
	if (!ET || !ET.prototype ||
	    typeof ET.prototype.addEventListener !== 'function')
		return;
	var want_type = matcher(type);
	var want_handler = matcher(pattern);
	var real = ET.prototype.addEventListener;
	ET.prototype.addEventListener = function (t, h) {
		var ts = '', hs = '';
		try { ts = String(t == null ? '' : t); } catch (e) {}
		try {
			hs = (h == null) ? '' : String(
			  typeof h === 'function' ? h
			  : (h && typeof h.handleEvent === 'function' ? h.handleEvent : h));
		} catch (e) {}
		// Both, per uBlock. A page that cannot be read back -- a native
		// function, a bound one -- stringifies to something short rather
		// than throwing, so the handler test simply does not match and the
		// listener is kept, which is the safe direction.
		if (want_type(ts) && want_handler(hs))
			return;
		return real.apply(this, arguments);
	};
};

// --- the boosters -----------------------------------------------------------
//
// adjust-setInterval / adjust-setTimeout: multiply a matching timer's delay.
// A page that polls for an ad every second and refuses to proceed until it
// answers is made to poll quickly instead of being broken outright, which is
// what the whole `nano-*` family is for.
//
// uBlock's numbers, kept because a rule was written against them: the delay
// defaults to 1000 and `*` means any; the boost defaults to 0.05 and is
// clamped to [0.001, 50], so a rule cannot stop a timer altogether by asking
// for a multiplier of zero, nor stall the page with a huge one.
var adjust_timer = function (which, needle, delay_arg, boost_arg) {
	var real = window[which];
	if (typeof real !== 'function') return;
	var want = matcher(needle);
	var delay = (String(delay_arg == null ? '' : delay_arg) === '*')
	             ? -1 : parseInt(delay_arg, 10);
	if (!isFinite(delay)) delay = 1000;
	var boost = parseFloat(boost_arg);
	boost = isFinite(boost) ? Math.min(Math.max(boost, 0.001), 50) : 0.05;
	window[which] = function (fn, ms) {
		var args = Array.prototype.slice.call(arguments);
		var src = '';
		try { src = String(fn == null ? '' : fn); } catch (e) {}
		// **Strict equality on the delay, as uBlock has it.** A page passing
		// a string delay does not match, which is uBlock's behaviour and
		// therefore what the rules were written against.
		if ((delay === -1 || ms === delay) && want(src))
			args[1] = ms * boost;
		return real.apply(this, args);
	};
};

C['adjust-setInterval'] = function (needle, delay, boost) {
	adjust_timer('setInterval', needle, delay, boost);
};

C['adjust-setTimeout'] = function (needle, delay, boost) {
	adjust_timer('setTimeout', needle, delay, boost);
};

// noeval: let the page call `eval` and have nothing happen.
//
// **This cannot reach a direct `eval(...)`**, which is a syntactic form the
// language resolves without reading `window.eval` -- uBlock's own version has
// the same limit, since it replaces the same property. What it does reach is
// the indirect call an obfuscated loader makes, which is the case the rules
// using it are aimed at.
C['noeval'] = function () {
	try { window.eval = function () {}; } catch (e) {}
};

// noeval-if(needle): the same, for matching code only.
C['noeval-if'] = function (needle) {
	var real = window.eval;
	if (typeof real !== 'function') return;
	var raw = String(needle == null ? '' : needle);
	// **An empty needle logs in uBlock and prevents nothing**, so patching
	// here would be stricter than the rule asks. Nothing is installed rather
	// than silently blocking every eval on the page.
	if (raw === '') return;
	var want = matcher(raw);
	window.eval = function (code) {
		var a = '';
		try { a = String(code == null ? '' : code); } catch (e) {}
		if (want(a)) return undefined;
		return real.apply(this, arguments);
	};
};

// prevent-requestAnimationFrame(needle): hand a matching callback a no-op
// frame rather than refusing the call, so a page waiting on the return value
// still gets a handle.
//
// A leading `!` inverts the match, which is uBlock's spelling.
C['prevent-requestAnimationFrame'] = function (needle) {
	var real = window.requestAnimationFrame;
	if (typeof real !== 'function') return;
	var raw = String(needle == null ? '' : needle);
	if (raw === '') return;        // logs in uBlock; prevents nothing
	var negated = raw.charAt(0) === '!';
	var want = matcher(negated ? raw.slice(1) : raw);
	window.requestAnimationFrame = function (fn) {
		var args = Array.prototype.slice.call(arguments);
		var src = '';
		try { src = String(fn == null ? '' : fn); } catch (e) {}
		if (want(src) !== negated)
			args[0] = function () {};
		return real.apply(this, args);
	};
};

// prevent-refresh(delay): stop a `<meta http-equiv=refresh>` from taking the
// page somewhere.
//
// With no argument the page is given half the time the meta asked for and
// then stopped, which is uBlock's arithmetic -- `parseFloat(content) * 500`
// -- and is deliberate: a reader gets a moment of the page before the load is
// cut, where stopping immediately would look like a broken site.
C['prevent-refresh'] = function (delay) {
	var raw = String(delay == null ? '' : delay);
	var defuse = function () {
		var meta = null;
		try {
			meta = document.querySelector(
			  'meta[http-equiv="refresh" i][content]');
		} catch (e) { return; }
		if (!meta) return;
		var content = '';
		try { content = String(meta.getAttribute('content') || ''); }
		catch (e) {}
		var ms = (raw === '')
		          ? Math.max(parseFloat(content) || 0, 0) * 500 : 0;
		var stop = function () { try { window.stop(); } catch (e) {} };
		if (ms === 0) stop();
		else window.setTimeout(stop, ms);
	};
	try {
		window.addEventListener('load', defuse,
		                         { capture: true, once: true });
	} catch (e) { /* a page with no window events keeps its refresh */ }
};

// disable-newtab-links(): swallow a click on any `<a target=...>`.
//
// Walks up from the target, because the anchor is rarely the element clicked
// -- an image or a span inside it is -- and a handler that only looked at
// `ev.target` would miss nearly every real case.
C['disable-newtab-links'] = function () {
	try {
		document.addEventListener('click', function (ev) {
			var at = ev && ev.target;
			while (at) {
				var name = '';
				try { name = String(at.localName || '').toLowerCase(); }
				catch (e) {}
				if (name === 'a' && at.hasAttribute &&
				    at.hasAttribute('target')) {
					try { ev.stopPropagation(); ev.preventDefault(); }
					catch (e) {}
					break;
				}
				at = at.parentNode;
			}
		}, true);
	} catch (e) {}
};

// --- the node-text family ---------------------------------------------------
//
// Rewrites the text inside matching nodes. `remove-node-text` clears it;
// `trusted-replace-node-text` rewrites it, and is trusted because a
// replacement that lands in a `<script>` is code the page then runs -- which
// is why uBlock gives the non-trusted spelling `replace-node-text` as an
// alias of the trusted one rather than as a scriptlet of its own.
//
// Extra arguments arrive as `key, value` pairs, which is uBlock's varargs
// convention: `includes` (or `condition`), `excludes` and `sedCount`.
var node_text = function (node_name, pattern, replacement, extras) {
	var want_name = re_of(node_name, 'i', true);
	if (!want_name) return;
	var re_pattern = String(pattern == null ? '' : pattern) === ''
	                  ? null : re_of(pattern, 'gms', false);
	if (String(pattern == null ? '' : pattern) !== '' && !re_pattern) return;
	var after_text = String(replacement == null ? '' : replacement);

	var opts = {};
	for (var i = 0; i + 1 < extras.length; i += 2)
		opts[String(extras[i])] = extras[i + 1];
	var re_inc = (opts.includes || opts.condition)
	              ? re_of(opts.includes || opts.condition, 'ms', false) : null;
	var re_exc = opts.excludes ? re_of(opts.excludes, 'ms', false) : null;
	var left = parseInt(opts.sedCount, 10);
	if (!isFinite(left) || left <= 0) left = -1;      // -1: no limit

	var handle = function (node) {
		var before;
		try { before = String(node.textContent); } catch (e) { return; }
		if (re_inc) { re_inc.lastIndex = 0;
		              if (!re_inc.test(before)) return; }
		if (re_exc) { re_exc.lastIndex = 0;
		              if (re_exc.test(before)) return; }
		if (re_pattern) {
			re_pattern.lastIndex = 0;
			if (!re_pattern.test(before)) return;
			re_pattern.lastIndex = 0;
		}
		try {
			node.textContent = re_pattern
			  ? before.replace(re_pattern, after_text) : after_text;
		} catch (e) { return; }
		if (left > 0) left -= 1;
	};

	var walk = function (root) {
		var w;
		try {
			// Elements and text nodes both: a rule may name `script` or the
			// text node itself, which spells as `#text`.
			w = document.createTreeWalker(root,
			  (window.NodeFilter
			    ? (window.NodeFilter.SHOW_ELEMENT | window.NodeFilter.SHOW_TEXT)
			    : 5));
		} catch (e) { return; }
		var mine = null;
		try { mine = document.currentScript; } catch (e) {}
		for (;;) {
			var node = w.nextNode();
			if (!node) break;
			// **Never this script's own node.** Rewriting the element that is
			// running is how a scriptlet eats itself.
			if (mine && node === mine) continue;
			var name = '';
			try { name = String(node.nodeName || ''); } catch (e) {}
			if (want_name.test(name)) handle(node);
			else if (name === 'TEMPLATE' && node.content) walk(node.content);
			if (left === 0) break;
		}
	};

	var sweep = function () {
		if (left === 0) return;
		if (document.documentElement) walk(document.documentElement);
	};
	watch_dom(sweep, String(opts.stay || '') === 'true', false);
};

C['remove-node-text'] = function (node_name, includes) {
	// uBlock's own spelling: the second argument is `includes`, and the text
	// is cleared rather than rewritten.
	node_text(node_name, '', '', ['includes', includes]);
};

C['trusted-replace-node-text'] = function (node_name, pattern, replacement) {
	var extras = Array.prototype.slice.call(arguments, 3);
	node_text(node_name, pattern, replacement, extras);
};

// --- the anti-anti-adblock shims -------------------------------------------
//
// **These four take no arguments and are not filters at all**: each one hands
// a named library a cooperative stand-in, so a page that refuses to work
// until its ad script answers gets an answer. They are ports of uBlock's
// neutered resources rather than of its scriptlets, which is why they have no
// parameters -- the rules invoke them bare, `##+js(nofab)`.
//
// Ported from those files rather than written from a description of what they
// do: the detail that matters is which property each library reads and what
// it expects back, and that is only in the source.

// popads-dummy: hand PopAds an empty object and let the page carry on.
C['popads-dummy'] = function () {
	try {
		delete window.PopAds;
		delete window.popns;
		Object.defineProperties(window, {
			PopAds: { value: {} },
			popns:  { value: {} }
		});
	} catch (e) {}
};

// popads.net: the louder version. Assigning to either property throws a token
// that the page's own error handler cannot read, and a handler installed here
// swallows exactly that token -- so the library's assignment fails, the page's
// other errors are untouched, and nothing appears in the console.
C['popads.net'] = function () {
	try {
		var magic = String.fromCharCode(Date.now() % 26 + 97) +
		             Math.floor(Math.random() * 982451653 + 982451653)
		               .toString(36);
		var was = window.onerror;
		window.onerror = function (msg, src, line, col, err) {
			if (typeof msg === 'string' && msg.indexOf(magic) !== -1)
				return true;
			if (typeof was === 'function')
				return was(msg, src, line, col, err);
			return undefined;
		};
		var throw_magic = function () { throw new ReferenceError(magic); };
		delete window.PopAds;
		delete window.popns;
		Object.defineProperties(window, {
			PopAds: { set: throw_magic },
			popns:  { set: throw_magic }
		});
	} catch (e) {}
};

// nofab: a FuckAdBlock that always reports no adblocker.
//
// **`onNotDetected` calls its callback and `onDetected` does not**, which is
// the whole trick -- the page's "no blocker here" path runs and its "blocker
// found" path never does. Six globals, three constructors and three
// instances, because the library has been spelled all six ways.
C['nofab'] = function () {
	try {
		var noop = function () {};
		var Fab = function () {};
		Fab.prototype.check = noop;
		Fab.prototype.clearEvent = noop;
		Fab.prototype.emitEvent = noop;
		Fab.prototype.on = function (a, b) { if (!a) b(); return this; };
		Fab.prototype.onDetected = function () { return this; };
		Fab.prototype.onNotDetected = function (a) { a(); return this; };
		Fab.prototype.setOption = noop;
		Fab.prototype.options = { set: noop, get: noop };
		var fab = new Fab();
		var give_class = { get: function () { return Fab; },
		                    set: function () {} };
		var give_inst  = { get: function () { return fab; },
		                    set: function () {} };
		var classes = ['FuckAdBlock', 'BlockAdBlock', 'SniffAdBlock'];
		var insts   = ['fuckAdBlock', 'blockAdBlock', 'sniffAdBlock'];
		for (var i = 0; i < classes.length; i++) {
			// **Assigned where the page already has one, defined where it does
			// not.** A page that declared the global first keeps a writable
			// property; redefining it would throw and leave the rest of the
			// shim uninstalled.
			if (window.hasOwnProperty(classes[i])) window[classes[i]] = Fab;
			else Object.defineProperty(window, classes[i], give_class);
			if (window.hasOwnProperty(insts[i])) window[insts[i]] = fab;
			else Object.defineProperty(window, insts[i], give_inst);
		}
	} catch (e) {}
};

// prevent-bab: refuse the BlockAdBlock script by what it contains.
//
// **Signature matching rather than a name**, because the script is generated
// per page and has none. uBlock's four signatures and its four-fifths
// threshold are kept exactly: a token list that matches 80% of its tokens is
// the script, and the fourth list is fifteen tokens of the obfuscated loader.
// Changing the threshold is changing which pages this fires on, so it is not
// a number to improve on without the corpus uBlock has.
C['prevent-bab'] = function () {
	var signatures = [
		['blockadblock'],
		['babasbm'],
		[/getItem\('babn'\)/],
		['getElementById', 'String.fromCharCode',
		  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789',
		  'charAt', 'DOMContentLoaded', 'AdBlock', 'addEventListener',
		  'doScroll', 'fromCharCode', '<<2|r>>4', 'sessionStorage',
		  'clientWidth', 'localStorage', 'Math', 'random']
	];
	var check = function (text) {
		if (typeof text !== 'string') return false;
		for (var i = 0; i < signatures.length; i++) {
			var tokens = signatures[i], hits = 0;
			for (var j = 0; j < tokens.length; j++) {
				var t = tokens[j];
				var hit = (t instanceof RegExp) ? t.test(text)
				                                 : text.indexOf(t) >= 0;
				if (hit) hits += 1;
			}
			if (hits / tokens.length >= 0.8) return true;
		}
		return false;
	};
	var real_eval = window.eval;
	if (typeof real_eval === 'function') {
		window.eval = function (code) {
			if (!check(code)) return real_eval.apply(this, arguments);
			// Undo what the script had already done to the page before it
			// called us: the body is hidden and a message element inserted.
			try {
				if (document.body && document.body.style)
					document.body.style.removeProperty('visibility');
				var el = document.getElementById('babasbmsgx');
				if (el && el.parentNode) el.parentNode.removeChild(el);
			} catch (e) {}
			return undefined;
		};
	}
	var real_timeout = window.setTimeout;
	if (typeof real_timeout === 'function') {
		window.setTimeout = function (fn) {
			var args = Array.prototype.slice.call(arguments);
			if (typeof fn === 'string' && /\.bab_elementid.$/.test(fn))
				args[0] = function () {};
			return real_timeout.apply(this, args);
		};
	}
};

// --- cookies, links, and the stack ------------------------------------------

// **The values a cookie may be given without trust.** uBlock's list, copied
// whole rather than summarised: every one is an answer to a consent question,
// which is why the untrusted spelling can write a cookie at all. A number
// within a signed 16-bit range is also allowed, and anything else needs
// `trusted-set-cookie`.
var safe_cookie_values = [
	'accept', 'reject', 'accepted', 'rejected', 'notaccepted',
	'allow', 'disallow', 'deny', 'allowed', 'denied',
	'approved', 'disapproved', 'checked', 'unchecked',
	'dismiss', 'dismissed', 'enable', 'disable', 'enabled', 'disabled',
	'essential', 'nonessential', 'forbidden', 'forever',
	'hide', 'hidden', 'necessary', 'required', 'ok', 'on', 'off',
	'true', 't', 'false', 'f', 'yes', 'y', 'no', 'n',
	'all', 'none', 'functional', 'granted', 'done',
	'decline', 'declined', 'closed', 'next', 'mandatory',
	'disagree', 'agree', 'set', 'unset', 'given'
];

// The write itself, shared with `trusted-set-cookie`: both halves encoded,
// and the path checked rather than encoded, for the reason recorded there.
var write_cookie = function (name, value, days, path) {
	var bits = encodeURIComponent(String(name)) + '=' +
	            encodeURIComponent(String(value == null ? '' : value));
	var n = Number(days);
	if (isFinite(n) && n > 0) {
		var until = new Date();
		until.setTime(until.getTime() + n * 86400000);
		try { bits += '; expires=' + until.toUTCString(); } catch (e) {}
	}
	var where = String(path == null ? '' : path);
	bits += '; path=' + (/^\/[A-Za-z0-9._~\-\/]*$/.test(where) ? where : '/');
	try { document.cookie = bits; } catch (e) {}
};

// set-cookie(name, value, path): the consent-answer version.
C['set-cookie'] = function (name, value, path) {
	if (!name) return;
	var raw = String(value == null ? '' : value).toLowerCase();
	var m = /^("?)(.+)\1$/.exec(raw);
	var unquoted = (m && m[2]) || raw;
	if (safe_cookie_values.indexOf(unquoted) < 0) {
		// **A bare number is allowed, within a signed 16-bit range.** uBlock's
		// bound, and it is what stops this writing an arbitrary identifier.
		if (!/^-?\d+$/.test(unquoted)) return;
		var as_num = parseInt(unquoted, 10) || 0;
		if (as_num < -32767 || as_num > 32767) return;
	}
	write_cookie(name, value, '', path);
};

// remove-cookie(needle): clear every cookie whose name matches, and keep
// clearing for a while, because a page that sets one on a timer would
// otherwise win by being later.
C['remove-cookie'] = function (needle) {
	var want = matcher(needle);
	var host = '';
	try { host = String(location.hostname || ''); } catch (e) {}
	// The parent domains as well as this one: a cookie set on `.example.com`
	// is not removed by expiring it on `www.example.com`.
	var scopes = [''];
	var at = host;
	for (;;) {
		scopes.push(at);
		var dot = at.indexOf('.');
		if (dot < 0) break;
		at = at.slice(dot + 1);
		if (at.indexOf('.') < 0) break;
	}
	var remove = function () {
		var all = '';
		try { all = String(document.cookie || ''); } catch (e) { return; }
		var parts = all.split(';');
		for (var i = 0; i < parts.length; i++) {
			var name = parts[i].split('=')[0].replace(/^\s+|\s+$/g, '');
			if (!name || !want(name)) continue;
			for (var j = 0; j < scopes.length; j++) {
				var d = scopes[j] ? '; domain=' + scopes[j] : '';
				try {
					document.cookie = name +
					  '=; expires=Thu, 01 Jan 1970 00:00:00 GMT; path=/' + d;
				} catch (e) {}
			}
		}
	};
	remove();
	// Bounded, like the DOM sweeps: a page that is still fighting after ten
	// seconds has won, and a timer that never stops is a cost on every page
	// the rule matches.
	if (typeof window.setInterval === 'function' &&
	    typeof window.setTimeout === 'function') {
		var timer = window.setInterval(remove, 500);
		window.setTimeout(function () {
			try { window.clearInterval(timer); } catch (e) {}
		}, 10000);
	}
};

// href-sanitizer(selector, source): put the real destination on a link that
// points at a tracker, taking it from the link's own text or an attribute.
//
// **Only a same-looking URL is accepted**, which is uBlock's check and the
// reason this is not a redirect machine: the candidate has to parse as a URL
// against the document, carry no control characters, and use a web scheme.
C['href-sanitizer'] = function (selector, source) {
	var sel = String(selector == null ? '' : selector);
	if (!sel) return;
	var from = String(source == null ? '' : source) || 'text';
	var validate = function (text) {
		if (typeof text !== 'string' || text === '') return '';
		if (/[\x00-\x20\x7f]/.test(text)) return '';
		var url;
		try { url = new window.URL(text, document.location); }
		catch (e) { return ''; }
		if (url.protocol !== 'https:' && url.protocol !== 'http:') return '';
		return url.href;
	};
	var sweep = function () {
		var found;
		try { found = document.querySelectorAll(sel); } catch (e) { return; }
		for (var i = 0; i < found.length; i++) {
			var el = found[i], text = '';
			try {
				if (from === 'text') text = String(el.textContent || '');
				else if (from.indexOf('?') === 0) {
					var here = new window.URL(el.href, document.location);
					text = here.searchParams.get(from.slice(1)) || '';
				} else text = String(el.getAttribute(from) || '');
			} catch (e) { continue; }
			var href = validate(text.replace(/^\s+|\s+$/g, ''));
			if (!href) continue;
			try { el.setAttribute('href', href); } catch (e) {}
		}
	};
	watch_dom(sweep, false, false);
};

// abort-on-stack-trace(chain, needle): throw when the named property is
// touched from a call stack the needle matches.
//
// **The stack is normalised before matching**, because a raw one differs
// between engines and carries column numbers nothing should depend on. Each
// frame becomes `function url:line`, with the document's own URL spelled
// `inlineScript` and this script's `injectedScript`, which is what uBlock's
// rules are written against.
//
// A leading `!` inverts the test, as it does elsewhere.
C['abort-on-stack-trace'] = function (chain, needle) {
	if (!chain) return;
	var raw = String(needle == null ? '' : needle);
	// uBlock only logs when the needle is empty; nothing is installed here
	// rather than aborting on every read.
	if (raw === '') return;
	var negated = raw.charAt(0) === '!';
	var want = matcher(negated ? raw.slice(1) : raw);
	var token = 'hydra_' + Math.floor(Math.random() * 982451653).toString(36);

	var here = '';
	// `location` rather than `window.location`: the same object in a page,
	// and the spelling the dispatcher already uses.
	try { here = String(location.href || '').split('#')[0]; } catch (e) {}
	var stack_matches = function () {
		var stack = '';
		try { stack = String(new Error(token).stack || ''); } catch (e) {}
		if (!stack) return false;
		var out = [];
		var lines = stack.split(/[\n\r]+/);
		for (var i = 0; i < lines.length; i++) {
			var line = lines[i].replace(/^\s+|\s+$/g, '');
			if (!line || line.indexOf(token) >= 0) continue;
			// **The column is optional and the url may be empty.** uBlock's
			// own pattern requires `:line:column`, which is Chrome's and
			// Firefox's shape -- an engine that reports `fn@:1` matches
			// nothing, so every frame is dropped and the needle can never
			// fire. Measured: QJSEngine answers exactly that. Relaxing it
			// keeps both browser formats working and stops a whole scriptlet
			// going silently inert on an engine that is merely terser.
			var m = /(.*?@)?(\S*)(:\d+)(?::\d+)?\)?$/.exec(line);
			if (!m) continue;
			var url = m[2];
			if (url.charAt(0) === '(') url = url.slice(1);
			if (url === here) url = 'inlineScript';
			else if (url.indexOf('<anonymous>') === 0) url = 'injectedScript';
			var fn = (m[1] !== undefined) ? m[1].slice(0, -1)
			                               : line.slice(0, m.index).trim();
			if (fn.indexOf('at') === 0) fn = fn.slice(2).replace(/^\s+/, '');
			out.push((fn + ' ' + url + m[3] + ':1').replace(/^\s+/, ''));
		}
		return want(out.join('\n'));
	};

	var at = owner_of(chain);
	if (!at) return;
	var held;
	try { held = at.o[at.k]; } catch (e) {}
	try {
		Object.defineProperty(at.o, at.k, {
			get: function () {
				if (stack_matches() !== negated)
					throw new ReferenceError(token);
				return held;
			},
			set: function (v) {
				if (stack_matches() !== negated)
					throw new ReferenceError(token);
				held = v;
			},
			configurable: true
		});
	} catch (e) {}
};

// --- refusing a request outright -------------------------------------------

// **uBlock's `propsToMatch`, which is not just a url pattern.** Space
// separated, each term either `prop:pattern` or a bare pattern for the
// implicit key. The rule for telling them apart is uBlock's and is not
// obvious: a term splits at the first colon, and if the part before it holds
// anything outside `[$\w -]` then the whole term is a pattern instead -- which
// is how `/doubleclick|googlesyndication/` stays a url pattern while
// `method:HEAD` is a property test.
var props_matcher = function (raw, implicit) {
	var terms = String(raw == null ? '' : raw).split(/\s+/).filter(Boolean);
	var needles = [];
	for (var i = 0; i < terms.length; i++) {
		var term = terms[i];
		var at = term.indexOf(':');
		var prop = at < 0 ? term : term.slice(0, at);
		var pattern = at < 0 ? undefined : term.slice(at + 1);
		if (prop === '') continue;
		if (pattern !== undefined && /[^$\w -]/.test(prop)) {
			prop = term;
			pattern = undefined;
		}
		if (pattern !== undefined) needles.push([prop, matcher(pattern)]);
		else if (implicit) needles.push([implicit, matcher(prop)]);
	}
	return function (subject) {
		if (!needles.length) return false;
		for (var k = 0; k < needles.length; k++) {
			var key = needles[k][0], want = needles[k][1];
			var have = '';
			try { have = String(subject[key] == null ? '' : subject[key]); }
			catch (e) { return false; }
			if (!want(have)) return false;
		}
		return true;
	};
};

// The body a `directive` asks for. uBlock's words, and the random filler is
// what `true` and `length:` mean -- a page checking that something came back
// is satisfied, without being handed anything that parses.
var generated_body = function (directive) {
	var d = String(directive == null ? '' : directive);
	var randomize = function (len) {
		var out = [];
		var size = 0;
		do {
			var chunk = Math.random().toString(36).slice(2);
			out.push(chunk);
			size += chunk.length;
		} while (size < len);
		return out.join(' ').slice(0, len);
	};
	if (d === 'true') return randomize(10);
	if (d === 'emptyObj') return '{}';
	if (d === 'emptyArr') return '[]';
	if (d === 'emptyStr' || d === '') return '';
	var m = /^length:(\d+)(?:-(\d+))?$/.exec(d);
	if (m) {
		var min = parseInt(m[1], 10);
		var max = m[2] ? parseInt(m[2], 10) : min;
		if (!isFinite(min) || min < 0) return '';
		if (!isFinite(max) || max < min) max = min;
		return randomize(min + Math.floor(Math.random() * (max - min + 1)));
	}
	// **An unknown directive gets an empty body rather than a guess.** The
	// alternative is handing a page the directive's own text, which is a
	// filter list's words appearing in a response.
	return '';
};

// prevent-xhr(propsToMatch, directive): answer a matching request here
// instead of sending it.
//
// **The shape of the answer follows `responseType`**, as uBlock has it: an
// arraybuffer gets an empty buffer, a blob an empty blob, json an empty
// object, and anything textual gets what the directive asks for. Handing a
// page a string where it asked for an ArrayBuffer is how a block turns into
// a crash in the page's own code.
C['prevent-xhr'] = function (props, directive) {
	var XHR = window.XMLHttpRequest;
	if (typeof XHR !== 'function' || !XHR.prototype) return;
	var raw_props = String(props == null ? '' : props);
	if (raw_props === '') return;         // logs in uBlock; blocks nothing
	var wanted = props_matcher(raw_props, 'url');
	var open = XHR.prototype.open;
	var send = XHR.prototype.send;
	if (typeof open !== 'function' || typeof send !== 'function') return;

	XHR.prototype.open = function (method, url) {
		try {
			this.__hydra_blocked = wanted({
				method: String(method == null ? '' : method),
				url: String(url == null ? '' : url)
			});
		} catch (e) { this.__hydra_blocked = false; }
		return open.apply(this, arguments);
	};

	XHR.prototype.send = function () {
		var xhr = this;
		var blocked = false;
		try { blocked = !!xhr.__hydra_blocked; } catch (e) {}
		if (!blocked) return send.apply(this, arguments);

		var kind = '';
		try { kind = String(xhr.responseType || ''); } catch (e) {}
		var text = '', value = '';
		if (kind === 'arraybuffer') {
			try { value = new window.ArrayBuffer(0); } catch (e) { value = ''; }
		} else if (kind === 'blob') {
			try { value = new window.Blob([]); } catch (e) { value = ''; }
		} else if (kind === 'json') {
			text = '{}';
			value = {};
		} else {
			text = generated_body(directive);
			value = text;
		}
		var define = function (name, v) {
			try {
				Object.defineProperty(xhr, name, {
					get: function () { return v; }, configurable: true
				});
			} catch (e) {}
		};
		define('readyState', 4);
		define('status', 200);
		define('statusText', 'OK');
		define('responseURL', '');
		define('responseText', text);
		define('response', value);
		// **Announced rather than merely answered.** A page waiting on
		// `onload` would hang for ever on a request that simply never
		// completes, which looks like a broken site rather than a blocked
		// request.
		var fire = function () {
			var names = ['readystatechange', 'load', 'loadend'];
			for (var i = 0; i < names.length; i++) {
				try {
					if (typeof xhr.dispatchEvent === 'function' &&
					    typeof window.Event === 'function') {
						xhr.dispatchEvent(new window.Event(names[i]));
						continue;
					}
				} catch (e) {}
				var h = null;
				try { h = xhr['on' + names[i]]; } catch (e) {}
				if (typeof h === 'function') {
					try { h.call(xhr, { type: names[i], target: xhr }); }
					catch (e) {}
				}
			}
		};
		if (typeof window.setTimeout === 'function') window.setTimeout(fire, 1);
		else fire();
	};
};

// trusted-prevent-fetch(propsToMatch, body, type): the same for fetch, with
// a body the rule chooses -- which is why it needs trust where `no-fetch-if`,
// answering with a rejection, does not.
C['trusted-prevent-fetch'] = function (props, body, type) {
	if (typeof window.fetch !== 'function') return;
	var raw_props = String(props == null ? '' : props);
	if (raw_props === '') return;
	var wanted = props_matcher(raw_props, 'url');
	var real = window.fetch;
	window.fetch = function (input, init) {
		var url = '', method = 'GET';
		try {
			url = (typeof input === 'string') ? input
			      : (input && input.url) ? input.url : '';
			method = (init && init.method) ? String(init.method)
			         : (input && input.method) ? String(input.method) : 'GET';
		} catch (e) {}
		var hit = false;
		try { hit = !!url && wanted({ url: url, method: method }); }
		catch (e) {}
		if (!hit) return real.apply(this, arguments);
		var text = generated_body(body);
		var kind = String(type == null ? '' : type);
		try {
			return window.Promise.resolve(new window.Response(text, {
				status: 200, statusText: 'OK',
				headers: kind ? { 'Content-Type': kind } : undefined
			}));
		} catch (e) {}
		return window.Promise.resolve({
			ok: true, status: 200, statusText: 'OK', url: url,
			text: function () { return window.Promise.resolve(text); },
			json: function () {
				return window.Promise.resolve(JSON.parse(text || 'null'));
			},
			clone: function () { return this; }
		});
	};
};

// trusted-replace-argument(path, position, value): change what a call is
// given. `add:N` offsets a number, `repl:/a/b/` rewrites a string, and
// anything else is a value from the shared vocabulary or a literal.
//
// A negative position counts from the end, and `this` is spelled as the
// position rather than as a value, both of which are uBlock's.
C['trusted-replace-argument'] = function (path, position, value) {
	if (!path) return;
	var at = owner_of(path);
	if (!at) return;
	var real;
	try { real = at.o[at.k]; } catch (e) { return; }
	if (typeof real !== 'function') return;
	var pos_raw = String(position == null ? '' : position);
	var offset = parseInt(pos_raw, 10);
	if (!isFinite(offset)) offset = 0;
	var raw = String(value == null ? '' : value);
	var replace_with;
	if (raw.indexOf('add:') === 0) {
		var delta = parseFloat(raw.slice(4));
		if (!isFinite(delta)) return;
		replace_with = function (arg) { return Number(arg) + delta; };
	} else if (raw.indexOf('repl:') === 0) {
		var rewrite = null;
		var body = raw.slice(5);
		var parts = body.split('/');
		// `/search/replacement/flags`, which is the only spelling uBlock
		// accepts here.
		if (parts.length >= 4 && parts[0] === '') {
			try {
				var re = new RegExp(parts[1], parts[3] || '');
				rewrite = function (arg) {
					return String(arg).replace(re, parts[2]);
				};
			} catch (e) { return; }
		}
		if (!rewrite) return;
		replace_with = rewrite;
	} else {
		var chosen = vocabulary(raw, false);
		var fixed = chosen ? chosen.v : raw;
		replace_with = function () { return fixed; };
	}
	try {
		at.o[at.k] = function () {
			var args = Array.prototype.slice.call(arguments);
			if (pos_raw === 'this') return real.apply(replace_with(this), args);
			var i = offset >= 0 ? offset : args.length + offset;
			if (i >= 0 && i < args.length) args[i] = replace_with(args[i]);
			return real.apply(this, args);
		};
	} catch (e) {}
};

C['set-local-storage-item'] = function (key, value) {
	if (!key) return;
	var chosen = vocabulary(value, true);
	if (!chosen) return;
	try {
		if (chosen.remove) window.localStorage.removeItem(String(key));
		else window.localStorage.setItem(String(key), String(chosen.v));
	} catch (e) { /* storage refused, which a page can also see */ }
};
)JS";

// **The catalog, with the names rules actually use.** A list rule calls a
// scriptlet by whichever spelling its author knew -- `aopr` and
// `abort-on-property-read` are the same request, and `acis` was the name
// before `acs` -- so refusing an alias would refuse the rule rather than the
// capability. The aliases are uBlock's own; the code behind every one of them
// is this project's.
//
// `patterns` is a bitmask of argument positions that are matched against
// something rather than named: those are checked before the call is kept,
// because a `/re/` from a filter list ends up in a `RegExp` in the page.
struct catalog_entry {
	const char *name;
	int         patterns;    // argument positions matched against something
	int         selectors;   // argument positions that are CSS selectors
	bool        trusted;     // only for a list the person marked trusted
};

const catalog_entry k_entries[] = {
	{ "json-prune",                      0,      0,      false },
	{ "set-constant",                    0,      0,      false },
	{ "abort-on-property-read",          0,      0,      false },
	{ "abort-on-property-write",         0,      0,      false },
	{ "abort-current-script",            1 << 1, 0,      false },
	{ "prevent-setTimeout",              1 << 0, 0,      false },
	{ "prevent-setInterval",             1 << 0, 0,      false },
	{ "no-fetch-if",                     1 << 0, 0,      false },
	{ "nowebrtc",                        0,      0,      false },
	{ "prevent-window-open",             1 << 0, 0,      false },
	// Both arguments are patterns: the event type and the handler's source.
	{ "prevent-addEventListener", (1 << 0) | (1 << 1), 0,      false },
	{ "set-local-storage-item",          0,      0,      false },
	// The booster family: the needle is matched against the callback.
	{ "adjust-setInterval",              1 << 0, 0,      false },
	{ "adjust-setTimeout",               1 << 0, 0,      false },
	{ "noeval",                          0,      0,      false },
	{ "noeval-if",                       1 << 0, 0,      false },
	{ "prevent-requestAnimationFrame",   1 << 0, 0,      false },
	{ "prevent-refresh",                 0,      0,      false },
	{ "disable-newtab-links",            0,      0,      false },
	{ "prevent-xhr",                     1 << 0, 0,      false },
	{ "trusted-prevent-fetch",           1 << 0, 0,      true  },
	{ "trusted-replace-argument",        0,      0,      true  },
	{ "set-cookie",                      0,      0,      false },
	{ "remove-cookie",                   1 << 0, 0,      false },
	{ "href-sanitizer",                  0,      1 << 0, false },
	{ "abort-on-stack-trace",            1 << 1, 0,      false },
	// The shims: no arguments at all, which is how the rules invoke them.
	{ "popads-dummy",                    0,      0,      false },
	{ "popads.net",                      0,      0,      false },
	{ "nofab",                           0,      0,      false },
	{ "prevent-bab",                     0,      0,      false },
	// The node name and the text test are both patterns.
	{ "remove-node-text",         (1 << 0) | (1 << 1), 0,      false },
	{ "trusted-replace-node-text",
	                              (1 << 0) | (1 << 1), 0,      true  },
	{ "json-prune-fetch-response",       1 << 2, 0,      false },
	{ "json-prune-xhr-response",         1 << 2, 0,      false },
	// (names, selector, behaviour): the selector is the second argument, and
	// a rule may leave it out, in which case the names become it.
	{ "remove-attr",                     0,      1 << 1, false },
	{ "remove-class",                    0,      1 << 1, false },
	// **The trusted class: not filters but small programs**, and only for a
	// list somebody marked trusted. See `requires_trust` for the argument.
	{ "trusted-set-constant",            0,      0,      true  },
	{ "trusted-set-local-storage-item",  0,      0,      true  },
	{ "trusted-set-cookie",              0,      0,      true  },
	// Two patterns: what to look for, and which url to look in.
	{ "trusted-replace-fetch-response",
	                          (1 << 0) | (1 << 2), 0,      true  },
	{ "trusted-replace-xhr-response",
	                          (1 << 0) | (1 << 2), 0,      true  },
};


struct catalog_alias {
	const char *from;
	const char *to;
};

const catalog_alias k_aliases[] = {
	{ "aopr",                        "abort-on-property-read"  },
	{ "aopw",                        "abort-on-property-write" },
	{ "acs",                         "abort-current-script"    },
	{ "abort-current-inline-script", "abort-current-script"    },
	{ "acis",                        "abort-current-script"    },
	{ "nostif",                      "prevent-setTimeout"      },
	{ "no-setTimeout-if",            "prevent-setTimeout"      },
	{ "setTimeout-defuser",          "prevent-setTimeout"      },
	{ "nosiif",                      "prevent-setInterval"     },
	{ "no-setInterval-if",           "prevent-setInterval"     },
	{ "setInterval-defuser",         "prevent-setInterval"     },
	// **`set` is worth 302 rules of the 2453 in uBlock's own list**, which is
	// more than any other missing name and was missing because nobody had
	// read a real list through this. Measured 2026-10-08 against
	// `filters.txt`; the alias comes from uBlock's `set-constant.js`, which
	// declares `set.js`, rather than from anybody's memory of it.
	{ "set",                         "set-constant"            },
	{ "trusted-set",                 "trusted-set-constant"    },
	{ "trusted-rpfr",                "trusted-replace-fetch-response" },
	{ "prevent-fetch",               "no-fetch-if"             },
	{ "window.open-defuser",         "prevent-window-open"     },
	{ "nowoif",                      "prevent-window-open"     },
	{ "no-window-open-if",           "prevent-window-open"     },
	{ "nano-sib",                    "adjust-setInterval"      },
	{ "nano-setInterval-booster",    "adjust-setInterval"      },
	{ "nano-stb",                    "adjust-setTimeout"       },
	{ "nano-setTimeout-booster",     "adjust-setTimeout"       },
	{ "prevent-eval-if",             "noeval-if"               },
	{ "norafif",                     "prevent-requestAnimationFrame" },
	{ "no-requestAnimationFrame-if", "prevent-requestAnimationFrame" },
	{ "refresh-defuser",             "prevent-refresh"         },
	{ "no-xhr-if",                   "prevent-xhr"             },
	{ "cookie-remover",              "remove-cookie"           },
	{ "urlskip",                     "href-sanitizer"          },
	{ "aost",                        "abort-on-stack-trace"    },
	{ "nobab",                       "prevent-bab"             },
	{ "bab-defuser",                 "prevent-bab"             },
	{ "rmnt",                        "remove-node-text"        },
	// **uBlock gives `replace-node-text` as an alias of the TRUSTED one**,
	// not as a scriptlet of its own, because a replacement landing in a
	// `<script>` is code the page runs. Both spellings therefore need trust.
	{ "rpnt",                        "trusted-replace-node-text" },
	{ "trusted-rpnt",                "trusted-replace-node-text" },
	{ "replace-node-text",           "trusted-replace-node-text" },
	{ "aeld",                        "prevent-addEventListener" },
	{ "addEventListener-defuser",    "prevent-addEventListener" },
	{ "ra",                          "remove-attr"             },
	{ "rc",                          "remove-class"            },
};

// The canonical name a rule is asking for, or empty when nothing is.
QString canonical_name(const QString &asked) {
	for (const catalog_entry &e : k_entries) {
		if (asked == QLatin1String(e.name))
			return asked;
	}
	for (const catalog_alias &a : k_aliases) {
		if (asked == QLatin1String(a.from))
			return QString::fromLatin1(a.to);
	}
	return QString();
}

int pattern_args(const QString &canonical) {
	for (const catalog_entry &e : k_entries) {
		if (canonical == QLatin1String(e.name))
			return e.patterns;
	}
	return 0;
}

bool entry_needs_trust(const QString &canonical) {
	for (const catalog_entry &e : k_entries) {
		if (canonical == QLatin1String(e.name))
			return e.trusted;
	}
	return false;
}

int selector_args(const QString &canonical) {
	for (const catalog_entry &e : k_entries) {
		if (canonical == QLatin1String(e.name))
			return e.selectors;
	}
	return 0;
}

// **A JS string literal, escaped here rather than hoped for.** The calls are
// emitted as JSON and read back with `JSON.parse`, so every argument is a
// string at every point -- but the JSON itself has to cross into the script as
// source, and that crossing is the only place a filter list could reach the
// parser.
//
// Which characters matter, measured rather than assumed: Qt's JSON writer
// escapes `"`, `\` and the control characters, and leaves a single quote and
// U+2028/U+2029 raw. So the load-bearing cases here are the single quote,
// which would close the literal, and the two Unicode line separators, which
// are line terminators in JS however they arrived. The backslash rule is not
// redundant either -- it keeps JSON's own `\n` intact across the crossing, so
// `JSON.parse` still sees an escape rather than a literal newline.
QString js_string(const QString &text) {
	QString out = QStringLiteral("'");
	for (const QChar c : text) {
		switch (c.unicode()) {
			case '\\': out += QStringLiteral("\\\\");   break;
			case '\'': out += QStringLiteral("\\'");    break;
			case '\n': out += QStringLiteral("\\n");    break;
			case '\r': out += QStringLiteral("\\r");    break;
			case 0x2028: out += QStringLiteral("\\u2028"); break;
			case 0x2029: out += QStringLiteral("\\u2029"); break;
			default:   out += c;
		}
	}
	return out + QStringLiteral("'");
}

}  // namespace

namespace scriptlets {

bool vetted(const QString &name) {
	return !canonical_name(name).isEmpty();
}

bool requires_trust(const QString &name) {
	const QString canonical = canonical_name(name);
	return !canonical.isEmpty() && entry_needs_trust(canonical);
}

QStringList names() {
	QStringList out;
	for (const catalog_entry &e : k_entries)
		out << QString::fromLatin1(e.name);
	out.sort();
	return out;
}

bool parse_call(const QString &inside, scriptlet_call *out, QString *why) {
	const auto fail = [why](const char *text) {
		if (why)
			*why = QString::fromLatin1(text);
		return false;
	};
	// Commas separate, `\,` is a comma in an argument. Split by hand rather
	// than with `split(',')` for that one reason.
	QStringList parts;
	QString cur;
	for (int i = 0; i < inside.size(); ++i) {
		const QChar c = inside.at(i);
		if (c == QLatin1Char('\\') && i + 1 < inside.size() &&
		    inside.at(i + 1) == QLatin1Char(',')) {
			cur += QLatin1Char(',');
			++i;
		} else if (c == QLatin1Char(',')) {
			parts << cur.trimmed();
			cur.clear();
		} else {
			cur += c;
		}
	}
	parts << cur.trimmed();
	// **An argument wrapped in matching quotes means what is inside them**,
	// as uBlock reads it: a list quotes an argument that has to carry a
	// leading or trailing space or an embedded quote of the other kind. Kept
	// as written, `trusted-rpfr, '"adPlacements"', '"no_ads"'` searched YouTube's
	// player response for the quotes as well, found nothing, and the rule that
	// strips the ad payload never fired -- with nothing to say so. Stripped
	// before the pattern check below, so a quoted regex is checked as the
	// regex it is.
	for (int i = 1; i < parts.size(); ++i) {
		const QString &p = parts.at(i);
		if (p.size() < 2)
			continue;
		const QChar q = p.at(0);
		if ((q == QLatin1Char('\'') || q == QLatin1Char('"') ||
		     q == QLatin1Char('`')) && p.at(p.size() - 1) == q)
			parts[i] = p.mid(1, p.size() - 2);
	}
	if (parts.isEmpty() || parts.first().isEmpty())
		return fail("names no scriptlet");
	const QString asked = parts.takeFirst();
	const QString name = canonical_name(asked);
	if (name.isEmpty())
		return fail("names a scriptlet this build does not implement");

	// **A pattern argument is checked before the call is kept.** A `/re/` from
	// a filter list is compiled into a `RegExp` in the page, which is exactly
	// the position `site_rules` is in with a consent rule -- so it gets that
	// refusal rather than a second one written here. The measurement behind it
	// is in `site_rules.cpp`: PCRE2 auto-possessifies the worst shape to
	// nothing, so a timing probe sees zero while the page's own engine takes
	// minutes.
	const int patterned = pattern_args(name);
	for (int i = 0; i < parts.size(); ++i) {
		if (!(patterned & (1 << i)))
			continue;
		// **A leading `!` inverts the match and does not stop it being a
		// pattern.** `prevent-requestAnimationFrame` spells negation that
		// way, so `!/^(a+)+$/` would otherwise reach the page's own engine
		// unexamined -- the refusal skipped by one character.
		QString arg = parts.at(i);
		if (arg.startsWith(QLatin1Char('!')))
			arg = arg.mid(1);
		if (arg.size() < 3 || !arg.startsWith(QLatin1Char('/')))
			continue;                      // a plain substring, not a regex
		const int end = arg.lastIndexOf(QLatin1Char('/'));
		if (end <= 0)
			continue;
		const QString inner = arg.mid(1, end - 1);
		const QString refused = site_rules::why_pattern_backtracks(inner);
		if (refused.isEmpty())
			continue;
		if (why)
			*why = QString("its pattern %1").arg(refused);
		return false;
	}

	// **And a selector argument gets the refusal a container rule gets.**
	// `remove-attr, href, *` would take the address off every link on the
	// page; `site_rules` refuses the same four names for the same reason,
	// where the cost is pressing every button rather than stripping every
	// element. One list, in the one place it is written down.
	const int selected = selector_args(name);
	for (int i = 0; i < parts.size(); ++i) {
		if (!(selected & (1 << i)) || parts.at(i).isEmpty())
			continue;
		const QString broad =
		  site_rules::why_selector_too_broad(parts.at(i));
		if (broad.isEmpty())
			continue;
		if (why)
			*why = QString("its selector %1").arg(broad);
		return false;
	}

	// **One per-name refusal, and it is this catalog's rather than
	// uBlock's.** `prevent-addEventListener` with both arguments empty means
	// "every type, every handler", because an empty pattern matches anything
	// -- so `##+js(aeld)` would take every listener on the page and leave
	// something indistinguishable from a blank document. uBlock accepts it
	// and relies on its list authors; this build refuses it, for the same
	// reason a container selector is refused: a rule that cannot plausibly
	// have meant what it says is a rule to send back.
	if (name == QLatin1String("prevent-addEventListener")) {
		const bool no_type = parts.isEmpty() || parts.at(0).isEmpty();
		const bool no_handler = parts.size() < 2 || parts.at(1).isEmpty();
		if (no_type && no_handler)
			return fail("would refuse every listener on the page, naming "
			             "neither an event type nor a handler");
	}

	if (out) {
		out->name = name;
		out->args = parts;
	}
	return true;
}

QString source_for(const QList<scriptlet_call> &calls) {
	QJsonArray arr;
	for (const scriptlet_call &c : calls) {
		// Checked again here, not because `parse_call` is unreliable but
		// because this is the function that writes the script: a caller that
		// built a call some other way must not be the reason an unvetted name
		// runs. The catalog being closed has to be true at the point of use.
		if (!vetted(c.name))
			continue;
		// **The second half of the same rule.** `read` drops these when the
		// list is not trusted and counts them; this refuses them again where
		// the script is written, so a call built any other way cannot carry
		// the power either. The catalog being closed and the trusted class
		// being gated both have to hold here, not only upstream.
		if (requires_trust(c.name) && !c.trusted)
			continue;
		QJsonObject o;
		// **The canonical name, not the one it was given.** `parse_call`
		// resolves an alias, so a call that came through it already carries
		// one -- but this function writes the script, and the runner looks the
		// name up in the catalog object. A call built any other way with an
		// alias in it would be emitted, looked up, not found, and silently do
		// nothing.
		//
		// Measured: a test that built calls by hand with `aopw`, `acs`,
		// `nostif` and `window.open-defuser` had four scriptlets quietly not
		// run, which is the worst shape of failure here -- the script
		// evaluates, the page is unchanged, and nothing says why.
		o.insert(QStringLiteral("n"), canonical_name(c.name));
		// **The scope travels with the call and is matched in the page, not
		// here.** A scriptlet has to be in place before the page's own
		// scripts run, so it is injected when the view is made rather than
		// when a navigation is noticed -- by which time the document it
		// should have patched already exists. One script covers every site
		// the rules name and each frame decides whether it is one of them,
		// which is also what makes it right inside an iframe: an embedded
		// player's frame has its own hostname, and that is the one the rule
		// was written about.
		o.insert(QStringLiteral("s"), c.scope);
		QJsonArray args;
		for (const QString &a : c.args)
			args.append(a);
		o.insert(QStringLiteral("a"), args);
		arr.append(o);
	}
	if (arr.isEmpty())
		return QString();

	const QString json = QString::fromUtf8(
	  QJsonDocument(arr).toJson(QJsonDocument::Compact));
	return QStringLiteral("(function(){'use strict';")
	     + QString::fromUtf8(k_catalog)
	     + QStringLiteral("var calls;try{calls=JSON.parse(")
	     + js_string(json)
	     + QStringLiteral(");}catch(e){return;}"
	                       "var host='';try{host=String(location.hostname||'');}"
	                       "catch(e){}"
	                       // **A scope is a list, not a host.** uBlock writes
	                       // `m.youtube.com,www.youtube.com##+js(...)`, and this
	                       // compared the whole string against the hostname, so
	                       // every rule naming more than one site -- 301 of
	                       // 2444 in uBlock's own list, YouTube's `set` and
	                       // `json-prune` among them -- ran nowhere. Each entry
	                       // is an exact host or a parent of it, the test
	                       // `cosmetic_filters` applies to a selector; `~host`
	                       // excludes. A scope of exclusions only matches
	                       // nothing, since a scriptlet for every site but one
	                       // is the unscoped kind `read` already refuses. An
	                       // empty scope means every frame, and only a caller
	                       // that has already matched emits one.
	                       "var in_scope=function(sc){"
	                       "if(!sc)return true;"
	                       "var l=String(sc).split(','),hit=false;"
	                       "for(var j=0;j<l.length;j++){"
	                       "var e=l[j].replace(/^\\s+|\\s+$/g,'');"
	                       "var neg=e.charAt(0)==='~';if(neg)e=e.slice(1);"
	                       "if(!e)continue;"
	                       "var m=host===e||host.slice(-(e.length+1))==='.'+e;"
	                       "if(neg&&m)return false;if(!neg&&m)hit=true;}"
	                       "return hit;};"
	                       // **What ran is reported, because a silent runner
	                       // could not be diagnosed from inside the browser.**
	                       // The scope fault above was found only by reading a
	                       // live page over DevTools: nothing here said that
	                       // half the rules had been skipped. One line per
	                       // frame that had anything in scope, through the
	                       // console the browser already listens to, and with
	                       // `console.debug` and `JSON.stringify` captured
	                       // before any scriptlet or page script can have
	                       // replaced them. A page cannot read the console, so
	                       // this tells the browser without telling the page.
	                       "var log=null,str=null;"
	                       "try{log=console.debug;str=JSON.stringify;}catch(e){}"
	                       "var rep={h:host,r:[],e:[],k:0};"
	                       "for(var i=0;i<calls.length;i++){"
	                       "if(!in_scope(calls[i].s)){rep.k++;continue;}"
	                       "var f=C[calls[i].n];"
	                       "if(!f){rep.e.push(calls[i].n+': not in this build');"
	                       "continue;}"
	                       // One failing scriptlet must not take the others
	                       // with it: they are independent patches and a page
	                       // that defeats one says nothing about the next.
	                       "try{f.apply(null,calls[i].a);rep.r.push(calls[i].n);}"
	                       "catch(e){var m='';try{m=String(e&&e.message||e);}"
	                       "catch(x){}rep.e.push(calls[i].n+': '+m);}}"
	                       "if((rep.r.length||rep.e.length)&&log&&str){"
	                       "try{log.call(console,'hydra-scriptlets '+str(rep));}"
	                       "catch(e){}}"
	                       "})();");
}

QString report_prefix() {
	return QStringLiteral("hydra-scriptlets ");
}

bool parse_report(const QString &line, report *out) {
	const QString prefix = report_prefix();
	if (!line.startsWith(prefix))
		return false;
	const QJsonDocument doc =
	  QJsonDocument::fromJson(line.mid(prefix.size()).toUtf8());
	if (!doc.isObject())
		return false;
	const QJsonObject o = doc.object();
	report r;
	r.host    = o.value(QStringLiteral("h")).toString();
	r.skipped = o.value(QStringLiteral("k")).toInt();
	for (const QJsonValue &v : o.value(QStringLiteral("r")).toArray())
		r.ran << v.toString();
	for (const QJsonValue &v : o.value(QStringLiteral("e")).toArray())
		r.failed << v.toString();
	if (out)
		*out = r;
	return true;
}

QString describe(const report &r) {
	// Grouped rather than listed: YouTube alone runs `set-constant` four
	// times, and a line that repeats a name says less than one that counts it.
	QStringList order;
	QHash<QString, int> times;
	for (const QString &n : r.ran) {
		if (!times.contains(n))
			order << n;
		++times[n];
	}
	QStringList names;
	for (const QString &n : order)
		names << (times.value(n) > 1
		            ? QString("%1 x%2").arg(n).arg(times.value(n)) : n);
	QString line = QString("%1: %2 ran").arg(r.host).arg(r.ran.size());
	if (!names.isEmpty())
		line += " (" + names.join(", ") + ")";
	line += QString(", %1 failed").arg(r.failed.size());
	if (!r.failed.isEmpty())
		line += " (" + r.failed.join("; ") + ")";
	line += QString(", %1 for other sites").arg(r.skipped);
	return line;
}

}  // namespace scriptlets

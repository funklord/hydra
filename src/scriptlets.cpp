#include "scriptlets.h"

#include "site_rules.h"

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

	var parts = String(path).split('.');
	var last = parts.pop();
	var owner = window;
	for (var i = 0; i < parts.length; i++) {
		if (owner[parts[i]] === undefined || owner[parts[i]] === null)
			owner[parts[i]] = {};
		owner = owner[parts[i]];
		if (typeof owner !== 'object' && typeof owner !== 'function') return;
	}
	try {
		Object.defineProperty(owner, last, {
			get: function () { return value; },
			set: function () {},
			configurable: false
		});
	} catch (e) { /* already non-configurable: the page wins, and says so */ }
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
				childList: true, subtree: true, attributes: kind === 'attr'
			});
		} catch (e) { return; }
		if (!stay && typeof window.setTimeout === 'function')
			window.setTimeout(function () { obs.disconnect(); }, 10000);
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
	var at = owner_of(path);
	if (!at) return;
	try {
		Object.defineProperty(at.o, at.k, {
			get: function () { return v; },
			set: function () {},
			configurable: false
		});
	} catch (e) {}
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
	var bits = encodeURIComponent(String(name)) + '=' +
	            encodeURIComponent(String(value == null ? '' : value));
	var n = Number(days);
	if (isFinite(n) && n > 0) {
		var until = new Date();
		until.setTime(until.getTime() + n * 86400000);
		try { bits += '; expires=' + until.toUTCString(); } catch (e) {}
	}
	var where = String(path == null ? '' : path);
	// A path is the one part that cannot be percent-encoded and still mean
	// what it says, so it is checked instead: a path is a path.
	bits += '; path=' + (/^\/[A-Za-z0-9._~\-\/]*$/.test(where) ? where : '/');
	try { document.cookie = bits; } catch (e) {}
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
		const QString arg = parts.at(i);
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
	                       "for(var i=0;i<calls.length;i++){"
	                       "var sc=calls[i].s;"
	                       // An exact host or a subdomain of it, which is the
	                       // same test `cosmetic_filters` applies to a scoped
	                       // selector. An empty scope means every frame, and
	                       // only a caller that has already matched emits one.
	                       "if(sc&&host!==sc&&"
	                       "host.slice(-(sc.length+1))!=='.'+sc)continue;"
	                       "var f=C[calls[i].n];if(!f)continue;"
	                       // One failing scriptlet must not take the others
	                       // with it: they are independent patches and a page
	                       // that defeats one says nothing about the next.
	                       "try{f.apply(null,calls[i].a);}catch(e){}}"
	                       "})();");
}

}  // namespace scriptlets

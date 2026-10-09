// Scriptlets: the rule kind that can reach an ad served from the content's
// own host. Part of the filter pipeline (architecture doc sec 12), which
// does not describe this capability -- see `scriptlets.h`.
//
// **These cases run the generated script in a real JavaScript engine.** The
// property that matters is not that the text looks right; it is that a filter
// list cannot put code into it and that the patch does what it says. QJSEngine
// answers both: a syntax error fails the evaluation, and a canary global
// answers whether a hostile argument executed.
#include "scriptlets.h"

#include <QCoreApplication>
#include <QJSEngine>
#include <QJSValue>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// An engine with just enough page in it for a scriptlet to patch: the catalog
// reaches for `window`, which a page has and a bare engine does not.
//
// **The fuller half is for the rest of the catalog**, which wraps things a
// page provides -- timers, fetch, window.open, storage, a peer connection.
// Each stub records what it was asked, so a test can tell "dropped" from
// "passed through" rather than only observing that nothing exploded.
static const char *k_page_stubs = R"JS(
window.__ran = [];
window.__fetched = [];
window.__opened = [];
window.__store = {};
window.setTimeout = function (fn, ms) {
	window.__ran.push(['t', String(ms)]);
	return 1;
};
window.setInterval = function (fn, ms) {
	window.__ran.push(['i', String(ms)]);
	return 2;
};
window.fetch = function (u) {
	window.__fetched.push(String(u));
	return { real: true };
};
window.open = function (u) {
	window.__opened.push(String(u));
	return { real: true };
};
window.localStorage = {
	setItem: function (k, v) { window.__store[k] = String(v); },
	removeItem: function (k) { delete window.__store[k]; }
};
window.RTCPeerConnection = function () { this.real = true; };
// An EventTarget with a prototype, because that is where the defuser
// patches: a listener added to an element has to be reachable too, and
// stubbing `window.addEventListener` alone would make a test pass for a
// scriptlet that only covered the window.
// eval, rAF and the timers record what they were given, so a test can tell
// "prevented" from "passed through" rather than only that nothing exploded.
window.__evalled = [];
window.eval = function (code) { window.__evalled.push(String(code)); return 'real'; };
window.__framed = [];
window.requestAnimationFrame = function (fn) {
	window.__framed.push(typeof fn === 'function' ? String(fn) : String(fn));
	window.__frame_fn = fn;
	return 7;
};
window.__stopped = 0;
window.stop = function () { window.__stopped++; };
window.__intervals = [];
window.clearInterval = function (id) { window.__cleared = id; };
window.__listened = [];
window.EventTarget = function () {};
window.EventTarget.prototype.addEventListener = function (t, h) {
	window.__listened.push(String(t));
};
window.__elem = Object.create(window.EventTarget.prototype);
window.Promise = { resolve: function (v) { return { then: function (f) {
	f(v); return this; } }; } };
// `document.cookie` as an accessor that records, because a write is the whole
// observable effect of one scriptlet here and a plain property would swallow
// the second write and every attribute.
window.__cookies = [];
Object.defineProperty(document, 'cookie', {
	set: function (v) { window.__cookies.push(String(v)); },
	get: function () { return window.__cookies.join('; '); },
	configurable: true
});
document.currentScript = null;
document.documentElement = { tag: 'html' };
document.addEventListener = function (n, f) { window.__domReady = f; };
// A page with three elements in it, and a record of what was asked for.
window.__sel = null;
window.__gone = [];
var mkel = function (name) {
	return {
		name: name,
		removeAttribute: function (a) { window.__gone.push(name + ':attr:' + a); },
		classList: {
			remove: function (c) { window.__gone.push(name + ':class:' + c); }
		}
	};
};
window.__nodes = [mkel('one'), mkel('two')];
document.querySelectorAll = function (sel) {
	window.__sel = String(sel);
	if (String(sel).indexOf('!broken') >= 0) throw new Error('bad selector');
	return window.__nodes;
};
window.__observers = [];
window.MutationObserver = function (cb) {
	this.cb = cb;
	this.live = true;
	this.observe = function () { window.__observers.push(this); };
	this.disconnect = function () { this.live = false; };
};
)JS";

static void give_window(QJSEngine *eng, const char *host = "x.test") {
	eng->globalObject().setProperty("window", eng->newObject());
	// **A hostname, because the generated script matches the scope in the
	// page.** The scope travels with the call and each frame decides whether
	// it is one the rules name, so an engine with no `location` is a frame
	// that matches nothing -- which would make every case below pass for the
	// wrong reason.
	QJSValue loc = eng->newObject();
	loc.setProperty("hostname", QString::fromLatin1(host));
	eng->globalObject().setProperty("location", loc);
	eng->globalObject().setProperty("document", eng->newObject());
}

// The same, plus the page facilities the rest of the catalog wraps.
static void give_page(QJSEngine *eng, const char *host = "x.test") {
	give_window(eng, host);
	eng->evaluate(QString::fromLatin1(k_page_stubs));
}

// **A fetch worth pruning, which the simpler stub is not.** The one above
// answers with a marker so a test can see whether a request went through;
// this one answers with a body, carries a status, and refuses a second read --
// because a Response body can only be read once, and that is the constraint
// the scriptlet's shape follows from.
//
// The promise flattens, like a real one: the code under test returns the inner
// promise from the outer `then`, so a stub that nested them would make the
// test read a promise where the page reads a response.
static const char *k_fetch_stubs = R"JS(
window.P = function (v) {
	if (v && typeof v.then === 'function') return v;
	return { then: function (f) { return window.P(f(v)); } };
};
window.Promise = { resolve: function (v) { return window.P(v); } };
window.__body = '{"adPlacements":[1,2],"streamingData":{"ok":1}}';
window.__reads = 0;
window.fetch = function (u) {
	window.__fetched.push(String(u));
	var res = {
		ok: true, status: 207, statusText: 'Odd', url: String(u),
		headers: { h: 1 }, __read: false, __original: true,
		text: function () {
			if (this.__read) throw new Error('body already read');
			this.__read = true;
			window.__reads++;
			return window.P(window.__body);
		}
	};
	return window.P(res);
};
// An XMLHttpRequest with the shape that matters: `responseText` and
// `response` are accessors on the PROTOTYPE, because that is where the
// scriptlet captures the real ones from, and a stub with plain data
// properties would let a broken implementation pass.
window.__sent = [];
var X = function () {
	this.readyState = 0;
	this.responseType = '';
	this.__l = [];
	this.__raw = '';
};
Object.defineProperty(X.prototype, 'responseText', {
	get: function () { return this.__raw; }, configurable: true
});
Object.defineProperty(X.prototype, 'response', {
	get: function () {
		if (this.responseType === 'json') {
			try { return JSON.parse(this.__raw); } catch (e) { return null; }
		}
		if (this.responseType === 'arraybuffer')
			return { bytes: String(this.__raw).length };
		return this.__raw;
	}, configurable: true
});
X.prototype.open = function (m, u) { this.__m = m; this.__u = u; };
X.prototype.send = function () { window.__sent.push(String(this.__u)); };
X.prototype.addEventListener = function (n, f) { this.__l.push([n, f]); };
window.XMLHttpRequest = X;
// Complete a request: set the body, the state, and fire the handlers in the
// order they were added -- which is the ordering the whole case turns on.
window.finish = function (x, raw, state) {
	x.__raw = raw;
	x.readyState = (state === undefined) ? 4 : state;
	for (var i = 0; i < x.__l.length; i++)
		if (x.__l[i][0] === 'load') x.__l[i][1]({ target: x });
};

// Read a response the way a player would, and report what it saw.
window.ask = function (url) {
	var seen = null;
	window.fetch(url).then(function (res) {
		if (!res || typeof res.text !== 'function') { seen = 'no text'; return; }
		res.text().then(function (t) { seen = t; });
	});
	return seen;
};
)JS";

// **A document with enough shape to walk**, for the families that rewrite
// nodes rather than globals. A TreeWalker over a flat list is faithful enough
// for what the code under test does -- it asks for the next node and reads
// `nodeName` and `textContent` -- and `window.URL`, `document.querySelector`
// and the load/click plumbing are here for the same reason.
//
// Nothing here pretends to be a browser. What it has to be is a fixture the
// code can reach the hazard through, which the engine's own bare globals are
// not.
static const char *k_dom_stubs = R"JS(
window.__nodes_seen = [];
var mknode = function (name, text) {
	return { nodeName: name, textContent: text, content: null };
};
window.__tree = [];
document.documentElement = { tag: 'html' };
document.currentScript = null;
window.NodeFilter = { SHOW_ELEMENT: 1, SHOW_TEXT: 4 };
document.createTreeWalker = function (root, what) {
	var i = -1;
	return { nextNode: function () {
		i += 1;
		return i < window.__tree.length ? window.__tree[i] : null;
	} };
};
// Links, for href-sanitizer.
window.__links = [];
window.__queried = null;
document.querySelectorAll = function (sel) {
	window.__queried = String(sel);
	if (String(sel).indexOf('!broken') >= 0) throw new Error('bad selector');
	return window.__links;
};
window.__meta = null;
document.querySelector = function (sel) { return window.__meta; };
window.URL = function (text, base) {
	var t = String(text);
	if (/^https?:\/\//.test(t)) {
		this.href = t;
		this.protocol = t.slice(0, t.indexOf(':') + 1);
	} else if (/^[a-z]+:/.test(t)) {
		this.href = t;
		this.protocol = t.slice(0, t.indexOf(':') + 1);
	} else {
		this.href = 'https://x.test/' + t.replace(/^\//, '');
		this.protocol = 'https:';
	}
	this.hostname = 'x.test';
	this.searchParams = { get: function () { return null; } };
};
document.location = 'https://x.test/page';
// The window load/click plumbing the last two need.
window.__onload = null;
window.addEventListener = function (name, fn) {
	if (name === 'load') window.__onload = fn;
};
window.__onclick = null;
document.addEventListener = function (name, fn) {
	if (name === 'click') window.__onclick = fn;
	if (name === 'DOMContentLoaded') window.__onready = fn;
};
document.body = { style: { removeProperty: function () {} } };
document.getElementById = function () { return null; };
)JS";

// Run one call and hand back the engine's own answer to an expression.
static QString ask(QJSEngine *eng, const char *expr) {
	const QJSValue v = eng->evaluate(QString::fromLatin1(expr));
	return v.isError() ? QStringLiteral("!") + v.toString() : v.toString();
}

static QString run_one(QJSEngine *eng, const char *name,
                        const QStringList &args, bool trusted = false) {
	scriptlet_call c;
	c.scope   = "x.test";
	c.name    = QString::fromLatin1(name);
	c.args    = args;
	c.trusted = trusted;
	const QJSValue r = eng->evaluate(scriptlets::source_for({ c }));
	return r.isError() ? r.toString() : QString();
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("the catalog is closed, and names outside it are refused");
	{
		check(scriptlets::vetted("json-prune") &&
		          scriptlets::vetted("set-constant"),
		       QString("the implemented names are vetted (%1)")
		           .arg(scriptlets::names().join(", ")));
		// **The whole security position in one line.** A rule names a
		// scriptlet; it does not carry one. Anything this build has not
		// implemented and reviewed does not run.
		// **`trusted-click-element` is the deliberate absence now**, and the
		// reason is in project.md: `consent_blocker` already clicks a banner's
		// buttons from rules written here and reviewed here, so a scriptlet
		// that clicks whatever a selector names would be a second and less
		// guarded path to the same action. The trusted names that ARE in the
		// catalog were absent for want of anything saying which lists are
		// trusted, which the settings page now does.
		// **`aost` has since been implemented**, so it moved out of this list
		// -- which is the list noticing a capability arrive rather than a
		// regression, as `nowebrtc` and `trusted-set-cookie` did before it.
		for (const char *no : { "trusted-click-element", "trusted-prune-fetch",
		                         "trusted-prune-inbound-object",
		                         "json-edit", "eval", "" }) {
			check(!scriptlets::vetted(QString::fromLatin1(no)),
			       QString("\"%1\" is not in the catalog")
			           .arg(QString::fromLatin1(no)));
		}

		scriptlet_call c;
		QString why;
		check(scriptlets::parse_call("json-prune, adPlacements", &c, &why),
		       "a call to an implemented scriptlet parses");
		check(c.name == "json-prune" && c.args == QStringList{ "adPlacements" },
		       QString("with its argument (%1 / %2)")
		           .arg(c.name, c.args.join("|")));
		check(!scriptlets::parse_call("trusted-click-element, a, b", &c, &why),
		       QString("one to an unimplemented scriptlet does not (%1)")
		           .arg(why));
		check(!scriptlets::parse_call("", &c, &why),
		       QString("nor does an empty call (%1)").arg(why));
		// An escaped comma belongs to the argument, which is how a list says
		// "this value has a comma in it".
		check(scriptlets::parse_call("json-prune, a\\,b, needle", &c, &why) &&
		          c.args.size() == 2 && c.args.first() == "a,b",
		       QString("an escaped comma stays in the argument (%1)")
		           .arg(c.args.join("|")));
	}

	section("json-prune removes the property a player would have read");
	{
		// The shape this exists for: a player asks its backend for a
		// configuration document and reads an ad array out of it.
		QList<scriptlet_call> calls;
		calls.push_back({ "x.test", "json-prune",
		                   { "adPlacements playerAds" } });
		const QString src = scriptlets::source_for(calls);
		check(!src.isEmpty(), "a script is generated");
		// **The composed form is a self-invoking function**, which is the
		// property `test_scripts` asserts of every injected script in the
		// tree and cannot assert of the catalog: that raw string is a
		// fragment, defining the functions the wrapper then calls, and is
		// never injected on its own. Asserted here, where the composition
		// happens.
		check(src.startsWith("(function(){") && src.endsWith("})();"),
		       QString("wrapped as a self-invoking function (%1 ... %2)")
		           .arg(src.left(12), src.right(5)));

		QJSEngine eng;
		give_window(&eng);
		const QJSValue loaded = eng.evaluate(src);
		check(!loaded.isError(),
		       QString("and it evaluates (%1)")
		           .arg(loaded.isError() ? loaded.toString() : QString("no "
		                                                                "error")));
		const QJSValue after = eng.evaluate(
		  "JSON.stringify(JSON.parse('{\"adPlacements\":[1,2],"
		  "\"playerAds\":{},\"streamingData\":{\"ok\":1}}'))");
		check(!after.isError(), "the page can still parse JSON");
		check(!after.toString().contains("adPlacements") &&
		          !after.toString().contains("playerAds"),
		       QString("both named properties are gone (%1)")
		           .arg(after.toString()));
		check(after.toString().contains("streamingData"),
		       QString("and the rest of the document is untouched (%1)")
		           .arg(after.toString()));
	}

	section("a needle keeps it from pruning every document on the page");
	{
		// Without this a rule aimed at one response prunes anything that
		// happens to share a property name, which is how a scriptlet breaks
		// the page it was meant to fix.
		QList<scriptlet_call> calls;
		calls.push_back({ "x.test", "json-prune", { "ads", "player.id" } });
		QJSEngine eng;
		give_window(&eng);
		check(!eng.evaluate(scriptlets::source_for(calls)).isError(),
		       "the script evaluates");
		const QJSValue matched = eng.evaluate(
		  "JSON.stringify(JSON.parse('{\"ads\":1,\"player\":{\"id\":7}}'))");
		check(!matched.toString().contains("ads"),
		       QString("a document carrying the needle is pruned (%1)")
		           .arg(matched.toString()));
		const QJSValue other = eng.evaluate(
		  "JSON.stringify(JSON.parse('{\"ads\":1,\"other\":2}'))");
		check(other.toString().contains("\"ads\""),
		       QString("one without it is left alone (%1)")
		           .arg(other.toString()));
	}

	section("set-constant pins a flag, and only to a value from its vocabulary");
	{
		// Built field by field rather than brace-initialised across two
		// lines: the style gate reads a continuation at this depth as a
		// nesting level, and three plain assignments say the same thing
		// without arguing with it.
		scriptlet_call off;
		off.scope = "x.test";
		off.name  = "set-constant";
		off.args  = { "cfg.adsEnabled", "false" };
		const QList<scriptlet_call> calls{ off };
		QJSEngine eng;
		give_window(&eng);
		check(!eng.evaluate(scriptlets::source_for(calls)).isError(),
		       "the script evaluates");
		check(eng.evaluate("window.cfg.adsEnabled").toBool() == false,
		       "the flag reads false");
		// The page cannot write it back, which is the point of a constant.
		eng.evaluate("try { window.cfg.adsEnabled = true; } catch (e) {}");
		check(eng.evaluate("window.cfg.adsEnabled").toBool() == false,
		       "and stays false after the page assigns to it");

		// **A value outside the vocabulary is refused rather than set.** An
		// arbitrary string would be the one place a list could put content of
		// its own choosing into a page global.
		QJSEngine eng2;
		give_window(&eng2);
		scriptlet_call strange;
		strange.scope = "x.test";
		strange.name  = "set-constant";
		strange.args  = { "cfg.name", "something arbitrary" };
		const QList<scriptlet_call> odd{ strange };
		check(!eng2.evaluate(scriptlets::source_for(odd)).isError(),
		       "a call with a value outside the vocabulary still evaluates");
		check(eng2.evaluate("typeof window.cfg").toString() == "undefined" ||
		          eng2.evaluate("window.cfg.name").isUndefined(),
		       QString("and sets nothing (%1)")
		           .arg(eng2.evaluate("String(window.cfg && window.cfg.name)")
		                    .toString()));
	}

	section("an argument cannot become code, however it is spelled");
	{
		// **The one mistake that would matter.** Arguments arrive from a
		// filter list, so interpolating one into the script text would make a
		// list rule a way to run code. They are emitted as JSON and read with
		// JSON.parse; these are the spellings that would escape a naive
		// emitter, and the canary is what says whether any of them ran.
		const QStringList hostile = {
			QStringLiteral("1'); window.PWNED=1; ('"),
			QStringLiteral("1\"); window.PWNED=1; (\""),
			QStringLiteral("1\\'); window.PWNED=1; ('"),
			QStringLiteral("1\n window.PWNED=1; \n"),
			QStringLiteral("1</script><script>window.PWNED=1;"),
			QString::fromUtf8("1\xe2\x80\xa8 window.PWNED=1;"),   // U+2028
			// **The one that runs BEFORE the parse, which the others do
			// not.** Measured: with the escaping removed, the first few
			// payloads land inside the runner's own `try` after a
			// `JSON.parse` that throws on the broken string, so they never
			// execute and the case passes for a reason that is nothing to do
			// with the escaping. This one is a concatenation evaluated while
			// the argument to JSON.parse is still being built, so it runs
			// first or not at all -- which is what makes the sabotage
			// discriminate.
			QStringLiteral("x'+(window.PWNED=1)+'y"),
		};
		for (int i = 0; i < hostile.size(); ++i) {
			QList<scriptlet_call> calls;
			calls.push_back({ "x.test", "set-constant",
			                   { "cfg.flag", hostile.at(i) } });
			const QString src = scriptlets::source_for(calls);
			QJSEngine eng;
			give_window(&eng);
			const QJSValue r = eng.evaluate(src);
			check(!r.isError(),
			       QString("hostile argument %1 leaves valid script (%2)")
			           .arg(i).arg(r.isError() ? r.toString()
			                                    : QString("no error")));
			check(eng.evaluate("typeof window.PWNED").toString() == "undefined",
			       QString("and does not run (%1)").arg(i));
		}
	}

	section("an unvetted name reaches no script even if a caller builds one");
	{
		// `parse_call` refuses it, and `source_for` refuses it again: the
		// catalog being closed has to hold at the point the script is
		// written, not only where rules are read.
		QList<scriptlet_call> calls;
		calls.push_back({ "x.test", "eval-this", { "window.PWNED=1" } });
		check(scriptlets::source_for(calls).isEmpty(),
		       "a call nothing implements generates no script at all");
		calls.push_back({ "x.test", "set-constant", { "cfg.ok", "true" } });
		const QString src = scriptlets::source_for(calls);
		QJSEngine eng;
		give_window(&eng);
		check(!eng.evaluate(src).isError(), "a mixed list still evaluates");
		check(eng.evaluate("window.cfg.ok").toBool(),
		       "the vetted call runs");
		check(eng.evaluate("typeof window.PWNED").toString() == "undefined",
		       "and the unvetted one is not in the script");
	}

	section("a scriptlet runs only on the site its rule named");
	{
		// The scope is matched in the page, so this is the case that says the
		// matching happens at all -- and the one that would otherwise let a
		// rule written for one site patch every page a person visits.
		QList<scriptlet_call> calls;
		calls.push_back({ "player.test", "set-constant", { "cfg.ok", "true" } });
		const QString src = scriptlets::source_for(calls);

		QJSEngine on_site;
		give_window(&on_site, "player.test");
		check(!on_site.evaluate(src).isError(), "the script evaluates on the "
		                                         "named site");
		check(on_site.evaluate("window.cfg.ok").toBool(),
		       "and the scriptlet runs there");

		QJSEngine sub;
		give_window(&sub, "www.player.test");
		check(!sub.evaluate(src).isError() &&
		          sub.evaluate("window.cfg.ok").toBool(),
		       "a subdomain counts, as it does for a scoped selector");

		QJSEngine elsewhere;
		give_window(&elsewhere, "other.test");
		check(!elsewhere.evaluate(src).isError(),
		       "the script evaluates elsewhere too");
		check(elsewhere.evaluate("typeof window.cfg").toString() == "undefined",
		       QString("and does nothing there (%1)")
		           .arg(elsewhere.evaluate("String(typeof window.cfg)")
		                    .toString()));
		// The near-miss that a suffix test gets wrong: a host ENDING in the
		// scope's text without the dot is a different site.
		QJSEngine lookalike;
		give_window(&lookalike, "notplayer.test");
		check(!lookalike.evaluate(src).isError() &&
		          lookalike.evaluate("typeof window.cfg").toString() ==
		              "undefined",
		       "and nothing on a host that merely ends with those letters");
	}

	section("the names rules actually use, and the canonical one kept");
	{
		// A rule calls a scriptlet by whichever spelling its author knew, so
		// refusing an alias would refuse the rule rather than the capability.
		struct pair { const char *asked; const char *canonical; };
		const QList<pair> aliases = {
			{ "aopr",  "abort-on-property-read"  },
			{ "aopw",  "abort-on-property-write" },
			{ "acs",   "abort-current-script"    },
			{ "acis",  "abort-current-script"    },
			{ "abort-current-inline-script", "abort-current-script" },
			{ "nostif", "prevent-setTimeout"     },
			{ "no-setTimeout-if", "prevent-setTimeout" },
			{ "nosiif", "prevent-setInterval"    },
			{ "prevent-fetch", "no-fetch-if"     },
			{ "window.open-defuser", "prevent-window-open" },
			{ "nowoif", "prevent-window-open"    },
		};
		for (const pair &p : aliases) {
			scriptlet_call c;
			QString why;
			const bool ok = scriptlets::parse_call(
			  QString::fromLatin1(p.asked) + ", x", &c, &why);
			check(ok && c.name == QString::fromLatin1(p.canonical),
			       QString("%1 is %2 (%3)")
			           .arg(QString::fromLatin1(p.asked),
			                 QString::fromLatin1(p.canonical),
			                 ok ? c.name : why));
		}
		// **The five aliases read out of uBlock's own source**, not recalled:
		// `set-constant.js` declares `set.js`, and the rest were taken the
		// same way on 2026-10-08. `set` alone is 302 of the 2453 scriptlet
		// rules in uBlock's `filters.txt`, and it was missing because
		// nothing here had ever read a real list.
		const QList<pair> from_ublock = {
			{ "set",                 "set-constant"                   },
			{ "trusted-set",         "trusted-set-constant"           },
			{ "trusted-rpfr",        "trusted-replace-fetch-response" },
			{ "setTimeout-defuser",  "prevent-setTimeout"             },
			{ "setInterval-defuser", "prevent-setInterval"            },
		};
		for (const pair &p : from_ublock) {
			scriptlet_call c;
			QString why;
			const bool ok = scriptlets::parse_call(
			  QString::fromLatin1(p.asked) + ", cfg.x, false", &c, &why);
			check(ok && c.name == QString::fromLatin1(p.canonical),
			       QString("%1 is %2 (%3)")
			           .arg(QString::fromLatin1(p.asked),
			                 QString::fromLatin1(p.canonical),
			                 ok ? c.name : why));
		}

		// The count lives here, once. It moves when the catalog does, which
		// is the point: a name added without a test is an entry nothing ran.
		check(scriptlets::names().size() == 41,
		       QString("forty-one scriptlets in the catalog (%1)")
		           .arg(scriptlets::names().size()));
		// The trusted four, named here rather than counted: the question a
		// reader has is which scriptlets can act on a page's behalf, and a
		// number does not answer it.
		for (const char *t : { "trusted-set-constant",
		                        "trusted-set-local-storage-item",
		                        "trusted-set-cookie",
		                        "trusted-replace-fetch-response" }) {
			const QString name = QString::fromLatin1(t);
			check(scriptlets::vetted(name) && scriptlets::requires_trust(name),
			       QString("%1 is in the catalog and needs trust").arg(name));
		}
		// **Asked of the catalog entry rather than of the spelling.** A name
		// cannot acquire the power by looking like one of those, and -- the
		// half that would actually hurt -- an ordinary scriptlet cannot lose
		// its ability to run by being renamed.
		for (const char *u : { "json-prune", "set-constant", "remove-attr",
		                        "json-prune-fetch-response" }) {
			check(!scriptlets::requires_trust(QString::fromLatin1(u)),
			       QString("%1 does not").arg(QString::fromLatin1(u)));
		}
	}

	section("a pattern argument that backtracks is refused, by the shared rule");
	{
		// **The same refusal `site_rules` applies to a consent rule**, because
		// the position is the same: a `/re/` from a filter list is compiled
		// into a `RegExp` in the page. Reusing it rather than writing a second
		// one is the point; the measurement behind it is in site_rules.cpp.
		scriptlet_call c;
		QString why;
		check(!scriptlets::parse_call("nostif, /^(a+)+$/", &c, &why),
		       QString("a quantified group is refused (%1)").arg(why));
		check(why.contains("pattern"),
		       QString("and said to be the pattern's fault (%1)").arg(why));
		check(scriptlets::parse_call("nostif, /ads?\\.js/", &c, &why),
		       QString("while an ordinary pattern is kept (%1)").arg(why));
		check(scriptlets::parse_call("nostif, adsbygoogle", &c, &why),
		       "as is a plain substring");
		// The check is aimed at the argument that is a pattern, not at every
		// argument: `set-constant`'s value is a word, not something matched.
		check(scriptlets::parse_call("set-constant, a.b, /^(a+)+$/", &c, &why),
		       "and an argument that is not a pattern is not checked as one");
	}

	section("aborting on a property, read and written");
	{
		QJSEngine eng;
		give_page(&eng);
		check(run_one(&eng, "abort-on-property-read", { "ads.enabled" })
		          .isEmpty(), "aopr evaluates");
		check(ask(&eng, "(function(){try{var x=window.ads.enabled;return 'read';}"
		                 "catch(e){return 'threw';}})()") == "threw",
		       "reading it throws");
		check(ask(&eng, "typeof window.ads") == "object",
		       "and the path to it was built");

		QJSEngine eng2;
		give_page(&eng2);
		check(run_one(&eng2, "aopw", { "detector.installed" }).isEmpty(),
		       "aopw evaluates, by its alias");
		check(ask(&eng2, "(function(){try{var x=window.detector.installed;"
		                  "return 'read';}catch(e){return 'threw:'+e;}})()")
		          == "read",
		       QString("reading is allowed (%1 / typeof detector=%2)")
		           .arg(ask(&eng2, "(function(){try{var x="
		                            "window.detector.installed;return 'read';}"
		                            "catch(e){return 'threw:'+e;}})()"),
		                 ask(&eng2, "typeof window.detector")));
		check(ask(&eng2, "(function(){try{window.detector.installed=1;"
		                  "return 'wrote';}catch(e){return 'threw';}})()")
		          == "threw",
		       "and writing throws");
	}

	section("aborting only the script that matches");
	{
		// **Narrower than aborting every reader**, which is the point: the
		// page's own code reads the same globals.
		QJSEngine eng;
		give_page(&eng);
		check(run_one(&eng, "acs", { "cfg.token", "adsbygoogle" }).isEmpty(),
		       "acs evaluates");
		eng.evaluate("window.cfg = window.cfg || {}; ");
		eng.evaluate("document.currentScript = "
		              "{ textContent: 'var x = adsbygoogle.push(1);' };");
		check(ask(&eng, "(function(){try{var x=window.cfg.token;return 'read';}"
		                 "catch(e){return 'threw';}})()") == "threw",
		       QString("a script whose text matches is stopped (%1, cfg=%2)")
		           .arg(ask(&eng, "(function(){try{var x=window.cfg.token;"
		                           "return 'read';}catch(e){return 'threw:'+e;}"
		                           "})()"),
		                 ask(&eng, "typeof window.cfg")));
		eng.evaluate("document.currentScript = "
		              "{ textContent: 'var y = player.start();' };");
		check(ask(&eng, "(function(){try{var x=window.cfg.token;return 'read';}"
		                 "catch(e){return 'threw';}})()") == "read",
		       "and one that does not is left alone");
		eng.evaluate("document.currentScript = null;");
		check(ask(&eng, "(function(){try{var x=window.cfg.token;return 'read';}"
		                 "catch(e){return 'threw';}})()") == "read",
		       "as is a read from no script at all");
	}

	section("dropping a timer by what its callback says");
	{
		QJSEngine eng;
		give_page(&eng);
		check(run_one(&eng, "nostif", { "showAd" }).isEmpty(),
		       "nostif evaluates");
		// **Named functions, because this engine does not hand over a body.**
		// `String(fn)` is `function showAd() { [native code] }` here, where a
		// browser gives the source -- so the needle is matched against the
		// name in this test and against the whole source in a page. The
		// matcher is the same either way; what differs is how much text it is
		// shown, which is the engine's doing and not the scriptlet's.
		eng.evaluate("window.setTimeout(function showAd(){ 1; }, 500);");
		eng.evaluate("window.setTimeout(function playVideo(){ 1; }, 500);");
		check(ask(&eng, "window.__ran.length") == "1",
		       QString("the matching callback never reached the real timer "
		                "(%1)").arg(ask(&eng, "window.__ran.length")));
		check(ask(&eng, "String(window.__ran[0][1])") == "500",
		       "and the other one did");

		// With a delay named, only that delay is dropped.
		QJSEngine eng2;
		give_page(&eng2);
		check(run_one(&eng2, "prevent-setInterval", { "beacon", "1000" })
		          .isEmpty(), "prevent-setInterval with a delay evaluates");
		eng2.evaluate("window.setInterval(function beacon(){ 1; }, 1000);");
		eng2.evaluate("window.setInterval(function beacon(){ 1; }, 250);");
		check(ask(&eng2, "window.__ran.length") == "1",
		       QString("only the one at that delay is dropped (%1)")
		           .arg(ask(&eng2, "window.__ran.length")));
	}

	section("answering a fetch instead of sending it");
	{
		QJSEngine eng;
		give_page(&eng);
		check(run_one(&eng, "no-fetch-if", { "/ads/" }).isEmpty(),
		       "no-fetch-if evaluates");
		eng.evaluate("window.__r = null;"
		              "window.fetch('https://x.test/ads/beacon')"
		              ".then(function(v){ window.__r = v; });");
		check(ask(&eng, "window.__fetched.length") == "0",
		       QString("the matching request was not sent (%1)")
		           .arg(ask(&eng, "window.__fetched.length")));
		check(ask(&eng, "String(window.__r && window.__r.real)") != "true",
		       "and what came back is not the real answer");
		eng.evaluate("window.fetch('https://x.test/player.json');");
		check(ask(&eng, "window.__fetched.length") == "1",
		       "while anything else goes through");
	}

	section("the remaining three, each in one line of evidence");
	{
		QJSEngine eng;
		give_page(&eng);
		check(run_one(&eng, "nowebrtc", {}).isEmpty(), "nowebrtc evaluates");
		check(ask(&eng, "String(new window.RTCPeerConnection().real)")
		          != "true",
		       "a peer connection is a stub");

		QJSEngine eng2;
		give_page(&eng2);
		check(run_one(&eng2, "window.open-defuser", { "popunder" }).isEmpty(),
		       "prevent-window-open evaluates, by its alias");
		eng2.evaluate("window.__a = window.open('https://x.test/popunder/1');");
		check(ask(&eng2, "window.__opened.length") == "0" &&
		          ask(&eng2, "String(window.__a.closed)") == "false",
		       QString("a matching popup is refused and a stub handed back "
		                "(opened=%1 a=%2)")
		           .arg(ask(&eng2, "window.__opened.length"),
		                 ask(&eng2, "String(window.__a && window.__a.closed)")));
		eng2.evaluate("window.open('https://x.test/help');");
		check(ask(&eng2, "window.__opened.length") == "1",
		       "and anything else opens");

		QJSEngine eng3;
		give_page(&eng3);
		check(run_one(&eng3, "set-local-storage-item", { "adsOff", "true" })
		          .isEmpty(), "set-local-storage-item evaluates");
		check(ask(&eng3, "String(window.__store.adsOff)") == "true",
		       "the flag is stored");
		check(run_one(&eng3, "set-local-storage-item",
		               { "adsOff", "$remove$" }).isEmpty(), "and $remove$ runs");
		check(ask(&eng3, "String(typeof window.__store.adsOff)") == "undefined",
		       "taking it out again");
		// **Only the vocabulary**, which is the same limit `set-constant` has
		// and for the same reason: an arbitrary string would be content of a
		// list's choosing arriving in a page's storage.
		check(run_one(&eng3, "set-local-storage-item",
		               { "note", "anything at all" }).isEmpty(),
		       "a value outside the vocabulary still evaluates");
		check(ask(&eng3, "String(typeof window.__store.note)") == "undefined",
		       "and stores nothing");
	}

	section("taking an attribute or a class off what a selector names");
	{
		QJSEngine eng;
		give_page(&eng);
		check(run_one(&eng, "remove-attr", { "onclick" }).isEmpty(),
		       "remove-attr evaluates");
		// **With no selector, the names are the selector.** A rule that named
		// no elements would otherwise have to mean all of them.
		check(ask(&eng, "window.__sel") == "[onclick]",
		       QString("the selector defaults to the attribute (%1)")
		           .arg(ask(&eng, "window.__sel")));
		check(ask(&eng, "window.__gone.join(',')") ==
		          "one:attr:onclick,two:attr:onclick",
		       QString("and it comes off every element found (%1)")
		           .arg(ask(&eng, "window.__gone.join(',')")));

		QJSEngine eng2;
		give_page(&eng2);
		check(run_one(&eng2, "rc", { "promo sponsored" }).isEmpty(),
		       "remove-class evaluates, by its alias");
		check(ask(&eng2, "window.__sel") == ".promo,.sponsored",
		       QString("two classes make two selectors (%1)")
		           .arg(ask(&eng2, "window.__sel")));
		check(ask(&eng2, "window.__gone.join(',')").contains("one:class:promo") &&
		          ask(&eng2, "window.__gone.join(',')")
		              .contains("two:class:sponsored"),
		       QString("and both come off both elements (%1)")
		           .arg(ask(&eng2, "window.__gone.join(',')")));

		// A selector given explicitly is used as given.
		QJSEngine eng3;
		give_page(&eng3);
		check(run_one(&eng3, "remove-attr",
		               { "href", "a.promo-link" }).isEmpty(),
		       "a selector may be given");
		check(ask(&eng3, "window.__sel") == "a.promo-link",
		       QString("and is used verbatim (%1)")
		           .arg(ask(&eng3, "window.__sel")));

		// A mistyped selector is a mistyped rule, not an emergency.
		QJSEngine eng4;
		give_page(&eng4);
		check(run_one(&eng4, "remove-attr", { "href", "!broken" }).isEmpty(),
		       "a selector the engine refuses does not break the script");
		check(ask(&eng4, "window.__gone.length") == "0",
		       QString("and removes nothing (%1)")
		           .arg(ask(&eng4, "window.__gone.length")));
	}

	section("the observer stops, unless the rule asked it to stay");
	{
		// **An observer that queries the document on every mutation for the
		// life of the page is a cost paid on every page the rule matches**,
		// and most rules want the elements gone as the page builds rather
		// than policed for ever.
		QJSEngine eng;
		give_page(&eng);
		run_one(&eng, "remove-attr", { "onclick" });
		check(ask(&eng, "window.__observers.length") == "1",
		       QString("it observes (%1)")
		           .arg(ask(&eng, "window.__observers.length")));
		eng.evaluate("for (var i = 0; i < 70; i++) window.__observers[0].cb();");
		check(ask(&eng, "String(window.__observers[0].live)") == "false",
		       QString("and gives up after its budget (%1)")
		           .arg(ask(&eng, "String(window.__observers[0].live)")));

		QJSEngine eng2;
		give_page(&eng2);
		run_one(&eng2, "remove-attr", { "onclick", "", "stay" });
		eng2.evaluate("for (var i = 0; i < 70; i++) "
		               "window.__observers[0].cb();");
		check(ask(&eng2, "String(window.__observers[0].live)") == "true",
		       QString("while `stay` keeps it (%1)")
		           .arg(ask(&eng2, "String(window.__observers[0].live)")));
		// It also sweeps again on each mutation, which is the point of
		// observing at all.
		check(ask(&eng2, "window.__gone.length").toInt() > 2,
		       QString("and each pass sweeps (%1)")
		           .arg(ask(&eng2, "window.__gone.length")));
	}

	section("a selector that names the whole page is refused");
	{
		// **The refusal a container rule gets, for the same reason.**
		// `remove-attr, href, *` would take the address off every link.
		for (const char *broad : { "*", "body", "html", "div" }) {
			scriptlet_call c;
			QString why;
			const bool kept = scriptlets::parse_call(
			  QString("remove-attr, href, ") + QString::fromLatin1(broad),
			  &c, &why);
			check(!kept && why.contains("selector"),
			       QString("\"%1\" is refused (%2)")
			           .arg(QString::fromLatin1(broad),
			                 kept ? QStringLiteral("kept") : why));
		}
		scriptlet_call c;
		QString why;
		check(scriptlets::parse_call("remove-attr, href, a.promo", &c, &why),
		       "while a specific one is kept");
		check(scriptlets::parse_call("remove-attr, onclick", &c, &why),
		       "as is leaving the selector out");
	}

	section("pruning a fetch's body, whichever way the page reads it");
	{
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_fetch_stubs));
		check(run_one(&eng, "json-prune-fetch-response",
		               { "adPlacements", "", "/player" }).isEmpty(),
		       "json-prune-fetch-response evaluates");

		// A matching url: the body the page reads has the property gone, and
		// the rest of the document intact.
		const QString got = ask(&eng, "window.ask('https://x.test/player?v=1')");
		check(!got.contains("adPlacements"),
		       QString("the named property is gone from the body (%1)")
		           .arg(got));
		check(got.contains("streamingData"),
		       QString("and the rest of it is there (%1)").arg(got));
		check(ask(&eng, "window.__fetched.length") == "1",
		       "the request still went out -- this prunes the answer, not the "
		       "asking");
		check(ask(&eng, "window.__reads") == "1",
		       QString("and the original body was read exactly once (%1)")
		           .arg(ask(&eng, "window.__reads")));

		// The status and headers come across, because the page checks them.
		check(ask(&eng, "(function(){var s=null;"
		                 "window.fetch('https://x.test/player2').then("
		                 "function(r){ s=String(r.status); });return s;})()")
		          == "207",
		       "the status is carried over");

		// A url the rule did not name is untouched, and the proof is that the
		// original object comes back rather than a copy.
		QJSEngine eng2;
		give_page(&eng2);
		eng2.evaluate(QString::fromLatin1(k_fetch_stubs));
		run_one(&eng2, "json-prune-fetch-response",
		         { "adPlacements", "", "/player" });
		check(ask(&eng2, "(function(){var o=null;"
		                  "window.fetch('https://x.test/other').then("
		                  "function(r){ o=String(r.__original); });return o;})()")
		          == "true",
		       "a url the rule did not name gets the original response");

		// A body that is not JSON is handed back unchanged -- and still
		// readable, which is the part that needs saying: the original was
		// spent by then.
		QJSEngine eng3;
		give_page(&eng3);
		eng3.evaluate(QString::fromLatin1(k_fetch_stubs));
		eng3.evaluate("window.__body = '<html>not json</html>';");
		run_one(&eng3, "json-prune-fetch-response",
		         { "adPlacements", "", "/player" });
		check(ask(&eng3, "window.ask('https://x.test/player')")
		          == "<html>not json</html>",
		       QString("a body that is not JSON comes through whole (%1)")
		           .arg(ask(&eng3, "String(window.__body)")));

		// With a Response constructor present it is used, which is the path a
		// real page takes.
		QJSEngine eng4;
		give_page(&eng4);
		eng4.evaluate(QString::fromLatin1(k_fetch_stubs));
		eng4.evaluate("window.Response = function (text, init) {"
		               " this.__made = true; this.status = init.status;"
		               " this.text = function () { return window.P(text); }; };");
		run_one(&eng4, "json-prune-fetch-response",
		         { "adPlacements", "", "/player" });
		check(ask(&eng4, "(function(){var m=null;"
		                  "window.fetch('https://x.test/player').then("
		                  "function(r){ m=String(r.__made); });return m;})()")
		          == "true",
		       "a real Response is built where the frame has one");
	}

	section("pruning an XHR, read from a handler registered before send");
	{
		// **The case the obvious implementation loses.** Waiting for `load`
		// and rewriting the body then is too late for a page that added its
		// own handler first: listeners fire in the order they were added, so
		// it has already read the original. The getters go on the instance at
		// `send` time instead, before the request can complete.
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_fetch_stubs));
		check(run_one(&eng, "json-prune-xhr-response",
		               { "adPlacements", "", "/player" }).isEmpty(),
		       "json-prune-xhr-response evaluates");

		eng.evaluate("window.__seen = null;"
		              "window.x = new window.XMLHttpRequest();"
		              // Registered FIRST, which is the hostile ordering.
		              "window.x.addEventListener('load', function () {"
		              "  window.__seen = window.x.responseText; });"
		              "window.x.open('GET', 'https://x.test/player?v=1');"
		              "window.x.send();"
		              "window.finish(window.x,"
		              "  '{\"adPlacements\":[1,2],\"ok\":2}');");
		const QString seen = ask(&eng, "String(window.__seen)");
		check(!seen.contains("adPlacements"),
		       QString("the handler saw a pruned body (%1)").arg(seen));
		check(seen.contains("\"ok\":2"),
		       QString("with the rest of the document intact (%1)").arg(seen));
		check(ask(&eng, "window.__sent.length") == "1",
		       "the request still went out -- this prunes the answer");

		// Read twice, the same answer both times.
		check(ask(&eng, "String(window.x.responseText === "
		                 "window.x.responseText)") == "true",
		       "and reading it again gives the same body");

		// A url the rule did not name is untouched.
		eng.evaluate("window.y = new window.XMLHttpRequest();"
		              "window.y.open('GET', 'https://x.test/other');"
		              "window.y.send();"
		              "window.finish(window.y, '{\"adPlacements\":[9]}');");
		check(ask(&eng, "String(window.y.responseText)")
		          .contains("adPlacements"),
		       QString("a url the rule did not name keeps its body (%1)")
		           .arg(ask(&eng, "String(window.y.responseText)")));

		// A partial read, before the body is complete, is handed over as it
		// is: a fragment is not JSON and pretending otherwise would hand the
		// page something it would not have had.
		eng.evaluate("window.p = new window.XMLHttpRequest();"
		              "window.p.open('GET', 'https://x.test/player');"
		              "window.p.send();"
		              "window.finish(window.p, '{\"adPlacements\":[1]', 3);");
		check(ask(&eng, "String(window.p.responseText)")
		          .contains("adPlacements"),
		       QString("a partial body is untouched (%1)")
		           .arg(ask(&eng, "String(window.p.responseText)")));

		// **And one that is valid JSON while still incomplete**, which is
		// what actually tests the state guard: the truncated body above
		// fails to parse, so it comes back raw either way and a sabotage of
		// the guard passed against it. A server can send a whole document
		// and the connection stay open, and what the page reads then is what
		// it would have read without any of this.
		eng.evaluate("window.w = new window.XMLHttpRequest();"
		              "window.w.open('GET', 'https://x.test/player');"
		              "window.w.send();"
		              "window.finish(window.w,"
		              "  '{\"adPlacements\":[1],\"ok\":2}', 3);");
		check(ask(&eng, "String(window.w.responseText)")
		          .contains("adPlacements"),
		       QString("valid JSON before the body is done is untouched (%1)")
		           .arg(ask(&eng, "String(window.w.responseText)")));
		// And once it is done, the same instance prunes.
		eng.evaluate("window.w.readyState = 4;");
		check(!ask(&eng, "String(window.w.responseText)")
		           .contains("adPlacements"),
		       QString("and the same read prunes once it is (%1)")
		           .arg(ask(&eng, "String(window.w.responseText)")));

		// Not JSON at all: unchanged.
		eng.evaluate("window.h = new window.XMLHttpRequest();"
		              "window.h.open('GET', 'https://x.test/player');"
		              "window.h.send();"
		              "window.finish(window.h, '<html>no</html>');");
		check(ask(&eng, "String(window.h.responseText)") == "<html>no</html>",
		       QString("a body that is not JSON comes through whole (%1)")
		           .arg(ask(&eng, "String(window.h.responseText)")));
	}

	section("an XHR's other response shapes, pruned or left alone");
	{
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_fetch_stubs));
		run_one(&eng, "json-prune-xhr-response",
		         { "adPlacements", "", "/player" });

		// `responseType = 'json'` means the engine has already parsed it, so
		// the object is pruned rather than the text.
		eng.evaluate("window.j = new window.XMLHttpRequest();"
		              "window.j.responseType = 'json';"
		              "window.j.open('GET', 'https://x.test/player');"
		              "window.j.send();"
		              "window.finish(window.j,"
		              "  '{\"adPlacements\":[1],\"ok\":2}');");
		check(ask(&eng, "JSON.stringify(window.j.response)")
		          .contains("ok") &&
		          !ask(&eng, "JSON.stringify(window.j.response)")
		               .contains("adPlacements"),
		       QString("a json response is pruned as an object (%1)")
		           .arg(ask(&eng, "JSON.stringify(window.j.response)")));

		// **A shape this cannot read is handed over as it is.** Replacing an
		// arraybuffer with text would give the page something it did not ask
		// for, which is worse than leaving the ad data in it.
		eng.evaluate("window.b = new window.XMLHttpRequest();"
		              "window.b.responseType = 'arraybuffer';"
		              "window.b.open('GET', 'https://x.test/player');"
		              "window.b.send();"
		              "window.finish(window.b, '{\"adPlacements\":[1]}');");
		// Compared against the body's own length rather than a number counted
		// by hand -- the first version of this line said 22 for a 20-byte
		// body, which is an assertion failing on the test's arithmetic rather
		// than on the code.
		check(ask(&eng, "String(window.b.response.bytes === "
		                 "window.b.__raw.length)") == "true",
		       QString("an arraybuffer is untouched (%1 of %2 bytes)")
		           .arg(ask(&eng, "String(window.b.response.bytes)"),
		                 ask(&eng, "String(window.b.__raw.length)")));
	}

	section("the pattern checked is the one the scriptlet matches with");
	{
		// **The bitmask has to index the right argument.** For this scriptlet
		// the url pattern is the third, so a backtracking regex there is
		// refused while the same text in the first two -- which are property
		// paths, not patterns -- is not checked as one.
		scriptlet_call c;
		QString why;
		check(!scriptlets::parse_call(
		          "json-prune-fetch-response, ads, , /^(a+)+$/", &c, &why),
		       QString("a backtracking url pattern is refused (%1)").arg(why));
		check(scriptlets::parse_call(
		          "json-prune-fetch-response, /^(a+)+$/, , /ads/", &c, &why),
		       "while the same text as a property path is not checked as one");
		check(scriptlets::parse_call(
		          "json-prune-fetch-response, adPlacements, , /player/",
		          &c, &why),
		       QString("and an ordinary rule is kept (%1)").arg(why));
	}

	section("a trusted scriptlet runs only for a list marked trusted");
	{
		// **The case this class exists to make.** The same rule, the same
		// generated script, the only difference being the flag the
		// subscription carried -- so a list nobody vouched for cannot write a
		// cookie however it spells its rules.
		QJSEngine untrusted;
		give_page(&untrusted);
		check(run_one(&untrusted, "trusted-set-cookie",
		               { "consent", "yes", "7" }, false).isEmpty(),
		       "an untrusted list's call leaves a script that evaluates");
		check(ask(&untrusted, "String(window.__cookies.length)") == "0",
		       QString("and writes no cookie (%1)")
		           .arg(ask(&untrusted, "window.__cookies.join('/')")));

		QJSEngine trusted;
		give_page(&trusted);
		check(run_one(&trusted, "trusted-set-cookie",
		               { "consent", "yes", "7" }, true).isEmpty(),
		       "a trusted list's call runs");
		check(ask(&trusted, "String(window.__cookies.length)") == "1",
		       QString("and writes one cookie (%1)")
		           .arg(ask(&trusted, "window.__cookies.join('/')")));
		check(ask(&trusted, "window.__cookies[0]").startsWith("consent=yes;"),
		       QString("with the pair the rule named (%1)")
		           .arg(ask(&trusted, "window.__cookies[0]")));
		check(ask(&trusted, "window.__cookies[0]").contains("expires="),
		       "and an expiry, because the rule gave a number of days");
		check(ask(&trusted, "window.__cookies[0]").endsWith("path=/"),
		       "and a path, because a cookie without one is the page's path");

		// **The gate is in two places and this is the second.** `read` drops
		// such a call from an untrusted list, so a call reaching here with
		// the flag clear is one some other caller built -- and it still does
		// nothing. Sabotage: deleting the `requires_trust` test in
		// `source_for` turns the first pair of checks above red.
		scriptlet_call c;
		c.scope   = "x.test";
		c.name    = "trusted-set-cookie";
		c.args    = { "a", "b" };
		c.trusted = false;
		check(scriptlets::source_for({ c }).isEmpty(),
		       "a trusted call with the flag clear generates no script at all");
		c.trusted = true;
		check(!scriptlets::source_for({ c }).isEmpty(),
		       "and the same call with it set does");
	}

	section("a cookie's halves are encoded, so a value cannot add attributes");
	{
		// **The injection this class would otherwise offer.** A value
		// carrying `;` would add a domain, a path or a longer expiry than the
		// rule asked for, which is a filter list writing a cookie it did not
		// say it was writing. Both halves go through `encodeURIComponent`, so
		// the separator cannot survive in either.
		QJSEngine eng;
		give_page(&eng);
		check(run_one(&eng, "trusted-set-cookie",
		               { "ok", "yes; domain=.evil.test; max-age=99999999" },
		               true).isEmpty(),
		       "a value carrying attributes runs");
		const QString written = ask(&eng, "window.__cookies[0]");
		// **Asserted on the separator, not on the words.**
		// `encodeURIComponent` leaves `-` and `=` alone, so "max-age" and
		// "domain=" survive inside the value as text -- harmlessly, because
		// what makes a directive a directive is the `;` that introduces it,
		// and that is `%3B`. The first version of these two lines hunted for
		// the words and failed on a cookie that was correct.
		check(!written.contains("; domain="),
		       QString("and adds no domain directive (%1)").arg(written));
		check(!written.contains("; max-age"),
		       QString("nor a max-age one (%1)").arg(written));
		check(written.count(QStringLiteral("; ")) == 1,
		       QString("exactly one directive, the one it wrote (%1)")
		           .arg(written));
		check(written.contains("yes%3B%20domain"),
		       QString("the text is kept, as one value (%1)").arg(written));
		// A path cannot be percent-encoded and still mean what it says, so it
		// is checked instead and anything else falls back to the whole site.
		QJSEngine p;
		give_page(&p);
		check(run_one(&p, "trusted-set-cookie",
		               { "ok", "yes", "1", "/x; domain=.evil.test" },
		               true).isEmpty(), "a path carrying attributes runs");
		check(!ask(&p, "window.__cookies[0]").contains("evil") &&
		          ask(&p, "window.__cookies[0]").endsWith("path=/"),
		       QString("and is refused as a path (%1)")
		           .arg(ask(&p, "window.__cookies[0]")));
	}

	section("the trusted setters take a value the vocabulary has no word for");
	{
		// **What separates these from their untrusted namesakes**, and the
		// whole of why they are a second class: `set-constant` can say false
		// or a number, and this can say anything at all.
		QJSEngine eng;
		give_page(&eng);
		check(run_one(&eng, "trusted-set-constant",
		               { "cfg.token", "a-string-no-word-covers" },
		               true).isEmpty(), "a bare string is set");
		check(ask(&eng, "window.cfg.token") == "a-string-no-word-covers",
		       QString("and reads back as itself (%1)")
		           .arg(ask(&eng, "window.cfg.token")));

		QJSEngine j;
		give_page(&j);
		check(run_one(&j, "trusted-set-constant",
		               { "cfg.obj", "{\"ok\":1}" }, true).isEmpty(),
		       "a JSON value is set");
		check(ask(&j, "String(window.cfg.obj.ok)") == "1",
		       QString("and is parsed (%1)")
		           .arg(ask(&j, "String(window.cfg.obj.ok)")));

		// The shared vocabulary is still consulted first, so a rule saying
		// `false` gets the boolean rather than the five-character string.
		QJSEngine v;
		give_page(&v);
		check(run_one(&v, "trusted-set-constant", { "cfg.off", "false" },
		               true).isEmpty(), "a word the vocabulary covers is set");
		check(ask(&v, "typeof window.cfg.off") == "boolean",
		       QString("as the type the word names (%1)")
		           .arg(ask(&v, "typeof window.cfg.off")));

		// Storage, and the contrast: the untrusted one refuses a value it has
		// no word for rather than writing the text.
		QJSEngine st;
		give_page(&st);
		check(run_one(&st, "set-local-storage-item", { "k", "some-opaque-id" })
		          .isEmpty(), "the untrusted storage scriptlet runs");
		check(ask(&st, "typeof window.__store.k") == "undefined",
		       QString("and writes nothing (%1)")
		           .arg(ask(&st, "String(window.__store.k)")));
		check(run_one(&st, "trusted-set-local-storage-item",
		               { "k", "some-opaque-id" }, true).isEmpty(),
		       "the trusted one runs");
		check(ask(&st, "window.__store.k") == "some-opaque-id",
		       QString("and writes it (%1)")
		           .arg(ask(&st, "String(window.__store.k)")));
		check(run_one(&st, "trusted-set-local-storage-item",
		               { "k", "$remove$" }, true).isEmpty(),
		       "and $remove$ still removes");
		check(ask(&st, "typeof window.__store.k") == "undefined",
		       QString("the item (%1)")
		           .arg(ask(&st, "String(window.__store.k)")));
	}

	// **YouTube's rule as uBlock's list writes it, parsed and run.** The
	// quotes are the list's way of saying the argument is `"adPlacements"`
	// including its double quotes; kept as written, the search carried the
	// single quotes too, never matched, and the rule that strips the ad
	// payload from the player response did nothing on every video.
	section("a quoted argument means what is inside the quotes");
	{
		scriptlet_call c;
		QString why;
		// Parsed before each check rather than inside it, because the
		// label is built before the condition runs and would otherwise
		// print the previous call's arguments.
		bool ok = scriptlets::parse_call(
		  "trusted-rpfr, '\"adPlacements\"', '\"no_ads\"', player?", &c, &why);
		const QStringList unquoted = { "\"adPlacements\"", "\"no_ads\"",
		                               "player?" };
		check(ok && c.args == unquoted,
		       QString("the quotes are stripped (%1)").arg(c.args.join("|")));
		ok = scriptlets::parse_call("json-prune, 'a\", b", &c, &why);
		check(ok && c.args == QStringList{ "'a\"", "b" },
		       QString("and quotes that do not match are kept (%1)")
		           .arg(c.args.join("|")));
		ok = scriptlets::parse_call("json-prune, ', b", &c, &why);
		check(ok && c.args.first() == "'",
		       QString("as is a lone quote (%1)").arg(c.args.join("|")));

		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_fetch_stubs));
		scriptlets::parse_call(
		  "trusted-rpfr, '\"adPlacements\"', '\"no_ads\"', player?", &c, &why);
		c.scope   = "x.test";
		c.trusted = true;
		eng.evaluate(scriptlets::source_for({ c }));
		const QString body =
		  ask(&eng, "window.ask('https://x.test/youtubei/v1/player?key=k')");
		check(body.contains("\"no_ads\"") && !body.contains("adPlacements"),
		       QString("and the player response loses its ads (%1)").arg(body));
	}

	section("trusted-replace-fetch-response rewrites a matching body only");
	{
		// Same machinery as the pruner -- one `filter_fetch`, two callers --
		// so what is new here is the rewrite and the url scoping.
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_fetch_stubs));
		check(run_one(&eng, "trusted-replace-fetch-response",
		               { "\"ok\":1", "\"ok\":0", "/player/" },
		               true).isEmpty(), "the rewrite installs");
		check(ask(&eng, "window.ask('https://x.test/player/get')")
		          .contains("\"ok\":0"),
		       QString("a matching body is rewritten (%1)")
		           .arg(ask(&eng, "window.ask('https://x.test/player/get')")));
		check(!ask(&eng, "window.ask('https://x.test/other/get')")
		           .contains("\"ok\":0"),
		       QString("one on another url is not (%1)")
		           .arg(ask(&eng, "window.ask('https://x.test/other/get')")));

		// The status survives, because a page checks it.
		QJSEngine keep;
		give_page(&keep);
		keep.evaluate(QString::fromLatin1(k_fetch_stubs));
		run_one(&keep, "trusted-replace-fetch-response",
		         { "ok", "fine", "/player/" }, true);
		check(ask(&keep, "(function () { var st = 0; "
		                  "window.fetch('https://x.test/player/a')"
		                  ".then(function (r) { st = r.status; }); "
		                  "return String(st); })()") == "207",
		       "the rewritten response keeps the original's status");

		// A regex search, and the replacement is literal: `$&` in it is text
		// rather than a back-reference, which a list cannot have meant.
		QJSEngine re;
		give_page(&re);
		re.evaluate(QString::fromLatin1(k_fetch_stubs));
		check(run_one(&re, "trusted-replace-fetch-response",
		               { "/[0-9]+/", "9", "/player/" }, true).isEmpty(),
		       "a regex search installs");
		check(ask(&re, "window.ask('https://x.test/player/get')")
		          .contains("[9,9]"),
		       QString("and replaces every match (%1)")
		           .arg(ask(&re, "window.ask('https://x.test/player/get')")));

		// **The two searches differ in what a `$` means, and the difference
		// belongs to the search rather than to a choice.** A regex gets
		// `String.replace`'s semantics, because `/(a)(b)/` with `$2$1` is
		// what the rules in this family are written against; a substring has
		// no groups for `$1` to name, so split and join leave it as text.
		// This was asserted the wrong way round first -- the comment claimed
		// a literal replacement on both paths and the regex path disagreed,
		// which is the code being right and its description being wrong.
		QJSEngine grp;
		give_page(&grp);
		grp.evaluate(QString::fromLatin1(k_fetch_stubs));
		run_one(&grp, "trusted-replace-fetch-response",
		         { "/ok/", "[$&]", "/player/" }, true);
		check(ask(&grp, "window.ask('https://x.test/player/get')")
		          .contains("\"[ok]\":1"),
		       QString("a regex replacement reads its groups (%1)")
		           .arg(ask(&grp, "window.ask('https://x.test/player/get')")));

		QJSEngine lit;
		give_page(&lit);
		lit.evaluate(QString::fromLatin1(k_fetch_stubs));
		run_one(&lit, "trusted-replace-fetch-response",
		         { "ok", "[$&]", "/player/" }, true);
		check(ask(&lit, "window.ask('https://x.test/player/get')")
		          .contains("[$&]"),
		       QString("a substring replacement is literal text (%1)")
		           .arg(ask(&lit, "window.ask('https://x.test/player/get')")));
	}

	section("both of this scriptlet's pattern arguments are checked");
	{
		// **Two patterns on one rule**: what to look for and which url to
		// look in, at positions 0 and 2. The bitmask names both, so a
		// backtracking regex in either is refused -- and the first position
		// is the one a reader would expect to be a plain string.
		scriptlet_call c;
		QString why;
		check(!scriptlets::parse_call(
		          "trusted-replace-fetch-response, /^(a+)+$/, x, /ads/",
		          &c, &why),
		       QString("a backtracking search is refused (%1)").arg(why));
		check(!scriptlets::parse_call(
		          "trusted-replace-fetch-response, x, y, /^(a+)+$/",
		          &c, &why),
		       QString("so is a backtracking url (%1)").arg(why));
		check(scriptlets::parse_call(
		          "trusted-replace-fetch-response, ads, , /player/", &c, &why),
		       QString("and an ordinary rule is kept (%1)").arg(why));
		// The replacement is not a pattern and is not checked as one: it is
		// text, and refusing text for looking like a regex would refuse a
		// rule that meant exactly what it said.
		check(scriptlets::parse_call(
		          "trusted-replace-fetch-response, a, /^(b+)+$/, /player/",
		          &c, &why),
		       QString("the replacement is not checked as a pattern (%1)")
		           .arg(why));
	}

	section("the listener defuser needs BOTH the type and the handler");
	{
		// **uBlock's rule is `matchesBoth`, and that is the whole case.** A
		// rule aimed at one ad handler must not take every listener of that
		// type with it.
		//
		// **The handler is matched against `String(handler)`, which this
		// engine cannot give for a function.** QJSEngine answers
		// `function() { [native code] }` for every function, source and all
		// -- measured, not assumed -- so a fixture passing a real function
		// cannot reach the handler matcher at all. The first version of this
		// section did exactly that and failed; had its polarity been
		// reversed it would have passed for ever, for the wrong reason.
		//
		// A handler given as a string carries its text in every engine, so
		// that is the fixture that reaches the hazard. In a browser the
		// function case works too, and nothing here can show it.
		QJSEngine eng;
		give_page(&eng);
		check(run_one(&eng, "prevent-addEventListener",
		               { "load", "adsbygoogle" }).isEmpty(),
		       "the defuser installs");
		const char *add =
		  "(function (t, h) { window.__elem.addEventListener(t, h); "
		  "return window.__listened.join('/'); })";
		check(ask(&eng, (QString::fromLatin1(add) +
		          "('load', 'adsbygoogle.push({})')").toUtf8().constData())
		          .isEmpty(),
		       "a matching type AND handler is refused");
		// **The two discriminating cases.** An `||` in place of the `&&`
		// passes every other assertion in this section and fails these.
		check(ask(&eng, (QString::fromLatin1(add) +
		          "('load', 'startThePlayer()')").toUtf8().constData())
		          == "load",
		       "the same type with another handler is kept");
		check(ask(&eng, (QString::fromLatin1(add) +
		          "('click', 'adsbygoogle.push({})')").toUtf8().constData())
		          == "load/click",
		       "and the same handler on another type is kept");

		// An empty handler pattern matches anything, so a type alone takes
		// every listener of that type -- which is what a rule naming one
		// means, and is why naming neither is refused in the next section.
		QJSEngine only_type;
		give_page(&only_type);
		run_one(&only_type, "prevent-addEventListener", { "load" });
		check(ask(&only_type, (QString::fromLatin1(add) +
		          "('load', function () {})").toUtf8().constData()).isEmpty(),
		       "a type alone refuses that type, whatever the handler is");
		check(ask(&only_type, (QString::fromLatin1(add) +
		          "('click', function () {})").toUtf8().constData())
		          == "click",
		       "and leaves the others alone");
	}

	section("a defuser naming neither a type nor a handler is refused");
	{
		// **This catalog's refusal rather than uBlock's.** An empty pattern
		// matches anything, so `##+js(aeld)` means every type and every
		// handler -- a page with no listeners at all, which no rule can have
		// meant. uBlock accepts it and relies on its authors; this refuses
		// it, as a container selector is refused.
		scriptlet_call c;
		QString why;
		check(!scriptlets::parse_call("aeld", &c, &why),
		       QString("naming nothing is refused (%1)").arg(why));
		check(why.contains("every listener"),
		       QString("saying what it would have done (%1)").arg(why));
		check(!scriptlets::parse_call("aeld, , ", &c, &why),
		       QString("and so are two empty arguments (%1)").arg(why));
		check(scriptlets::parse_call("aeld, load", &c, &why) &&
		          c.name == "prevent-addEventListener",
		       "a type alone is enough, and resolves through the alias");
		check(scriptlets::parse_call("addEventListener-defuser, , ads", &c,
		                              &why),
		       "so is a handler alone, under the long alias");
	}

	section("trusted-replace-xhr-response rewrites the other transport");
	{
		// uBlock's YouTube rules use this one and the fetch one together, so
		// the pair shares `rewriter` and `filter_xhr` rather than carrying
		// two copies of the search semantics.
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_fetch_stubs));
		check(run_one(&eng, "trusted-replace-xhr-response",
		               { "\"adPlacements\"", "\"no_ads\"", "/player/" },
		               true).isEmpty(), "the rewrite installs");
		check(ask(&eng, "(function () { var x = new window.XMLHttpRequest(); "
		                 "x.open('GET', 'https://x.test/player/get'); x.send(); "
		                 "window.finish(x, '{\"adPlacements\":[1]}'); "
		                 "return x.responseText; })()")
		          .contains("\"no_ads\""),
		       "a matching url is rewritten");
		check(!ask(&eng, "(function () { var x = new window.XMLHttpRequest(); "
		                  "x.open('GET', 'https://x.test/other/get'); x.send(); "
		                  "window.finish(x, '{\"adPlacements\":[1]}'); "
		                  "return x.responseText; })()")
		           .contains("no_ads"),
		       "another url is not");
		// The shared readyState guard: a progressive read is a fragment.
		check(ask(&eng, "(function () { var x = new window.XMLHttpRequest(); "
		                 "x.open('GET', 'https://x.test/player/early'); "
		                 "x.send(); "
		                 "window.finish(x, '{\"adPlacements\":[1]}', 3); "
		                 "return x.responseText; })()")
		          .contains("adPlacements"),
		       "and a read before the body is complete is untouched");

		// **The `json` responseType cannot see the bytes**, so the rewrite
		// runs on the re-serialised object. Asserted because it is a real
		// limit, not because it is desirable.
		QJSEngine js;
		give_page(&js);
		js.evaluate(QString::fromLatin1(k_fetch_stubs));
		run_one(&js, "trusted-replace-xhr-response",
		         { "\"adPlacements\"", "\"no_ads\"", "/player/" }, true);
		check(ask(&js, "(function () { var x = new window.XMLHttpRequest(); "
		                "x.responseType = 'json'; "
		                "x.open('GET', 'https://x.test/player/get'); x.send(); "
		                "window.finish(x, '{\"adPlacements\":[1]}'); "
		                "return typeof x.response.no_ads; })()") == "object",
		       QString("a json response is rewritten through its values (%1)")
		           .arg(ask(&js, "JSON.stringify(window.b)")));

		// **And the trust gate, on the transport that carries the YouTube
		// rules.** Same call, flag clear: nothing happens.
		QJSEngine untrusted;
		give_page(&untrusted);
		untrusted.evaluate(QString::fromLatin1(k_fetch_stubs));
		check(run_one(&untrusted, "trusted-replace-xhr-response",
		               { "\"adPlacements\"", "\"no_ads\"", "/player/" },
		               false).isEmpty(),
		       "an untrusted list's call leaves a script that evaluates");
		check(ask(&untrusted,
		           "(function () { var x = new window.XMLHttpRequest(); "
		           "x.open('GET', 'https://x.test/player/get'); x.send(); "
		           "window.finish(x, '{\"adPlacements\":[1]}'); "
		           "return x.responseText; })()").contains("adPlacements"),
		       "and the body reaches the page as it was");
	}

	section("the boosters shorten a matching timer and leave the rest");
	{
		// **uBlock's numbers, because rules were written against them**: the
		// delay defaults to 1000, `*` matches any, and the boost defaults to
		// 0.05 clamped to [0.001, 50].
		QJSEngine eng;
		give_page(&eng);
		check(run_one(&eng, "adjust-setInterval",
		               { "poll", "1000", "0.02" }).isEmpty(),
		       "the interval booster installs");
		check(ask(&eng, "(function () { window.setInterval('pollForAds()', "
		                 "1000); return window.__ran.join('/'); })()")
		          == "i,20",
		       QString("a matching callback and delay is boosted (%1)")
		           .arg(ask(&eng, "window.__ran.join('/')")));
		check(ask(&eng, "(function () { window.setInterval('pollForAds()', "
		                 "250); return String(window.__ran.length); })()")
		          == "2" &&
		          ask(&eng, "window.__ran[1][1]") == "250",
		       QString("another delay is left alone (%1)")
		           .arg(ask(&eng, "window.__ran[1][1]")));
		check(ask(&eng, "(function () { window.setInterval('somethingElse()', "
		                 "1000); return window.__ran[2][1]; })()") == "1000",
		       "and so is another callback at the matching delay");

		// `*` for any delay, and the clamp at each end.
		QJSEngine any;
		give_page(&any);
		run_one(&any, "adjust-setTimeout", { "ad", "*", "0" });
		check(ask(&any, "(function () { window.setTimeout('adTimer()', 400); "
		                 "return window.__ran[0][1]; })()") == "0.4",
		       QString("a zero boost is clamped to 0.001, not to zero (%1)")
		           .arg(ask(&any, "window.__ran[0][1]")));
		QJSEngine big;
		give_page(&big);
		run_one(&big, "adjust-setTimeout", { "ad", "*", "9999" });
		check(ask(&big, "(function () { window.setTimeout('adTimer()', 2); "
		                 "return window.__ran[0][1]; })()") == "100",
		       QString("and a huge one to 50 (%1)")
		           .arg(ask(&big, "window.__ran[0][1]")));
	}

	section("the eval pair, and what neither of them can reach");
	{
		// **Neither can touch a direct `eval(...)`**, which the language
		// resolves without reading the property -- uBlock has the same limit
		// for the same reason. What they reach is the indirect call an
		// obfuscated loader makes.
		QJSEngine all;
		give_page(&all);
		check(run_one(&all, "noeval", {}).isEmpty(), "noeval installs");
		check(ask(&all, "String(window.eval('anything()'))") == "undefined",
		       "and every indirect eval returns undefined");
		check(ask(&all, "String(window.__evalled.length)") == "0",
		       "with nothing reaching the real one");

		QJSEngine some;
		give_page(&some);
		run_one(&some, "noeval-if", { "adsbygoogle" });
		check(ask(&some, "String(window.eval('adsbygoogle.push()'))")
		          == "undefined",
		       "noeval-if prevents matching code");
		check(ask(&some, "String(window.eval('startPlayer()'))") == "real",
		       QString("and passes the rest through (%1)")
		           .arg(ask(&some, "window.__evalled.join('/')")));

		// An empty needle logs in uBlock and prevents nothing, so nothing is
		// installed rather than silently blocking every eval.
		QJSEngine none;
		give_page(&none);
		run_one(&none, "noeval-if", { "" });
		check(ask(&none, "String(window.eval('anything()'))") == "real",
		       "an empty needle installs nothing at all");
	}

	section("prevent-requestAnimationFrame swaps the callback, and inverts");
	{
		QJSEngine eng;
		give_page(&eng);
		run_one(&eng, "prevent-requestAnimationFrame", { "adLoop" });
		check(ask(&eng, "(function () { var ran = 0; "
		                 "window.requestAnimationFrame('adLoop()'); "
		                 "return window.__framed[0]; })()") != "adLoop()",
		       QString("a matching callback is replaced (%1)")
		           .arg(ask(&eng, "window.__framed[0]")));
		check(ask(&eng, "(function () { "
		                 "window.requestAnimationFrame('drawFrame()'); "
		                 "return window.__framed[1]; })()") == "drawFrame()",
		       "and another is passed through as it was");
		// **The handle still comes back**, which is the point of swapping the
		// callback rather than refusing the call: a page storing the id and
		// cancelling later does not break.
		check(ask(&eng, "String(window.requestAnimationFrame('adLoop()'))")
		          == "7",
		       "with the real handle returned either way");

		QJSEngine inv;
		give_page(&inv);
		run_one(&inv, "prevent-requestAnimationFrame", { "!keepThis" });
		check(ask(&inv, "(function () { "
		                 "window.requestAnimationFrame('keepThis()'); "
		                 "return window.__framed[0]; })()") == "keepThis()",
		       "a negated needle keeps what it names");
		check(ask(&inv, "(function () { "
		                 "window.requestAnimationFrame('anythingElse()'); "
		                 "return window.__framed[1]; })()") != "anythingElse()",
		       "and replaces everything else");
	}

	section("set-cookie writes an answer, and refuses an identifier");
	{
		// **The whole of why this is not trusted**: every value it accepts is
		// an answer to a consent question, or a small number. uBlock's list,
		// and a signed 16-bit range.
		QJSEngine eng;
		give_page(&eng);
		run_one(&eng, "set-cookie", { "consent", "accept" });
		check(ask(&eng, "window.__cookies.join('/')").startsWith("consent=accept"),
		       QString("a word from the list is written (%1)")
		           .arg(ask(&eng, "window.__cookies.join('/')")));
		run_one(&eng, "set-cookie", { "uid", "a1b2c3d4e5f6" });
		check(ask(&eng, "String(window.__cookies.length)") == "1",
		       QString("an identifier is refused (%1)")
		           .arg(ask(&eng, "window.__cookies.join('/')")));
		run_one(&eng, "set-cookie", { "seen", "1" });
		check(ask(&eng, "String(window.__cookies.length)") == "2",
		       "a small number is allowed");
		run_one(&eng, "set-cookie", { "big", "99999" });
		check(ask(&eng, "String(window.__cookies.length)") == "2",
		       QString("one outside a signed 16-bit range is not (%1)")
		           .arg(ask(&eng, "window.__cookies.join('/')")));
		// The same value through the trusted spelling, which has no list.
		QJSEngine t;
		give_page(&t);
		run_one(&t, "trusted-set-cookie", { "uid", "a1b2c3d4e5f6" }, true);
		check(ask(&t, "window.__cookies.join('/')").contains("a1b2c3d4e5f6"),
		       "and trusted-set-cookie writes what set-cookie refused");
	}

	section("remove-cookie expires a matching name on this host and above");
	{
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate("window.__cookies = [];"
		              "Object.defineProperty(document, 'cookie', {"
		              "  get: function () { return 'keep=1; dropme=2'; },"
		              "  set: function (v) { window.__cookies.push(String(v)); },"
		              "  configurable: true });"
		              // **Under the scope the call names.** The first version
		              // of this fixture used an unrelated host, so the
		              // dispatcher's scope check skipped the scriptlet and
		              // nothing ran -- a section that would have passed for
		              // the wrong reason had its polarity been reversed.
		              "location.hostname = 'a.b.x.test';");
		run_one(&eng, "remove-cookie", { "dropme" });
		const QString wrote = ask(&eng, "window.__cookies.join(' | ')");
		check(wrote.contains("dropme=;"),
		       QString("the matching cookie is expired (%1)").arg(wrote));
		check(!wrote.contains("keep="),
		       "and the other is left alone");
		// **Parent domains too**, because a cookie set on `.b.example` is not
		// removed by expiring it on `a.b.x.test`.
		check(wrote.contains("domain=a.b.x.test") &&
		          wrote.contains("domain=b.x.test"),
		       QString("on this host and its parents (%1)").arg(wrote));
	}

	section("abort-on-stack-trace throws only for a matching stack");
	{
		// The stack is normalised to `function url:line` per frame before
		// matching, so a needle names a function rather than a column.
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate("window.cfg = { token: 'kept' };");
		check(run_one(&eng, "abort-on-stack-trace",
		               { "cfg.token", "adReader" }).isEmpty(),
		       "the guard installs");
		check(ask(&eng, "(function adReader() { "
		                 "try { return 'read ' + window.cfg.token; } "
		                 "catch (e) { return 'threw'; } })()") == "threw",
		       "a read from the named function throws");
		check(ask(&eng, "(function playerReader() { "
		                 "try { return 'read ' + window.cfg.token; } "
		                 "catch (e) { return 'threw'; } })()")
		          == "read kept",
		       "and a read from anywhere else gets the value");
	}

	section("trusted-replace-argument changes what a call is given");
	{
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate("window.__sent_args = [];"
		              "window.api = { track: function (a, b) { "
		              "  window.__sent_args.push(String(a) + ',' + String(b)); "
		              "  return 'sent'; } };");
		check(run_one(&eng, "trusted-replace-argument",
		               { "api.track", "1", "false" }, true).isEmpty(),
		       "the replacer installs");
		check(ask(&eng, "(function () { window.api.track('keep', 'drop'); "
		                 "return window.__sent_args[0]; })()") == "keep,false",
		       QString("the named position is replaced and the rest kept (%1)")
		           .arg(ask(&eng, "window.__sent_args[0]")));
		// add: offsets a number rather than replacing it.
		QJSEngine add;
		give_page(&add);
		add.evaluate("window.__sent_args = [];"
		              "window.api = { bid: function (n) { "
		              "  window.__sent_args.push(String(n)); return n; } };");
		run_one(&add, "trusted-replace-argument", { "api.bid", "0", "add:5" },
		         true);
		check(ask(&add, "(function () { window.api.bid(10); "
		                 "return window.__sent_args[0]; })()") == "15",
		       QString("add: offsets it (%1)")
		           .arg(ask(&add, "window.__sent_args[0]")));
		// And the trust gate.
		QJSEngine no;
		give_page(&no);
		no.evaluate("window.__sent_args = [];"
		             "window.api = { track: function (a) { "
		             "  window.__sent_args.push(String(a)); return 'sent'; } };");
		run_one(&no, "trusted-replace-argument", { "api.track", "0", "false" },
		         false);
		check(ask(&no, "(function () { window.api.track('keep'); "
		                "return window.__sent_args[0]; })()") == "keep",
		       "an untrusted list changes nothing");
	}

	section("the shims answer, rather than refusing");
	{
		// **These are not filters.** Each hands a named library a
		// cooperative stand-in so a page that will not proceed until its ad
		// script answers gets an answer -- which is why they take no
		// arguments and why the rules invoke them bare.
		QJSEngine pd;
		give_page(&pd);
		check(run_one(&pd, "popads-dummy", {}).isEmpty(),
		       "popads-dummy installs");
		check(ask(&pd, "typeof window.PopAds") == "object" &&
		          ask(&pd, "typeof window.popns") == "object",
		       QString("and both globals answer (%1/%2)")
		           .arg(ask(&pd, "typeof window.PopAds"),
		                 ask(&pd, "typeof window.popns")));

		// popads.net is the louder one: assigning throws a token its own
		// error handler swallows, so the library's write fails silently.
		QJSEngine pn;
		give_page(&pn);
		check(run_one(&pn, "popads.net", {}).isEmpty(), "popads.net installs");
		check(ask(&pn, "(function () { try { window.PopAds = { x: 1 }; "
		                "return 'assigned'; } catch (e) { "
		                "return e instanceof ReferenceError ? 'threw' : '?'; "
		                "} })()") == "threw",
		       "and an assignment to it throws a ReferenceError");
		check(ask(&pn, "String(typeof window.onerror)") == "function",
		       "with a handler installed to swallow it");

		// nofab: the whole trick is which callback runs.
		QJSEngine nf;
		give_page(&nf);
		check(run_one(&nf, "nofab", {}).isEmpty(), "nofab installs");
		check(ask(&nf, "(function () { var seen = []; "
		                "window.fuckAdBlock.onNotDetected(function () { "
		                "  seen.push('not-detected'); }); "
		                "window.fuckAdBlock.onDetected(function () { "
		                "  seen.push('detected'); }); "
		                "return seen.join('/'); })()") == "not-detected",
		       QString("onNotDetected runs and onDetected does not (%1)")
		           .arg(ask(&nf, "typeof window.fuckAdBlock")));
		check(ask(&nf, "String(typeof window.FuckAdBlock) + ',' + "
		                "String(typeof window.blockAdBlock) + ',' + "
		                "String(typeof window.sniffAdBlock)")
		          == "function,object,object",
		       "and all six spellings are present");

		// prevent-bab recognises the script by what it contains.
		QJSEngine bb;
		give_page(&bb);
		check(run_one(&bb, "prevent-bab", {}).isEmpty(),
		       "prevent-bab installs");
		check(ask(&bb, "String(window.eval('var x = blockadblock_check();'))")
		          == "undefined",
		       "a signature match is swallowed");
		check(ask(&bb, "String(window.eval('startThePlayer();'))") == "real",
		       QString("and ordinary code is passed through (%1)")
		           .arg(ask(&bb, "window.__evalled.join('/')")));
		// **The 80% threshold, which is uBlock's and is the whole
		// classifier.** The fourth signature is fifteen tokens of the
		// obfuscated loader; twelve of them is a match and a handful is not.
		check(ask(&bb, "String(window.eval('getElementById String.fromCharCode "
		                "charAt DOMContentLoaded AdBlock addEventListener'))")
		          == "real",
		       "six tokens of the long signature is not a match");
	}

	section("prevent-xhr answers a matching request and never sends it");
	{
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_fetch_stubs));
		check(run_one(&eng, "prevent-xhr", { "doubleclick" }).isEmpty(),
		       "the blocker installs");
		const char *blocked =
		  "(function () { var x = new window.XMLHttpRequest(); "
		  "x.open('GET', 'https://doubleclick.test/a'); x.send(); "
		  "return [x.status, x.readyState, JSON.stringify(x.responseText), "
		  "window.__sent.length].join('/'); })()";
		check(ask(&eng, blocked) == "200/4/\"\"/0",
		       QString("answered 200, complete, empty, and never sent (%1)")
		           .arg(ask(&eng, blocked)));
		check(ask(&eng, "(function () { var x = new window.XMLHttpRequest(); "
		                 "x.open('GET', 'https://news.test/a'); x.send(); "
		                 "return String(window.__sent.length); })()") != "0",
		       "another url is sent as usual");

		// **The answer's SHAPE follows responseType**, which is the part that
		// turns a block into a crash when it is got wrong: a page asking for
		// json and handed a string calls `.foo` on it and dies.
		QJSEngine js;
		give_page(&js);
		js.evaluate(QString::fromLatin1(k_fetch_stubs));
		run_one(&js, "prevent-xhr", { "doubleclick" });
		check(ask(&js, "(function () { var x = new window.XMLHttpRequest(); "
		                "x.responseType = 'json'; "
		                "x.open('GET', 'https://doubleclick.test/a'); x.send(); "
		                "return typeof x.response; })()") == "object",
		       "a json request gets an object");

		// A directive asks for a body of a given length.
		QJSEngine len;
		give_page(&len);
		len.evaluate(QString::fromLatin1(k_fetch_stubs));
		run_one(&len, "prevent-xhr", { "doubleclick", "length:10" });
		check(ask(&len, "(function () { var x = new window.XMLHttpRequest(); "
		                 "x.open('GET', 'https://doubleclick.test/a'); x.send(); "
		                 "return String(x.responseText.length); })()") == "10",
		       QString("length:10 gives ten characters (%1)")
		           .arg(ask(&len, "'see above'")));
		// A property test rather than a url, which is the parsing rule that
		// is easy to get wrong: `method:HEAD` is a property, while
		// `/a|b/` stays a url pattern because of the characters in it.
		QJSEngine pm;
		give_page(&pm);
		pm.evaluate(QString::fromLatin1(k_fetch_stubs));
		run_one(&pm, "prevent-xhr", { "method:HEAD" });
		check(ask(&pm, "(function () { var x = new window.XMLHttpRequest(); "
		                "x.open('HEAD', 'https://news.test/a'); x.send(); "
		                "return String(x.status) + '/' + "
		                "String(window.__sent.length); })()") == "200/0",
		       "a method test matches on the method");
		check(ask(&pm, "(function () { var x = new window.XMLHttpRequest(); "
		                "x.open('GET', 'https://news.test/a'); x.send(); "
		                "return String(window.__sent.length); })()") == "1",
		       "and leaves another method alone");
	}

	section("trusted-prevent-fetch answers, and only for a trusted list");
	{
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_fetch_stubs));
		check(run_one(&eng, "trusted-prevent-fetch",
		               { "doubleclick", "emptyObj" }, true).isEmpty(),
		       "the blocker installs");
		check(ask(&eng, "window.ask('https://doubleclick.test/a')") == "{}",
		       QString("a matching fetch is answered with the body asked for "
		                "(%1)").arg(ask(&eng,
		                 "window.ask('https://doubleclick.test/a')")));
		check(ask(&eng, "String(window.__fetched.length)") == "0",
		       "and nothing went out");
		check(ask(&eng, "window.ask('https://news.test/a')")
		          .contains("adPlacements"),
		       "another url is fetched as usual");

		QJSEngine no;
		give_page(&no);
		no.evaluate(QString::fromLatin1(k_fetch_stubs));
		run_one(&no, "trusted-prevent-fetch", { "doubleclick", "emptyObj" },
		         false);
		check(ask(&no, "window.ask('https://doubleclick.test/a')")
		          .contains("adPlacements"),
		       "an untrusted list's call does nothing at all");
	}

	section("the node-text family rewrites matching nodes only");
	{
		// **`remove-node-text(name, includes)` clears the text** of nodes
		// whose name matches AND whose text contains the pattern. Both halves
		// are needed, so the fixture carries a node that fails each one.
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_dom_stubs));
		eng.evaluate("window.__tree = ["
		              "  mknode('SCRIPT', 'var ads = loadAds();'),"
		              "  mknode('SCRIPT', 'var player = start();'),"
		              "  mknode('DIV', 'loadAds() mentioned here')"
		              "];");
		check(run_one(&eng, "remove-node-text",
		               { "script", "loadAds" }).isEmpty(),
		       "remove-node-text installs");
		check(ask(&eng, "window.__tree[0].textContent") == "",
		       QString("the matching script is cleared (%1)")
		           .arg(ask(&eng, "window.__tree[0].textContent")));
		check(ask(&eng, "window.__tree[1].textContent")
		          == "var player = start();",
		       "a script whose text does not match is left alone");
		// **The node name is anchored**, which is uBlock's behaviour and the
		// reason `script` does not also take `noscript`: a DIV mentioning the
		// same text is not a script.
		check(ask(&eng, "window.__tree[2].textContent")
		          == "loadAds() mentioned here",
		       "and so is another element with matching text");

		// The trusted spelling rewrites rather than clears, and uBlock gives
		// `replace-node-text` as an alias of the TRUSTED one -- a replacement
		// landing in a <script> is code the page runs.
		QJSEngine rep;
		give_page(&rep);
		rep.evaluate(QString::fromLatin1(k_dom_stubs));
		rep.evaluate("window.__tree = [ mknode('SCRIPT', 'a=1; ads=2; b=3;') ];");
		check(run_one(&rep, "trusted-replace-node-text",
		               { "script", "ads=2", "ads=0" }, true).isEmpty(),
		       "trusted-replace-node-text installs");
		check(ask(&rep, "window.__tree[0].textContent") == "a=1; ads=0; b=3;",
		       QString("and rewrites in place (%1)")
		           .arg(ask(&rep, "window.__tree[0].textContent")));

		QJSEngine untrusted;
		give_page(&untrusted);
		untrusted.evaluate(QString::fromLatin1(k_dom_stubs));
		untrusted.evaluate("window.__tree = [ mknode('SCRIPT', 'ads=2;') ];");
		run_one(&untrusted, "trusted-replace-node-text",
		         { "script", "ads=2", "ads=0" }, false);
		check(ask(&untrusted, "window.__tree[0].textContent") == "ads=2;",
		       "while an untrusted list changes nothing");

		// **This script's own node is never rewritten**, which is how a
		// scriptlet would otherwise eat itself.
		QJSEngine mine;
		give_page(&mine);
		mine.evaluate(QString::fromLatin1(k_dom_stubs));
		mine.evaluate("window.__tree = [ mknode('SCRIPT', 'loadAds();') ];"
		               "document.currentScript = window.__tree[0];");
		run_one(&mine, "remove-node-text", { "script", "loadAds" });
		check(ask(&mine, "window.__tree[0].textContent") == "loadAds();",
		       "the running script's own node is skipped");
	}

	section("href-sanitizer only accepts something that is a URL");
	{
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_dom_stubs));
		eng.evaluate("window.__set = [];"
		              "var mklink = function (text) { return {"
		              "  textContent: text, href: 'https://tracker.test/r',"
		              "  getAttribute: function () { return this.href; },"
		              "  setAttribute: function (k, v) { "
		              "    this.href = v; window.__set.push(v); } }; };"
		              "window.__links = [ mklink('https://real.test/page'),"
		              "                    mklink('not a url at all'),"
		              "                    mklink('javascript:alert(1)') ];");
		check(run_one(&eng, "href-sanitizer", { "a[href]" }).isEmpty(),
		       "href-sanitizer installs");
		check(ask(&eng, "window.__links[0].href") == "https://real.test/page",
		       QString("a link whose text is a URL is rewritten (%1)")
		           .arg(ask(&eng, "window.__links[0].href")));
		check(ask(&eng, "window.__links[1].href") == "https://tracker.test/r",
		       "one whose text is not is left alone");
		// **The scheme check is the one that matters.** Without it this would
		// take the destination from page text and put `javascript:` in an
		// href, which is a worse thing than the tracker it replaced.
		check(ask(&eng, "window.__links[2].href") == "https://tracker.test/r",
		       QString("and a javascript: URL is refused (%1)")
		           .arg(ask(&eng, "window.__links[2].href")));
	}

	section("disable-newtab-links walks up from what was clicked");
	{
		// An anchor is rarely the element clicked -- an image or a span
		// inside it is -- so a handler that only read `ev.target` would miss
		// nearly every real case.
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_dom_stubs));
		check(run_one(&eng, "disable-newtab-links", {}).isEmpty(),
		       "the handler installs");
		check(ask(&eng, "String(typeof window.__onclick)") == "function",
		       "on the document, in the capture phase");
		check(ask(&eng, "(function () { var stopped = 0; "
		                 "var a = { localName: 'a', parentNode: null, "
		                 "  hasAttribute: function (n) { return n === 'target'; } }; "
		                 "var img = { localName: 'img', parentNode: a, "
		                 "  hasAttribute: function () { return false; } }; "
		                 "window.__onclick({ target: img, "
		                 "  stopPropagation: function () { stopped++; }, "
		                 "  preventDefault: function () { stopped++; } }); "
		                 "return String(stopped); })()") == "2",
		       "a click inside an <a target> is stopped");
		check(ask(&eng, "(function () { var stopped = 0; "
		                 "var a = { localName: 'a', parentNode: null, "
		                 "  hasAttribute: function () { return false; } }; "
		                 "window.__onclick({ target: a, "
		                 "  stopPropagation: function () { stopped++; }, "
		                 "  preventDefault: function () { stopped++; } }); "
		                 "return String(stopped); })()") == "0",
		       "and an <a> with no target is not");
	}

	section("prevent-refresh stops the load a meta refresh asked for");
	{
		QJSEngine eng;
		give_page(&eng);
		eng.evaluate(QString::fromLatin1(k_dom_stubs));
		eng.evaluate("window.__meta = { getAttribute: function () "
		              "{ return '0; url=https://elsewhere.test/'; } };");
		check(run_one(&eng, "prevent-refresh", {}).isEmpty(),
		       "the defuser installs");
		check(ask(&eng, "String(typeof window.__onload)") == "function",
		       "waiting for load rather than acting at once");
		check(ask(&eng, "(function () { window.__onload(); "
		                 "return String(window.__stopped); })()") == "1",
		       QString("and a zero-second refresh is stopped immediately (%1)")
		           .arg(ask(&eng, "String(window.__stopped)")));

		// **With no argument a reader gets half the time the meta asked
		// for**, which is uBlock's arithmetic and is deliberate: stopping a
		// five-second refresh at once would look like a broken page.
		QJSEngine later;
		give_page(&later);
		later.evaluate(QString::fromLatin1(k_dom_stubs));
		later.evaluate("window.__meta = { getAttribute: function () "
		                "{ return '4; url=https://elsewhere.test/'; } };");
		run_one(&later, "prevent-refresh", {});
		check(ask(&later, "(function () { window.__onload(); "
		                   "return String(window.__stopped) + '/' + "
		                   "String(window.__ran.length); })()") == "0/1",
		       QString("a four-second one is deferred, not stopped (%1)")
		           .arg(ask(&later, "window.__ran.join('/')")));
		check(ask(&later, "String(window.__ran[0][1])") == "2000",
		       QString("by half of it (%1)")
		           .arg(ask(&later, "String(window.__ran[0][1])")));

		// No meta, nothing to defuse.
		QJSEngine none;
		give_page(&none);
		none.evaluate(QString::fromLatin1(k_dom_stubs));
		run_one(&none, "prevent-refresh", {});
		check(ask(&none, "(function () { window.__onload(); "
		                  "return String(window.__stopped); })()") == "0",
		       "a page with no meta refresh is untouched");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

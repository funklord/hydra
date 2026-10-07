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
window.Promise = { resolve: function (v) { return { then: function (f) {
	f(v); return this; } }; } };
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

// Run one call and hand back the engine's own answer to an expression.
static QString ask(QJSEngine *eng, const char *expr) {
	const QJSValue v = eng->evaluate(QString::fromLatin1(expr));
	return v.isError() ? QStringLiteral("!") + v.toString() : v.toString();
}

static QString run_one(QJSEngine *eng, const char *name,
                        const QStringList &args) {
	scriptlet_call c;
	c.scope = "x.test";
	c.name  = QString::fromLatin1(name);
	c.args  = args;
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
		// **`json-prune-xhr-response` is the deliberate absence now**, and the
		// reason is in project.md: the point at which an XHR's body can be
		// replaced is a `load` listener whose order against the page's own is
		// not guaranteed, so it would prune sometimes -- and a count claiming
		// coverage it does not have is worse than an honest absence.
		for (const char *no : { "trusted-set-cookie", "trusted-replace-fetch",
		                         "aost", "trusted-prune-inbound-object",
		                         "eval", "" }) {
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
		check(!scriptlets::parse_call("trusted-set-cookie, a, b", &c, &why),
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
		// The count lives here, once. It moves when the catalog does, which
		// is the point: a name added without a test is an entry nothing ran.
		check(scriptlets::names().size() == 15,
		       QString("fifteen scriptlets in the catalog (%1)")
		           .arg(scriptlets::names().size()));
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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

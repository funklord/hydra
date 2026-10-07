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
		for (const char *no : { "trusted-set-cookie", "trusted-replace-fetch",
		                         "aost", "remove-attr", "eval", "" }) {
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
		check(scriptlets::names().size() == 11,
		       QString("eleven scriptlets in the catalog (%1)")
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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

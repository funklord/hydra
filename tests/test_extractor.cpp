// Generated site extractors: the sandbox, and the rule that a proposal cannot
// invent a URL.
#include "site_extractor.h"
#include "extractor_dialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static QList<evidence_request> sample() {
	// Shaped like the site that motivated all this: the manifest is disguised
	// with a .txt extension and a query string, among ordinary page traffic.
	QList<evidence_request> ev;
	int n = 0;
	auto add = [&](const char *u, const char *k) {
		ev << evidence_request{ QUrl(QString::fromUtf8(u)), QString::fromUtf8(k), n++ };
	};
	add("https://site.example/watch/1", "other");
	add("https://site.example/theme/app.js", "script");
	add("https://cdn.example/img/poster.jpg", "image");
	add("https://sil5.player.example/v4/db/abc/cf-master.1774687168.txt?k=UCp&kx=17", "other");
	add("https://sil5.player.example/v4/db/abc/seg-00001.ts", "other");
	add("https://metrics.example/watch/9?p=1", "script");
	return ev;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const QUrl page("https://site.example/watch/1");
	const auto ev = sample();

	section("a plausible extractor");
	{
		// What a model would reasonably propose for this evidence.
		const QString src = R"JS(
			extract = function (page, requests) {
			  for (var i = 0; i < requests.length; i++) {
			    var u = requests[i].url;
			    if (u.indexOf('cf-master') !== -1)
			      return { url: u, kind: 'hls', headers: { Referer: page.url } };
			  }
			  return null;
			};
		)JS";
		const extractor_verdict v = site_extractor::check(src, page, ev);
		check(v.usable, QString("it is accepted (%1)").arg(v.message));
		check(v.result.kind == "hls", "with the kind it claimed");
		check(v.result.url.toString().contains("cf-master"),
		      "and the disguised manifest URL detection could not see");
		check(v.result.headers.value("Referer") == page.toString(),
		      "headers come through — the CDN wants them (§11.3)");
	}

	section("the rule that matters: it cannot invent a URL");
	{
		const QString src = R"JS(
			extract = function () {
			  return { url: 'https://attacker.example/anything.m3u8', kind: 'hls' };
			};
		)JS";
		const extractor_verdict v = site_extractor::check(src, page, ev);
		check(!v.usable, "a URL the page never requested is refused");
		check(v.invented, "and is reported as invented, not merely wrong");
		check(v.message.contains("never requested"), "with a reason that says so");
	}
	{
		// A subtler one: a real host, but an address assembled by the script.
		const QString src = R"JS(
			extract = function (page, requests) {
			  return { url: 'https://sil5.player.example/v4/db/abc/secret.txt',
			           kind: 'hls' };
			};
		)JS";
		const extractor_verdict v = site_extractor::check(src, page, ev);
		check(v.invented,
		      "a plausible-looking address on a real host is still invented");
	}

	section("a segment is not a stream");
	{
		// A real model did exactly this: returned seg-00000.ts, which the page
		// really requested, so the observed-URL rule alone let it through.
		QList<evidence_request> many = sample();
		int n = many.size();
		for (int i = 0; i < 30; ++i)
			many << evidence_request{
				QUrl(QString("https://sil5.player.example/v4/db/abc/seg-%1.ts")
				         .arg(i, 5, 10, QChar('0'))), "other", n++ };

		const QString src =
			"extract = function(p, r){ for (var i=0;i<r.length;i++) "
			"if (r[i].url.indexOf('seg-')!==-1) "
			"return { url: r[i].url, kind: 'hls' }; return null; };";
		const extractor_verdict v = site_extractor::check(src, page, many);
		check(!v.usable, "picking out of a flood of repeats is refused");
		check(v.is_segment, "and reported as a segment, not as invented");
		check(!v.invented, "since the address was genuinely requested");

		// The manifest, fetched once, still passes.
		const QString ok =
			"extract = function(p, r){ for (var i=0;i<r.length;i++) "
			"if (r[i].url.indexOf('cf-master')!==-1) "
			"return { url: r[i].url, kind: 'hls' }; return null; };";
		check(site_extractor::check(ok, page, many).usable,
		      "while a request the page made once still passes");
	}

	section("the page is not the stream");
	{
		// A real model did this one too: asked for the stream in the page, it
		// returned the page. The document is the most certainly-observed request
		// there is, so neither the invented rule nor the segment rule catches it,
		// and the media list would have offered the HTML as though it were video.
		const QString src =
			"extract = function(p, r){ return { url: p.url, kind: 'direct' }; };";
		const extractor_verdict v = site_extractor::check(src, page, ev);
		check(!v.usable, "returning the page's own address is refused");
		check(v.is_page, "and reported as the page, not as invented");
		check(!v.invented, "since the page really was requested");
		check(!v.is_segment, "and it is not a segment either");
		check(v.message.contains("page's own address"),
		      "with a reason that names what went wrong");

		// Same address, dressed differently. A fragment never reaches the server
		// and a trailing slash is the same resource, so neither is an escape.
		const QString dressed =
			"extract = function(p, r){ return { url: p.url + '#top', "
			"kind: 'direct' }; };";
		check(site_extractor::check(dressed, page, ev).is_page,
		      "a fragment on the page url does not get around it");

		// And the rule stays narrow: another request on the page's own host is
		// still a perfectly good answer. It has to be one the browser did not
		// fetch as an image or a script, or the furniture rule below refuses it
		// for its own reasons and this proves nothing.
		QList<evidence_request> with_sibling = ev;
		with_sibling << evidence_request{
			QUrl("https://site.example/watch/1/stream"), "other",
			with_sibling.size() };
		const QString same_host =
			"extract = function(p, r){ for (var i=0;i<r.length;i++) "
			"if (r[i].url.indexOf('/stream')!==-1) "
			"return { url: r[i].url, kind: 'direct' }; return null; };";
		const extractor_verdict sh =
			site_extractor::check(same_host, page, with_sibling);
		check(sh.usable && !sh.is_page,
		      "while a different request on the same host still passes");
	}

	section("scripts that misbehave");
	{
		const extractor_verdict loop = site_extractor::check(
			"extract = function(){ while (true) {} };", page, ev);
		check(!loop.usable && loop.timed_out,
		      QString("an endless loop is interrupted rather than hanging (%1)")
		          .arg(loop.message));

		const extractor_verdict thrown = site_extractor::check(
			"extract = function(){ throw new Error('boom'); };", page, ev);
		check(!thrown.usable && thrown.message.contains("threw"), "a throw is caught");

		const extractor_verdict none = site_extractor::check(
			"extract = function(){ return null; };", page, ev);
		check(!none.usable && none.message.contains("found nothing"),
		      "returning nothing is a clean failure");

		const extractor_verdict junk = site_extractor::check("not javascript {", page, ev);
		check(!junk.usable, "unparseable source is refused");

		const extractor_verdict nofn = site_extractor::check("var x = 1;", page, ev);
		check(!nofn.usable && nofn.message.contains("extract"),
		      "a script defining no extract() says so");

		const extractor_verdict badkind = site_extractor::check(
			"extract = function(p, r){ return { url: r[3].url, kind: 'magic' }; };",
			page, ev);
		check(!badkind.usable && !badkind.invented,
		      "an unknown kind is refused, but not called invented");
	}

	section("the sandbox has nothing in it");
	{
		for (const char *probe : { "typeof XMLHttpRequest", "typeof fetch",
		                            "typeof document", "typeof window",
		                            "typeof require", "typeof process" }) {
			const QString src = QString(
				"extract = function(){ return { url: '%1:' + (%2), kind: 'direct' }; };")
				.arg("x", probe);
			const extraction r = site_extractor::run(src, page, ev);
			// It should run and report "undefined" — the point is that the name
			// resolves to nothing, not that the script fails.
			check(r.ok && r.url.toString().endsWith("undefined"),
			      QString("%1 is not available (%2)")
			          .arg(probe, r.ok ? r.url.toString() : r.error));
		}
	}

	section("a request's type and a stream's kind are different words");
	{
		// They were both called `kind`, and a model read it the obvious way:
		// `request.kind === 'hls'`, which is never true of anything, so it
		// returned null and looked like a model failure rather than a naming
		// one. The request side is `type` now, and the collision has to be
		// gone rather than merely discouraged.
		const QString reads_type =
			"extract = function(p, r){ for (var i=0;i<r.length;i++) "
			"if (r[i].type === 'image') "
			"return { url: r[i].url, kind: 'direct' }; return null; };";
		const extraction got = site_extractor::run(reads_type, page, ev);
		check(got.ok && got.url.toString().contains("poster.jpg"),
		      "a request exposes its browser type as `type`");

		const QString reads_kind =
			"extract = function(p, r){ return { url: String(r[0].kind), "
			"kind: 'direct' }; };";
		const extraction gone = site_extractor::run(reads_kind, page, ev);
		check(gone.ok && gone.url.toString() == "undefined",
		      "and no longer carries a `kind` to be confused with the stream's");

		// The return value keeps `kind`, since that one is the proposal's own
		// conclusion rather than something observed.
		const QString returns_kind =
			"extract = function(p, r){ for (var i=0;i<r.length;i++) "
			"if (r[i].url.indexOf('cf-master')!==-1) "
			"return { url: r[i].url, kind: 'hls' }; return null; };";
		const extractor_verdict v =
			site_extractor::check(returns_kind, page, ev);
		check(v.usable && v.result.kind == "hls",
		      "while the returned object still declares its kind");
	}

	section("page furniture is not a stream");
	{
		// A real run picked a Yandex cookie-sync pixel and was accepted:
		// genuinely requested, fetched once, not the page, so every other rule
		// waved it through and the media list would have offered a tracking
		// pixel as a video. The interceptor knew it was an image all along.
		const QString pick =
			"extract = function(p, r){ for (var i=0;i<r.length;i++) "
			"if (r[i].url.indexOf('%1')!==-1) "
			"return { url: r[i].url, kind: 'direct' }; return null; };";

		const extractor_verdict img =
			site_extractor::check(pick.arg("poster.jpg"), page, ev);
		check(!img.usable && img.is_asset, "an image is refused");
		check(!img.invented && !img.is_segment,
		      "and not mistaken for an invention or a segment");
		check(img.message.contains("image"), "the reason names what it was");

		const extractor_verdict scr =
			site_extractor::check(pick.arg("app.js"), page, ev);
		check(!scr.usable && scr.is_asset, "a script is refused");

		// The manifest is fetched as "other", so the rule leaves it alone.
		check(site_extractor::check(pick.arg("cf-master"), page, ev).usable,
		      "while the manifest still passes");

		// Judged on every sighting, not the first: an address fetched both as
		// an image and as something else is not settled by which came first.
		QList<evidence_request> mixed = sample();
		mixed << evidence_request{ QUrl("https://site.example/both"), "image",
		                            mixed.size() };
		mixed << evidence_request{ QUrl("https://site.example/both"), "other",
		                            mixed.size() };
		const extractor_verdict both =
			site_extractor::check(pick.arg("/both"), page, mixed);
		check(both.usable && !both.is_asset,
		      "an address seen as both an image and something else still passes");
	}

	section("every way of spelling the function");
	{
		// The wrapper claimed all three forms worked. The declaration form did
		// not: it hoists, and the wrapper's own initialiser then overwrote it
		// with null, so the gate said "defines no extract() function" about a
		// script that plainly defined one. A real model wrote exactly this on
		// its first well-formatted answer against real evidence.
		const QString body =
			"{ for (var i=0;i<r.length;i++) if (r[i].url.indexOf('cf-master')!==-1) "
			"return { url: r[i].url, kind: 'hls' }; return null; }";

		const QString decl   = "function extract(p, r) " + body;
		const QString assign = "extract = function (p, r) " + body;
		const QString var_   = "var extract = function (p, r) " + body;

		check(site_extractor::check(decl, page, ev).usable,
		      "a function declaration is accepted");
		check(site_extractor::check(assign, page, ev).usable,
		      "a bare assignment is accepted");
		check(site_extractor::check(var_, page, ev).usable,
		      "and a var-initialised function expression is accepted");

		// The guard still has to fire when there really is no extract().
		const extractor_verdict none =
			site_extractor::check("var other = 1;", page, ev);
		check(!none.usable && none.message.contains("no extract()"),
		      "while a script defining nothing is still refused");
	}

	section("a url too long to show is a trap");
	{
		// summarise() truncates each url for display. The model can only return
		// what it was shown, and the gate compares against the full address —
		// so anything past the display limit is unreturnable by construction.
		// Real evidence has such requests: the analytics calls on the measured
		// site run past 300 characters. No stream has yet been long enough for
		// this to bite, which is exactly why it is worth a check rather than a
		// comment.
		QList<evidence_request> long_ev = sample();
		QString big = "https://long.example/v4/db/abc/cf-master.1774687168.txt?k=";
		big += QString("x").repeated(400);
		long_ev << evidence_request{ QUrl(big), "other", long_ev.size() };

		int kept = 0;
		const QString shown = extractor_dialog::summarise(long_ev, &kept);
		check(!shown.contains(big), "the full address never reaches the payload");

		// A model returning precisely what it was shown is judged an inventor.
		const QString truncated = big.left(300);
		const QString src =
			QString("extract = function(p, r){ return { url: '%1', kind: 'hls' }; };")
				.arg(truncated);
		const extractor_verdict v = site_extractor::check(src, page, long_ev);
		check(!v.usable && v.invented,
		      "and returning the shown form is refused as invented");
	}

	section("the store");
	{
		const QString path = QDir::temp().filePath("hydra-extractors.json");
		QFile::remove(path);
		extractor_store s;
		s.set_for("site.example", "extract = function(){ return null; };", "first try");
		s.set_for("other.example", "extract = function(){ return null; };", "");
		check(s.has("site.example") && s.hosts().size() == 2, "entries are kept");
		check(s.save(path), "saves");

		extractor_store t;
		check(t.load(path), "loads");
		check(t.source_for("site.example").contains("extract"), "source round-trips");
		check(t.note_for("site.example") == "first try", "note round-trips");
		check(!t.has("nobody.example"), "and does not invent hosts");

		t.remove("site.example");
		check(!t.has("site.example"), "removal works");
	}

	section("folding the evidence for review");
	{
		QList<evidence_request> many;
		int n = 0;
		many << evidence_request{ QUrl("https://s.example/watch/1"), "other", n++ };
		many << evidence_request{
			QUrl("https://p.example/db/abc/cf-master.1774687168.txt?k=UCp"), "other", n++ };
		for (int i = 0; i < 250; ++i)
			many << evidence_request{
				QUrl(QString("https://p.example/db/abc/seg-%1.ts")
				         .arg(i, 5, 10, QChar('0'))), "other", n++ };

		int kept = 0;
		const QString folded = extractor_dialog::summarise(many, &kept);
		check(kept == 3, QString("252 requests fold to 3 shapes (%1)").arg(kept));
		check(folded.contains("cf-master"), "the manifest survives folding");
		check(folded.contains("+249 more like this"),
		      "and the repeats are counted rather than silently dropped");
		check(folded.count('\n') + 1 == 3, "one line per shape");
	}

	section("replies that arrive fenced");
	{
		check(extractor_dialog::strip_fences("extract = 1;") == "extract = 1;",
		      "plain source is untouched");
		check(extractor_dialog::strip_fences("```js\nextract = 1;\n```")
		          == "extract = 1;",
		      "a fenced block is unwrapped");
		check(extractor_dialog::strip_fences("```\nextract = 2;\n```")
		          == "extract = 2;",
		      "including one with no language tag");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

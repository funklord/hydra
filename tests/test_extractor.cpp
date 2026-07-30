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

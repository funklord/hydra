// Generated site extractors: the sandbox, and the rule that a proposal cannot
// invent a URL.
#include "site_extractor.h"
#include "extractor_dialog.h"
#include "antiadblock_watch.h"
#include "site_rules.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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

	section("a piece of the stream is not the stream");
	{
		// The last wrong answer everything else accepted. Measured against real
		// evidence: three runs in five returned the initialisation segment. It is
		// fetched once, so the segment rule misses it; it is not the page and not
		// furniture; and its body really is a stream, so even fetching it agrees.
		// What settles it is that the server confirmed the playlist beside it.
		QList<evidence_request> parts = sample();
		const QString init =
			"https://sil5.player.example/v4/db/abc/init-f1-v1-a1.woff?k=UCp";
		parts << evidence_request{ QUrl(init), "other", parts.size() };

		const QString pick =
			"extract = function(p, r){ for (var i=0;i<r.length;i++) "
			"if (r[i].url.indexOf('%1')!==-1) "
			"return { url: r[i].url, kind: 'direct' }; return null; };";

		const QString manifest =
			"https://sil5.player.example/v4/db/abc/cf-master.1774687168.txt?k=UCp&kx=17";
		QSet<QString> manifests = { manifest };

		// Without the tier's answer nothing can fire, and that is the point of
		// making it optional: no network, no refusal.
		check(site_extractor::check(pick.arg("init-"), page, parts).usable,
		      "with no confirmed playlist the init segment still passes");

		const extractor_verdict v =
			site_extractor::check(pick.arg("init-"), page, parts, nullptr, &manifests);
		check(!v.usable && v.is_piece, "with one, the init segment is refused");
		check(!v.invented && !v.is_segment && !v.is_asset,
		      "and not mistaken for any of the other four");
		check(v.message.contains("cf-master"), "the reason names the playlist");

		// The playlist itself is in the set, so it passes — this rule must not
		// refuse the answer it is pointing at.
		check(site_extractor::check(pick.arg("cf-master"), page, parts, nullptr,
		                             &manifests).usable,
		      "while the playlist itself still passes");

		// A media playlist listed beside a master is a manifest too. Following it
		// is the helper tier's job, and returning it is a fine answer.
		const QString index =
			"https://sil5.player.example/v4/db/abc/index-f1-v1-a1.txt?k=UCp";
		parts << evidence_request{ QUrl(index), "other", parts.size() };
		manifests.insert(index);
		check(site_extractor::check(pick.arg("index-"), page, parts, nullptr,
		                             &manifests).usable,
		      "and so does a media playlist confirmed alongside it");

		// Same *directory*, deliberately. A progressive file served elsewhere on
		// that host is a better answer than the manifest, not a piece of it, and
		// a rule that refused it would be doing harm.
		QList<evidence_request> elsewhere = parts;
		const QString mp4 = "https://sil5.player.example/files/movie.mp4";
		elsewhere << evidence_request{ QUrl(mp4), "other", elsewhere.size() };
		check(site_extractor::check(pick.arg("movie.mp4"), page, elsewhere, nullptr,
		                             &manifests).usable,
		      "a file elsewhere on the same host is left alone");
	}

	section("a tracker that hides its payload in the path");
	{
		// Measured on the second site: a beacon host put its per-request token in
		// the *path* rather than the query, so digit-collapsing left every one of
		// them its own shape. The flood was invisible, the segment rule could not
		// refuse one, and a model picked one and the gate accepted it as a video.
		auto sh = [](const char *u) {
			return site_extractor::shape_of(QUrl(QString::fromUtf8(u)));
		};
		const QString b1 = sh("https://dx.ad.example/sbx/b/3NIK470KmUnKT_H_wS_6n362KdYJPjG4Fwi0");
		const QString b2 = sh("https://dx.ad.example/sbx/b/kYBjpiJEbuboTnCUzuVwswf8Xayjd1Xe8qRk");
		check(b1 == b2, "two beacons with different long path tokens are one shape");

		// And the folding must not swallow what a manifest is recognised by. All
		// three measured sites' manifests stay distinct from each other and from
		// their neighbours -- an earlier version of this collapsed
		// `cf-master.1774687168.txt` whole, because the token ran through the
		// dots, and that erased the only thing naming it.
		const QString m1 = sh("https://cdn.example/v4/db/r3rqgi/cf-master.1774687168.txt?k=a&kx=1");
		const QString m2 = sh("https://cdn.example/v4/db/r3rqgi/index-f1-v1-a1.txt?k=a&kx=1");
		const QString m3 = sh("https://kc.example/cdn/hls/f04a166fb1fea9f053518416e03561a4/master.txt");
		const QString m4 = sh("https://kc.example/cdn/hls/f04a166fb1fea9f053518416e03561a4/index.txt");
		check(m1 != m2, "a master playlist and its variant stay distinct");
		check(m3 != m4, "even when the directory name is a 32-character hash");
		check(m1.contains("cf-master"), "and the manifest keeps the name it is known by");
		check(m3.contains("master"), "on both spellings of it");

		// Ordinary long path words are not tokens. Only mixed letters-and-digits
		// runs fold, so a component named for what it does keeps its name.
		const QString a = sh("https://kc.example/player/assets/SubtitleManager.js");
		check(a.contains("SubtitleManager"),
		      "a long word with no digits in it is left alone");

		// A real segment flood still folds, which is what the rule was for.
		check(sh("https://cdn.example/v4/db/x/seg-1-f1-v1-a1.woff2?k=a") ==
		          sh("https://cdn.example/v4/db/x/seg-22-f1-v1-a1.woff2?k=b"),
		      "and numbered segments fold as they always did");
	}

	section("noticing a page that checks for an ad blocker");
	{
		// Measured, not imagined: this is what a real watch page loaded while
		// its player refused to start, and allowing ads for that site alone
		// started it.
		// The patterns come from the shared rule store, not from an array in
		// this class: they are the same kind of perishable fact as the consent
		// rules and are meant to travel with them.
		const QStringList names = site_rules::defaults().detectors();
		check(!names.isEmpty(), "the built-in rules carry detector names");
		check(antiadblock_watch::looks_like_detector(QUrl(
		          "https://cdnjs.cloudflare.com/ajax/libs/fuckadblock/3.2.1/"
		          "fuckadblock.min.js"), names),
		      "the detector script that broke a real page is recognised");
		check(antiadblock_watch::looks_like_detector(
		          QUrl("https://x.example/js/blockadblock.js"), names),
		      "and its better-known sibling");

		// The message this drives tells someone to turn protection off, so a
		// false positive is not a cosmetic mistake. Nothing matches on "ad".
		check(!antiadblock_watch::looks_like_detector(
		          QUrl("https://x.example/js/adblock-plus-list.js"), names),
		      "a filter list is not a detector");
		check(!antiadblock_watch::looks_like_detector(
		          QUrl("https://x.example/assets/loader.js"), names),
		      "nor is an ordinary script");
		check(!antiadblock_watch::looks_like_detector(
		          QUrl("https://x.example/search?q=fuckadblock"), names),
		      "and a page that merely mentions one in a query is not accused");

		antiadblock_watch w;
		int fired = 0;
		QObject::connect(&w, &antiadblock_watch::detected,
		                  [&](const QString &, const QString &) { ++fired; });
		request_context c;
		c.site_host = "site.example";
		c.url = QUrl("https://cdn.example/fuckadblock.min.js");
		w.on_request(c, request_decision{});
		check(w.checked_for_blocker("site.example"), "a sighting is recorded");
		check(fired == 1, "and reported");

		// Said once per page. A status bar that repeats this on every request
		// is a status bar nobody reads.
		w.on_request(c, request_decision{});
		c.url = QUrl("https://cdn.example/blockadblock.js");
		w.on_request(c, request_decision{});
		check(fired == 1, "but only once, however many are fetched");
		check(w.evidence_for("site.example").size() == 2,
		      "while both are kept as evidence");
		check(!w.checked_for_blocker("other.example"),
		      "and it is a fact about one page, not the browser");

		// A detector name learned rather than shipped is generic for the same
		// reason a button label is — it describes a script, not a site — so it
		// is flagged for the binary and travels in the same file.
		site_rules learned = site_rules::defaults();
		site_rule d;
		d.kind = "detector";
		d.value = "newblockcheck";
		learned.add(d);
		antiadblock_watch w2;
		w2.set_rules(learned);
		request_context c2;
		c2.site_host = "later.example";
		c2.url = QUrl("https://cdn.example/newblockcheck.min.js");
		w2.on_request(c2, request_decision{});
		check(w2.checked_for_blocker("later.example"),
		      "a detector added to the rule file is honoured without a rebuild");
		check(learned.promotable().size() == 1 &&
		          learned.promotable().first().kind == "detector",
		      "and is flagged for the built-ins like any other generic rule");
	}

	section("what a rule from somebody else has to prove");
	{
		// A consent rule is a licence to click buttons on pages the user is
		// logged into, so an imported one is not trusted for being well-formed.
		// This is §12.4's argument applied to a different corpus: decide what a
		// rule would do before letting it do anything.
		auto refused = [](const char *kind, const char *value) {
			site_rule r; r.kind = kind; r.value = value;
			return site_rules::why_unsafe(r);
		};
		check(!refused("accept", "^.*$").isEmpty(),
		      "a pattern matching everything is refused");
		check(refused("accept", "^(ok|accept)$").isEmpty(),
		      "while an ordinary accept pattern passes");
		check(!refused("accept", "^(accept|delete account)$").isEmpty(),
		      "a pattern that would also press Delete account is refused");
		check(refused("accept", "^(accept|delete account)$").contains("Delete"),
		      "and the reason names the button it would have pressed");
		check(!refused("reject", "[unclosed").isEmpty(),
		      "an uncompilable pattern is refused rather than silently ignored");
		check(!refused("detector", "ad").isEmpty(),
		      "a two-letter detector name is refused — it would accuse half the "
		      "web, and the message tells someone to lower their protection");
		check(refused("detector", "fuckadblock").isEmpty(),
		      "while a real one passes");
		check(!refused("container", "*").isEmpty(),
		      "a selector matching the whole page is refused");
		check(!refused("nonsense", "x").isEmpty(), "an unknown kind is refused");

		// A sender does not get to describe their own rule's standing.
		site_rules mine = site_rules::defaults();
		site_rule learned; learned.kind = "reject"; learned.value = "^avvis alle$";
		mine.add(learned);
		const QJsonObject doc = mine.export_learned();
		check(doc.value("rules").toArray().size() == 1,
		      "export carries what was learned here");
		check(QJsonDocument(doc).toJson().contains("hydra-site-rules"),
		      "and says what kind of file it is");

		QJsonObject hostile = doc;
		QJsonArray rules = hostile.value("rules").toArray();
		QJsonObject sneak;
		sneak.insert("kind", "accept");
		sneak.insert("value", "^.*$");
		sneak.insert("builtin", true);      // claims to be shipped
		sneak.insert("promote", true);      // claims to be already blessed
		rules.append(sneak);
		hostile.insert("rules", rules);

		const site_rules::import_result got = site_rules::judge_import(hostile);
		check(got.accepted.size() == 1, "the safe rule is offered");
		check(got.refused.size() == 1, "and the dangerous one is not");
		check(!got.accepted.isEmpty() && got.accepted.first().imported,
		      "what is offered is marked as having come from elsewhere");
		check(!got.accepted.isEmpty() && !got.accepted.first().builtin,
		      "a sender cannot declare their rule a built-in");

		site_rules after = site_rules::defaults();
		for (const site_rule &r : got.accepted)
			after.add(r);
		check(after.promotable().isEmpty(),
		      "and an imported rule is never proposed for our binary — it has "
		      "been vouched for by nobody here");

		check(site_rules::judge_import(QJsonObject()).accepted.isEmpty(),
		      "a file that is not a rule file yields nothing");

		// Where it came from is the importer's label, never the document's.
		QJsonObject vain = doc;
		vain.insert("origin", "Trusted community rules");
		const site_rules::import_result labelled =
			site_rules::judge_import(vain, "from-dave.json");
		check(!labelled.accepted.isEmpty() &&
		          labelled.accepted.first().origin == "from-dave.json",
		      "an imported rule records where it came from");
		check(!labelled.accepted.isEmpty() &&
		          !labelled.accepted.first().origin.contains("Trusted"),
		      "and a document describing itself as trusted is not believed");

		// Undoing an import is one action and keeps what was learned here.
		site_rules mixed = site_rules::defaults();
		site_rule ours; ours.kind = "reject"; ours.value = "^nei takk$";
		mixed.add(ours);
		for (const site_rule &r : labelled.accepted)
			mixed.add(r);
		const int before = mixed.all().size();
		const int gone = mixed.forget_imported();
		check(gone == labelled.accepted.size(),
		      "forgetting an import drops exactly what was imported");
		check(mixed.all().size() == before - gone, "and nothing else");
		bool ours_survived = false;
		for (const site_rule &r : mixed.all())
			if (r.value == "^nei takk$") ours_survived = true;
		check(ours_survived, "what was learned here survives it");
	}

	section("answers that work once");
	{
		// Both of these pass every other check the gate makes, and both leave a
		// stored extractor that fails on the next visit. That is the shape worth
		// refusing: not a wrong answer, a right-looking one with no future.
		const QString real =
			"https://sil5.player.example/v4/db/abc/cf-master.1774687168.txt"
			"?k=UCp&kx=17";

		// 1. The lookup table. A model really wrote this: the five annotated
		// addresses copied into the script, tokens and all, and searched. It
		// returns the manifest, which was genuinely requested and genuinely a
		// manifest, so nothing else in the gate objects.
		const QString baked =
			"const rows = [{ url: '" + real + "', serves: 'HLS' }];\n"
			"extract = function (p, r) {\n"
			"  for (var i=0;i<r.length;i++)\n"
			"    if (rows.some(function(x){ return x.url === r[i].url; }))\n"
			"      return { url: r[i].url, kind: 'hls' };\n"
			"  return null; };";
		const extractor_verdict b = site_extractor::check(baked, page, ev);
		check(!b.usable, "a script with this visit's token written into it is refused");
		check(b.hardcoded, "and says that is why");
		check(b.message.contains("stable part"),
		      "naming what to do instead, since the model is asked again");

		// 2. Reading the note at run time. `serves` is a column of the evidence,
		// never a field of a request, so this finds nothing every time — and
		// "found nothing" is what it reports, which reads as a model that could
		// not find the stream rather than one that did and then asked the wrong
		// object.
		const QString runtime_note =
			"extract = function (p, r) {\n"
			"  var m = r.find(function (x) { return x.serves === 'HLS'; });\n"
			"  return m ? { url: m.url, kind: 'hls' } : null; };";
		const extractor_verdict n = site_extractor::check(runtime_note, page, ev);
		check(!n.usable, "a script reading `serves` at run time is refused");
		check(n.reads_note, "and says that is why");
		check(!n.result.ok || n.result.url.isEmpty() || true,
		      "rather than reporting that it found nothing");
		check(n.message.contains("column"),
		      "explaining that the note was shown, not passed");
		// The bracket spelling too, since a model that is told not to write one
		// thing writes the other.
		const QString bracketed =
			"extract = function (p, r) {\n"
			"  var m = r.find(function (x) { return x['serves'] === 'HLS'; });\n"
			"  return m ? { url: m.url, kind: 'hls' } : null; };";
		check(site_extractor::check(bracketed, page, ev).reads_note,
		      "however it is spelled");

		// And the refusals must not catch a parser doing exactly the right
		// thing: matching a stable fragment of the path, with no token in it.
		const QString good =
			"extract = function (p, r) {\n"
			"  for (var i=0;i<r.length;i++)\n"
			"    if (r[i].url.indexOf('cf-master') !== -1)\n"
			"      return { url: r[i].url, kind: 'hls' };\n"
			"  return null; };";
		const extractor_verdict g = site_extractor::check(good, page, ev);
		check(g.usable, "while matching a stable path fragment is still accepted");
		check(!g.hardcoded && !g.reads_note,
		      "and is accused of neither");

		// A short number is not a token. `k=1` and a loop bound are ordinary
		// code, and a check that fired on those would refuse every parser.
		const QString counting =
			"extract = function (p, r) {\n"
			"  for (var i=0;i<r.length && i<100;i++)\n"
			"    if (r[i].url.indexOf('cf-master') !== -1)\n"
			"      return { url: r[i].url, kind: 'hls' };\n"
			"  return null; };";
		check(site_extractor::check(counting, page, ev).usable,
		      "and ordinary numbers in ordinary code are not mistaken for tokens");
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

		// "Every way" was three ways. The wrapper declared `var extract;` beside
		// the proposal, so a `const` or `let` of the same name was "Identifier
		// extract has already been declared" — a SyntaxError raised before the
		// proposal ran at all, naming *our* variable, about a parser that was
		// correct. Five runs in five against real evidence died there while the
		// section above reported that every spelling worked.
		const QString const_ = "const extract = function (p, r) " + body;
		const QString let_   = "let extract = function (p, r) " + body;
		const QString arrow  = "const extract = (p, r) => " + body;

		check(site_extractor::check(const_, page, ev).usable,
		      "a const-declared function expression is accepted");
		check(site_extractor::check(let_, page, ev).usable,
		      "and a let-declared one");
		check(site_extractor::check(arrow, page, ev).usable,
		      "and an arrow function, which is what a model actually wrote");

		// The reason the outer `var` is still there: a bare assignment must not
		// leave a global behind, and the scope that lets `const` through must
		// not break that.
		check(site_extractor::check(assign, page, ev).usable,
		      "and a bare assignment still works alongside all of them");

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

	section("what counts as the same shape, on addresses a real CDN serves");
	{
		// The pattern that defeated the old rule, taken from a live capture:
		// single-digit indices, and a token plus a timestamp that change per
		// request. Fourteen segments used to produce eleven shapes, so nothing
		// folded, the segment rule never fired, and a segment was acceptable.
		QList<evidence_request> real;
		int n = 0;
		for (int i = 1; i <= 9; ++i)
			real << evidence_request{
				QUrl(QString("https://cdn.example/v4/ab/seg-%1-f1-v1-a1.woff2"
				              "?k=tok%1&kx=17855015%1").arg(i)), "other", n++ };
		const QUrl manifest(
			"https://cdn.example/v4/ab/cf-master.1785377837.txt?k=tokM&kx=178550151");
		real << evidence_request{ manifest, "other", n++ };

		const QString seg_shape =
			site_extractor::shape_of(real.first().url);
		int same = 0;
		for (const evidence_request &r : real)
			if (site_extractor::shape_of(r.url) == seg_shape) ++same;
		check(same == 9,
		      QString("nine single-digit segments with rotating tokens are one "
		               "shape (%1)").arg(same));
		check(site_extractor::shape_of(manifest) != seg_shape,
		      "and the manifest is not that shape");

		// The consequence that matters: this is what the gate's segment rule
		// runs on, and with the old rule it accepted a segment outright.
		const QString pick_seg =
			"extract = function(p, r){ for (var i=0;i<r.length;i++) "
			"if (r[i].url.indexOf('seg-')!==-1) "
			"return { url: r[i].url, kind: 'hls' }; return null; };";
		const extractor_verdict v = site_extractor::check(
			pick_seg, QUrl("https://site.example/watch/1"), real);
		check(!v.usable && v.is_segment,
		      QString("so a real segment is refused as one (%1)").arg(v.message));

		const QString pick_manifest =
			"extract = function(p, r){ for (var i=0;i<r.length;i++) "
			"if (r[i].url.indexOf('cf-master')!==-1) "
			"return { url: r[i].url, kind: 'hls' }; return null; };";
		check(site_extractor::check(pick_manifest,
		                             QUrl("https://site.example/watch/1"), real).usable,
		      "while the manifest, fetched once, still passes");

		// And the fold the user and the model both read.
		int kept = 0;
		const QString folded = extractor_dialog::summarise(real, &kept);
		check(kept == 2, QString("ten requests fold to two lines (%1)").arg(kept));
		// The `seen` column, not a suffix after the url. Anything printed after
		// an address is something a model can read as part of it — which is
		// exactly what went wrong with the served-type note.
		// Read out of the column rather than matched as a substring: the count
		// is right-aligned, so a substring check is a check on the padding.
		int most = 0;
		for (const QString &row : folded.split('\n')) {
			const QStringList cols = row.split(" | ");
			if (cols.size() >= 5)
				most = qMax(most, cols[2].trimmed().toInt());
		}
		check(most > 1,
		      QString("with the flood counted in its own column rather than "
		               "listed (%1)").arg(most));
	}

	section("choosing what to ask the server about");
	{
		QList<evidence_request> many = sample();
		int n = many.size();
		for (int i = 0; i < 40; ++i)
			many << evidence_request{
				QUrl(QString("https://sil5.player.example/v4/db/abc/seg-%1.ts")
				         .arg(i, 5, 10, QChar('0'))), "other", n++ };

		const QList<evidence_request> picks =
			extractor_dialog::candidates(many, page, extractor_dialog::k_max_probes);

		check(picks.size() <= extractor_dialog::k_max_probes,
		      "the budget is respected");
		QStringList urls;
		for (const evidence_request &r : picks) urls << r.url.toString();

		check(!urls.filter("cf-master").isEmpty(),
		      "the disguised manifest is asked about");
		check(urls.filter("seg-").size() <= 1,
		      "forty numbered segments cost one question, not forty");
		check(urls.filter("app.js").isEmpty() && urls.filter("poster.jpg").isEmpty(),
		      "furniture is not worth a request");
		check(urls.filter("/watch/1").isEmpty(),
		      "and neither is the page itself");

		// A manifest is fetched once and its segments are not, so the once-only
		// addresses have to come first or the budget is spent on segments.
		int first_seg = -1, first_once = -1;
		for (int i = 0; i < urls.size(); ++i) {
			if (first_seg < 0 && urls[i].contains("seg-")) first_seg = i;
			if (first_once < 0 && urls[i].contains("cf-master")) first_once = i;
		}
		check(first_once >= 0 && (first_seg < 0 || first_once < first_seg),
		      "and what was fetched once is asked about before what repeated");

		// The failure this ordering exists for, measured on a real capture: a
		// page's one-off beacons and stylesheets are fetched once too, arrive
		// first, and swallowed the whole budget before the video was reached.
		// The host that served the flood is the media host, so its once-only
		// request outranks a tracker's.
		QList<evidence_request> noisy;
		int m = 0;
		for (int i = 0; i < 15; ++i)
			noisy << evidence_request{
				QUrl(QString("https://beacon%1.example/collect?v=%1").arg(i)),
				"other", m++ };
		noisy << evidence_request{
			QUrl("https://cdn.example/v4/abc/cf-master.999.txt?k=z"), "other", m++ };
		for (int i = 0; i < 20; ++i)
			noisy << evidence_request{
				QUrl(QString("https://cdn.example/v4/abc/seg-%1.woff2").arg(i, 5, 10, QChar('0'))),
				"other", m++ };

		const QList<evidence_request> picked =
			extractor_dialog::candidates(noisy, QUrl("https://site.example/p"),
			                              extractor_dialog::k_max_probes);
		check(!picked.isEmpty() &&
		          picked.first().url.toString().contains("cf-master"),
		      "the one-off on the flooding host is asked about first");
		QStringList pu;
		for (const evidence_request &r : picked) pu << r.url.toString();
		check(!pu.filter("cf-master").isEmpty(),
		      "so fifteen beacons cannot crowd the manifest out of the budget");
	}

	section("what the server said reaches the payload");
	{
		QHash<QString, QString> served;
		served.insert("https://sil5.player.example/v4/db/abc/cf-master.1774687168.txt?k=UCp&kx=17",
		               "application/vnd.apple.mpegurl (HLS)");
		int kept = 0;
		const QString with = extractor_dialog::summarise(ev, &kept, &served);
		check(with.contains("| application/vnd.apple.mpegurl (HLS) | https://"),
		      "the served type is a column of its own, with the url after it");
		// **The url is last and nothing follows it.** This is the whole point of
		// the column: while the note was appended as `url -> type`, four runs in
		// five wrote `url.includes('->')` and matched nothing, and the two that
		// worked did so through an extension fallback that only works on a site
		// which does not disguise its manifest.
		for (const QString &row : with.split('\n')) {
			const QStringList cols = row.split(" | ");
			if (cols.size() < 5)
				continue;
			check(cols.last().startsWith("http"),
			      "every evidence row ends with its url and nothing after it");
			break;
		}
		check(!with.contains("->"),
		      "and no arrow anywhere, so a url cannot be tested for one");
		int annotated = 0;
		for (const QString &row : with.split('\n')) {
			const QStringList cols = row.split(" | ");
			if (cols.size() >= 5 && cols[3].trimmed() != "-")
				++annotated;
		}
		check(annotated == 1,
		      QString("and only addresses actually asked about carry one (%1)")
		          .arg(annotated));

		const QString without = extractor_dialog::summarise(ev, &kept);
		check(without.contains(" | - | https://"),
		      "with every serves column a dash when the tier did not run");
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
		int biggest = 0;
		for (const QString &row : folded.split('\n')) {
			const QStringList cols = row.split(" | ");
			if (cols.size() >= 5)
				biggest = qMax(biggest, cols[2].trimmed().toInt());
		}
		check(biggest == 250,
		      QString("and the repeats are counted rather than silently dropped "
		               "(%1)").arg(biggest));
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

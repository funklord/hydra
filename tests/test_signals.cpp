// Passive evidence for the filter-evolution loop (architecture doc §12.1).
//
// Two lists come out of here and they are not the same thing. *Observed* is
// everything a page asked for — the corpus a proposed rule is simulated against,
// so a dry-run can say "this would have blocked four of these". *Suspects* is
// the much smaller set that got through and looks ad-shaped, which is what the
// model is shown as evidence.
//
// Getting the difference wrong is not a crash. It is a dry-run that reports a
// rule as harmless because the corpus was too small, or a model proposing rules
// against the site's own assets because the first-party filter let them through.
#include "filter_signals.h"

#include <QCoreApplication>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static request_context ctx_for(const QString &url, const QString &site) {
	request_context c;
	c.url          = QUrl(url);
	c.request_host = c.url.host();
	c.site_host    = site;
	return c;
}
static void feed(filter_signals &s, const QString &url, const QString &site,
                  bool blocked = false) {
	request_decision d;
	d.block = blocked;
	s.on_request(ctx_for(url, site), d);
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("what counts as ad-shaped");
	{
		using fs = filter_signals;
		check(fs::looks_ad_shaped("https://ads.other.example/ads/x.js",
		                           "ads.other.example", "news.example"),
		      "third-party with an ad-shaped path");
		check(fs::looks_ad_shaped("https://t.other.example/PIXEL/1.gif",
		                           "t.other.example", "news.example"),
		      "and the shapes are matched without regard to case");

		// The rule that keeps a filter list from breaking the page it was meant
		// to fix: a site's own requests are not the ad, whatever they look like.
		check(!fs::looks_ad_shaped("https://news.example/ads/house-ad.js",
		                            "news.example", "news.example"),
		      "the site's own request is never a suspect, ad-shaped or not");
		check(!fs::looks_ad_shaped("https://static.news.example/ads/x.js",
		                            "static.news.example", "news.example"),
		      "and neither is its own subdomain");
		check(fs::looks_ad_shaped("https://notnews.example/ads/x.js",
		                            "notnews.example", "news.example"),
		      "but a host that merely ends with the same letters is third-party");

		check(!fs::looks_ad_shaped("https://other.example/app.js",
		                            "other.example", "news.example"),
		      "a third-party request with an ordinary path is not evidence");
		check(!fs::looks_ad_shaped("https://other.example/ads/x.js", "", "news.example"),
		      "no request host, no judgement");
		check(!fs::looks_ad_shaped("https://other.example/ads/x.js", "other.example", ""),
		      "and no site host either — first-party cannot be decided without one");
	}

	section("the two lists are different lists");
	{
		filter_signals s;
		feed(s, "https://news.example/style.css", "news.example");
		feed(s, "https://other.example/app.js", "news.example");
		feed(s, "https://ads.other.example/ads/banner.js", "news.example");

		check(s.observed_for("news.example").size() == 3,
		      QString("everything the page asked for is observed (%1)")
		          .arg(s.observed_for("news.example").size()));
		check(s.suspects_for("news.example") ==
		          QStringList({"https://ads.other.example/ads/banner.js"}),
		      QString("but only the ad-shaped third party is a suspect (%1)")
		          .arg(s.suspects_for("news.example").join(", ")));
		check(s.count_for("news.example") == 1, "and the count is of suspects");
	}

	section("a blocked request is the system working, not a gap");
	{
		filter_signals s;
		feed(s, "https://ads.other.example/ads/banner.js", "news.example", true);
		check(s.suspects_for("news.example").isEmpty(),
		      "something already blocked is not evidence of a missing rule");
		check(s.observed_for("news.example").size() == 1,
		      "though it is still part of the corpus a rule is simulated against");
	}

	section("repeats do not inflate the evidence");
	{
		filter_signals s;
		for (int i = 0; i < 50; ++i) {
			feed(s, "https://ads.other.example/ads/banner.js", "news.example");
			feed(s, "https://news.example/logo.png", "news.example");
		}
		check(s.suspects_for("news.example").size() == 1,
		      QString("the same url fifty times is one suspect (%1)")
		          .arg(s.suspects_for("news.example").size()));
		check(s.observed_for("news.example").size() == 2,
		      "and two observations, not a hundred");
	}

	section("one page's evidence is not another's");
	{
		filter_signals s;
		feed(s, "https://ads.other.example/ads/a.js", "one.example");
		feed(s, "https://ads.other.example/ads/b.js", "two.example");
		check(s.suspects_for("one.example").size() == 1 &&
		          s.suspects_for("two.example").size() == 1,
		      "each site keeps its own");
		check(s.suspects_for("one.example") != s.suspects_for("two.example"),
		      "and they are not the same evidence");
		check(s.suspects_for("three.example").isEmpty(),
		      "a site never visited has none rather than everyone else's");

		s.clear_site("one.example");
		check(s.suspects_for("one.example").isEmpty() &&
		          s.observed_for("one.example").isEmpty(),
		      "clearing a site clears both of its lists");
		check(s.suspects_for("two.example").size() == 1,
		      "and leaves the other site alone");
	}

	section("a page cannot make the evidence unbounded");
	{
		// A page that requests thousands of distinct urls -- infinite scroll, a
		// tracker with a nonce in every path -- must not grow this without limit.
		// It is fed from the interceptor on every request and read on the UI
		// thread, so unbounded here is a memory leak that a site controls.
		filter_signals s;
		for (int i = 0; i < 600; ++i)
			feed(s, QString("https://ads.other.example/ads/%1.js").arg(i), "big.example");
		check(s.suspects_for("big.example").size() <= 400,
		      QString("suspects are capped (%1)").arg(s.suspects_for("big.example").size()));
		check(s.observed_for("big.example").size() <= 400,
		      QString("and so is the corpus (%1)").arg(s.observed_for("big.example").size()));
		check(s.suspects_for("big.example").size() >= 100,
		      "while still keeping enough to be evidence");
	}

	section("what is not recorded at all");
	{
		filter_signals s;
		feed(s, "https://ads.other.example/ads/x.js", "");
		check(s.observed_for("").isEmpty(),
		      "a request with no page behind it belongs to no site");
		feed(s, "", "news.example");
		check(s.observed_for("news.example").isEmpty(),
		      "and an empty url is not a request");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

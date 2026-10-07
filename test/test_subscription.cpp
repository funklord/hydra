// Subscribing to an upstream filter list (architecture doc sec 12.5).
//
// `filter_list`'s header asked for this shape before anything could subscribe:
// the user's own rules are "kept deliberately apart from any imported EasyList
// so a scheduled upstream update never clobbers custom rules". So the reading
// half is what these cases are about -- what of a real list this build can
// enforce, and what it must refuse outright.
//
// The refusal cases are the ones that matter most, and none of them is a
// malformed rule: they are a 200 carrying the wrong body. That is the fetch
// failure that would otherwise promote an empty list over a working one and
// turn ad blocking off without saying so.
#include "filter_subscription.h"
#include "filter_list.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

using filter_subscription::line_kind;

static QString kind_name(line_kind k) {
	switch (k) {
		case line_kind::comment:     return QStringLiteral("comment");
		case line_kind::network:     return QStringLiteral("network");
		case line_kind::cosmetic:    return QStringLiteral("cosmetic");
		case line_kind::unsupported: return QStringLiteral("unsupported");
		case line_kind::unsafe:      return QStringLiteral("unsafe");
	}
	return QStringLiteral("?");
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("every line of a real list falls in a named bucket");
	{
		// **One case per syntax, and the point is that the buckets are named.**
		// A subscription that contributes a tenth of its lines is worth having
		// or is not, and the person deciding needs to know which tenth -- so
		// "not a rule" is not an answer this gives.
		struct one { const char *line; line_kind want; const char *what; };
		const QList<one> cases = {
			{ "[Adblock Plus 2.0]", line_kind::comment, "a list header" },
			{ "! Title: EasyList",  line_kind::comment, "a comment" },
			{ "",                   line_kind::comment, "a blank line" },
			{ "||ads.example.com^", line_kind::network, "a host rule" },
			{ "/banner_ad.",        line_kind::network, "a substring rule" },
			{ "||cdn.example.com/*/ads/", line_kind::network,
			   "a wildcard rule" },
			{ "example.com##.ad-banner", line_kind::cosmetic,
			   "a scoped element-hiding rule" },
			// The five this build does not implement, each for its own reason.
			{ "@@||ads.example.com^", line_kind::unsupported,
			   "an exception rule" },
			{ "||ads.example.com^$third-party", line_kind::unsupported,
			   "a rule carrying options" },
			{ "/^https?:\\/\\/ads\\./", line_kind::unsupported,
			   "a regular-expression rule" },
			{ "youtube.com##+js(json-prune, adPlacements)",
			   line_kind::unsupported, "a scriptlet" },
			{ "example.com##^script:has-text(ads)", line_kind::unsupported,
			   "an HTML filter" },
			{ "example.com#?#div:has(> .ad)", line_kind::unsupported,
			   "a procedural cosmetic rule" },
			{ "example.com#@#.ad-banner", line_kind::unsupported,
			   "a cosmetic exception" },
			{ "##.ad-banner", line_kind::unsupported,
			   "a generic cosmetic rule, applied on no site" },
			// And the one that is refused rather than merely unread.
			{ "example.com##.ad}body{display:none", line_kind::unsafe,
			   "a selector that closes its own rule" },
		};
		for (const one &c : cases) {
			QString why;
			const line_kind got =
			  filter_subscription::classify(QString::fromLatin1(c.line), nullptr,
			                                 &why);
			check(got == c.want,
			       QString("%1 is %2 (%3)%4")
			           .arg(QString::fromLatin1(c.what), kind_name(c.want),
			                 kind_name(got),
			                 why.isEmpty() ? QString()
			                               : QString(" -- ") + why.left(60)));
		}
	}

	section("a body that is not a filter list is refused whole");
	{
		// **Each of these parses as nothing and would promote an empty list
		// over a working one.** That is ad blocking turning itself off and
		// reading like an upstream that got quieter.
		const subscription_read empty = filter_subscription::read(QString());
		check(!empty.ok() && empty.refusal.contains("empty"),
		       QString("an empty body (%1)").arg(empty.refusal));

		const subscription_read blank = filter_subscription::read("\n\n   \n");
		check(!blank.ok(),
		       QString("a body of whitespace (%1)").arg(blank.refusal));

		const subscription_read page = filter_subscription::read(
		  "<!DOCTYPE html>\n<html><body>Sign in to continue</body></html>\n");
		check(!page.ok() && page.refusal.contains("web page"),
		       QString("a captive portal's login page (%1)")
		           .arg(page.refusal.left(70)));

		// Comments only: a real file, nothing enforceable in it.
		const subscription_read heads = filter_subscription::read(
		  "[Adblock Plus 2.0]\n! Title: Nothing\n! Expires: 1 day\n");
		check(!heads.ok() && heads.refusal.contains("no rule"),
		       QString("a header with no rules (%1)").arg(heads.refusal));

		// And a list of nothing but syntax this build cannot read, which is
		// the same outcome by a different route -- and the refusal says how
		// many lines it looked at rather than claiming the file was empty.
		const subscription_read unread = filter_subscription::read(
		  "@@||a.example^\n@@||b.example^\nexample.com##+js(nowebrtc)\n");
		check(!unread.ok() && unread.refusal.contains("3 candidate"),
		       QString("three lines, none enforceable (%1)")
		           .arg(unread.refusal));
	}

	section("a short fetch does not replace a long list");
	{
		// **The shrink guard.** A truncated or half-migrated body parses
		// cleanly and is simply short, so the rule count is the only signal
		// that the fetch was not the list.
		const QString small = "||one.example^\n||two.example^\n";
		const subscription_read first = filter_subscription::read(small);
		check(first.ok() && first.accepted == 2,
		       QString("two rules, with nothing to compare against (%1)")
		           .arg(first.summary()));

		const subscription_read against_nine =
		  filter_subscription::read(small, 9);
		check(!against_nine.ok() && against_nine.refusal.contains("2 rule"),
		       QString("refused against a copy holding nine (%1)")
		           .arg(against_nine.refusal.left(60)));

		// A quarter is the line, so eight is accepted and nine is not: the
		// threshold is asserted at the boundary rather than somewhere safely
		// inside it, since a threshold nobody tested at the edge is a number
		// somebody will quietly change.
		const subscription_read against_eight =
		  filter_subscription::read(small, 8);
		check(against_eight.ok(),
		       QString("and accepted against a copy holding eight (%1)")
		           .arg(against_eight.summary()));
	}

	section("what it reports about a mixed list, and the rules it hands back");
	{
		const QString list =
		  "[Adblock Plus 2.0]\n"
		  "! Title: Mixed\n"
		  "||ads.example.com^\n"
		  "||tracker.example.net^\n"
		  "/sponsored_banner.\n"
		  "example.com##.ad-slot\n"
		  "@@||ads.example.com^$document\n"
		  "||cdn.example.com^$script\n"
		  "youtube.com##+js(json-prune, adPlacements)\n";
		const subscription_read rep = filter_subscription::read(list);
		check(rep.ok(), QString("the list is usable (%1)").arg(rep.summary()));
		check(rep.accepted == 4,
		       QString("four rules this build enforces (%1)").arg(rep.accepted));
		check(rep.unsupported == 3,
		       QString("three lines it cannot (%1)").arg(rep.unsupported));
		check(rep.lines == 7,
		       QString("seven candidate lines, the header and comment not "
		                "counted as a gap (%1)").arg(rep.lines));
		check(rep.summary().contains("4 rule(s) in use") &&
		          rep.summary().contains("3 line(s)"),
		       QString("and the summary names both numbers (%1)")
		           .arg(rep.summary()));

		// **The rules have to be ones the engine enforces, not text it
		// stored.** Reading a list correctly and handing back rules nothing
		// acts on is the shape this tree has met before: a correct function
		// and no working feature. So they go into a list and are asked.
		filter_list subscribed;
		for (const filter_rule &r : rep.rules)
			subscribed.add(r);
		check(subscribed.blocks("https://ads.example.com/a.js", "news.test"),
		       "a host rule from the list blocks its host");
		check(subscribed.blocks("https://x.test/sponsored_banner.png",
		                         "news.test"),
		       "and a substring rule matches inside a path");
		check(!subscribed.blocks("https://cdn.example.com/app.js", "news.test"),
		       "while the option-carrying rule blocks nothing, as counted");
		check(!subscribed.blocks("https://news.test/index.html", "news.test"),
		       "and the page's own address is untouched");
	}

	section("the cosmetic rules it keeps are scoped and safe");
	{
		const subscription_read rep = filter_subscription::read(
		  "a.example##.promo\n"
		  "##.promo\n"
		  "b.example##.x}body{display:none\n");
		check(rep.ok() && rep.accepted == 1,
		       QString("one of the three is kept (%1)").arg(rep.summary()));
		check(rep.unsafe == 1,
		       QString("the selector that closes its rule is refused (%1)")
		           .arg(rep.unsafe));
		check(rep.unsupported == 1,
		       QString("and the unscoped one is unread (%1)")
		           .arg(rep.unsupported));
		check(rep.rules.size() == 1 && rep.rules.first().cosmetic &&
		          rep.rules.first().scope == "a.example",
		       QString("what survives is the scoped one (%1)")
		           .arg(rep.rules.isEmpty() ? QString("nothing")
		                                     : rep.rules.first().text));
	}

	section("the index round-trips, and a cache name is never a path");
	{
		QTemporaryDir dir;
		check(dir.isValid(), "a scratch directory of this process's own");
		const QString index = QDir(dir.path()).filePath("subs.json");

		// A missing index is the ordinary first run and says nothing.
		check(filter_subscription::load_index(index).isEmpty(),
		       "no index yet reads as no subscriptions");

		subscription a;
		a.name    = "EasyList";
		a.url     = QUrl("https://easylist.test/easylist.txt");
		a.file    = "easylist.txt";
		a.rules   = 1234;
		a.note    = "1234 rule(s) in use";
		a.fetched = QDateTime::fromString("2026-10-07T12:00:00", Qt::ISODate);
		subscription b;
		b.name    = "Off for now";
		b.url     = QUrl("https://other.test/list.txt");
		b.file    = "off-for-now.txt";
		b.enabled = false;

		check(filter_subscription::save_index(index, { a, b }),
		       "the index saves");
		const QList<subscription> back =
		  filter_subscription::load_index(index);
		check(back.size() == 2,
		       QString("and loads both (%1)").arg(back.size()));
		if (back.size() == 2) {
			check(back.at(0).name == a.name && back.at(0).url == a.url &&
			          back.at(0).file == a.file && back.at(0).rules == 1234 &&
			          back.at(0).enabled,
			       "with every field of the first intact");
			check(back.at(0).fetched == a.fetched,
			       QString("including when it was last fetched (%1)")
			           .arg(back.at(0).fetched.toString(Qt::ISODate)));
			// **The one that would be lost by a default.** `toBool()` with no
			// argument answers false, and `toBool(true)` answers true for a
			// key that is absent -- so a disabled subscription read back as
			// enabled would quietly start blocking again, and the default
			// that avoids that is the wrong one for an absent key.
			check(!back.at(1).enabled,
			       "and the disabled one still disabled");
		}

		// A file that is not a list at all: refused, and the entries that were
		// there are left on disk rather than half-read and written back.
		const QString bad = QDir(dir.path()).filePath("bad.json");
		{
			QFile f(bad);
			check(f.open(QIODevice::WriteOnly | QIODevice::Truncate),
			       "a malformed index is written for the next case");
			f.write("{ this is not json");
		}
		check(filter_subscription::load_index(bad).isEmpty(),
		       "a malformed index reads as none, and complains in the log");

		// Minting: readable, free, and never a path.
		check(filter_subscription::mint_cache_name(dir.path(), "EasyList") ==
		          "easylist.txt",
		       QString("a plain name becomes its own file (%1)")
		           .arg(filter_subscription::mint_cache_name(dir.path(),
		                                                      "EasyList")));
		const QString nasty =
		  filter_subscription::mint_cache_name(dir.path(), "../../etc/passwd");
		check(!nasty.contains('/') && !nasty.contains(".."),
		       QString("and a name that is a path does not stay one (%1)")
		           .arg(nasty));
		check(filter_subscription::mint_cache_name(dir.path(), "!!!") ==
		          "list.txt",
		       "a name with nothing usable in it still gets a file");
		// A taken name steps aside rather than overwriting the body of a
		// subscription somebody already has.
		{
			QFile f(QDir(dir.path()).filePath("easylist.txt"));
			f.open(QIODevice::WriteOnly | QIODevice::Truncate);
			f.write("||taken.example^\n");
		}
		check(filter_subscription::mint_cache_name(dir.path(), "EasyList") ==
		          "easylist-2.txt",
		       QString("a taken name steps aside (%1)")
		           .arg(filter_subscription::mint_cache_name(dir.path(),
		                                                      "EasyList")));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

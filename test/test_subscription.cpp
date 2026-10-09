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
#include "subscription_updater.h"
#include "echo_server.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// A real event loop against a timer. `processEvents` returns as soon as the
// queue is empty, which on a fetch that has not answered yet is immediately --
// the fixture fault another suite paid for and recorded.
static void spin(int ms) {
	QEventLoop loop;
	QTimer::singleShot(ms, &loop, &QEventLoop::quit);
	loop.exec();
}

using filter_subscription::line_kind;

static QString kind_name(line_kind k) {
	switch (k) {
		case line_kind::comment:     return QStringLiteral("comment");
		case line_kind::network:     return QStringLiteral("network");
		case line_kind::cosmetic:    return QStringLiteral("cosmetic");
		case line_kind::scriptlet:   return QStringLiteral("scriptlet");
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
			// A scriptlet naming the catalog is its own kind; the section
			// below is about those. These are the ones this build reads and
			// does not implement, each for its own reason.
			{ "@@||ads.example.com^", line_kind::unsupported,
			   "an exception rule" },
			{ "||ads.example.com^$third-party", line_kind::unsupported,
			   "a rule carrying options" },
			{ "/^https?:\\/\\/ads\\./", line_kind::unsupported,
			   "a regular-expression rule" },
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

	section("a scriptlet rule naming the catalog is kept, and counted apart");
	{
		// **The kind that reaches an ad served from the content's own host.**
		// Counted apart from rules because it is not matched against
		// anything: it is a patch applied to the page's globals from a closed
		// catalog, and summing the two would put code behind a number that
		// says "rules".
		// **The kind is taken first and the message built after.** C++ does
		// not order a call's arguments, so `check(classify(...) == k,
		// QString(...).arg(why))` may build the message from the PREVIOUS
		// call's `why` -- measured here, where an unscoped scriptlet reported
		// "names a scriptlet this build does not implement", which was the
		// reason from two lines earlier. The assertion was right and its
		// evidence was not, which is the half nobody checks.
		QString why;
		scriptlet_call call;
		line_kind got = filter_subscription::classify(
		  "youtube.com##+js(json-prune, adPlacements playerAds)", nullptr,
		  &why, &call);
		check(got == line_kind::scriptlet,
		       QString("a call to an implemented scriptlet is a scriptlet "
		                "(%1)").arg(kind_name(got)));
		check(call.scope == "youtube.com" && call.name == "json-prune" &&
		          call.args.size() == 1 &&
		          call.args.first() == "adPlacements playerAds",
		       QString("carrying its site, name and argument (%1 / %2 / %3)")
		           .arg(call.scope, call.name, call.args.join("|")));

		// Not in the catalog: unsupported, with the reason, rather than
		// stored as something that will never run.
		why.clear();
		got = filter_subscription::classify(
		  "youtube.com##+js(trusted-click-element, .accept)", nullptr, &why);
		check(got == line_kind::unsupported,
		       QString("one this build does not implement is not (%1: %2)")
		           .arg(kind_name(got), why));
		// **Unscoped is refused**, because a patch to every page's globals is
		// not something a subscribed list gets to ask for.
		why.clear();
		got = filter_subscription::classify("##+js(json-prune, x)", nullptr,
		                                     &why);
		check(got == line_kind::unsupported && why.contains("unscoped"),
		       QString("and an unscoped one is refused for being unscoped "
		                "(%1: %2)").arg(kind_name(got), why));

		// **The third name has now been replaced twice by the catalog
		// growing**, which is this case working rather than failing:
		// `nowebrtc` stood here, then `aost`, and each time the count went
		// red the day the capability arrived. `json-edit` is the one that
		// will stay put, because it is declined on a measurement rather than
		// deferred -- 1667 lines of uBlock's JSON query language for three
		// rules in 2453.
		const subscription_read rep = filter_subscription::read(
		  "! Title: Annoyances\n"
		  "youtube.com##+js(json-prune, adPlacements)\n"
		  "player.test##+js(set-constant, cfg.ads, false)\n"
		  "other.test##+js(json-edit, x)\n");
		// **A list of nothing but scriptlets is usable.** An annoyance list
		// can be exactly that, and refusing it for having no network rule
		// would refuse the half of the ecosystem this build has just learned
		// to read.
		check(rep.ok(), QString("a list of only scriptlets is usable (%1)")
		                     .arg(rep.refusal));
		check(rep.scriptlets == 2 && rep.calls.size() == 2,
		       QString("two of the three are in the catalog (%1)")
		           .arg(rep.scriptlets));
		check(rep.unsupported == 1,
		       QString("and the third is counted unread (%1)")
		           .arg(rep.unsupported));
		check(rep.accepted == 0,
		       QString("with no rules claimed (%1)").arg(rep.accepted));
		check(rep.summary().contains("2 scriptlet(s)"),
		       QString("the summary says so (%1)").arg(rep.summary()));
	}

	section("a trusted scriptlet is kept only from a list marked trusted");
	{
		// **The whole of what the trust flag changes**, in one body read
		// twice. Everything but the trusted call reads the same either way,
		// which is the property worth asserting: ticking the box cannot
		// alter what a network rule does.
		const QString body =
		  "! Title: A list with one powerful rule\n"
		  "||ads.example^\n"
		  "player.test##+js(set-constant, cfg.ads, false)\n"
		  "player.test##+js(trusted-set-cookie, consent, yes, 7)\n";

		const subscription_read plain = filter_subscription::read(body);
		check(plain.ok(), QString("an untrusted read is usable (%1)")
		                       .arg(plain.refusal));
		check(plain.scriptlets == 1 && plain.calls.size() == 1 &&
		          plain.calls.first().name == "set-constant",
		       QString("only the ordinary scriptlet is kept (%1)")
		           .arg(plain.calls.isEmpty() ? QString("none")
		                                       : plain.calls.first().name));
		// **Counted rather than silently dropped.** A list whose useful half
		// is in its trusted rules and a list that has none look identical
		// from a silent drop, and the first is the only one where ticking
		// the box would change anything.
		check(plain.needs_trust == 1,
		       QString("and the trusted one is counted (%1)")
		           .arg(plain.needs_trust));
		check(plain.summary().contains("needing trust"),
		       QString("the summary says so (%1)").arg(plain.summary()));
		check(plain.accepted == 1,
		       QString("the network rule is unaffected (%1)")
		           .arg(plain.accepted));

		const subscription_read ok = filter_subscription::read(body, 0, true);
		check(ok.scriptlets == 2 && ok.needs_trust == 0,
		       QString("a trusted read keeps both (%1)").arg(ok.summary()));
		check(ok.accepted == plain.accepted && ok.unsupported ==
		          plain.unsupported && ok.unsafe == plain.unsafe,
		       "and nothing else about the read changes");
		// The flag travels on the call, because by the time a script is
		// written the list it came from is out of reach.
		bool carried = !ok.calls.isEmpty();
		for (const scriptlet_call &c : ok.calls)
			if (!c.trusted)
				carried = false;
		check(carried, "each kept call carries the trust it was read under");

		// A list whose only content is a trusted rule, untrusted, has
		// nothing this build will run -- refused, with the count still said.
		const subscription_read only = filter_subscription::read(
		  "player.test##+js(trusted-set-cookie, a, b)\n");
		check(!only.ok(),
		       QString("a list of only trusted rules is refused untrusted "
		                "(%1)").arg(only.summary()));
		check(only.needs_trust == 1,
		       QString("with the one that needed trust counted (%1)")
		           .arg(only.needs_trust));
		check(filter_subscription::read(
		          "player.test##+js(trusted-set-cookie, a, b)\n", 0, true)
		          .ok(),
		       "and the same list is usable once the list is trusted");
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
		// The scriptlet here names one the catalog does not implement, which
		// is what makes this body unusable. `nowebrtc` stood here until the
		// catalog grew to eleven and implemented it -- at which point this
		// case correctly stopped holding, which is the test noticing a
		// capability arrive rather than a regression.
		//
		// **It happened a second time and was nearly missed, because the
		// case went on passing.** `trusted-set-cookie` replaced `nowebrtc`
		// here and then entered the catalog itself -- and this stayed green,
		// because an untrusted read drops a trusted call and the body is
		// unusable either way. Passing for the trust gate while its comment
		// claims an unimplemented name is the shape this tree calls right by
		// coincidence, and what found it was sabotaging the gate and reading
		// WHICH checks went red rather than that some did.
		const subscription_read unread = filter_subscription::read(
		  "@@||a.example^\n@@||b.example^\n"
		  "example.com##+js(trusted-click-element, .accept)\n");
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
		check(rep.unsupported == 2,
		       QString("two lines it cannot (%1)").arg(rep.unsupported));
		check(rep.scriptlets == 1,
		       QString("and one scriptlet, which is neither (%1)")
		           .arg(rep.scriptlets));
		check(rep.lines == 7,
		       QString("seven candidate lines, the header and comment not "
		                "counted as a gap (%1)").arg(rep.lines));
		check(rep.summary().contains("4 rule(s) in use") &&
		          rep.summary().contains("1 scriptlet(s)") &&
		          rep.summary().contains("2 line(s)"),
		       QString("and the summary names all three numbers (%1)")
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

	section("a fresh install subscribes to something, once");
	{
		// **The gap this closes: a new install enforced nothing.** There was
		// no default and Add asked for an address somebody had to already
		// know, so the whole filter pipeline sat idle on a fresh profile.
		const QList<subscription> seeds =
		  filter_subscription::default_subscriptions();
		check(seeds.size() >= 2,
		       QString("there are defaults (%1)").arg(seeds.size()));
		bool all_ok = !seeds.isEmpty();
		for (const subscription &s : seeds) {
			if (s.name.isEmpty() || !s.url.isValid() ||
			    s.url.scheme() != QLatin1String("https") || !s.enabled ||
			    !s.trusted)
				all_ok = false;
		}
		// **Trusted, asserted** -- and this assertion was the opposite way
		// round a few hours earlier, deliberately, so that reversing it
		// would have to be a decision somebody took rather than a line that
		// drifted. It was reversed on the copyright holder's instruction:
		// naming a list in `default_subscriptions` IS the statement about
		// its publisher, so withholding trust afterwards only kept 22 of
		// uBlock's rules from the person for a judgement already made.
		check(all_ok,
		       "each is named, https, enabled and trusted");
		// **The half that did NOT move**, and it matters more now: what this
		// file names is trusted, and nothing else is. A subscription somebody
		// adds, or one read back from an index written before the key
		// existed, is untrusted -- so an upgrade cannot retroactively trust
		// a list nobody here chose.
		subscription typed_in;
		check(!typed_in.trusted,
		       "while a subscription made any other way starts untrusted");

		QTemporaryDir dir;
		check(dir.isValid(), "a scratch profile");
		const QString index = QDir(dir.path()).filePath("subs.json");

		// First run: no index file at all.
		{
			subscription_updater up(index, dir.path());
			check(up.subscriptions().size() == seeds.size(),
			       QString("a first run is seeded (%1)")
			           .arg(up.subscriptions().size()));
			check(QFile::exists(index),
			       "and the index is written, so this happens once");
			bool filed = !up.subscriptions().isEmpty();
			for (const subscription &s : up.subscriptions())
				if (s.file.isEmpty() || s.file.contains('/'))
					filed = false;
			check(filed, "each with a cache name of its own, and not a path");
		}

		// **The case that matters more than the seeding: an index that
		// exists is never overwritten.** `load_index` answers with an empty
		// list for three different situations -- no file, a file with no
		// entries, and a file it REFUSED as malformed -- and it refuses
		// rather than repairs precisely so a damaged index does not lose
		// subscriptions. Seeding on an empty answer would undo that.
		{
			const QString empty = QDir(dir.path()).filePath("empty.json");
			QFile f(empty);
			check(f.open(QIODevice::WriteOnly | QIODevice::Truncate),
			       "an index holding no subscriptions is written");
			f.write("[]\n");
			f.close();
			subscription_updater up(empty, dir.path());
			check(up.subscriptions().isEmpty(),
			       QString("somebody who removed every subscription keeps "
			                "none (%1)").arg(up.subscriptions().size()));
		}
		{
			const QString broken = QDir(dir.path()).filePath("broken.json");
			QFile f(broken);
			check(f.open(QIODevice::WriteOnly | QIODevice::Truncate),
			       "and a malformed index is written");
			f.write("{ not json at all");
			f.close();
			subscription_updater up(broken, dir.path());
			check(up.subscriptions().isEmpty(),
			       "a malformed index is not replaced by the defaults either");
			// The file is left exactly as it was, which is what makes the
			// refusal recoverable by hand.
			QFile again(broken);
			check(again.open(QIODevice::ReadOnly) &&
			          again.readAll() == QByteArray("{ not json at all"),
			       "and is left on disk untouched");
		}
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
		a.trusted = true;

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
			// **Trust round-trips, and its absent-key default is the
			// opposite one.** `enabled` defaults true because a
			// subscription somebody added is on; `trusted` defaults false
			// because an index written before this build existed has no
			// such key, and reading it as true would turn the power on
			// retroactively for every list already subscribed to.
			check(back.at(0).trusted && !back.at(1).trusted,
			       "the trusted one trusted and the other not");
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

	section("a fetch is promoted only when the body is a list");
	{
		// **The case this whole class is built for.** A list server that
		// answers 200 with a login page, an error page or a half-migrated
		// mirror would otherwise overwrite a working list with one that
		// blocks nothing -- ad blocking off, silently, looking exactly like
		// an upstream that got quieter. So the interesting assertion is not
		// that a good body is taken; it is that a bad one changes nothing.
		echo_server srv;
		srv.content_type = "text/plain";
		const QString base = srv.start();
		check(!base.isEmpty(), "a local list server is listening");
		const QByteArray good =
		  "[Adblock Plus 2.0]\n! Title: Good\n"
		  "||ads.example.com^\n||track.example.net^\nexample.com##.ad-slot\n";
		srv.files["/good.txt"] = good;
		srv.files["/page.txt"] =
		  "<!DOCTYPE html>\n<html><body>Sign in</body></html>\n";

		QTemporaryDir scratch;
		check(scratch.isValid(), "a scratch directory");
		const QString index = QDir(scratch.path()).filePath("subs.json");
		const QString cache = QDir(scratch.path()).filePath("bodies");

		subscription_updater up(index, cache);
		subscription one;
		one.name = "Good";
		one.url  = QUrl(base + "/good.txt");
		one.file = "good.txt";
		check(up.set_subscriptions({ one }), "the subscription is recorded");

		int promoted = -1, refused = -1;
		QObject::connect(&up, &subscription_updater::updated,
		                  [&promoted, &refused](int p, int r) {
			promoted = p;
			refused  = r;
		});

		check(up.update(true) == 1, "one subscription is due when forced");
		for (int i = 0; i < 200 && promoted < 0; ++i)
			spin(25);
		check(promoted == 1 && refused == 0,
		       QString("it is promoted (promoted=%1 refused=%2)")
		           .arg(promoted).arg(refused));
		const QString body_path = QDir(cache).filePath("good.txt");
		check(QFile::exists(body_path),
		       "the body is cached under the name the index gave it");
		const auto slurp = [](const QString &p) {
			QFile f(p);
			f.open(QIODevice::ReadOnly);
			return f.readAll();
		};
		check(slurp(body_path) == good,
		       "exactly as fetched, not as parsed");
		check(up.subscriptions().size() == 1 &&
		          up.subscriptions().first().rules == 3,
		       QString("three rules counted (%1)")
		           .arg(up.subscriptions().isEmpty()
		                  ? -1 : up.subscriptions().first().rules));
		check(!up.subscriptions().first().fetched.isNull(),
		       "and the time of the fetch recorded");

		// **Now the same subscription answered with a web page.** Nothing
		// about the working copy may move: not the body, not the count, not
		// the fetch time.
		const QDateTime was = up.subscriptions().first().fetched;
		QList<subscription> subs = up.subscriptions();
		subs[0].url = QUrl(base + "/page.txt");
		up.set_subscriptions(subs);
		promoted = refused = -1;
		check(up.update(true) == 1, "it is fetched again");
		for (int i = 0; i < 200 && promoted < 0; ++i)
			spin(25);
		check(promoted == 0 && refused == 1,
		       QString("and refused (promoted=%1 refused=%2)")
		           .arg(promoted).arg(refused));
		check(slurp(body_path) == good,
		       "the cached body is the one that worked");
		check(up.subscriptions().first().rules == 3,
		       QString("the count did not move (%1)")
		           .arg(up.subscriptions().first().rules));
		check(up.subscriptions().first().fetched == was,
		       "nor the time of the last good fetch");
		check(up.subscriptions().first().note.contains("web page"),
		       QString("and the note says why (%1)")
		           .arg(up.subscriptions().first().note.left(60)));

		// A 404 that still carries a body, which is what a moved list server
		// answers with. Qt calls that a successful reply.
		srv.file_status = 404;
		subs = up.subscriptions();
		subs[0].url = QUrl(base + "/good.txt");
		up.set_subscriptions(subs);
		promoted = refused = -1;
		up.update(true);
		for (int i = 0; i < 200 && promoted < 0; ++i)
			spin(25);
		check(promoted == 0 && refused == 1,
		       QString("a 404 with a body is refused (promoted=%1 refused=%2)")
		           .arg(promoted).arg(refused));
		check(up.subscriptions().first().note.contains("404"),
		       QString("naming the status (%1)")
		           .arg(up.subscriptions().first().note.left(50)));
		check(slurp(body_path) == good, "and the good copy is still there");

		// Nothing is due when the last good fetch is recent and it is not
		// forced -- which is what stops a browser re-fetching every launch.
		srv.file_status = 200;
		check(up.update(false) == 0,
		       "nothing is due an hour after a fetch");
	}

	section("an index with no trusted key trusts the lists we ship");
	{
		// **The upgrade, and why it is not a weakening.** An index written
		// before the `trusted` key existed says nothing about trust, and
		// reading its silence as a refusal left EasyList and uBlock's filters
		// permanently untrusted -- dropping 22 scriptlet rules, five of them
		// youtube's, and nothing re-seeds an index that already exists.
		// Settled by the copyright holder 2026-10-09: the lists arrive in the
		// same binary as the code that trusts them.
		const QString idx = QDir::temp().filePath("hydra-trust-index.json");
		auto put = [&](const QByteArray &json) {
			QFile f(idx);
			f.open(QIODevice::WriteOnly | QIODevice::Truncate);
			f.write(json);
			f.close();
		};

		// Silence about a list we ship: adopted.
		put("[{\"name\":\"uBlock filters\","
		     "\"url\":\"https://ublockorigin.github.io/uAssets/filters/"
		     "filters.txt\",\"file\":\"u.txt\"}]");
		QList<subscription> a = filter_subscription::load_index(idx);
		check(a.size() == 1 && a.first().trusted,
		       "a shipped list with no trusted key comes back trusted");

		// **Control one: an explicit false is a decision, not silence.**
		// Without this the change reads as "always trust what we ship",
		// which would take the switch away from the person.
		put("[{\"name\":\"uBlock filters\","
		     "\"url\":\"https://ublockorigin.github.io/uAssets/filters/"
		     "filters.txt\",\"trusted\":false,\"file\":\"u.txt\"}]");
		QList<subscription> b = filter_subscription::load_index(idx);
		check(b.size() == 1 && !b.first().trusted,
		       "and an explicit false on the same list is still obeyed");

		// **Control two: silence about a list we do NOT ship stays false.**
		// This is the retroactive grant the original comment refused, and it
		// is still refused.
		put("[{\"name\":\"Someone's list\","
		     "\"url\":\"https://example.invalid/list.txt\","
		     "\"file\":\"s.txt\"}]");
		QList<subscription> c = filter_subscription::load_index(idx);
		check(c.size() == 1 && !c.first().trusted,
		       "while a list we do not ship is untrusted on silence as before");

		QFile::remove(idx);
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

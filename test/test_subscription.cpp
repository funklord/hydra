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
#include "policy_engine.h"
#include "network_rules.h"
#include "request_filter.h"
#include "subscription_updater.h"
#include "echo_server.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
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
		case line_kind::option:      return QStringLiteral("option");
		case line_kind::badfilter:   return QStringLiteral("badfilter");
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
			// Exceptions and rules with options were unsupported until
			// 2026-10-29; they are carried by `network_rules` now, and one
			// with an option that reader does not know is still refused.
			{ "@@||ads.example.com^", line_kind::option, "an exception rule" },
			{ "||ads.example.com^$third-party", line_kind::option,
			   "a rule carrying options" },
			{ "||ads.example.com^$third-party,badfilter", line_kind::badfilter,
			   "a badfilter" },
			// A scriptlet naming the catalog is its own kind; the section
			// below is about those. These are the ones this build reads and
			// does not implement, each for its own reason.
			{ "||ads.example.com^$removeparam=utm_source", line_kind::unsupported,
			   "a rule with an option this build does not read" },
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

	// **uBlock's `replace=` on a script request, carried in the page.** The
	// interceptor cannot rewrite a body, and this is how YouTube's second
	// video keeps its ad payload out: switching videos in the page fetches
	// `/youtubei/v1/get_watch`, and the only rules in the list for it are
	// these two. Before this they were counted as unsupported, and the
	// second video played a pre-roll the first no longer did.
	section("a replace= rule on a script request becomes a page rewrite");
	{
		const QString yt = QStringLiteral(
		  "||youtube.com/youtubei/v1/get_watch?$xhr,1p,"
		  "replace=/\"adPlacements\"/\"no_ads\"/");
		scriptlet_call call;
		QString why;
		const line_kind got =
		  filter_subscription::classify(yt, nullptr, &why, &call);
		check(got == line_kind::scriptlet &&
		          call.name == "trusted-replace-fetch-response" &&
		          call.scope == "youtube.com",
		       QString("it is a fetch rewrite scoped to the host (%1 %2 %3)")
		           .arg(kind_name(got), call.name, call.scope));
		const QStringList want_head = { "/\"adPlacements\"/", "\"no_ads\"" };
		check(call.args.size() == 3 && call.args.mid(0, 2) == want_head,
		       QString("searching and replacing what the rule says (%1)")
		           .arg(call.args.join(" | ")));
		const QString url = call.args.value(2);
		const QRegularExpression re(url.mid(1, url.size() - 2));
		check(re.match("https://www.youtube.com/youtubei/v1/get_watch"
		               "?prettyPrint=false").hasMatch() &&
		          !re.match("https://www.youtube.com/youtubei/v1/player?x=1")
		               .hasMatch() &&
		          !re.match("https://evil.test/youtube.com/youtubei/v1/"
		                    "get_watch?").hasMatch(),
		       QString("on the url it names and no other (%1)").arg(url));

		const subscription_read rep = filter_subscription::read(
		  yt + "\n||ads.example^\n", 0, true);
		QStringList names;
		for (const scriptlet_call &c : rep.calls)
			names << c.name;
		const QStringList both = { "trusted-replace-fetch-response",
		                           "trusted-replace-xhr-response" };
		check(names == both,
		       QString("and read carries it on both transports (%1)")
		           .arg(names.join(", ")));
		const subscription_read untrusted = filter_subscription::read(
		  yt + "\n||ads.example^\n", 0, false);
		check(untrusted.calls.isEmpty() && untrusted.needs_trust == 1,
		       QString("but only for a trusted list (%1 calls, %2 needing "
		               "trust)").arg(untrusted.calls.size())
		           .arg(untrusted.needs_trust));

		struct one { const char *line; const char *what; };
		const QList<one> refused = {
			{ "||youtube.com/x?$xhr,replace=/a/b/", "without 1p" },
			{ "||youtube.com/x?$1p,replace=/a/b/", "without xhr" },
			{ "||youtube.com/x?$xhr,1p,domain=a.test,replace=/a/b/",
			   "with an option it does not read" },
			{ "youtube.com/x?$xhr,1p,replace=/a/b/", "not anchored to a host" },
			{ "||youtube.com/x?$xhr,1p,replace=/(a+)+$/b/",
			   "with a pattern that backtracks" },
		};
		for (const one &c : refused) {
			QString said;
			const line_kind k = filter_subscription::classify(
			  QString::fromLatin1(c.line), nullptr, &said);
			check(k == line_kind::unsupported,
			       QString("refused %1 (%2 -- %3)")
			           .arg(QString::fromLatin1(c.what), kind_name(k), said));
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
		// red the day the capability arrived. `json-edit` stood here next,
		// declined on a measurement, until 2026-10-25 when uBlock's YouTube
		// rules came to need its query language and it was ported.
		// `jsonl-edit-xhr-response`, which rides on that language, is still
		// not in the catalog.
		const subscription_read rep = filter_subscription::read(
		  "! Title: Annoyances\n"
		  "youtube.com##+js(json-prune, adPlacements)\n"
		  "player.test##+js(set-constant, cfg.ads, false)\n"
		  "other.test##+js(jsonl-edit-xhr-response, x)\n");
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
		// The two exceptions that stood here became readable 2026-10-29, and
		// so did the popup rules after them; `$removeparam` is not.
		const subscription_read unread = filter_subscription::read(
		  "||a.example^$removeparam=x\n||b.example^$removeparam=y\n"
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
		// Two that it could not until 2026-10-29: the `$document` exception
		// and the `$script` rule are carried as rules with options now.
		check(rep.unsupported == 0 && rep.exceptions == 1 && rep.option_blocks == 1,
		       QString("an exception and a rule with options, carried apart "
		               "(%1 unsupported)").arg(rep.unsupported));
		check(rep.scriptlets == 1,
		       QString("and one scriptlet, which is neither (%1)")
		           .arg(rep.scriptlets));
		check(rep.lines == 7,
		       QString("seven candidate lines, the header and comment not "
		                "counted as a gap (%1)").arg(rep.lines));
		check(rep.summary().contains("4 rule(s) in use") &&
		          rep.summary().contains("1 scriptlet(s)") &&
		          rep.summary().contains("1 with options") &&
		          rep.summary().contains("1 exception(s)"),
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

	// **The unscoped one is generic hiding, read since 2026-10-29** -- kept
	// apart from the site rules, applied through `generic_cosmetics` with the
	// lists' own exceptions. It was refused here while a subscribed list's
	// generic rules were held to the bar set for a proposed one.
	section("the cosmetic rules it keeps are safe, and the unscoped one is generic");
	{
		const subscription_read rep = filter_subscription::read(
		  "a.example##.promo\n"
		  "##.promo\n"
		  "b.example##.x}body{display:none\n");
		check(rep.ok() && rep.accepted == 2,
		       QString("two of the three are kept (%1)").arg(rep.summary()));
		check(rep.unsafe == 1,
		       QString("the selector that closes its rule is refused (%1)")
		           .arg(rep.unsafe));
		check(rep.unsupported == 0 && rep.generic == QStringList({ ".promo" }),
		       QString("and the unscoped one is read as generic (%1)")
		           .arg(rep.generic.join(' ')));
		check(rep.rules.size() == 1 && rep.rules.first().cosmetic &&
		          rep.rules.first().scope == "a.example",
		       QString("what survives is the scoped one (%1)")
		           .arg(rep.rules.isEmpty() ? QString("nothing")
		                                     : rep.rules.first().text));
	}

	// **Two lists that overlap install each shared rule once, and a list the
	// size of a real subscription installs in a moment.** `replace` checked
	// for duplicates by scanning every rule it had kept, so it was quadratic:
	// 55 seconds for EasyList and uBlock filters together, on the UI thread,
	// under the lock the request interceptor waits on. Nothing exercised it
	// at a real size, and a dialog that froze was the only report.
	//
	// 40,000 rules is about two thirds of the holder's set. The quadratic
	// scan takes tens of seconds on it and the set a few tens of
	// milliseconds, so a bound of five seconds separates the two by a wide
	// margin in both directions, loaded machine or not.
	section("replace drops overlap and stays linear at a real list's size");
	{
		QList<filter_rule> rules;
		for (const char *line : { "||ads.example^", "||track.example^",
		                          "||ads.example^" }) {
			filter_rule r;
			filter_list::parse_rule(line, &r);
			rules.push_back(r);
		}
		filter_list list;
		list.replace(rules);
		check(list.rules().size() == 2,
		       QString("a rule two lists share is kept once (%1 kept)")
		           .arg(list.rules().size()));
		check(list.blocks("https://ads.example/x.js", "page.example"),
		       "and it still blocks");

		QList<filter_rule> big;
		for (int i = 0; i < 40000; ++i) {
			filter_rule r;
			filter_list::parse_rule(QString("||host%1.example^").arg(i), &r);
			big.push_back(r);
		}
		big.push_back(big.first());
		QElapsedTimer t;
		t.start();
		list.replace(big);
		const qint64 ms = t.elapsed();
		check(list.rules().size() == 40000,
		       QString("40,000 rules and one repeat install as 40,000 (%1)")
		           .arg(list.rules().size()));
		check(ms < 5000,
		       QString("in under five seconds (%1 ms)").arg(ms));
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

	// **A shipped list added later reaches a profile that already exists,
	// once.** Seeding runs only without an index, so the holder's profile --
	// two lists old -- would never have been given the four added on
	// 2026-10-09. And once, not whenever absent: a list somebody removed has
	// to stay removed, which is what the offered record beside the index is.
	section("a list shipped later is offered to an existing profile, once");
	{
		QTemporaryDir dir;
		const QString index = QDir(dir.path()).filePath("subs.json");
		const QList<subscription> seeds =
		  filter_subscription::default_subscriptions();
		// A profile from before: the original two, and no offered record.
		QList<subscription> old;
		for (subscription s : seeds)
			if (s.url.toString().endsWith("easylist.txt") ||
			    s.url.toString().endsWith("/filters.txt")) {
				s.file = filter_subscription::mint_cache_name(dir.path(), s.name);
				old << s;
			}
		check(old.size() == 2, "the original two are still shipped");
		filter_subscription::save_index(index, old);
		{
			subscription_updater up(index, dir.path());
			check(up.subscriptions().size() == seeds.size(),
			       QString("the lists shipped since are added (%1 of %2)")
			           .arg(up.subscriptions().size()).arg(seeds.size()));
			bool fresh = true;
			for (const subscription &s : up.subscriptions())
				if (s.file.isEmpty() || s.file.contains('/'))
					fresh = false;
			check(fresh, "each with a cache name of its own");
			// The person then removes one.
			QList<subscription> fewer = up.subscriptions();
			fewer.removeLast();
			up.set_subscriptions(fewer);
		}
		{
			subscription_updater up(index, dir.path());
			check(up.subscriptions().size() == seeds.size() - 1,
			       QString("and one removed afterwards stays removed (%1)")
			           .arg(up.subscriptions().size()));
		}
		// A profile from before that had removed EasyList keeps it removed:
		// it was offered at seeding, which is what a missing record means.
		const QString index2 = QDir(dir.path()).filePath("subs2.json");
		filter_subscription::save_index(index2, { old.last() });
		{
			subscription_updater up(index2, dir.path());
			bool easylist = false;
			for (const subscription &s : up.subscriptions())
				easylist = easylist ||
				           s.url.toString().endsWith("easylist.txt");
			check(!easylist && up.subscriptions().size() == seeds.size() - 1,
			       QString("an original list removed before this record "
			               "existed is not brought back (%1)")
			           .arg(up.subscriptions().size()));
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

	// **`!#include`, which uBlock's lists are built from.** Its `filters.txt`
	// is a head and ten includes, and the head alone was all that arrived:
	// Dailymotion's whole answer to its blocker detection lives in an
	// included yearly file.
	section("a list's includes are fetched, assembled and cached whole");
	{
		const QString head =
		  "! Title: head\n||head.example^\n!#include part-a.txt\n"
		  "!#include ../escape.txt\n!#include sub/dir.txt\n"
		  "!#if env_mobile\n!#include mobile.txt\n!#endif\n"
		  "!#include part-a.txt\n!#include part-b.txt\n";
		check(filter_subscription::includes_in(head) ==
		          QStringList({ "part-a.txt", "part-b.txt" }),
		       QString("only plain names in the list's own directory, outside "
		               "any condition, each once (%1)")
		           .arg(filter_subscription::includes_in(head).join(" ")));
		const QString whole = filter_subscription::assemble(
		  head, { { "part-a.txt", "||a.example^" } });
		check(whole.contains("||a.example^") &&
		          whole.contains("!#include part-b.txt") &&
		          whole.contains("!#include mobile.txt"),
		       "assembled in place, and a part not supplied stays a comment");

		echo_server srv;
		srv.content_type = "text/plain";
		const QString base = srv.start();
		srv.files["/lists/head.txt"] =
		  "! Title: head\n||head.example^\n!#include part-a.txt\n"
		  "!#include part-b.txt\n";
		srv.files["/lists/part-a.txt"] = "||a.example^\n";
		srv.files["/lists/part-b.txt"] = "||b.example^\nb.example##.ad\n";

		QTemporaryDir scratch;
		const QString index = QDir(scratch.path()).filePath("subs.json");
		const QString cache = QDir(scratch.path()).filePath("bodies");
		subscription_updater up(index, cache);
		subscription one;
		one.name = "Head";
		one.url  = QUrl(base + "/lists/head.txt");
		one.file = "head.txt";
		up.set_subscriptions({ one });
		int promoted = -1, refused = -1;
		QObject::connect(&up, &subscription_updater::updated,
		                  [&promoted, &refused](int p, int r) {
			promoted = p;
			refused  = r;
		});
		up.update(true);
		for (int i = 0; i < 200 && promoted < 0; ++i)
			spin(25);
		const auto slurp = [](const QString &p) {
			QFile f(p);
			f.open(QIODevice::ReadOnly);
			return QString::fromUtf8(f.readAll());
		};
		const QString cached = slurp(QDir(cache).filePath("head.txt"));
		check(promoted == 1 && up.subscriptions().first().rules == 4,
		       QString("the head and both parts are read as one list "
		               "(promoted=%1, %2 rules)")
		           .arg(promoted).arg(up.subscriptions().first().rules));
		check(cached.contains("||a.example^") && cached.contains("b.example##.ad"),
		       "and cached whole, so a load needs no network");

		// **A head cached before includes were followed is due at once.**
		// Written over the assembled copy, as the holder's profile had it,
		// with a fetch time of now: an unforced update fetches it anyway,
		// and only it.
		{
			QFile f(QDir(cache).filePath("head.txt"));
			f.open(QIODevice::WriteOnly | QIODevice::Truncate);
			f.write(srv.files["/lists/head.txt"]);
		}
		QList<subscription> fresh = up.subscriptions();
		fresh[0].fetched = QDateTime::currentDateTime();
		up.set_subscriptions(fresh);
		promoted = refused = -1;
		check(up.update(false) == 1,
		       "a cached head with includes left in it is due though fresh");
		for (int i = 0; i < 200 && promoted < 0; ++i)
			spin(25);
		check(promoted == 1 && slurp(QDir(cache).filePath("head.txt")) == cached,
		       QString("and comes back assembled (promoted=%1)").arg(promoted));
		promoted = refused = -1;
		check(up.update(false) == 0,
		       "after which a fresh assembled copy is not due again");

		// One part answered with a web page -- a portal, an error page: the
		// update is refused and the whole copy stays.
		srv.files["/lists/part-b.txt"] =
		  "<!DOCTYPE html>\n<html><body>Sign in</body></html>\n";
		promoted = refused = -1;
		up.update(true);
		for (int i = 0; i < 200 && promoted < 0; ++i)
			spin(25);
		check(promoted == 0 && refused == 1,
		       QString("a part that is a web page refuses the update (promoted=%1 "
		               "refused=%2)").arg(promoted).arg(refused));
		check(slurp(QDir(cache).filePath("head.txt")) == cached &&
		          up.subscriptions().first().note.contains("part-b.txt"),
		       QString("keeping the whole copy, and saying which part (%1)")
		           .arg(up.subscriptions().first().note));
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

	// ------------------------------------------------------------- redirects --
	// **uBlock's answer to YouTube's ad_status.js check, as the list writes
	// it.** The holder's report of 2026-10-26 showed this browser refusing
	// that file while YouTube said "Ad blockers are not allowed"; uBlock
	// answers it with a stand-in that sets the flag the real one sets.
	const QString k_ad_status =
	  "||doubleclick.net/instream/ad_status.js^$script,xhr,"
	  "redirect=doubleclick_instream_ad_status.js:5";

	section("a redirect rule is read, with its options");
	{
		network_rule r;
		QString why;
		check(filter_subscription::classify(k_ad_status, nullptr, &why, nullptr, &r) ==
		        line_kind::option,
		      QString("the list's line is a redirect (%1)").arg(why));
		check(r.resource == "doubleclick_instream_ad_status.js" && r.priority == 5 &&
		        r.host == "doubleclick.net" && !r.rule_only &&
		        r.types == (request_type::script | request_type::xhr),
		      QString("naming its resource, priority, host and types (%1 %2 %3 %4)")
		          .arg(r.resource).arg(r.priority).arg(r.host).arg(r.types));
		check(filter_subscription::classify(
		        "||ads.example^$image,redirect-rule=1x1.gif,domain=a.example|~b.a.example",
		        nullptr, nullptr, nullptr, &r) == line_kind::option &&
		        r.rule_only && r.resource == "1x1.gif" &&
		        r.include == QStringList{ "a.example" } &&
		        r.exclude == QStringList{ "b.a.example" },
		      "redirect-rule= and its domains");
		check(filter_subscription::classify("||x.example^$script,redirect=noopjs",
		                                    nullptr, nullptr, nullptr, &r) ==
		        line_kind::option && r.resource == "noop.js",
		      "an alias resolves to the resource's own name");
		check(filter_subscription::classify(
		        "/instream/ad_status.js^$script,redirect=noopjs,domain=a.example",
		        nullptr, nullptr, nullptr, &r) == line_kind::option,
		      "a path pattern is a pattern, not a regex");
		why.clear();
		check(filter_subscription::classify("||x.example^$script,redirect=google-ima.js",
		                                    nullptr, &why) == line_kind::unsupported &&
		        why.contains("google-ima.js"),
		      QString("a resource this build does not carry is refused, named (%1)")
		          .arg(why));
		why.clear();
		check(filter_subscription::classify(
		        "||x.example^$script,redirect=noopjs,denyallow=y.example",
		        nullptr, &why) == line_kind::unsupported && why.contains("denyallow"),
		      QString("and so is an option this reader does not know (%1)").arg(why));
		const subscription_read rep = filter_subscription::read(
		  "! Title: t\n||ads.example^\n" + k_ad_status + "\n");
		check(rep.redirects == 1 && rep.option_rules.size() == 1 &&
		        rep.summary().contains("1 answered with a stand-in"),
		      QString("a list counts them apart and says so (%1)").arg(rep.summary()));
		redirect_resources::resource res;
		check(redirect_resources::find("doubleclick_instream_ad_status.js", &res) &&
		        res.body == "window.google_ad_status = 1;\n" &&
		        res.mime == "application/javascript",
		      "and the stand-in sets what the real file sets, and nothing else");
		check(redirect_resources::find("1x1.gif", &res) && res.body.size() == 43 &&
		        res.body.startsWith("GIF89a"),
		      "an image stand-in is a whole image");
	}

	section("a redirect answers a request a blanket rule would refuse");
	{
		policy_engine policy;
		request_filter filter(&policy);
		filter_list subscribed;
		filter_rule blanket;
		filter_list::parse_rule("||doubleclick.net^", &blanket);
		subscribed.replace({ blanket });
		filter.set_subscription_list(&subscribed);
		QList<network_rule> rules;
		network_rule r;
		parse_network_rule(k_ad_status, &r, nullptr);
		rules << r;
		parse_network_rule("||ads.example/pixel^$image,redirect-rule=1x1.gif,domain=site.example",
		                    &r, nullptr);
		rules << r;
		filter.set_network_rules(rules);

		const auto ask = [&](const QString &url, const QString &site, unsigned type) {
			request_context ctx;
			ctx.url = QUrl(url);
			ctx.request_host = ctx.url.host();
			ctx.site_host = site;
			ctx.kind = type == request_type::script ? resource_kind::script
			         : type == request_type::image ? resource_kind::image
			                                       : resource_kind::other;
			ctx.type = type;
			return filter.decide(ctx);
		};
		request_decision d = ask("https://static.doubleclick.net/instream/ad_status.js",
		                         "www.youtube.com", request_type::script);
		check(d.block && d.redirect == "doubleclick_instream_ad_status.js",
		      QString("ad_status.js is answered with the stand-in (%1)").arg(d.redirect));
		d = ask("https://static.doubleclick.net/instream/ad_status.js",
		        "www.youtube.com", request_type::image);
		check(d.block && d.redirect.isEmpty(),
		      "but not as an image, which the rule does not name");
		d = ask("https://static.doubleclick.net/other.js", "www.youtube.com",
		        request_type::script);
		check(d.block && d.redirect.isEmpty(),
		      "and the blanket rule still refuses the rest of the host");
		d = ask("https://ads.example/pixel", "site.example", request_type::image);
		check(!d.block, "a redirect-rule= answers nothing that is not blocked");
		filter_rule ads;
		filter_list::parse_rule("||ads.example^", &ads);
		subscribed.replace({ blanket, ads });
		d = ask("https://ads.example/pixel", "site.example", request_type::image);
		check(d.block && d.redirect == "1x1.gif",
		      QString("and answers what is, on its own site (%1)").arg(d.redirect));
		d = ask("https://ads.example/pixel", "other.example", request_type::image);
		check(d.block && d.redirect.isEmpty(), "and on no other");
		policy.set_setting("www.youtube.com", policy::feature::ads, policy::setting::allow);
		d = ask("https://static.doubleclick.net/instream/ad_status.js",
		        "www.youtube.com", request_type::script);
		check(!d.block && d.redirect.isEmpty(),
		      "and a site whose ads are allowed gets the real file");
	}

	// --------------------------------------------- options and exceptions --
	// **What the lists ask that only options can say.** 15 thousand lines of
	// the holder's lists carried options and were dropped, and every `@@`
	// exception with them -- uBlock's Unbreak list among them, whose 1283
	// exceptions exist to repair what blockers break. Measured 2026-10-29.
	section("rules with options and exceptions are read, and badfilter cancels");
	{
		network_rule r;
		QString why, target;
		check(parse_network_rule("@@||cdn.example^$script,domain=news.example", &r, &why) ==
		        parsed::option && r.act == network_rule::action::allow &&
		        r.types == request_type::script && r.include == QStringList{ "news.example" },
		      QString("an exception with options (%1)").arg(why));
		check(parse_network_rule("@@||example.org^", &r, &why) == parsed::option &&
		        r.act == network_rule::action::allow,
		      "and one with none, which nothing else carries");
		check(parse_network_rule("@@||site.example^$document", &r, &why) == parsed::option &&
		        r.document,
		      "a $document exception exempts a page");
		check(parse_network_rule("||site.example^$document", &r, &why) == parsed::refused,
		      QString("while a $document block is refused (%1)").arg(why));
		check(parse_network_rule("||ads.example^$script,~third-party,important", &r, &why) ==
		        parsed::option && r.act == network_rule::action::block && r.party == 1 &&
		        r.important,
		      "a block with type, party and important");
		check(parse_network_rule("||ads.example^$~xmlhttprequest", &r, &why) ==
		        parsed::option && r.not_types == request_type::xhr,
		      "a negated type");
		check(parse_network_rule("||ads.example^$third-party,badfilter", &r, &why, &target) ==
		        parsed::badfilter && target == "||ads.example^$third-party",
		      QString("badfilter names the rule it cancels (%1)").arg(target));
		check(parse_network_rule("||ads.example^$removeparam=x", &r, &why) == parsed::refused &&
		        why.contains("removeparam"),
		      QString("an unknown option is refused, named (%1)").arg(why));
		check(parse_network_rule("||ads.example^", &r, &why) == parsed::none,
		      "and a plain rule is the block list's");
		const subscription_read rep = filter_subscription::read(
		  "! Title: Unbreak\n@@||cdn.example^$script,domain=news.example\n"
		  "@@||site.example^$document\n||x.example^$third-party,badfilter\n");
		check(rep.ok() && rep.exceptions == 2 && rep.badfilters == 1 &&
		        rep.cancelled == QStringList{ "||x.example^$third-party" },
		      QString("a list of exceptions is usable, and counted (%1)").arg(rep.summary()));
		QList<network_rule> list;
		parse_network_rule("||x.example^$third-party", &r, nullptr);
		list << r;
		parse_network_rule("||y.example^$third-party", &r, nullptr);
		list << r;
		QStringList plain{ "||x.example^$third-party", "||z.example^" };
		apply_badfilters(QSet<QString>{ "||x.example^$third-party" }, &list, &plain);
		check(list.size() == 1 && list.first().pattern == "||y.example^" &&
		        plain == QStringList{ "||z.example^" },
		      "badfilter drops the rule it names, from both kinds of list");
	}

	section("an exception lifts a block, and important outranks it");
	{
		policy_engine policy;
		request_filter filter(&policy);
		filter_list subscribed, own;
		filter_rule plain;
		filter_list::parse_rule("||cdn.example^", &plain);
		subscribed.replace({ plain });
		filter.set_subscription_list(&subscribed);
		filter_rule mine;
		filter_list::parse_rule("||mine.example^", &mine);
		own.add(mine);
		filter.set_filter_list(&own);
		QList<network_rule> rules;
		network_rule r;
		const char *const lines[] = {
			"@@||cdn.example^$script,domain=news.example",
			"@@||mine.example^",
			"@@||page.example^$document",
			"||tracker.example^$script,third-party",
			"||hard.example^$important",
			"@@||hard.example^",
			"||ads.example^$~image",
			"-advert-banner.$image,domain=shop.example",
			"pixel-tracker/$image",
			".com/ad/$image",
		};
		for (const char *line : lines) {
			if (parse_network_rule(QString::fromLatin1(line), &r, nullptr) == parsed::option)
				rules << r;
		}
		filter.set_network_rules(rules);
		const auto ask = [&](const QString &url, const QString &site, unsigned type) {
			request_context ctx;
			ctx.url = QUrl(url);
			ctx.request_host = ctx.url.host();
			ctx.site_host = site;
			ctx.kind = type == request_type::script ? resource_kind::script
			         : type == request_type::image ? resource_kind::image
			                                       : resource_kind::other;
			ctx.type = type;
			return filter.decide(ctx).block;
		};
		check(!ask("https://cdn.example/app.js", "news.example", request_type::script),
		      "the exception lifts the list's block where it applies");
		check(ask("https://cdn.example/app.js", "other.example", request_type::script) &&
		        ask("https://cdn.example/a.png", "news.example", request_type::image),
		      "and nowhere else -- not another site, not another type");
		check(ask("https://mine.example/x", "news.example", request_type::script),
		      "the person's own rule is not a list's to lift");
		check(!ask("https://cdn.example/app.js", "page.example", request_type::script),
		      "a $document exception exempts the whole page");
		check(ask("https://tracker.example/t.js", "news.example", request_type::script) &&
		        !ask("https://tracker.example/t.js", "tracker.example", request_type::script) &&
		        !ask("https://tracker.example/t.png", "news.example", request_type::image),
		      "a block with options keeps to its type and party");
		check(ask("https://hard.example/x", "news.example", request_type::other),
		      "an important block is not lifted");
		check(ask("https://ads.example/x.js", "a.example", request_type::script) &&
		        !ask("https://ads.example/x.png", "a.example", request_type::image),
		      "a negated type spares what it names");
		check(ask("https://img.shop.example/-advert-banner.gif", "shop.example", request_type::image) &&
		        !ask("https://img.shop.example/-advert-banner.gif", "else.example", request_type::image),
		      "a rule filed by its site applies only there");
		check(ask("https://any.example/pixel-tracker/1.gif", "x.example", request_type::image),
		      "and one with only a token still matches");
		check(ask("https://cdn.shop.com/ad/1.gif", "x.example", request_type::image) &&
		        !ask("https://cdn.shop.com/add/1.gif", "x.example", request_type::image),
		      "and one with no token at all, filed by three characters");
	}

	section("a $popup rule refuses a window, and nothing else");
	{
		policy_engine policy;
		request_filter filter(&policy);
		QList<network_rule> rules;
		network_rule r;
		const char *const lines[] = {
			"||popads.example^$popup",
			"||mixed.example^$popup,script",
			"||pop.example^$popup",
			"@@||pop.example^$popup,domain=friend.example",
			"||lists-only.example^$script",
		};
		int read = 0;
		for (const char *line : lines)
			if (parse_network_rule(QString::fromLatin1(line), &r, nullptr) == parsed::option) {
				rules << r;
				++read;
			}
		check(read == 5, QString("all five are read, $popup understood (%1)").arg(read));
		filter.set_network_rules(rules);
		const auto ask = [&](const QString &url, unsigned type) {
			request_context ctx;
			ctx.url = QUrl(url);
			ctx.request_host = ctx.url.host();
			ctx.site_host = "news.example";
			ctx.type = type;
			return filter.decide(ctx).block;
		};
		check(filter.blocks_popup(QUrl("https://popads.example/x"), "news.example"),
		      "a window to an address a $popup rule names is refused");
		check(!ask("https://popads.example/x.js", request_type::script) &&
		        !ask("https://popads.example/x", request_type::unknown),
		      "and a request to it is not -- an untyped one included, as Android's often are");
		check(filter.blocks_popup(QUrl("https://mixed.example/"), "news.example") &&
		        ask("https://mixed.example/a.js", request_type::script) &&
		        !ask("https://mixed.example/a.png", request_type::image),
		      "a rule naming a popup and a type decides both, and only those");
		check(!filter.blocks_popup(QUrl("https://lists-only.example/"), "news.example"),
		      "a rule without $popup decides no window");
		check(!filter.blocks_popup(QUrl("https://pop.example/"), "friend.example") &&
		        filter.blocks_popup(QUrl("https://pop.example/"), "news.example"),
		      "an exception lifts it where it says, and only there");
		policy.set_setting("news.example", policy::feature::ads, policy::setting::allow);
		check(!filter.blocks_popup(QUrl("https://popads.example/x"), "news.example"),
		      "and ads allowed in the shield let every window through");
	}

	section("generic hiding, its exceptions and the hosts that turn it off are read");
	{
		const subscription_read rep = filter_subscription::read(
		  "[Adblock Plus 2.0]\n"
		  "##.ad-slot\n"
		  "##div[id^=\"div-gpt-ad\"]\n"
		  "##+js(set-constant, x, 1)\n"
		  "news.example,~shop.news.example#@#.ad-slot\n"
		  "#@#.globally-lifted\n"
		  "site.example#@#+js(set-constant, x, 1)\n"
		  "@@||quiet.example^$generichide\n"
		  "@@||bare.example^$ehide,document\n"
		  "@@||other.example^$script\n",
		  0, true);
		check(rep.generic == QStringList({ ".ad-slot", "div[id^=\"div-gpt-ad\"]" }),
		      "two generic selectors, and a generic scriptlet is not one of them: " +
		        rep.generic.join(" | "));
		check(rep.cosmetic_exceptions.size() == 2 &&
		        rep.cosmetic_exceptions[0] == qMakePair(QString("news.example"), QString(".ad-slot")) &&
		        rep.cosmetic_exceptions[1] == qMakePair(QString(), QString(".globally-lifted")),
		      "an exception per named site, a negated site skipped, and one for everywhere");
		check(rep.generichide == QStringList({ "quiet.example" }) &&
		        rep.elemhide == QStringList({ "bare.example" }),
		      "$generichide and $ehide hosts, each in its own list");
		check(rep.unsupported >= 1, "a scriptlet exception is still unsupported");
	}

	section("a procedural selector is read into steps, and :style() is refused");
	{
		using namespace procedural_cosmetic;
		auto json = [](const QString &sel) {
			rule r;
			QString why;
			if (!parse(sel, &r, &why))
				return "refused: " + why;
			return QString::fromUtf8(QJsonDocument(to_json(r)).toJson(QJsonDocument::Compact));
		};
		check(!is_procedural("div.ad") && !is_procedural("div:has(> .ad)") &&
		        !is_procedural("a:not(.x)") && !is_procedural("p:nth-child(2)"),
		      "plain CSS, :has() and :not() of CSS included, is not procedural");
		check(is_procedural("div:has-text(Sponsored)") &&
		        is_procedural("div:has(span:has-text(Ad))") &&
		        is_procedural("div:style(color: red)"),
		      "an operator, one nested in :has(), and a refused one are");
		check(json("div.post:has-text(Sponsored)") ==
		        "{\"action\":\"hide\",\"arg\":\"\",\"steps\":[{\"css\":\"div.post\"},"
		        "{\"arg\":\"Sponsored\",\"op\":\"has-text\"}]}",
		      "css then has-text, hidden: " + json("div.post:has-text(Sponsored)"));
		check(json("span:has-text(/^Ad(vert)?$/i):upward(3) > div") ==
		        "{\"action\":\"hide\",\"arg\":\"\",\"steps\":[{\"css\":\"span\"},"
		        "{\"arg\":\"/^Ad(vert)?$/i\",\"op\":\"has-text\"},"
		        "{\"arg\":\"3\",\"op\":\"upward\"},{\"css\":\"> div\"}]}",
		      "a regex holding parentheses, upward, and CSS relative after it: " +
		        json("span:has-text(/^Ad(vert)?$/i):upward(3) > div"));
		check(json("article:has(h2:has-text(Annons)):not(.keep)") ==
		        "{\"action\":\"hide\",\"arg\":\"\",\"steps\":[{\"css\":\"article\"},"
		        "{\"op\":\"has\",\"steps\":[{\"css\":\"h2\"},"
		        "{\"arg\":\"Annons\",\"op\":\"has-text\"}]},{\"css\":\":not(.keep)\"}]}",
		      "a procedural :has() nests its steps; a CSS :not() stays CSS: " +
		        json("article:has(h2:has-text(Annons)):not(.keep)"));
		check(json(":matches-path(/^\\/news/) div.ad:has-text(Ad)") ==
		        "{\"action\":\"hide\",\"arg\":\"\",\"steps\":[{\"arg\":\"/^\\\\/news/\","
		        "\"negate\":false,\"op\":\"path\"},{\"css\":\"div.ad\"},"
		        "{\"arg\":\"Ad\",\"op\":\"has-text\"}]}" &&
		        json(":not(:matches-path(/amafav/)) a.x").contains("\"negate\":true"),
		      "a path gate opening a rule, and its negation: " +
		        json(":matches-path(/^\\/news/) div.ad:has-text(Ad)"));
		check(json(":xpath(\"//span[text()='Ad']\")").contains("\"arg\":\"//span[text()='Ad']\""),
		      "a quoted xpath loses its quotes");
		check(json("div.wall:remove()").contains("\"action\":\"remove\"") &&
		        json("body:remove-class(no-scroll)").contains("\"arg\":\"no-scroll\"") &&
		        json("img:remove-attr(srcset)").contains("\"action\":\"remove-attr\""),
		      "the three actions, each with its argument");
		check(json("div:style(position: static !important)").startsWith("refused: :style()") &&
		        json("div:others()").startsWith("refused: :others()"),
		      ":style() and :others() are refused by name: " +
		        json("div:style(position: static !important)"));
		check(json("div:remove():has-text(x)").startsWith("refused") &&
		        json(":has-text(x)").startsWith("refused") &&
		        json("div:has-text(x").startsWith("refused") &&
		        json("div{color:red}:has-text(x)").startsWith("refused"),
		      "an action not last, no selector first, unclosed, and unsafe CSS are refused");
	}

	section("a list's procedural rules are kept with their scope, and refused unscoped");
	{
		const subscription_read rep = filter_subscription::read(
		  "[Adblock Plus 2.0]\n"
		  "news.example##div.post:has-text(Sponsored)\n"
		  "a.example,b.example#?#.box:upward(2)\n"
		  "##div:has-text(Advertisement)\n"
		  "shop.example##div:style(display: block !important)\n"
		  "plain.example##.ad\n",
		  0, true);
		check(rep.procedural.size() == 2 &&
		        rep.procedural[0].scope == "news.example" &&
		        rep.procedural[0].selector == "div.post:has-text(Sponsored)" &&
		        rep.procedural[1].scope == "a.example,b.example",
		      "the scoped ## and #?# rules, each with its scope");
		check(rep.generic.isEmpty(),
		      "the unscoped one is not taken as a generic CSS selector: " + rep.generic.join(" | "));
		check(rep.rules.size() == 1 && rep.accepted == 3 && rep.unsupported == 2,
		      QString("the plain rule stays a rule; two refused (accepted %1, unsupported %2)")
		        .arg(rep.accepted).arg(rep.unsupported));
	}

	section("an entity scope, site.*, matches the site under any suffix");
	{
		check(filter_list::site_matches("google.*", "google.com") &&
		        filter_list::site_matches("google.*", "www.google.co.uk") &&
		        filter_list::site_matches("google.*", "www.google.de") &&
		        filter_list::site_matches("google.*", "google.com.au"),
		      "google.* on .com, .co.uk, .de and .com.au");
		check(!filter_list::site_matches("google.*", "google.evil.com") &&
		        !filter_list::site_matches("google.*", "notgoogle.com") &&
		        !filter_list::site_matches("google.*", "google"),
		      "and not on another site that holds the word");
		check(filter_list::site_matches("maps.google.*", "maps.google.de") &&
		        !filter_list::site_matches("maps.google.*", "www.google.de"),
		      "an entity with a subdomain names that subdomain only");
		const QStringList keys = { "www.google.co.*", "google.co.*", "co.*",
		                           "www.google.*", "google.*" };
		check(filter_list::entity_keys("www.google.co.uk") == keys,
		      "the keys a host answers to: " + filter_list::entity_keys("www.google.co.uk").join(" "));
		check(filter_list::scope_matches("google.*,~maps.google.*", "www.google.de") &&
		        !filter_list::scope_matches("google.*,~maps.google.*", "maps.google.de"),
		      "in a scope list, with an excluded entity");

		network_rule_set set;
		network_rule r;
		QList<network_rule> rules;
		parse_network_rule("||ads.example^$domain=google.*", &r, nullptr);
		rules << r;
		parse_network_rule("/promo-tile.$domain=amazon.*|~amazon.co.jp", &r, nullptr);
		rules << r;
		set.replace(rules);
		auto blocks = [&](const char *url, const char *site) {
			network_rule_set::request q;
			q.url = QString::fromLatin1(url);
			q.lower = q.url.toLower();
			q.host = QUrl(q.url).host();
			q.site = QString::fromLatin1(site);
			return set.blocks(q);
		};
		check(blocks("https://ads.example/x.js", "www.google.co.uk") &&
		        !blocks("https://ads.example/x.js", "google.evil.com"),
		      "domain=google.* is found through the site index, and only there");
		check(blocks("https://cdn.example/promo-tile.png", "www.amazon.de") &&
		        !blocks("https://cdn.example/promo-tile.png", "www.amazon.co.jp"),
		      "and an entity rule with no host, with an excluded suffix");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

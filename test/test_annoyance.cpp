// The "something got through here" log: what it keeps and what a round trip
// through disk does to it.
#include "annoyance_log.h"
#include "filter_signals.h"
#include "annoyed_dialog.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static annoyance_report make(const QString &host, const QStringList &suspects) {
	annoyance_report r;
	r.host = host;
	r.page = "https://" + host + "/watch?v=1";
	r.when = QDateTime::fromString("2026-08-04T17:30:00", Qt::ISODate);
	r.suspects = suspects;
	// Carried on every made-up report, so the round trip below proves the field
	// survives disk rather than merely existing in memory.
	r.capabilities = QStringList{ "17:30:01  Camera: allow",
	                               "17:29:58  Microphone: block" };
	r.observed = 42;
	return r;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	section("filing a report");
	{
		annoyance_log log;
		check(log.is_empty(), "a fresh log has nothing in it");
		log.add(make("a.test", { "https://ads.example/x?a=1" }));
		log.add(make("b.test", {}));
		log.add(make("a.test", { "https://ads.example/y" }));
		check(log.count_for("a.test") == 2, "reports are counted per site");
		check(log.count_for("b.test") == 1, "and do not leak between sites");
		check(log.count_for("never.test") == 0, "a site nobody filed against has none");
		check(log.for_host("a.test").size() == 2, "and can be read back per site");
	}

	section("what came of it, recorded after the fact");
	{
		// The outcome is known only once a dialog has been answered, and the
		// person may dismiss it. A report filed is worth keeping either way, so
		// the outcome is set separately rather than being part of filing.
		annoyance_log log;
		log.add(make("a.test", {}));
		log.add(make("a.test", {}));
		log.set_outcome("a.test", "zapped");
		const auto rs = log.for_host("a.test");
		check(rs.size() == 2 && rs.last().outcome == "zapped",
		      "the outcome lands on the most recent report for that site");
		check(rs.first().outcome.isEmpty(),
		      "and not on the earlier one, which stands as filed");
		// **`check(true)` tested that this did not crash and nothing else.**
		// "Does nothing" has two halves and neither was asserted: that no
		// phantom record appears for a host nobody filed against, and that
		// the sites which do have records are left alone.
		const int before = log.all().size();
		log.set_outcome("nobody.test", "zapped");
		check(log.for_host("nobody.test").isEmpty(),
		      "an outcome for a site with no reports invents no record");
		check(log.all().size() == before,
		      QString("and touches nothing else (%1, was %2)")
		          .arg(log.all().size()).arg(before));
	}

	section("a round trip through disk");
	{
		const QString path = QDir::temp().filePath("hydra-annoyance-test.ini");
		QFile::remove(path);

		annoyance_log log;
		// **A URL with commas in it**, which is the case that decides whether
		// the list is stored as a list or as a string somebody joined. Real
		// analytics addresses are full of them.
		const QStringList tricky = {
			"https://ads.example/px?tag_exp=1~2~3&list=a,b,c&x=1",
			"https://beacon.example/e?d=%7B%22a%22:1%7D,%22b%22",
		};
		log.add(make("a.test", tricky));
		log.set_outcome("a.test", "evolved");
		check(log.save(path), "it saves");

		annoyance_log back;
		check(back.load(path), "and loads");
		check(back.count_for("a.test") == 1, "with the report still there");
		const annoyance_report r = back.for_host("a.test").first();
		check(r.suspects == tricky,
		      QString("and addresses with commas survive intact (%1)")
		          .arg(r.suspects.size()));
		check(r.page == "https://a.test/watch?v=1", "the page address survives");
		check(r.observed == 42, "and how much traffic the page had made");
		check(r.outcome == "evolved", "and what came of it");
		check(r.capabilities.size() == 2 &&
		        r.capabilities.first().contains("Camera: allow"),
		      "and the capability evidence survives the file — the half of a "
		      "report that explains a page insisting it has no camera");
		check(r.when.isValid(), "and when it was filed");

		// Shrinking must not leave a tail: an array rewritten shorter used to
		// be the classic way to read back a record that was deleted.
		annoyance_log fewer;
		fewer.add(make("c.test", {}));
		check(fewer.save(path), "a shorter log saves over a longer one");
		annoyance_log after;
		check(after.load(path), "and loads");
		check(after.count_for("a.test") == 0 && after.count_for("c.test") == 1,
		      "leaving no trace of the reports that were removed");

		QFile::remove(path);
	}

	// The second kind of evidence a report carries, and the one that answers a
	// complaint the network half cannot: "it says I have no camera" produces no
	// ad-shaped request at all.
	section("capabilities a page asked for");
	{
		filter_signals sig;
		sig.note_capability("meet.test", "Camera", "https://meet.test/call", "allow");
		sig.note_capability("meet.test", "Microphone", "https://meet.test/call", "allow");
		// An embedded frame asking on the page's behalf, which is the case
		// worth being able to see: the rule was written about one host and the
		// asking was done by another.
		sig.note_capability("meet.test", "Camera", "https://sdk.other.test/f", "block");

		const QStringList caps = sig.capabilities_for("meet.test");
		check(caps.size() == 3, QString("all three are kept (%1)").arg(caps.size()));
		check(caps.first().contains("Camera") && caps.first().contains("block"),
		      "most recent first, so a page asking in a loop cannot push the "
		      "interesting first attempt off the end");
		check(caps.first().contains("asked by sdk.other.test"),
		      "and a frame that is not the site is named, because being granted "
		      "a camera the page never asked for is the interesting failure");
		check(!caps.at(1).contains("asked by"),
		      "while the site asking about itself does not repeat its own name");

		check(sig.capabilities_for("elsewhere.test").isEmpty(),
		      "and it is per site, like every other signal here");

		// Clearing a site has to forget these too, or "clear" is a lie about
		// the most sensitive thing in the record.
		sig.clear_site("meet.test");
		check(sig.capabilities_for("meet.test").isEmpty(),
		      "forgetting a site forgets what it asked for");
	}

	section("forgetting, which has to work or this should not exist");
	{
		annoyance_log log;
		log.add(make("a.test", {}));
		log.add(make("b.test", {}));
		log.clear_host("a.test");
		check(log.count_for("a.test") == 0, "a site can be forgotten");
		check(log.count_for("b.test") == 1, "without taking the others with it");
		log.clear_all();
		check(log.is_empty(), "and the whole record can be dropped at once");
	}

	section("a log that was never written");
	{
		annoyance_log log;
		check(!log.load(QDir::temp().filePath("hydra-no-such-annoyance.ini")),
		      "loading a file that does not exist says so");
		check(log.is_empty(), "and leaves nothing behind");
	}

	section("collapsing a suspect list into something readable");
	{
		// **Real addresses, from the kisskh capture.** The three analytics calls
		// are the case this exists for, and they are not as similar as they look:
		// their query strings carry 41, 42 and 44 *different keys*. An earlier
		// version grouped by `site_extractor::shape_of`, which keeps query keys,
		// so it saw three shapes and collapsed nothing at all.
		const QStringList suspects = {
			"https://region1.analytics.google.com/g/collect?v=2&tid=G-R3CRN9FY5Q"
			"&gtm=45je67u0&_p=1785782589217&_gaz=1&gcd=13l3l3l2l1l1&npa=1",
			"https://region1.analytics.google.com/g/collect?v=2&tid=G-R3CRN9FY5Q"
			"&gtm=45je67u0&_p=1785782589217&gcd=13l3l3l2l1l1&npa=1&ibt=1",
			"https://region1.analytics.google.com/g/collect?v=2&tid=G-R3CRN9FY5Q"
			"&gtm=45je67u0&_p=1785782589217&gcd=13l3l3l2l1l1&npa=1&ibt=1&ngs=1",
			"https://static.cloudflareinsights.com/beacon.min.js/"
			"v4513226cdae34746b4dedf0b4dfa099e1781791509496",
			"https://www.google.se/ads/ga-audiences?v=1&t=sr&tid=G-R3CRN9FY5Q",
		};
		const auto groups = annoyed_dialog::collapse_by_shape(suspects);
		check(groups.size() == 3,
		      QString("five addresses read as three endpoints (%1)")
		          .arg(groups.size()));
		if (groups.size() == 3) {
			check(groups[0].count == 3,
			      QString("the analytics endpoint stands for its three calls (%1)")
			          .arg(groups[0].count));
			check(groups[1].count == 1 && groups[2].count == 1,
			      "and the other two stand for themselves");
			check(groups[0].url.contains("g/collect"),
			      "the row shows the first address seen, not a reconstruction");
		}

		// The path-token case, which is why the query is dropped *and*
		// `shape_of` is still asked about the rest: two beacons whose payload is
		// in the path, not the query.
		const auto beacons = annoyed_dialog::collapse_by_shape({
			"https://static.cloudflareinsights.com/beacon.min.js/"
			"v4513226cdae34746b4dedf0b4dfa099e1781791509496",
			"https://static.cloudflareinsights.com/beacon.min.js/"
			"v9922117bbcf01122a9ffe1234567890abcdef1234567890",
		});
		check(beacons.size() == 1 && beacons[0].count == 2,
		      QString("two beacons differing only by a path token fold together (%1)")
		          .arg(beacons.size()));

		// Order is first-seen, so the list does not reshuffle itself between
		// reports of the same page.
		const auto order = annoyed_dialog::collapse_by_shape(
		    { "https://b.test/two", "https://a.test/one", "https://b.test/two" });
		check(order.size() == 2 && order[0].url.contains("b.test"),
		      "groups keep the order their shapes first appeared in");

		check(annoyed_dialog::collapse_by_shape({}).isEmpty(),
		      "and nothing collapses to nothing");
	}

	// **A log that could not be read must not present itself as an empty
	// one**, and the order of the first two statements used to guarantee it
	// did. `m_reports.clear()` ran before the file was even looked for, so
	// every failure emptied the log and then said so -- and the caller,
	// dropping the answer, was left with an empty store pointed at a file it
	// had never read. The next save wrote that back.
	section("a log that will not parse is left alone");
	{
		const QString dir = QDir::temp().filePath("hydra-annoyance-garbage");
		QDir(dir).removeRecursively();
		QDir().mkpath(dir);

		annoyance_report r;
		r.host = "example.com";
		r.page = "https://example.com/";
		annoyance_log log;
		log.add(r);
		const QString good = dir + "/good.ini";
		check(log.save(good), "a log with a report writes");
		check(log.all().size() == 1, "and holds it");

		// **Plain prose, chosen by measurement rather than by guess.** The
		// first draft used binary NULs, on the assumption that the more
		// damaged a file looked the more certainly QSettings would reject it.
		// Measured across six inputs, it is the other way round: NULs are
		// tolerated and report NoError, while ordinary text and JSON both
		// give FormatError. A test built on the first would have asserted
		// nothing, because the load would have succeeded.
		const QString bad = dir + "/bad.ini";
		{
			QFile f(bad);
			f.open(QIODevice::WriteOnly);
			f.write("this is not an ini file at all\n");
		}
		check(!log.load(bad), "a file that will not parse is refused");
		check(log.all().size() == 1,
		      QString("and the refusal left the reports alone (%1)")
		          .arg(log.all().size()));

		// The control: a path with no file must still be refused, and that
		// refusal is the ordinary first run rather than a failure.
		check(!log.load(dir + "/not-here.ini"),
		      "a path with no file is still refused");

		// And a real log round-trips, so the checks above are not passing
		// because loading never works.
		annoyance_log back;
		check(back.load(good) && back.all().size() == 1,
		      "while a log that was written reads back");

		QDir(dir).removeRecursively();
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

// The "something got through here" log: what it keeps and what a round trip
// through disk does to it.
#include "annoyance_log.h"

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

static annoyance_report make(const QString &host, const QStringList &suspects) {
	annoyance_report r;
	r.host = host;
	r.page = "https://" + host + "/watch?v=1";
	r.when = QDateTime::fromString("2026-08-04T17:30:00", Qt::ISODate);
	r.suspects = suspects;
	r.observed = 42;
	return r;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

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
		log.set_outcome("nobody.test", "zapped");
		check(true, "setting an outcome for a site with no reports does nothing");
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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

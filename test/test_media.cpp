// Saving media by what it is: the evidence a site gives, a model's reading of
// it checked against that evidence, and the tally that says which tasks a
// model is too weak for. Offline: the corpus is fixtures, and the model is a
// string a test writes.
//
// **The rules are scored here too, against the same labels a model is**, so
// the baseline a model has to beat is a number rather than an impression --
// and the rules' failures on this corpus are the reason the holder asked for
// a model at all: the uploader is a label, a broadcaster, a lyrics channel.
#include "media_corpus.h"
#include "media_evidence.h"
#include "media_interpretation.h"
#include "model_tally.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// From the tree root, which is where `make test` runs the suites; beside
// this file otherwise. `__FILE__` alone is not enough: the suites are built
// from `test/`, so it is a relative path and resolves against wherever the
// binary happens to be run from.
static QString corpus_dir() {
	for (const QString &d : { QStringLiteral("test/fixture/media"),
	                          QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() +
	                            QStringLiteral("/fixture/media") })
		if (QFileInfo(d + QStringLiteral("/labels.json")).exists())
			return d;
	return QString();
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("times on a line are found, and what only looks like one is not");
	{
		const auto lines = media_evidence::timestamped_lines(
		  "Intro\n0:00 Opening\n12:34 - Artist - Track\n1:02:03 Long one\n"
		  "score 3:2 tonight\n2026:10:09 a date\n5:99 not a time\n"
		  "(7:45)\n[1:00:00] Last [ID]");
		QStringList got;
		for (const auto &e : lines)
			got << QString("%1=%2").arg(e.start).arg(e.text);
		const QStringList want = { "0=Opening", "754=Artist - Track",
		                           "3723=Long one", "3600=Last [ID]" };
		check(got == want,
		       QString("h:mm:ss and m:ss with text left, and nothing else (%1)")
		           .arg(got.join(" | ")));
	}

	const QList<corpus_case> corpus = load_corpus(corpus_dir());
	check(corpus.size() == 12,
	       QString("the corpus loads, every labelled case with its fixture (%1)")
	           .arg(corpus.size()));
	const auto find = [&corpus](const char *name) {
		for (const corpus_case &c : corpus)
			if (c.name == QLatin1String(name))
				return c;
		return corpus_case();
	};

	section("evidence is collected and not judged");
	{
		const media_evidence ev = find("set_groovix").evidence;
		check(ev.uploader == "Groovix" && ev.duration > 3600 &&
		          ev.categories == QStringList{ "People & Blogs" },
		       "the site's own fields, as stated");
		QStringList sources;
		for (const auto &t : ev.tracklists)
			sources << QString("%1:%2").arg(t.source).arg(t.entries.size());
		check(sources.contains("chapters:21") &&
		          sources.contains("description:29"),
		       QString("every candidate tracklist, each on its own, none chosen "
		               "(%1)").arg(sources.join(", ")));
		check(ev.comments.size() == 20 && ev.comments.first().pinned &&
		          ev.comments.first().by_uploader,
		       "and the top comments, with who wrote them and whether pinned");
	}

	section("a name must come from the evidence");
	{
		const media_evidence ev = find("lyrics_rare_vibes_adele").evidence;
		check(media_interpretation::grounded("Adele", ev) &&
		          media_interpretation::grounded("  hello ", ev),
		       "a name in the title is grounded, case and spacing aside");
		check(!media_interpretation::grounded("Beyonce", ev) &&
		          !media_interpretation::grounded("", ev),
		       "an invented one, or none, is not");
	}

	section("a model's answer is kept task by task, or replaced by the rules'");
	{
		const media_evidence ev = find("label_monstercat_noisestorm").evidence;
		const media_reading good = media_interpretation::checked(
		  "Here you go:\n```json\n{\"kind\":\"music\",\"artist\":\"Noisestorm\","
		  "\"title\":\"Crab Rave\",\"is_set\":false,\"tracklist\":\"\","
		  "\"confidence\":0.9}\n```", ev);
		check(good.model_answered && good.rejected.isEmpty() &&
		          good.artist == "Noisestorm" && good.title == "Crab Rave" &&
		          good.from_model.size() == 4,
		       QString("a grounded answer is kept whole (%1)")
		           .arg(good.rejected.join("; ")));

		const media_reading invented = media_interpretation::checked(
		  "{\"kind\":\"song\",\"artist\":\"Noise Storm Official\","
		  "\"title\":\"Crab Rave\",\"tracklist\":\"comment 9\"}", ev);
		check(invented.artist == "Monstercat Instinct" &&
		          invented.title == "Crab Rave" &&
		          invented.rejected.size() == 3,
		       QString("an invented artist, a kind not on the list and a "
		               "tracklist that is no candidate are each replaced, and the "
		               "grounded title kept (%1)")
		           .arg(invented.rejected.join("; ")));

		const media_reading none = media_interpretation::checked(
		  "I think this is a song by Noisestorm.", ev);
		check(!none.model_answered && none.rejected.size() == 1 &&
		          none.rejected.first().startsWith("answer:"),
		       "a reply with no answer in it is the rules throughout");

		media_evidence backwards = ev;
		backwards.duration = 600;
		media_evidence::tracklist out_of_order;
		out_of_order.source  = "description";
		out_of_order.entries = { { 0, "a" }, { 300, "b" }, { 120, "c" } };
		backwards.tracklists = { out_of_order };
		const media_reading back = media_interpretation::checked(
		  "{\"kind\":\"music\",\"artist\":\"Noisestorm\",\"title\":\"Crab Rave\","
		  "\"tracklist\":\"description\"}", backwards);
		check(back.tracklist.isEmpty() &&
		          back.rejected.join(" ").contains("backwards"),
		       "a tracklist whose times go backwards is not taken");
	}

	section("the tally says which tasks a model is too weak for");
	{
		model_tally t;
		const QString m = "qwen2.5-coder:14b";
		for (int i = 0; i < 4; ++i)
			t.record(m, "artist", false);
		check(t.weak_tasks(m).isEmpty(),
		       "four failures are not yet a pattern");
		t.record(m, "artist", true);
		for (int i = 0; i < 10; ++i)
			t.record(m, "kind", i != 0);
		check(t.weak_tasks(m) == QStringList{ "artist" },
		       QString("4 of 5 rejected is weak, 1 of 10 is not (%1)")
		           .arg(t.weak_tasks(m).join(", ")));
		check(t.warnings(m).value(0).contains("4 of its last 5"),
		       QString("and it is said with the numbers (%1)")
		           .arg(t.warnings(m).value(0)));

		QTemporaryDir dir;
		{
			QSettings s(dir.filePath("t.ini"), QSettings::IniFormat);
			t.save(s);
		}
		model_tally back;
		QSettings s(dir.filePath("t.ini"), QSettings::IniFormat);
		back.load(s);
		check(back.of(m, "artist").answered == 5 &&
		          back.of(m, "artist").rejected == 4 &&
		          back.weak_tasks(m) == QStringList{ "artist" },
		       "it survives a restart, a model name with a colon included");
	}

	section("the rules, scored against the labels: the baseline to beat");
	{
		corpus_score rules, perfect;
		for (const corpus_case &c : corpus) {
			++rules.total;
			++perfect.total;
			const auto r = score_case(media_interpretation::by_rules(c.evidence), c);
			for (auto it = r.cbegin(); it != r.cend(); ++it)
				rules.right[it.key()] += it.value() ? 1 : 0;
			media_reading ideal;
			media_reading::kind_from(c.kind.first(), &ideal.what);
			ideal.artist = c.artist.first();
			ideal.title = c.title.first();
			ideal.is_set = c.is_set;
			ideal.tracklist = c.tracklist.first();
			const auto p = score_case(ideal, c);
			for (auto it = p.cbegin(); it != p.cend(); ++it)
				perfect.right[it.key()] += it.value() ? 1 : 0;
		}
		check(perfect.outside().isEmpty() && perfect.share("artist") == 1.0,
		       "a reading that is the labels scores full marks, so the scorer "
		       "is not the thing failing");
		QStringList line;
		for (auto it = rules.right.cbegin(); it != rules.right.cend(); ++it)
			line << QString("%1 %2/%3").arg(it.key()).arg(it.value())
			            .arg(rules.total);
		std::printf("     rules: %s\n", qPrintable(line.join(", ")));
		check(rules.outside().contains("artist") &&
		          rules.outside().contains("title"),
		       QString("the rules are outside the comfort zone on artist and "
		               "title -- the reason for a model (%1)")
		           .arg(rules.outside().join(", ")));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

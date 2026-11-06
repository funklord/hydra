// What a saved mix leaves beside itself, read back from the files.
//
// The chapters inside a file are `test_media`'s. These are the other half: the
// text a person reads and the two files mixing software does, built from a real
// set's yt-dlp answer, and the download job that writes them at the moment its
// file has its last name.
#include "download_manager.h"
#include "media_detector.h"
#include "media_evidence.h"
#include "media_lookup.h"
#include "media_sheet.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

#ifndef HYDRA_SRC_DIR
#define HYDRA_SRC_DIR "../src"
#endif

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &what) {
	std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", qUtf8Printable(what));
	std::fflush(stdout);
	(ok ? g_pass : g_fail)++;
}

static QString slurp(const QString &path) {
	QFile f(path);
	f.open(QIODevice::ReadOnly);
	return QString::fromUtf8(f.readAll());
}

// A transport that hands over a file and says it finished.
class instant_source : public download_source {
public:
	using download_source::download_source;
	QString id() const override { return "instant"; }
	QString display_name() const override { return "Instant"; }
	source_capabilities capabilities() const override { return {}; }
	bool accepts(const QUrl &, QString * = nullptr) const override { return true; }
	bool start(const download_request &req, QString *) override {
		const QString path = QDir(req.directory).filePath(req.file_name);
		QFile f(path);
		f.open(QIODevice::WriteOnly);
		f.write("bytes");
		f.close();
		const int job = req.id;
		QTimer::singleShot(0, this, [this, job, path] {
			download_progress p;
			p.state = download_state::running;
			p.path  = path;
			emit progressed(job, p);
			emit finished(job, true, QString());
		});
		return true;
	}
	void cancel(int) override {}
};

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);

	std::printf("-- times\n");
	{
		media_evidence::entry e;
		check(media_evidence::entry_of("1:02:03.250 Artist - Title", &e) &&
		      qFuzzyCompare(e.start, 3723.25) && e.text == "Artist - Title",
		      QString("a fraction of a second is kept (%1, %2)").arg(e.start).arg(e.text));
		check(media_evidence::entry_of("[12:30.5] Track", &e) &&
		      qFuzzyCompare(e.start, 750.5) && e.text == "Track",
		      QString("in brackets too (%1)").arg(e.text));
		check(media_evidence::entry_of("12:30 Track", &e) && e.start == 750,
		      "a plain time is as it was");
	}

	std::printf("-- a tracklist with no times\n");
	{
		const QString text = "Intro words here\n\n1. One - First\n2. Two - Second\n"
		                     "3. Three - Third\n\nThanks for listening\n";
		const auto runs = media_evidence::untimed_runs(text);
		check(runs.size() == 1 && runs.value(0).size() == 3,
		      QString("found (%1 run(s))").arg(runs.size()));
		check(media_evidence::untimed_runs("Instagram - https://i.example/a\n"
		                                   "Twitter - https://t.example/a\n"
		                                   "Site - www.example.com\n").isEmpty(),
		      "a list of links is not one");
		check(media_evidence::untimed_runs("One - First\nTwo - Second\n").isEmpty(),
		      "two lines are not one");
	}

	std::printf("-- from a real set\n");
	QFile fixture(QString::fromUtf8(HYDRA_SRC_DIR "/../test/fixture/media/set_groovix.json"));
	check(fixture.open(QIODevice::ReadOnly), "the fixture is there");
	const QByteArray json = fixture.readAll();
	const media_evidence ev = media_evidence::from_json(json);
	check(!ev.tracklists.isEmpty(), "it has candidates");
	const QString chosen = ev.tracklists.value(0).source;
	const QString whole = ev.description + "\nthe part the evidence cut off";
	media_sheet sheet = media_sheet::from_evidence(ev, chosen, "Groovix", "A Set",
	                                               QUrl("https://example.test/v"), whole);
	const QString text = sheet.text();
	check(text.startsWith("A Set\n=====\n"), "the title heads the text");
	check(text.contains("Artist: Groovix") && text.contains("From: https://example.test/v"),
	      "with who and where");
	check(text.contains(QString("Tracklist: %1 tracks, from the %2. Times are the "
	                            "source's, to the second.")
	                      .arg(sheet.entries.size()).arg(chosen)),
	      "it says how many, from where, and how fine");
	check(text.contains("00:00:00.000  "), "times line up");
	check(text.contains("the part the evidence cut off"), "and the whole description follows");

	const QString m3u = sheet.m3u("a.webm");
	check(m3u.startsWith("#EXTM3U\n"), "a playlist");
	check(m3u.count("#EXTVLCOPT:start-time=") == sheet.entries.size() &&
	      m3u.count("\na.webm\n") == sheet.entries.size(),
	      "with an entry per track, all naming the file");
	const QString cue = sheet.cue("a.webm");
	check(cue.contains("FILE \"a.webm\" WAVE"), "a cue sheet");
	check(cue.count("  TRACK ") == sheet.entries.size(), "with a track each");

	std::printf("-- exact forms\n");
	{
		media_sheet s;
		s.title    = "Say \"hi\"";
		s.duration = 3700;
		s.source   = "description";
		s.entries  = { { 0, "01. Alpha - One" }, { 62.5, "Beta" }, { 3600, "Gamma - Three" } };
		const QString c = s.cue("set.mp3");
		check(c.contains("TITLE \"Say 'hi'\""), "a quote cannot end a cue string");
		check(c.contains("FILE \"set.mp3\" MP3"), "an mp3 says so");
		check(c.contains("TRACK 01 AUDIO\n    TITLE \"One\"\n    PERFORMER \"Alpha\"\n"
		                 "    INDEX 01 00:00:00"), "number gone, artist and title split");
		check(c.contains("INDEX 01 01:02:38"), "half a second is 37.5 frames, rounded");
		check(c.contains("INDEX 01 60:00:00"), "and minutes do not roll into hours");
		const QString p = s.m3u("set.mp3");
		check(p.contains("#EXTVLCOPT:start-time=62.500\n#EXTVLCOPT:stop-time=3600.000\n"),
		      "the playlist keeps the fraction");
		check(p.contains("#EXTVLCOPT:stop-time=3700.000"), "and the last stops at the end");
		check(s.text().contains("to the millisecond"), "and the text says it is that fine");

		media_sheet many = s;
		many.entries.clear();
		for (int i = 0; i < 100; ++i)
			many.entries << media_evidence::entry{ i * 10.0, "t" };
		check(many.cue("x").isEmpty(), "a hundred tracks is past what a cue sheet holds");

		const media_sheet back = media_sheet::from_json(s.to_json());
		check(back.text() == s.text() && back.cue("x") == s.cue("x"), "it survives as JSON");
	}

	std::printf("-- saying there is none\n");
	{
		media_sheet s;
		s.title = "A Mix";
		s.description = "Enjoy.";
		check(s.summary() == "no tracklist found", "the summary says so");
		check(s.text().contains("Tracklist: none found."), "the text says so");
		check(s.text().contains("Enjoy."), "and keeps the description");
		check(s.m3u("x").isEmpty() && s.cue("x").isEmpty(), "with no playlist to write");

		s.untimed_source = "description";
		s.untimed = { "A - One", "B - Two", "C - Three" };
		check(s.summary().contains("without times"), "one with no times is still one");
		check(s.text().contains("which gives no times"), "and says what it lacks");
		check(s.m3u("x").isEmpty(), "and makes no playlist of positions it does not have");
	}

	std::printf("-- files\n");
	QTemporaryDir dir;
	{
		const QString media = dir.filePath("set.webm");
		QString error;
		check(sheet.write_beside(media, &error), QString("written (%1)").arg(error));
		check(QFile::exists(media + ".tracklist.txt") && QFile::exists(media + ".m3u") &&
		      QFile::exists(media + ".cue"), "all three, named from the whole file name");
		QFile mine(media + ".cue");
		mine.open(QIODevice::WriteOnly);
		mine.write("edited");
		mine.close();
		sheet.write_beside(media, &error);
		check(slurp(media + ".cue") == "edited", "an existing file is left alone");

		media_sheet none;
		none.title = "x";
		none.write_beside(dir.filePath("plain.mp4"), &error);
		check(QFile::exists(dir.filePath("plain.mp4.tracklist.txt")) &&
		      !QFile::exists(dir.filePath("plain.mp4.m3u")) &&
		      !QFile::exists(dir.filePath("plain.mp4.cue")),
		      "with no tracklist there is the text and nothing else");
	}

	std::printf("-- a download job\n");
	{
		download_manager manager;
		manager.set_directory(dir.path());
		manager.add_source(new instant_source);
		download_hints hints;
		hints.sheet = sheet.to_json();
		QString error;
		const int with = manager.enqueue(QUrl("https://example.test/a"), "n", &error, {},
		                                 "job.webm", hints);
		const int without = manager.enqueue(QUrl("https://example.test/b"), "n", &error, {},
		                                    "bare.webm");
		const auto done = [&manager](int id) {
			for (const download_job &j : manager.jobs())
				if (j.id == id)
					return j.status == download_state::done;
			return false;
		};
		QElapsedTimer t;
		t.start();
		while ((!done(with) || !done(without)) && t.elapsed() < 5000)
			QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
		check(done(with) && done(without), QString("both finish (%1)").arg(error));
		check(QFile::exists(dir.filePath("job.webm.tracklist.txt")) &&
		      QFile::exists(dir.filePath("job.webm.cue")),
		      "the mix leaves its files");
		check(!QFile::exists(dir.filePath("bare.webm.tracklist.txt")),
		      "a job with no sheet leaves nothing");
		QString detail;
		for (const download_job &j : manager.jobs())
			if (j.id == with)
				detail = j.detail;
		check(detail.contains("Mix:") && detail.contains("tracks, with times"),
		      QString("and its row says what it found (%1)").arg(detail));
	}

	std::printf("-- a lookup remembered across a restart\n");
	{
		const QDateTime now = QDateTime::currentDateTimeUtc();
		const QString path = dir.filePath("lookups.json");
		media_item row;
		row.url   = QUrl("https://example.test/stream");
		row.sheet = sheet.to_json();
		media_lookups kept;
		const QUrl page("https://example.test/v");
		kept.finish(page, { row, row, row }, QString(), now);
		check(kept.save(path, now), "saved");
		check(slurp(path).count("\"sheet\":\"{") == 1,
		      QString("the sheet is in the file once (%1)")
		        .arg(slurp(path).count("\"sheet\":\"{")));
		media_lookups back;
		check(back.load(path, now), "loaded");
		const media_lookup *found = back.find(page, now);
		check(found && found->rows.size() == 3 && found->rows.last().sheet == row.sheet,
		      "and every row has it again");
	}

	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

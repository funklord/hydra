// Parsing yt-dlp's answer, and the format preference. Offline: the risky part
// is reading the JSON correctly, not running the process.
#include "ytdlp_resolver.h"
#include "ytdlp_updater.h"
#include <QUrl>
#include <QTest>
#include <QFile>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QSet>
#include <QMap>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void note(const QString &w) { std::printf("  --    %s\n", qPrintable(w)); }
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// Shaped like real --dump-single-json output: several renditions, some
// video-only, one progressive, one HLS, headers attached.
static const char *k_json = R"({
  "_type": "video",
  "title": "An Episode",
  "extractor_key": "SomeSite",
  "webpage_url": "https://example.com/watch/1",
  "formats": [
    { "format_id": "audio-only", "url": "https://cdn.example.com/a.m4a",
      "ext": "m4a", "protocol": "https", "vcodec": "none", "acodec": "mp4a.40.2",
      "filesize": 1048576 },
    { "format_id": "video-only-1080", "url": "https://cdn.example.com/v1080.mp4",
      "ext": "mp4", "protocol": "https", "height": 1080,
      "vcodec": "avc1.640028", "acodec": "none", "filesize": 52428800 },
    { "format_id": "prog-480", "url": "https://cdn.example.com/p480.mp4",
      "ext": "mp4", "protocol": "https", "height": 480,
      "vcodec": "avc1.4d401e", "acodec": "mp4a.40.2", "filesize_approx": 20971520,
      "format_note": "SD",
      "http_headers": { "Referer": "https://example.com/", "User-Agent": "UA/1.0" } },
    { "format_id": "hls-720", "url": "https://cdn.example.com/master.m3u8",
      "ext": "mp4", "protocol": "m3u8_native", "height": 720,
      "vcodec": "avc1.4d401f", "acodec": "mp4a.40.2" }
  ]
})";

static const char *k_playlist = R"({
  "_type": "playlist",
  "entries": [ { "title": "First", "extractor": "Sub",
                 "webpage_url": "https://example.com/w/9",
                 "formats": [ { "format_id": "only", "url": "https://c/x.mp4",
                                "ext": "mp4", "protocol": "https",
                                "vcodec": "avc1", "acodec": "mp4a" } ] } ]
})";

// Some extractors report one URL and no formats array at all.
static const char *k_bare = R"({
  "title": "Bare", "extractor": "Bare", "webpage_url": "https://e/1",
  "url": "https://cdn/bare.mp4", "ext": "mp4", "protocol": "https",
  "vcodec": "avc1", "acodec": "mp4a"
})";

// Whether this tree has the submodule checked out, looked for the way the
// resolver does -- a checkout is the file `yt_dlp/__main__.py` -- but from
// here rather than through the resolver, so the premise stays independent of
// the thing being tested. Both roots the resolver searches from, the binary's
// directory and the working directory, because checking only one would call a
// copy absent that the resolver goes on to find. A couple of levels up, since
// a suite may be run from the repository root or from `test/`.
static bool vendored_checkout_present() {
	QStringList roots;
	roots << QCoreApplication::applicationDirPath() << QDir::currentPath();
	QStringList rels;
	rels << "third_party/yt-dlp" << "../third_party/yt-dlp"
	      << "../../third_party/yt-dlp";
	for (const QString &root : roots)
		for (const QString &rel : rels)
			if (QFileInfo::exists(QDir(QDir(root).absoluteFilePath(rel))
			                          .filePath("yt_dlp/__main__.py")))
				return true;
	return false;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("parsing a normal answer");
	{
		const resolved_media m = ytdlp_resolver::parse(k_json);
		check(m.ok, QString("it parses (%1)").arg(m.error));
		check(m.title == "An Episode", "title");
		check(m.extractor == "SomeSite", "extractor key preferred over extractor");
		check(m.formats.size() == 4,
		      QString("all four renditions kept (%1)").arg(m.formats.size()));

		const media_format *prog = nullptr;
		for (const media_format &f : m.formats)
			if (f.format_id == "prog-480") prog = &f;
		check(prog != nullptr, "the progressive rendition is present");
		check(prog && prog->filesize == 20971520,
		      "filesize_approx is used when filesize is absent");
		check(prog && prog->headers.value("Referer") == "https://example.com/",
		      "http_headers are carried — the CDN wants them (§11.3)");
		check(prog && prog->has_video && prog->has_audio, "codecs read as present");

		const media_format *vo = nullptr;
		for (const media_format &f : m.formats)
			if (f.format_id == "video-only-1080") vo = &f;
		check(vo && vo->has_video && !vo->has_audio,
		      "acodec \"none\" means no audio, not unknown");
	}

	// **Saved as what it is**, asked for 2026-10-10:
	// <upload time>_<name>_-_<track>_[<id>].<ext>
	section("a download is named from its metadata");
	{
		resolved_media m;
		m.title = "Rick Astley - Never Gonna Give You Up (Official Video)";
		m.artist = "Rick Astley";
		m.track = "Never Gonna Give You Up";
		m.uploader = "Rick Astley";
		m.id = "dQw4w9WgXcQ";
		m.timestamp = 1256453398;    // 2009-10-25 06:49:58 UTC
		m.upload_date = "20091025";
		const QString full = ytdlp_resolver::file_name_for(m, "mp4");
		check(full == "20091025T064958Z_Rick_Astley_-_Never_Gonna_Give_You_Up"
		              "_[dQw4w9WgXcQ].mp4",
		       QString("time, artist, track and id (%1)").arg(full));

		resolved_media day = m;
		day.timestamp = -1;
		check(ytdlp_resolver::file_name_for(day, "m4a").startsWith("20091025_"),
		       "the day alone when that is all the site gives");

		resolved_media clip;
		clip.title = "How to: fix it / part 2?";
		clip.uploader = "Sällskapet för \"Tekniker\"";
		clip.id = "abc123";
		clip.upload_date = "20240102";
		const QString c = ytdlp_resolver::file_name_for(clip, "webm");
		check(c == "20240102_Sällskapet_för_Tekniker_-_How_to_fix_it_part_2_[abc123].webm",
		       QString("the uploader and title when there is no music metadata; "
		               "what a filesystem refuses is gone and the letters stay "
		               "(%1)").arg(c));

		resolved_media longer = m;
		longer.track = QString(400, QLatin1Char('x'));
		const QString l = ytdlp_resolver::file_name_for(longer, "mp4");
		check(l.toUtf8().size() <= 200 && l.endsWith("_[dQw4w9WgXcQ].mp4") &&
		          l.startsWith("20091025T064958Z_Rick_Astley_-_x"),
		       QString("an over-long title is shortened, keeping the time, name, "
		               "id and extension (%1 bytes)").arg(l.toUtf8().size()));

		check(ytdlp_resolver::file_name_for(resolved_media(), "mp4") ==
		          "download.mp4",
		       "and with nothing known, still a name");

		const resolved_media parsed = ytdlp_resolver::parse(
		  "{\"title\":\"T\",\"id\":\"vid1\",\"artist\":\"A\",\"track\":\"Tr\","
		  "\"album\":\"Al\",\"uploader\":\"U\",\"channel\":\"C\","
		  "\"timestamp\":1700000000,\"upload_date\":\"20231114\","
		  "\"duration\":215.5,\"categories\":[\"Music\"],"
		  "\"formats\":[{\"format_id\":\"1\",\"url\":\"https://x.test/a\","
		  "\"ext\":\"m4a\",\"vcodec\":\"none\",\"acodec\":\"mp4a\"}]}");
		check(parsed.id == "vid1" && parsed.artist == "A" &&
		          parsed.track == "Tr" && parsed.uploader == "U" &&
		          parsed.timestamp == 1700000000 &&
		          parsed.upload_date == "20231114",
		       "and the fields it is built from are read from yt-dlp's answer");
	}

	section("choosing between them");
	{
		const resolved_media m = ytdlp_resolver::parse(k_json);
		const media_format b = ytdlp_resolver::best(m);
		// 480p progressive beats 720p HLS and 1080p video-only on purpose.
		check(b.format_id == "prog-480",
		      QString("progressive wins over a taller manifest (%1)").arg(b.format_id));
		check(b.protocol == "https", "and it is a plain HTTP URL");
	}

	section("shapes that are not the normal one");
	{
		const resolved_media p = ytdlp_resolver::parse(k_playlist);
		check(p.ok && p.title == "First", "a playlist falls back to its first entry");

		const resolved_media b = ytdlp_resolver::parse(k_bare);
		check(b.ok && b.formats.size() == 1,
		      "a bare url with no formats array still yields one");
		check(ytdlp_resolver::best(b).url.toString() == "https://cdn/bare.mp4",
		      "and best() finds it");

		const resolved_media bad = ytdlp_resolver::parse("Unsupported URL: nope");
		check(!bad.ok, "non-JSON is refused");
		check(bad.error.contains("JSON"), "with a reason that says so");

		const resolved_media empty = ytdlp_resolver::parse("{\"formats\":[]}");
		check(!empty.ok, "no formats is a failure, not an empty success");

		const resolved_media noent = ytdlp_resolver::parse("{\"_type\":\"playlist\",\"entries\":[]}");
		check(!noent.ok, "an empty playlist is a failure too");
	}

	section("finding the program");
	{
		ytdlp_resolver r;
		std::printf("  ..    %s\n", qPrintable(r.description()));

		// **The premise, stated and checked rather than assumed.** This used
		// to assert outright that something was found, on the grounds that the
		// vendored submodule is present in this tree -- true here, and false
		// on any clone made without `--recurse-submodules`, which the README
		// warns about because it is easy to do. CI found it the same way: it
		// checks out without submodules, and a suite about parsing yt-dlp's
		// output failed for the absence of yt-dlp.
		//
		// So the environment fact is separated from the claim about our code.
		// Where a copy exists the resolver must find it, which is the thing
		// worth asserting; where none does, reporting unavailable is the
		// correct answer and not a defect.
		// Looked for the way the resolver does -- a checkout is the file
		// `yt_dlp/__main__.py` -- but from here rather than through the
		// resolver, so the premise is established independently of the thing
		// being tested. A couple of levels up, since a suite may be run from
		// the repository root or from `test/`.
		// Both roots the resolver searches from -- the binary's directory and
		// the working directory -- because checking only one would call a copy
		// absent that the resolver goes on to find, and then assert it was not
		// found.
		const bool vendored = vendored_checkout_present();
		const bool on_path = !QStandardPaths::findExecutable("yt-dlp").isEmpty();
		// **A checkout is not on its own something that runs.** The vendored
		// branch is guarded by python3 -- the submodule is inert source
		// without an interpreter -- so "vendored, therefore available" is a
		// premise this suite cannot make. It would fail on a machine with the
		// submodule and no python3, for the absence of python3, while
		// reporting that the resolver had failed to find a copy that is
		// sitting right there.
		const bool python = !QStandardPaths::findExecutable("python3").isEmpty();
		if ((vendored && python) || on_path) {
			check(r.available(),
			      QString("a runnable yt-dlp was located (%1)")
			          .arg(vendored && python ? "vendored copy present"
			                                   : "one on PATH"));
		} else if (vendored) {
			note("skipped: the submodule is here but python3 is not, so there "
			      "is nothing to run it with");
			check(!r.available(),
			      "and the resolver says so rather than claiming a copy it "
			      "cannot start");
		} else {
			note("skipped: no yt-dlp vendored and none on PATH, so there is "
			      "nothing for the resolver to find");
			check(!r.available(),
			      "and the resolver says so rather than claiming one");
		}
		check(!r.busy(), "and it starts idle");
	}

	section("when nothing runs, it names the thing that is missing");
	{
		// **This message reaches the status bar**, via main_window, so it is a
		// person reading it rather than a developer. It used to say "clone
		// with --recurse-submodules, or install it" whatever the reason --
		// including on a tree whose submodule is fully checked out and which
		// simply has no python3, where both offered remedies are already
		// satisfied and the one thing actually wrong goes unmentioned. The
		// vendored branch is guarded by python3, so a missing interpreter
		// falls straight through the PATH fallback and out the bottom.
		//
		// PATH is emptied rather than mocked, because it is the only input
		// this depends on: no python3 to run the vendored copy with, and no
		// yt-dlp of its own to fall back to.
		const QByteArray saved = qgetenv("PATH");
		qputenv("PATH", "/nonexistent-so-nothing-resolves");
		QString said;
		bool runnable = true;
		{
			ytdlp_resolver r;
			said = r.description();
			runnable = r.available();
		}
		qputenv("PATH", saved);

		check(!runnable, "with nothing on PATH, nothing is runnable");
		if (vendored_checkout_present()) {
			check(said.contains("python3"),
			      QString("and the message names python3 -- \"%1\"").arg(said));
			check(!said.contains("--recurse-submodules"),
			      "rather than advising a checkout this tree already has");
		} else {
			note("skipped: no vendored checkout here, so the submodule advice "
			      "is the right advice and there is nothing to distinguish");
		}
	}

	// **Which copy ran, and how old it is**, because on 2026-10-10 an
	// installed build ran the distribution's 2025.04.30, YouTube refused it,
	// and all the holder saw was a ten-second status message.
	section("the version that runs is known, and how old it is");
	{
		check(ytdlp_resolver::age_days("2025.04.30") > 365 &&
		          ytdlp_resolver::age_days(QDate::currentDate()
		                                     .toString("yyyy.MM.dd")) == 0 &&
		          ytdlp_resolver::age_days("nightly") == -1,
		       "a version is aged by the date it is named for, and a name that "
		       "is not one is not guessed at");

		QTemporaryDir fake;
		QDir().mkpath(fake.filePath("checkout/yt_dlp"));
		{
			QFile m(fake.filePath("checkout/yt_dlp/__main__.py"));
			m.open(QIODevice::WriteOnly);
			QFile v(fake.filePath("checkout/yt_dlp/version.py"));
			v.open(QIODevice::WriteOnly);
			v.write("# x\n__version__ = '2026.01.02'\nRELEASE_GIT_HEAD = 'x'\n");
		}
		const bool python = !QStandardPaths::findExecutable("python3").isEmpty();
		if (python) {
			qputenv("HYDRA_YTDLP", fake.filePath("checkout").toLocal8Bit());
			ytdlp_resolver r;
			qunsetenv("HYDRA_YTDLP");
			check(r.version() == "2026.01.02" &&
			          r.description().contains("2026.01.02"),
			       QString("a vendored copy's version is read from its file "
			               "(%1)").arg(r.description()));
		} else {
			note("skipped: no python3 to run a vendored copy with");
		}

		// A copy on PATH is asked. The only thing on PATH is a script that
		// answers like an old yt-dlp, so nothing vendored can be run.
		QDir().mkpath(fake.filePath("bin"));
		{
			QFile y(fake.filePath("bin/yt-dlp"));
			y.open(QIODevice::WriteOnly);
			y.write("#!/bin/sh\necho 2025.04.30\n");
			y.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		}
		const QByteArray saved = qgetenv("PATH");
		qputenv("PATH", fake.filePath("bin").toLocal8Bit());
		QString said, version;
		{
			ytdlp_resolver r;
			said = r.description();
			version = r.version();
		}
		qputenv("PATH", saved);
		check(version == "2025.04.30" && said.contains("2025.04.30 from PATH"),
		       QString("a copy on PATH is asked for its version (%1)").arg(said));
	}

	section("a yt-dlp that never answers is stopped rather than waited for");

	// **The condition, not the mechanism.** `--socket-timeout` bounds yt-dlp's
	// network reads and nothing else, so a stuck extractor, a DNS wait or a
	// build that stops for input never reaches `finished` -- and neither
	// `resolved` nor `failed` is emitted. The shell's "Asking yt-dlp about
	// ..." carries no timeout of its own, deliberately, because an answer
	// replaces it; so a request that never ends leaves that sentence on the
	// status bar for good and `busy()` true, which makes every later attempt
	// answer "Still looking..." for the rest of the session. One wedged
	// process took the feature with it.
	//
	// Driven with a stand-in that hangs, because that is the whole condition:
	// a program that answers, however badly, exercises the paths that already
	// worked.
	//
	// **The stand-in is a fake CHECKOUT, not a fake program**, because that is
	// what `HYDRA_YTDLP` names -- a directory the resolver accepts when it
	// holds `yt_dlp/__main__.py`, which it then runs as `python3 -m yt_dlp`.
	// The first version of this pointed the variable at a shell script, the
	// resolver ignored it as not-a-checkout, and the run went off to the real
	// yt-dlp and the real network: it passed, on a timeout of somebody else's
	// process, and left a python3 behind. Reading the variable's meaning is
	// what fixed it.
	{
		const QString dir = QDir::temp().filePath("hydra-ytdlp-watchdog");
		QDir(dir).removeRecursively();
		QDir().mkpath(dir + "/yt_dlp");
		{
			QFile f(dir + "/yt_dlp/__main__.py");
			f.open(QIODevice::WriteOnly | QIODevice::Text);
			// Long enough that only the watchdog can end this, short enough
			// that a stray copy is gone within the minute.
			f.write("import time\ntime.sleep(45)\n");
			f.close();
		}
		{
			QFile f(dir + "/yt_dlp/__init__.py");
			f.open(QIODevice::WriteOnly | QIODevice::Text);
			f.close();
		}
		qputenv("HYDRA_YTDLP", dir.toUtf8());
		qputenv("HYDRA_YTDLP_TIMEOUT_MS", "700");

		ytdlp_resolver r;
		r.refresh();
		// The premise, stated rather than assumed: without it the checks below
		// would be about whatever else the resolver found, which is exactly
		// how the first version of this went wrong.
		check(r.description().contains(dir),
		       QString("the fake checkout is what the resolver found (%1)")
		           .arg(r.description()));

		QString said;
		bool answered = false;
		QObject::connect(&r, &ytdlp_resolver::failed,
		                  [&](const QString &e) { said = e; answered = true; });
		QObject::connect(&r, &ytdlp_resolver::resolved,
		                  [&](const resolved_media &) { answered = true; });

		r.resolve(QUrl("http://example.invalid/watch"));
		check(r.busy(), "it starts and the resolver reports itself busy");

		// Comfortably past the bound and nowhere near the sleep.
		for (int waited = 0; waited < 6000 && !answered; waited += 50)
			QTest::qWait(50);

		check(answered, "an answer arrives rather than none at all");
		check(said.contains("did not answer"),
		       QString("and it says the wait was given up on (\"%1\")")
		           .arg(said));
		check(!said.contains(" 0 second"),
		       QString("naming a whole number of seconds rather than none "
		                "(\"%1\")").arg(said));
		check(!r.busy(),
		       "the resolver is free again, so the next request is not refused");

		qunsetenv("HYDRA_YTDLP");
		qunsetenv("HYDRA_YTDLP_TIMEOUT_MS");
		QDir(dir).removeRecursively();
	}

	// **One row of sound, the best**: sound is always wanted at its best,
	// and six of it were six rows to read past. The holder, 2026-10-22.
	section("the one sound to list");
	{
		auto sound = [](double abr, const char *protocol, qint64 size) {
			media_format f;
			f.has_video = false;
			f.abr       = abr;
			f.protocol  = protocol;
			f.filesize  = size;
			return f;
		};
		media_format picture;
		picture.abr = 999;
		const QList<media_format> fs = { sound(48, "https", 10), picture,
		                                 sound(160, "m3u8_native", 30),
		                                 sound(160, "https", 20),
		                                 sound(129, "https", 90) };
		check(ytdlp_resolver::best_sound_format(fs) == 3,
		      QString("the highest rate, a file over a manifest, a picture never "
		              "(%1)").arg(ytdlp_resolver::best_sound_format(fs)));
		check(ytdlp_resolver::best_sound_format({ picture }) == -1,
		      "and none when there is no sound alone");
		const resolved_media r = ytdlp_resolver::parse(
		  R"({"title":"t","formats":[{"format_id":"a","url":"https://x.invalid/a",
		      "vcodec":"none","acodec":"opus","tbr":133.5}]})");
		check(!r.formats.isEmpty() && qFuzzyCompare(r.formats.first().abr, 133.5),
		      "the rate is read, from tbr where abr is absent");
	}

	// **Five rows for every size**: YouTube offers each one as H.264 and VP9,
	// each as a file and as a stream, and as AV1 -- one picture in five
	// formats, which a flat list read as five copies. The holder,
	// 2026-10-22: one per size, the most compatible leading. The fixture is
	// a real answer's format list, Big Buck Bunny, with its signed addresses
	// replaced.
	section("each size has one lead, and it is the most compatible file");
	{
		QFile fx;
		for (const QString &d : { QStringLiteral("test/fixture/media"),
		                          QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() +
		                            QStringLiteral("/fixture/media") }) {
			fx.setFileName(d + "/formats_bbb.json");
			if (fx.exists())
				break;
		}
		check(fx.open(QIODevice::ReadOnly), "the format list fixture is there");
		const resolved_media r = ytdlp_resolver::parse(fx.readAll());
		const QSet<int> leads = ytdlp_resolver::picture_leads(r.formats);
		QMap<QString, QString> lead_of;
		QSet<QString> groups;
		for (int i = 0; i < r.formats.size(); ++i) {
			const QString g = ytdlp_resolver::picture_group(r.formats[i]);
			if (g.isEmpty())
				continue;
			groups << g;
			if (leads.contains(i))
				lead_of[g] = r.formats[i].format_id;
		}
		check(leads.size() == groups.size() && groups.size() == 8,
		      QString("one lead for each of the eight sizes (%1 leads, %2 sizes)")
		          .arg(leads.size()).arg(groups.size()));
		check(lead_of.value("1080p60") == "299" && lead_of.value("720p60") == "298",
		      QString("H.264 as a file where there is one (1080p60: %1, 720p60: %2)")
		          .arg(lead_of.value("1080p60"), lead_of.value("720p60")));
		check(lead_of.value("1440p60") == "308" && lead_of.value("2160p60") == "315",
		      QString("VP9 as a file above it, never AV1 or a stream (1440p60: %1)")
		          .arg(lead_of.value("1440p60")));
		check(ytdlp_resolver::audio_codec_name("opus") == "Opus" &&
		          ytdlp_resolver::audio_codec_name("mp4a.40.2") == "AAC" &&
		          ytdlp_resolver::audio_codec_name("mp4a.40.34") == "MP3" &&
		          ytdlp_resolver::audio_codec_name("none").isEmpty(),
		      "and sound by its codec, not the file it travels in");
		check(ytdlp_resolver::codec_name("avc1.64002A") == "H.264" &&
		          ytdlp_resolver::codec_name("vp09.00.41.08") == "VP9" &&
		          ytdlp_resolver::codec_name("vp9") == "VP9" &&
		          ytdlp_resolver::codec_name("av01.0.09M.08") == "AV1",
		      "codecs are named as a person knows them");
		media_format thirty;
		thirty.height = 480;
		thirty.fps    = 30;
		check(ytdlp_resolver::picture_group(thirty) == "480p",
		      "and a size names its rate only where it is more than ordinary");
	}

	// **Kept current from yt-dlp's signed releases** -- the holder,
	// 2026-10-22, after finding the shipped copy three months old. The
	// fixture is a real release's SHA2-256SUMS and its signature.
	section("an update is used only if yt-dlp's release key signed it");
	{
		QString fixture;
		for (const QString &d : { QStringLiteral("test/fixture/ytdlp"),
		                          QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() +
		                            QStringLiteral("/fixture/ytdlp") })
			if (QFileInfo::exists(d + "/SHA2-256SUMS"))
				fixture = d;
		auto read = [](const QString &p) {
			QFile f(p);
			return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
		};
		const QByteArray sums = read(fixture + "/SHA2-256SUMS");
		const QByteArray sig  = read(fixture + "/SHA2-256SUMS.sig");
		const QByteArray key  = ytdlp_updater::shipped_key();
		check(!sums.isEmpty() && !sig.isEmpty() && !key.isEmpty(),
		      "the fixture and the shipped release key are there");
		check(ytdlp_updater::tag_from(QUrl("https://github.com/yt-dlp/yt-dlp/releases/tag/2026.10.21"))
		          == "2026.10.21",
		      "the release is read from where latest points");
		check(ytdlp_updater::sum_for(sums, "yt-dlp").size() == 64 &&
		          ytdlp_updater::sum_for(sums, "yt-dlp_linux") != ytdlp_updater::sum_for(sums, "yt-dlp") &&
		          ytdlp_updater::sum_for(sums, "nothing-of-that-name").isEmpty(),
		      "a file's checksum is its own line's, and none for a name not there");
		if (QStandardPaths::findExecutable("gpgv").isEmpty()) {
			std::printf("  --    no gpgv here, so signatures are not checked\n");
		} else {
			QString why;
			check(ytdlp_updater::verify(sums, sig, key, &why),
			      QString("a real release's checksums verify (%1)").arg(why));
			QByteArray forged = sums;
			forged[0] = forged[0] == 'a' ? 'b' : 'a';
			why.clear();
			check(!ytdlp_updater::verify(forged, sig, key, &why) && !why.isEmpty(),
			      QString("and a changed one does not (%1)").arg(why));
			why.clear();
			check(!ytdlp_updater::verify(sums, sig, QByteArray("not a key"), &why),
			      QString("nor without the key (%1)").arg(why));
		}
	}

	// **A newer fetched copy wins over the pin, an older one does not.**
	section("the resolver prefers an update only when it is newer");
	{
		QStandardPaths::setTestModeEnabled(true);
		// Removed below, so checked first to be test mode's and not the
		// person's own profile.
		const bool ours = ytdlp_updater::directory().contains("qttest");
		check(ours, QString("the update directory is test mode's (%1)")
		                .arg(ytdlp_updater::directory()));
		if (ours)
			QDir(ytdlp_updater::directory()).removeRecursively();
		QDir().mkpath(ytdlp_updater::directory());
		auto put = [](const QString &name, const QByteArray &bytes) {
			QFile f(QDir(ytdlp_updater::directory()).filePath(name));
			f.open(QIODevice::WriteOnly);
			f.write(bytes);
		};
		ytdlp_resolver pinned_only;
		const QString pinned = pinned_only.version();
		put("yt-dlp", "PK");
		put("version", "2099.01.01");
		ytdlp_resolver newer;
		if (pinned.isEmpty() || QStandardPaths::findExecutable("python3").isEmpty()) {
			std::printf("  --    no pinned copy or python3 here, so the choice is not checked\n");
		} else {
			check(newer.version() == "2099.01.01" && newer.description().contains("updated"),
			      QString("a newer update is used (%1)").arg(newer.description()));
			put("version", "2000.01.01");
			ytdlp_resolver older;
			check(older.version() == pinned,
			      QString("and an older one is not (%1)").arg(older.description()));
		}
		// **Only an environment that has imported curl_cffi is used.** A
		// half-made one -- python there, no marker -- must not be.
		const QString venv = QDir(ytdlp_updater::directory()).filePath("venv");
		QDir().mkpath(venv + "/bin");
		QFile::link(QStandardPaths::findExecutable("python3"), venv + "/bin/python");
		check(ytdlp_updater::python().isEmpty(),
		      "a Python environment that never imported curl_cffi is not used");
		put("venv/impersonation-ok", "0.16.3");
		check(ytdlp_updater::python() == venv + "/bin/python",
		      QString("one that did is (%1)").arg(ytdlp_updater::python()));
		if (ours)
			QDir(ytdlp_updater::directory()).removeRecursively();
		QStandardPaths::setTestModeEnabled(false);
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

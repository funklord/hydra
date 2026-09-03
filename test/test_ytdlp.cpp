// Parsing yt-dlp's answer, and the format preference. Offline: the risky part
// is reading the JSON correctly, not running the process.
#include "ytdlp_resolver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

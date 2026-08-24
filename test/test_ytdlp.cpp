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
		QStringList roots;
		roots << QCoreApplication::applicationDirPath() << QDir::currentPath();
		QStringList rels;
		rels << "third_party/yt-dlp" << "../third_party/yt-dlp"
		      << "../../third_party/yt-dlp";
		bool vendored = false;
		for (const QString &root : roots)
			for (const QString &rel : rels)
				if (QFileInfo::exists(QDir(QDir(root).absoluteFilePath(rel))
				                          .filePath("yt_dlp/__main__.py")))
					vendored = true;
		const bool on_path = !QStandardPaths::findExecutable("yt-dlp").isEmpty();
		if (vendored || on_path) {
			check(r.available(),
			      QString("a runnable yt-dlp was located (%1)")
			          .arg(vendored ? "vendored copy present"
			                         : "one on PATH"));
		} else {
			note("skipped: no yt-dlp vendored and none on PATH, so there is "
			      "nothing for the resolver to find");
			check(!r.available(),
			      "and the resolver says so rather than claiming one");
		}
		check(!r.busy(), "and it starts idle");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QProcess;
#include <QTimer>

// One playable rendition yt-dlp reported.
struct media_format {
	QString format_id;
	QUrl    url;
	QString ext;
	QString protocol;      // "https", "m3u8_native", "http_dash_segments"...
	QString note;
	int     height   = 0;
	qint64  filesize = -1;   // -1 when unknown
	bool    has_video = true;
	bool    has_audio = true;
	QMap<QString, QString> headers;   // what the CDN expects to see (sec 11.3)
};

struct resolved_media {
	bool    ok = false;
	QString error;
	QString title;
	QString extractor;
	QUrl    webpage_url;
	// yt-dlp's whole answer, for `media_evidence`: comments, chapters and
	// the description are read from it there rather than copied here.
	QByteArray json;
	// What a file is named from. Any may be empty: a site reports what it
	// has, and the name is built from whatever arrived.
	QString id;            // the site's own id: a YouTube video id
	QString artist;        // music metadata, where the site has it
	QString track;
	QString uploader;      // otherwise who put it up
	QString channel;
	qint64  timestamp = -1;   // upload time, seconds since the epoch, UTC
	QString upload_date;      // YYYYMMDD, when only the day is known
	QList<media_format> formats;
};

// Asks yt-dlp what the video on a page actually is (architecture doc sec 11.5).
//
// This is the *first* thing to try, before anything cleverer. Where yt-dlp
// supports a site it yields a real URL, and a URL is the best possible outcome
// because everything downstream already works with one: the external player,
// the download manager's Range resume, the local proxy's context injection.
// The Media Source tap (sec 11.6) and generated extractors (sec 11.5) are for the
// long tail this cannot reach -- measured: it does not support every site, and
// says so plainly rather than guessing.
//
// yt-dlp is a Python program, so it is run as a subprocess rather than linked.
// Two ways it can be present, preferred in this order:
//
//   1. `yt-dlp` on PATH -- the user's own, kept current by their package
//      manager, which is the point of preferring it.
//   2. the vendored `third_party/yt-dlp` submodule under the system python3 --
//      pinned, always there after a recursive clone, but only as current as
//      the submodule pointer.
//
// With neither, available() is false and the feature is simply absent; nothing
// else degrades.
class ytdlp_resolver : public QObject {
	Q_OBJECT
public:
	explicit ytdlp_resolver(QObject *parent = nullptr);
	~ytdlp_resolver() override;

	// Re-probe. Cheap; call at startup and when settings change.
	void refresh();
	bool available() const { return !m_program.isEmpty(); }

	// One line for a settings page or a status bar: what was found, and where.
	QString description() const;
	// The version that will run, "2026.07.04", or empty when not known.
	QString version() const { return m_version; }
	// How many days old a yt-dlp version is, by the date it is named for;
	// -1 when it is not a date. yt-dlp is versioned by release date, and
	// YouTube breaks old releases often enough that the age is the first
	// thing to look at when one fails.
	static int age_days(const QString &version);

	// Asynchronous: exactly one of resolved() / failed() follows.
	void resolve(const QUrl &page_url);
	void cancel();
	bool busy() const;

	// --- pure, and separately testable -----------------------------------
	// Parses `--dump-single-json` output. Handles a playlist by taking its
	// first entry, because a watch page that yields a playlist is nearly
	// always one video plus related items.
	static resolved_media parse(const QByteArray &json);

	// **The file a download of this is saved as**, asked for by the holder
	// on 2026-10-10:
	//
	//     <upload time>_<name>_-_<track>_[<id>].<ext>
	//
	// The time is the upload's, as ISO 8601 basic in UTC --
	// `20091025T060958Z`, or `20091025` when only the day is known -- so a
	// folder of them sorts by when they were published and the time holds
	// no `_` of its own. The name is the artist where the site says it is
	// music, else the uploader or channel; the track is the track title,
	// else the video title. Whitespace becomes `_`, which is what the
	// holder's `_-_` is; only what a filesystem refuses is removed, so a
	// name in any script survives. Parts that are missing are left out
	// with their separators, and an over-long name is shortened in the
	// track, the part a person needs least to tell two files apart.
	static QString file_name_for(const resolved_media &m, const QString &ext);
	// The same, with the name and track given -- by a checked reading of
	// the evidence, which knows better than the fields alone.
	static QString file_name_for(const resolved_media &m, const QString &ext,
	                             const QString &name, const QString &track);


	// The rendition to prefer, or a default-constructed one if there is none.
	//
	// Progressive HTTP wins over a manifest even at lower resolution: it needs
	// no assembly, resumes with a Range request, and every player takes it.
	// A manifest is only chosen when nothing progressive carries both streams.
	static media_format best(const resolved_media &m);

signals:
	void resolved(const resolved_media &m);
	void failed(const QString &error);

private:
	QString  m_program;      // resolved absolute path
	QStringList m_prefix;    // e.g. {"-m", "yt_dlp"} when running vendored
	QString  m_origin;       // "PATH" or the vendored directory, for description()
	// Why nothing was found, when nothing was. Empty unless the answer is
	// something other than "neither the submodule nor PATH has it" -- see
	// description(), which used to name a cause it had not tested.
	QString  m_why;
	QString  m_version;
	QPointer<QProcess> m_proc;
	// **A bound on a process this program starts, inside the program.** yt-dlp
	// is given `--socket-timeout`, which bounds its network reads and nothing
	// else: a stuck extractor, a DNS wait or a build that stops for input never
	// reaches `finished`, so neither `resolved` nor `failed` is emitted. The
	// shell's "Asking yt-dlp about ..." has no timeout of its own -- deliberately,
	// because an answer replaces it -- so a request that never ends leaves that
	// sentence on the status bar for good, and `busy()` stays true, which makes
	// every later attempt answer "Still looking..." for the rest of the session.
	// One wedged process therefore takes the feature with it.
	QTimer            *m_watchdog = nullptr;
};

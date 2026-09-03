#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QProcess;

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

	// Asynchronous: exactly one of resolved() / failed() follows.
	void resolve(const QUrl &page_url);
	void cancel();
	bool busy() const;

	// --- pure, and separately testable -----------------------------------
	// Parses `--dump-single-json` output. Handles a playlist by taking its
	// first entry, because a watch page that yields a playlist is nearly
	// always one video plus related items.
	static resolved_media parse(const QByteArray &json);

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
	QPointer<QProcess> m_proc;
};

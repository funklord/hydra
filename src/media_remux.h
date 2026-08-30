#pragma once

#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

class QProcess;

// The ffmpeg step architecture doc sec 11.2 asks for: "if `ffmpeg` is present the
// manager remuxes to a clean `.mp4`/`.mkv`, degrading to raw-segment save
// without it (optional dependency)".
//
// **Remux, never re-encode.** The segments are already H.264/AAC and the only
// thing wrong with them is the container: concatenated MPEG-TS plays, but it
// seeks badly and no phone or editor wants it. `-c copy` rewraps the same
// elementary streams into MP4 in seconds, where a re-encode would take minutes
// and lose a generation of quality for nothing. If a stream ever turns up that
// `-c copy` cannot rewrap, the honest outcome is to keep the `.ts` and say so
// -- which is what failure does here -- rather than silently spend ten minutes
// of somebody's CPU on a transcode they did not ask for.
//
// **It is optional and behaves like it.** `hls_assembler` has already produced
// a file that plays before this runs, so a missing ffmpeg costs the container
// and nothing else. That is why the assembler is not made to wait for this and
// why nothing here can fail the save.
class media_remux : public QObject {
	Q_OBJECT
public:
	explicit media_remux(QObject *parent = nullptr);

	// Where ffmpeg is, or empty. Looked up rather than assumed: `hydra.pro`
	// takes libraries as optional dependencies through pkg-config, but this is
	// a program run at the time somebody saves, so PATH at that moment is the
	// only thing that can answer.
	static QString tool() {
		return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
	}
	static bool available() { return !tool().isEmpty(); }

	// **Pure, and deliberately in the header.** Both of these decide something
	// worth testing -- what the output is called, and what ffmpeg is actually
	// told -- and neither needs a process. Keeping them inline means a suite
	// can check them without linking this class, which would otherwise pull a
	// QProcess and a moc into a link set for two string functions.

	// `.ts` becomes `.mp4`; anything else gains `.mp4` rather than losing what
	// it had, because a name this did not recognise is not one to truncate.
	static QString target_for(const QString &assembled) {
		if (assembled.endsWith(QStringLiteral(".ts"), Qt::CaseInsensitive))
			return assembled.left(assembled.size() - 3) + QStringLiteral(".mp4");
		return assembled + QStringLiteral(".mp4");
	}

	// `-y` because the target is ours and derived from the input, so a stale
	// one from an interrupted run must not stop this; `-c copy` is the whole
	// point; `-loglevel error` because ffmpeg's banner is not a diagnostic and
	// the message shown to a person should be the part that went wrong.
	static QStringList arguments(const QString &in, const QString &out) {
		return QStringList{ QStringLiteral("-y"),
			                 QStringLiteral("-loglevel"), QStringLiteral("error"),
			                 QStringLiteral("-i"), in,
			                 QStringLiteral("-c"), QStringLiteral("copy"),
			                 out };
	}

	// Rewrap `assembled`. Emits exactly once.
	void start(const QString &assembled);

signals:
	// `path` is the file to keep -- the remuxed one when this worked, and the
	// assembled one when it did not, so a caller can name what it has without
	// deciding whether the step ran.
	void finished(bool ok, const QString &path, const QString &message);

private:
	QProcess *m_proc = nullptr;
};

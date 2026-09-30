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

	// **Muxing two assembled streams, which is a different job from rewrapping
	// one.** A DASH manifest hands video and audio out separately, so the
	// assembler produces two files and neither is the programme; this is the
	// step that makes one that is. `-c copy` still: both sides were encoded by
	// whoever published them and re-encoding either would be a quality loss
	// nobody asked for.
	//
	// The maps are explicit. Without them ffmpeg's default selection takes one
	// stream of each kind by its own heuristic, which is right almost always
	// and silent when it is not; saying `0:v:0` and `1:a:0` means the first
	// video of the first input and the first audio of the second, which is what
	// the assembler wrote and nothing else.
	static QStringList arguments(const QString &video, const QString &audio,
	                              const QString &out) {
		return QStringList{ QStringLiteral("-y"),
			                 QStringLiteral("-loglevel"), QStringLiteral("error"),
			                 QStringLiteral("-i"), video,
			                 QStringLiteral("-i"), audio,
			                 QStringLiteral("-c"), QStringLiteral("copy"),
			                 QStringLiteral("-map"), QStringLiteral("0:v:0"),
			                 QStringLiteral("-map"), QStringLiteral("1:a:0"),
			                 out };
	}

	// Rewrap `assembled`. Emits exactly once.
	void start(const QString &assembled);

	// Mux `video` and `audio` into one file. Emits exactly once, and removes
	// **both** inputs on success rather than the one `start` removes -- two
	// half-programmes left beside the result are two files that look like
	// downloads and play as neither.
	void start_mux(const QString &video, const QString &audio);

signals:
	// `path` is the file to keep -- the remuxed one when this worked, and the
	// assembled one when it did not, so a caller can name what it has without
	// deciding whether the step ran.
	void finished(bool ok, const QString &path, const QString &message);

private:
	// Both entry points are the same three steps -- run ffmpeg, let the
	// *artifact* decide, then clean up -- and only the wording and what gets
	// removed differ. Shared, because the artifact check is where the reasoning
	// lives and two copies of it is one copy too many.
	// `verb` and `past` are both passed rather than one derived from the other:
	// deriving "combined" from "combine" is string surgery that reads worse than
	// two arguments and is wrong for the next verb somebody adds.
	void run(const QStringList &args, const QString &out,
	          const QStringList &remove_on_success, const QString &keep_on_failure,
	          const QString &verb, const QString &past);

	QProcess *m_proc = nullptr;
};

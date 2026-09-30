#include "media_remux.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>

media_remux::media_remux(QObject *parent) : QObject(parent) {}

void media_remux::start(const QString &assembled) {
	const QString ffmpeg = tool();
	if (ffmpeg.isEmpty()) {
		// The documented degradation, said out loud. Not an error: the file
		// that exists plays, and the only thing missing is the container the
		// optional dependency would have given it.
		emit finished(false, assembled,
		               QStringLiteral("ffmpeg is not installed, so the stream was "
		                               "kept as-is."));
		return;
	}
	const QString out = target_for(assembled);
	run(arguments(assembled, out), out, QStringList{ assembled }, assembled,
	     QStringLiteral("rewrap"), QStringLiteral("rewrapped"));
}

void media_remux::start_mux(const QString &video, const QString &audio) {
	const QString ffmpeg = tool();
	if (ffmpeg.isEmpty()) {
		// **Not the same degradation as a rewrap.** There, the file that exists
		// is the programme in the wrong container; here it is the video without
		// its sound, and calling that "kept as-is" would describe a silent file
		// as a saved one.
		emit finished(false, video,
		               QStringLiteral("ffmpeg is not installed, so the video and "
		                               "audio could not be combined. The video "
		                               "is saved without its sound."));
		return;
	}
	const QString out = target_for(video);
	run(arguments(video, audio, out), out, QStringList{ video, audio }, video,
	     QStringLiteral("combine"), QStringLiteral("combined"));
}

void media_remux::run(const QStringList &args, const QString &out,
                       const QStringList &remove_on_success,
                       const QString &keep_on_failure, const QString &verb,
                       const QString &past) {
	m_proc = new QProcess(this);
	// Merged, because ffmpeg says everything on stderr and a caller wanting
	// "what went wrong" should not have to know that.
	m_proc->setProcessChannelMode(QProcess::MergedChannels);

	connect(m_proc, &QProcess::errorOccurred, this,
	         [this, keep_on_failure, past](QProcess::ProcessError) {
		// Failing to start is not the same as failing to convert, and both
		// end the same way for the caller: the assembled file is what there
		// is. Said separately so a missing execute bit does not read as a
		// broken stream.
		emit finished(false, keep_on_failure,
		               QString("ffmpeg could not be run, so the stream was not "
		                        "%1.").arg(past));
		m_proc->deleteLater();
		m_proc = nullptr;
	});

	connect(m_proc, &QProcess::finished, this,
	         [this, out, remove_on_success, keep_on_failure, verb]
	         (int code, QProcess::ExitStatus status) {
		const QString said = QString::fromLocal8Bit(m_proc->readAll()).trimmed();
		m_proc->deleteLater();
		m_proc = nullptr;

		// **The artifact decides, not the exit code.** ffmpeg can return zero
		// having written nothing usable -- a truncated input is the ordinary
		// way -- so the file is asked whether it exists and has bytes before
		// anything is claimed, and before the inputs it replaces are removed.
		const bool ran   = (status == QProcess::NormalExit && code == 0);
		const QFileInfo made(out);
		if (!ran || !made.exists() || made.size() == 0) {
			QFile::remove(out);   // a zero-length stub is worse than nothing
			const QString base =
			  QString("ffmpeg could not %1 this stream, so it was kept as-is")
			      .arg(verb);
			emit finished(false, keep_on_failure,
			               said.isEmpty() ? base + QStringLiteral(".")
			                              : base + QStringLiteral(": ") + said);
			return;
		}

		// Only now, with a file that exists and is not empty, are the inputs
		// redundant. Their removal is best-effort: keeping them costs disk, and
		// failing the whole save because a delete failed would be losing the
		// good outcome over the tidy one.
		for (const QString &gone : remove_on_success)
			QFile::remove(gone);
		emit finished(true, out, QString());
	});

	m_proc->start(tool(), args);
}

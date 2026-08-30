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
	m_proc = new QProcess(this);
	// Merged, because ffmpeg says everything on stderr and a caller wanting
	// "what went wrong" should not have to know that.
	m_proc->setProcessChannelMode(QProcess::MergedChannels);

	connect(m_proc, &QProcess::errorOccurred, this, [this, assembled](QProcess::ProcessError) {
		// Failing to start is not the same as failing to convert, and both
		// end the same way for the caller: the assembled file is what there
		// is. Said separately so a missing execute bit does not read as a
		// broken stream.
		emit finished(false, assembled,
		               QStringLiteral("ffmpeg could not be run, so the stream was "
		                               "kept as-is."));
		m_proc->deleteLater();
		m_proc = nullptr;
	});

	connect(m_proc, &QProcess::finished, this,
	         [this, assembled, out](int code, QProcess::ExitStatus status) {
		const QString said = QString::fromLocal8Bit(m_proc->readAll()).trimmed();
		m_proc->deleteLater();
		m_proc = nullptr;

		// **The artifact decides, not the exit code.** ffmpeg can return zero
		// having written nothing usable -- a truncated input is the ordinary
		// way -- so the file is asked whether it exists and has bytes before
		// anything is claimed, and before the input it replaces is removed.
		const bool ran   = (status == QProcess::NormalExit && code == 0);
		const QFileInfo made(out);
		if (!ran || !made.exists() || made.size() == 0) {
			QFile::remove(out);   // a zero-length stub is worse than nothing
			emit finished(false, assembled,
			               said.isEmpty()
			                 ? QStringLiteral("ffmpeg could not rewrap this stream, "
			                                   "so it was kept as-is.")
			                 : QStringLiteral("ffmpeg could not rewrap this stream, "
			                                   "so it was kept as-is: ") + said);
			return;
		}

		// Only now, with a file that exists and is not empty, is the raw
		// concatenation redundant. Its removal is best-effort: keeping it
		// costs disk, and failing the whole save because a delete failed
		// would be losing the good outcome over the tidy one.
		QFile::remove(assembled);
		emit finished(true, out, QString());
	});

	m_proc->start(ffmpeg, arguments(assembled, out));
}

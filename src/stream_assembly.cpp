#include "stream_assembly.h"

#include "download_manager.h"
#include "hls_assembler.h"
#include "media_remux.h"
#include "player_launcher.h"

#include <QDir>

stream_assembly::stream_assembly(player_launcher *players,
                                  download_manager *downloads,
                                  local_proxy *proxy, QObject *parent)
  : QObject(parent), m_players(players), m_downloads(downloads),
    m_proxy(proxy) {}

bool stream_assembly::running() const {
	return m_assembler && !m_assembler->finished();
}

QString stream_assembly::output_path() const {
	return m_assembler ? m_assembler->output_path() : QString();
}

QString stream_assembly::scratch_path() const {
	return m_scratch.isValid() ? m_scratch.path() : QString();
}

void stream_assembly::watch(const media_item &item, const stream_context &ctx) {
	assemble(item, ctx, true);
}

void stream_assembly::save(const media_item &item, const stream_context &ctx) {
	assemble(item, ctx, false);
}

void stream_assembly::assemble(const media_item &item,
                                const stream_context &ctx, bool play_it) {
	if (!m_assembler)
		m_assembler = new hls_assembler(this);

	// **`Qt::UniqueConnection` does nothing for the lambdas below**, and the
	// three connections here relied on it. The flag deduplicates connections to
	// *member functions*; a functor is a fresh object every time, so nothing
	// matches and every press added another full set of handlers to the one
	// assembler this object keeps.
	//
	// The second Watch therefore ran the progress handler twice and launched
	// two players; the third launched three. Download drove one output path
	// from several assemblies at once. It got worse the more it was used and
	// differed every time, which is what "many bugs in watch/download" looks
	// like from outside.
	//
	// Each press replaces the handlers rather than joining them: the captures
	// below differ per press -- the output path and whether to play -- so
	// connecting once up front is not available either.
	m_assembler->disconnect(this);

	if (play_it && !m_scratch.isValid()) {
		// Nowhere to write means no assembly, said once rather than as a
		// failure from inside the assembler that reads like a network fault.
		emit status(QStringLiteral("Cannot assemble: no writable temporary "
		                            "directory."));
		return;
	}

	const QString out = play_it
	  // **A name of its own, not a shared one.** This was `stream.ts` for every
	  // assembly, and `hls_assembler::start` opens the output with Truncate --
	  // so watching a second stream cut the file the first player still had
	  // open, and then wrote over it. The player does not notice; it simply
	  // stops making sense.
	  ? m_scratch.filePath(QStringLiteral("stream-%1.ts").arg(++m_stream_seq))
	  : QDir(m_downloads->directory()).filePath(
	        item.label.section('/', -1).section('.', 0, 0) + ".ts");

	connect(m_assembler, &hls_assembler::progress, this,
	         [this, out, play_it, item](qint64 bytes, int done, int total) {
		emit status(QString("Assembling %1/%2 segments (%3 KiB)…")
		                .arg(done).arg(total).arg(bytes / 1024));
		// Hand the player the growing file as soon as there is something to
		// play -- that is the sec 11.3 tee-to-disk trick, and it is what turns a
		// live stream into a locally seekable one.
		if (play_it && done == 1) {
			const QUrl via = m_proxy ? m_proxy->publish_file(out, "video/mp2t")
			                          : QUrl();
			QString error;
			if (!m_players->play(item, &error, via))
				emit status(error);
		}
	});

	connect(m_assembler, &hls_assembler::completed, this, [this, out, play_it] {
		if (play_it) {
			// **Not remuxed, deliberately.** A player already has this file
			// open and has been reading it since the first segment landed --
			// that is the tee-to-disk trick above. Rewrapping it now would
			// replace the file underneath a running player to gain a container
			// nobody is going to seek around afterwards.
			emit status(QStringLiteral("Stream assembled; playback continues "
			                            "locally."));
			return;
		}

		// The sec 11.2 step: a saved stream should be a file the rest of the
		// world accepts, and concatenated MPEG-TS is not that. Optional, so
		// the message says what happened either way rather than only on
		// success -- "saved" with no mention of the container would leave
		// somebody wondering why they have a `.ts`.
		emit status(QString("Saved %1; rewrapping…").arg(out));
		auto *remux = new media_remux(this);
		connect(remux, &media_remux::finished, this,
		         [this, remux](bool ok, const QString &path, const QString &why) {
			emit status(ok ? QString("Saved %1.").arg(path)
			                : QString("Saved %1 — %2").arg(path, why));
			remux->deleteLater();
		});
		remux->start(out);
	});

	connect(m_assembler, &hls_assembler::failed, this,
	         [this](const QString &e) {
		emit status("Assembly failed: " + e);
	});

	emit status(QStringLiteral("Fetching manifest…"));
	m_assembler->start(item.url, ctx, out);
}

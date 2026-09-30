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
		// A live list has no end, so running out of segments is the end of the
		// *window it published* and not the end of the stream. `hls_assembler`
		// said as much in a comment and told nobody; saying "saved" with no
		// more than that is how somebody ends up with thirty seconds of a
		// broadcast and no reason to look for the rest.
		const bool live = m_assembler && m_assembler->was_live();
		if (play_it) {
			// **Not remuxed, deliberately.** A player already has this file
			// open and has been reading it since the first segment landed --
			// that is the tee-to-disk trick above. Rewrapping it now would
			// replace the file underneath a running player to gain a container
			// nobody is going to seek around afterwards.
			emit status(live
			  ? QStringLiteral("Live stream: captured what the playlist "
			                    "offered; playback continues locally.")
			  : QStringLiteral("Stream assembled; playback continues "
			                    "locally."));
			return;
		}

		// The sec 11.2 step: a saved stream should be a file the rest of the
		// world accepts, and concatenated MPEG-TS is not that. Optional, so
		// the message says what happened either way rather than only on
		// success -- "saved" with no mention of the container would leave
		// somebody wondering why they have a `.ts`.
		// The note travels to both messages rather than only the first: the
		// rewrap answers a second later and overwrites the line, so a caveat
		// left on the earlier one is a caveat nobody ends up looking at.
		const QString note = live
		  ? QStringLiteral(" Live stream, so this is the window the playlist "
		                    "offered rather than the whole broadcast.")
		  : QString();
		// **Two files or one, decided by what the assembler found.** A DASH
		// manifest carrying its audio separately produced a second file, and
		// neither half is the programme; `audio_path()` is non-empty exactly
		// then, so it answers "mux or rewrap" without a second flag to keep in
		// step with it.
		const QString audio = m_assembler ? m_assembler->audio_path() : QString();
		emit status(audio.isEmpty()
		  ? QString("Saved %1; rewrapping…%2").arg(out, note)
		  : QString("Saved %1; combining video and audio…%2").arg(out, note));
		auto *remux = new media_remux(this);
		connect(remux, &media_remux::finished, this,
		         [this, remux, note](bool ok, const QString &path,
		                              const QString &why) {
			emit status(ok ? QString("Saved %1.%2").arg(path, note)
			                : QString("Saved %1 — %2%3").arg(path, why, note));
			remux->deleteLater();
		});
		if (audio.isEmpty())
			remux->start(out);
		else
			remux->start_mux(out, audio);
	});

	connect(m_assembler, &hls_assembler::failed, this,
	         [this](const QString &e) {
		emit status("Assembly failed: " + e);
	});

	emit status(QStringLiteral("Fetching manifest…"));
	// **Only saving asks for the audio, and the asymmetry is the point.** A mux
	// needs both streams complete; watching hands the player a file that is
	// still growing, from the first segment. So Save passes somewhere to put a
	// separately-carried audio stream and Watch does not -- which is what makes
	// the assembler refuse such a manifest for Watch rather than produce a file
	// that plays perfectly and is silent. Whether Watch should instead wait for
	// both and play afterwards is recorded in project.md as the holder's.
	m_assembler->start(item.url, ctx, out,
	                    play_it ? QString() : out + QStringLiteral(".audio"));
}

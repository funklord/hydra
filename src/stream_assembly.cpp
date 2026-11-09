#include "stream_assembly.h"
#include "media_sheet.h"
#include "download_route.h"

#include "download_manager.h"
#include "hls_assembler.h"
#include "media_remux.h"
#include "player_launcher.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include <memory>

namespace {

// Whether a save to `path` could land on an existing file: the path itself,
// or the `.mp4` the rewrap turns it into (`media_remux::target_for`), which
// ffmpeg writes with `-y`. The `.ts` is removed after a rewrap, so checking
// it alone let the next save of the same title overwrite the earlier `.mp4`.
bool taken(const QString &path) {
	return QFileInfo::exists(path) || QFileInfo::exists(media_remux::target_for(path));
}

// `path`, or the first "name (n).ext" beside it that is not taken.
QString unused_path(const QString &path) {
	if (!taken(path))
		return path;
	const QFileInfo info(path);
	const QString dir = info.absolutePath();
	const QString base = info.completeBaseName();
	const QString tail = info.suffix().isEmpty() ? QString() : "." + info.suffix();
	for (int n = 2; n < 1000; ++n) {
		const QString next = QDir(dir).filePath(QString("%1 (%2)%3").arg(base).arg(n).arg(tail));
		if (!taken(next))
			return next;
	}
	return QDir(dir).filePath(base + "-" + QString::number(QDateTime::currentMSecsSinceEpoch()) + tail);
}

}  // namespace


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
	m_live_note.clear();

	QString route_fault;
	if (!download_route::shared().ready(&route_fault)) {
		emit status(route_fault);
		return;
	}

	if (play_it && !m_scratch.isValid()) {
		// Nowhere to write means no assembly, said once rather than as a
		// failure from inside the assembler that reads like a network fault.
		emit status(QStringLiteral("Cannot assemble: no writable temporary "
		                            "directory."));
		return;
	}

	// **Made if it is not there**, as the HTTP and torrent sources do. A
	// profile whose Downloads folder did not exist yet had every stream save
	// fail with "Cannot write", while a plain download beside it worked
	// (try_encrypted, 2026-11-09).
	if (!play_it)
		QDir().mkpath(m_downloads->directory());
	const QString saved_as = play_it ? QString() : QDir(m_downloads->directory()).filePath(
	        // The metadata's name where there is one, as a transport stream,
	        // which is what this writes; the list label's otherwise.
	        (item.file_name.isEmpty()
	           ? item.label.section('/', -1).section('.', 0, 0)
	           : QFileInfo(item.file_name).completeBaseName()) + ".ts");
	const QString out = play_it
	  // **A name of its own, not a shared one.** This was `stream.ts` for every
	  // assembly, and `hls_assembler::start` opens the output with Truncate --
	  // so watching a second stream cut the file the first player still had
	  // open, and then wrote over it. The player does not notice; it simply
	  // stops making sense.
	  ? m_scratch.filePath(QStringLiteral("stream-%1.ts").arg(++m_stream_seq))
	  // **Never over an earlier download.** The assembler opens its output
	  // with Truncate, and two videos whose playlists share a name -- most
	  // sites call theirs index.m3u8 -- were saved to one file, the second
	  // destroying the first (emulator, 2026-10-31). A taken name gets
	  // " (2)", " (3)", as a file manager does.
	  : unused_path(saved_as);

	connect(m_assembler, &hls_assembler::progress, this,
	         [this, out, play_it, item](qint64 bytes, int done, int total) {
		emit status(QString("Assembling %1/%2 segments (%3 KiB)…")
		                .arg(done).arg(total).arg(bytes / 1024));
		// Hand the player the growing file as soon as there is something to
		// play -- that is the sec 11.3 tee-to-disk trick, and it is what turns a
		// live stream into a locally seekable one.
		//
		// Not for a stream with its audio apart: the file growing here is silent,
		// so the player waits for both and the mux (`completed`, below).
		if (play_it && done == 1 && m_assembler && !m_assembler->has_audio_pass()) {
			const QUrl via = m_proxy ? m_proxy->publish_file(out, "video/mp2t")
			                          : QUrl();
			QString error;
			if (!m_players->play(item, &error, via))
				emit status(error);
		}
	});

	connect(m_assembler, &hls_assembler::completed, this,
	         [this, out, play_it, item, sheet = item.sheet] {
		// A live list has no end, so running out of segments is the end of the
		// *window it published* and not the end of the stream. `hls_assembler`
		// said as much in a comment and told nobody; saying "saved" with no
		// more than that is how somebody ends up with thirty seconds of a
		// broadcast and no reason to look for the rest.
		const bool live = m_assembler && m_assembler->was_live();
		// **Live, and whether the playlist was read more than once.** HLS
		// re-reads a growing list; a live MPD is captured as the one window it
		// offered, because continuing one means re-deriving `$Number$` and
		// `$Time$` addressing across reads and that is not built. The sentence
		// below used to be the same for both, which is how somebody ends up
		// with thirty seconds of a broadcast and a message implying otherwise
		// -- the exact outcome the comment above this one warns about.
		const bool followed = m_assembler && m_assembler->polled_live();
		// **A silent video is worth saying out loud.** An HLS master playlist
		// may keep its audio in a separate rendition, which this engine does
		// not fetch -- DASH's equivalent it does, through the audio pass and
		// the mux -- so the file is video only and the save used to report that
		// as finished with nothing said. Said rather than refused: the video is
		// worth having, and refusing would deny a save that is partly useful.
		const QString quiet =
		  (m_assembler && m_assembler->audio_is_separate())
		    ? QStringLiteral(" This stream keeps its audio in a separate "
		                      "playlist, which is not fetched, so the file has "
		                      "no sound.")
		    : QString();
		const QString live_said =
		  followed ? QStringLiteral("Live stream, so this is what the playlist "
		                             "offered while it was being read.")
		           : QStringLiteral("Live stream, and this is the one window "
		                             "the manifest offered -- it is not re-read "
		                             "as it grows, so there is likely more.");
		// **Video and audio apart: combined first, then played.** A mux needs both
		// complete, so this gives up playing from the first segment for this one
		// shape of manifest. Decided 2026-11-07: a wait is better than a refusal,
		// which left a person who pressed Watch with nothing at all.
		const QString apart = m_assembler ? m_assembler->audio_path() : QString();
		if (play_it && !apart.isEmpty()) {
			emit status(QStringLiteral("Combining video and audio…"));
			auto *remux = new media_remux(this);
			connect(remux, &media_remux::finished, this,
			         [this, remux, item](bool ok, const QString &path, const QString &why) {
				remux->deleteLater();
				if (!ok) {
					emit status(why);
					return;
				}
				const QUrl via = m_proxy ? m_proxy->publish_file(path, "video/mp4") : QUrl();
				QString error;
				emit status(m_players->play(item, &error, via)
				              ? QStringLiteral("Stream assembled and playing.") : error);
			});
			remux->start_mux(out, apart);
			return;
		}
		if (play_it) {
			// **Not remuxed, deliberately.** A player already has this file
			// open and has been reading it since the first segment landed --
			// that is the tee-to-disk trick above. Rewrapping it now would
			// replace the file underneath a running player to gain a container
			// nobody is going to seek around afterwards.
			emit status(
			  (live ? live_said + QStringLiteral(" Playback continues locally.")
			              + (m_live_note.isEmpty()
			                   ? QString()
			                   : QStringLiteral(" It stopped because ")
			                       + m_live_note + QStringLiteral("."))
			         : QStringLiteral("Stream assembled; playback continues "
			                           "locally."))
			  + quiet);
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
		const QString note =
		  (live ? QStringLiteral(" ") + live_said : QString())
		  + (m_live_note.isEmpty() ? QString()
		                            : QStringLiteral(" It stopped because ")
		                                + m_live_note + QStringLiteral("."))
		  + quiet;
		// **Two files or one, decided by what the assembler found.** A DASH
		// manifest carrying its audio separately produced a second file, and
		// neither half is the programme; `audio_path()` is non-empty exactly
		// then, so it answers "mux or rewrap" without a second flag to keep in
		// step with it.
		finish_saved(out, m_assembler ? m_assembler->audio_path() : QString(),
		             note, sheet);
	});

	// **The file is opened before anything is fetched**, so a stream refused
	// at its manifest left an empty one in the downloads folder, under a name
	// that reads as a saved video. What did arrive is kept: a partial stream
	// plays.
	connect(m_assembler, &hls_assembler::failed, this,
	         [this, out](const QString &e) {
		if (QFileInfo(out).size() == 0)
			QFile::remove(out);
		emit status("Assembly failed: " + e);
	});

	// **A live capture that stopped for a reason says the reason.** It arrives
	// before `completed`, so the note is on screen and then replaced by the
	// saved-or-playing line -- which is the wrong way round for something a
	// person needs to keep. Held and appended to the completion message
	// instead, for the reason the live caveat below is: a line that is
	// overwritten a second later is a line nobody reads.
	connect(m_assembler, &hls_assembler::status_note, this,
	         [this](const QString &note) { m_live_note = note; });

	// **An encrypted stream goes to ffmpeg**, which fetches the key and
	// decrypts AES-128 itself, where this engine would write ciphertext.
	// Not in the Play edition (`media_decrypt_allowed`).
	connect(m_assembler, &hls_assembler::encrypted, this,
	         [this, ctx, item, out, play_it](const QUrl &playlist) {
		decrypt(playlist, ctx, item, out, play_it);
	});
	m_assembler->set_hand_off_encrypted(media_decrypt_allowed());

	emit status(QStringLiteral("Fetching manifest…"));
	// Somewhere for a separately-carried audio stream, for Watch as for Save.
	m_assembler->start(item.url, ctx, out, out + QStringLiteral(".audio"));
}

void stream_assembly::finish_saved(const QString &out, const QString &audio,
                                   const QString &note, const QString &sheet) {
	emit status(audio.isEmpty()
	  ? QString("Saved %1; rewrapping…%2").arg(out, note)
	  : QString("Saved %1; combining video and audio…%2").arg(out, note));
	auto *remux = new media_remux(this);
	connect(remux, &media_remux::finished, this,
	         [this, remux, note, sheet](bool ok, const QString &path,
	                                     const QString &why) {
		// The file's last name: a rewrap changes it.
		QString mix;
		if (!sheet.isEmpty()) {
			const media_sheet s = media_sheet::from_json(sheet);
			QString error;
			mix = s.write_beside(path, &error)
			  ? QString(" Mix: %1.").arg(s.summary()) : QLatin1Char(' ') + error;
		}
		emit status(ok ? QString("Saved %1.%2%3").arg(path, note, mix)
		                : why.isEmpty() ? QString("Saved %1.%2%3").arg(path, note, mix)
		                : QString("Saved %1 — %2%3%4").arg(path, why, note, mix));
		remux->deleteLater();
	});
	if (audio.isEmpty())
		remux->start(out);
	else
		remux->start_mux(out, audio);
}

void stream_assembly::decrypt(const QUrl &playlist, const stream_context &ctx,
                              const media_item &item, const QString &out,
                              bool play_it) {
	auto drop_empty = [out] {
		if (QFileInfo(out).size() == 0)
			QFile::remove(out);
	};
	// **Never around the VPN.** ffmpeg speaks no SOCKS, so it cannot be put
	// through the route downloads are bound to, and fetching it directly
	// would send the one download a person routed on purpose outside it.
	if (download_route::shared().bound()) {
		drop_empty();
		emit status(QStringLiteral("This stream is encrypted. ffmpeg decrypts "
		                            "it, and it cannot be sent through the "
		                            "interface downloads are bound to, so it "
		                            "was not fetched."));
		return;
	}
	if (m_fetch) {
		drop_empty();
		emit status(QStringLiteral("An encrypted stream is already being "
		                            "decrypted; wait for it to finish."));
		return;
	}
	// What this browser would send to the playlist's host. ffmpeg sends the
	// same to the key and the segments, which are on that host on every
	// encrypted stream seen here; a key on another host gets the playlist's
	// cookies too, which is the price of handing the fetch to a tool that
	// takes one header set.
	const QString cookie = !ctx.cookies.isEmpty() ? ctx.cookies
	                     : ctx.cookies_for ? ctx.cookies_for(playlist) : QString();
	QString headers;
	if (!cookie.isEmpty())
		headers += QStringLiteral("Cookie: ") + cookie + QStringLiteral("\r\n");
	for (auto it = ctx.extra.cbegin(); it != ctx.extra.cend(); ++it)
		headers += it.key() + QStringLiteral(": ") + it.value() + QStringLiteral("\r\n");

	m_fetch = new media_remux(this);
	auto *fetch = m_fetch;
	// Watch plays the file as it grows, once there is enough of it for a
	// player to find its feet.
	QTimer *poll = nullptr;
	auto played = std::make_shared<bool>(false);
	auto play = [this, item, out, played] {
		if (*played)
			return;
		*played = true;
		const QUrl via = m_proxy ? m_proxy->publish_file(out, "video/mp2t") : QUrl();
		QString error;
		if (!m_players->play(item, &error, via))
			emit status(error);
	};
	if (play_it) {
		poll = new QTimer(fetch);
		poll->setInterval(500);
		connect(poll, &QTimer::timeout, this, [out, play] {
			if (QFileInfo(out).size() >= 512 * 1024)
				play();
		});
		poll->start();
	}
	connect(fetch, &media_remux::finished, this,
	         [this, fetch, out, play_it, play, sheet = item.sheet]
	         (bool ok, const QString &, const QString &why) {
		fetch->deleteLater();
		m_fetch = nullptr;
		if (!ok) {
			emit status(why);
			return;
		}
		if (play_it) {
			play();
			emit status(QStringLiteral("Decrypted by ffmpeg; playback continues "
			                            "locally."));
			return;
		}
		finish_saved(out, QString(), QStringLiteral(" Decrypted by ffmpeg."), sheet);
	});
	emit status(QStringLiteral("This stream is encrypted; ffmpeg is fetching "
	                            "and decrypting it…"));
	fetch->start_fetch(playlist, ctx.user_agent, ctx.referer, headers, out);
}

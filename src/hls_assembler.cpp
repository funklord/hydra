#include "hls_assembler.h"

#include "dash_manifest.h"

#include <QTimer>

#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

hls_assembler::hls_assembler(QObject *parent) : QObject(parent) {
	m_net = new QNetworkAccessManager(this);
}

hls_assembler::~hls_assembler() {
	stop();
}

QNetworkReply *hls_assembler::get(const QUrl &url, const QByteArray &range) {
	QNetworkRequest req(url);
	// The same context injection the proxy does -- a CDN that 403s a naked
	// stream URL will 403 our segment fetches too (sec 11.3).
	if (!m_ctx.referer.isEmpty())
		req.setRawHeader("Referer", m_ctx.referer.toUtf8());
	if (!m_ctx.user_agent.isEmpty())
		req.setRawHeader("User-Agent", m_ctx.user_agent.toUtf8());
	if (!m_ctx.cookies.isEmpty())
		req.setRawHeader("Cookie", m_ctx.cookies.toUtf8());
	if (!range.isEmpty())
		req.setRawHeader("Range", range);
	return m_net->get(req);
}

void hls_assembler::start(const QUrl &manifest, const stream_context &ctx,
                           const QString &output_path) {
	stop();
	// A new run, so anything deferred by the last one can tell it is stale.
	// Everything below is reset except `m_playlist`, which is what made the
	// stale retry harmful rather than merely wasteful: it walked the previous
	// stream's segment list while writing into this run's file.
	++m_run;
	m_ctx       = ctx;
	m_path      = output_path;
	m_written   = 0;
	m_index     = 0;
	m_attempt   = 0;
	m_finished  = false;
	m_stopped   = false;
	m_redirects = 0;

	m_file = new QFile(m_path, this);
	if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		emit failed("Cannot write " + m_path);
		delete m_file;
		m_file = nullptr;
		return;
	}
	fetch_manifest(manifest);
}

void hls_assembler::stop() {
	m_stopped = true;
	if (m_reply && m_reply->isRunning())
		m_reply->abort();
	if (m_file) {
		m_file->close();
		delete m_file;
		m_file = nullptr;
	}
}

void hls_assembler::fetch_manifest(const QUrl &url) {
	m_reply = get(url);
	QNetworkReply *reply = m_reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
		reply->deleteLater();
		if (m_stopped)
			return;
		if (reply->error() != QNetworkReply::NoError) {
			emit failed("Manifest fetch failed: " + reply->errorString());
			return;
		}
		const QByteArray body = reply->readAll();

		// **Which kind of manifest this is, decided by what it says rather
		// than by the URL.** An `.mpd` served as `.m3u8` or the other way
		// round is a CDN's business and not a reason to parse the wrong
		// grammar; and the extension is absent entirely from plenty of
		// manifest URLs. HLS opens with `#EXTM3U` by specification and an MPD
		// is XML, so the two are distinguishable at the first non-space byte.
		// Anything else is refused rather than guessed at, for the same reason
		// the parsers refuse what they cannot address: a wrong grammar yields
		// a wrong segment list, and a wrong segment list assembles a file that
		// plays and is not the programme.
		const QByteArray head = body.trimmed().left(7);
		if (head.startsWith('<')) {
			if (!assemble_dash(body, url))
				return;
			next_segment();
			return;
		}
		if (!head.startsWith("#EXTM3U")) {
			emit failed("The manifest is neither an HLS playlist nor an MPD.");
			return;
		}

		m_playlist = hls::parse(body, url);

		// **A manifest that was not understood is refused, not assembled.**
		// The only thing that sets this is a byte range whose numbers could not
		// be read, and a byte range decides which bytes of which file a segment
		// is -- so carrying on would write a file made of the wrong bytes and
		// call it finished. See `hls_playlist::error`.
		if (!m_playlist.error.isEmpty()) {
			emit failed("Playlist not understood: " + m_playlist.error);
			return;
		}

		if (m_playlist.is_master) {
			const hls_variant *v = hls::best_variant(m_playlist);
			if (!v) {
				emit failed("Master playlist listed no variants.");
				return;
			}
			// One hop only: a master pointing at another master is malformed,
			// and following it forever is how a fetch loop happens.
			if (++m_redirects > 1) {
				emit failed("Master playlist points at another master playlist.");
				return;
			}
			fetch_manifest(v->url);
			return;
		}

		if (m_playlist.segments.isEmpty()) {
			emit failed("Playlist listed no segments.");
			return;
		}
		next_segment();
	});
}

// **DASH, turned into the one thing everything below this understands: an
// ordered list of segment URLs.** The engine after this point -- the retries,
// the run guard, the growing-file contract -- is general over any such list,
// and a DASH representation is exactly one plus an initialisation segment that
// goes first. So this converts rather than duplicating, which is why there is
// no second assembler.
//
// The class is called `hls_assembler` and now assembles both. The name is
// narrower than the job; renaming it touches every user and is a mechanical
// change of its own rather than something to bundle here.
//
// Returns false having emitted `failed`.
bool hls_assembler::assemble_dash(const QByteArray &body, const QUrl &url) {
	const dash_manifest m = dash::parse(body, url);
	// The same refusal the HLS path makes, for the same reason: `dash::parse`
	// sets this only for what decides which bytes are fetched.
	if (!m.error.isEmpty()) {
		emit failed("MPD not understood: " + m.error);
		return false;
	}
	const dash_representation *v = dash::best_video(m);
	if (!v) {
		emit failed("The MPD offered no video stream to assemble.");
		return false;
	}

	// **A DASH manifest hands video and audio out separately, and this
	// assembles one list into one file.** Taking the video alone would produce
	// a file that plays perfectly and is silent -- the failure `best_audio`
	// exists to make visible -- so where there is a separate audio stream this
	// refuses and says which part is missing. Combining the two means two
	// assembled files and a mux, which is an input more than `media_remux`
	// takes, and that is a change to make deliberately rather than here.
	if (dash::best_audio(m)) {
		emit failed("This MPD carries its audio separately from its video, "
		             "and assembling the two into one file is not implemented. "
		             "The video alone would play silently, so it is refused "
		             "rather than handed over.");
		return false;
	}

	m_playlist = hls_playlist{};
	m_playlist.is_master = false;
	m_playlist.is_live   = m.is_live;
	if (!v->init.isEmpty()) {
		hls_segment init;
		init.url = v->init;
		m_playlist.segments.push_back(init);
	}
	for (const QUrl &u : v->segments) {
		hls_segment seg;
		seg.url = u;
		m_playlist.segments.push_back(seg);
	}
	if (m_playlist.segments.isEmpty()) {
		emit failed("The MPD's video stream listed no segments.");
		return false;
	}
	return true;
}

void hls_assembler::next_segment() {
	if (m_stopped)
		return;
	if (m_index >= m_playlist.segments.size()) {
		// A live playlist keeps growing, so "ran out of segments" is only the
		// end for VOD. Re-polling a live list is the next increment; for now
		// say plainly that what we captured is what there is.
		if (m_file) {
			m_file->flush();
			m_file->close();
		}
		m_finished = true;
		emit completed();
		return;
	}

	const hls_segment seg = m_playlist.segments.at(m_index);
	QByteArray range;
	if (seg.byte_length > 0) {
		const qint64 off = (seg.byte_offset >= 0) ? seg.byte_offset : 0;
		range = "bytes=" + QByteArray::number(off) + "-" +
		        QByteArray::number(off + seg.byte_length - 1);
	}

	m_reply = get(seg.url, range);
	QNetworkReply *reply = m_reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		if (m_stopped)
			return;
		if (reply->error() != QNetworkReply::NoError) {
			// **A segment that fails once is retried**, because one failure used
			// to end the assembly and discard everything already fetched --
			// reported as "segment 79 failed" after 78 had landed. Over a real
			// CDN and hundreds of segments a transient error somewhere is close
			// to certain, so the old behaviour meant long streams essentially
			// could not be assembled.
			//
			// Retried regardless of which error it was. Sorting them into
			// transient and permanent means guessing at somebody else's
			// summary: a 403 can be an expired token that will never succeed or
			// a CDN shedding load that will, and the reply cannot tell you
			// which. A small bounded number of attempts costs a few requests
			// when it is hopeless and rescues the case that is merely unlucky.
			if (++m_attempt < k_segment_attempts) {
				const int wait = k_retry_ms * m_attempt;   // 400, 800, 1200...
				// `next_segment()` re-reads `segments.at(m_index)`, and the
				// index only advances on success -- so this fetches the same
				// segment again rather than skipping past it.
				// **Bound to this run.** `m_stopped` is not enough: a press
				// that starts another assembly clears it, so a retry pending
				// from this one would wake into that one -- fetching the old
				// playlist's segments into the new run's file, advancing its
				// index and racing its manifest fetch.
				const int run = m_run;
				QTimer::singleShot(wait, this, [this, run] {
					if (!m_stopped && run == m_run)
						next_segment();
				});
				return;
			}
			emit failed(QString("Segment %1 failed after %2 attempts: %3")
			                .arg(m_index).arg(m_attempt).arg(reply->errorString()));
			return;
		}
		// Landed, so the next segment starts with a full budget of its own.
		m_attempt = 0;
		const QByteArray body = reply->readAll();
		if (m_file) {
			// Flushed so a reader can play what has landed so far -- and
			// read, because that is where a full disk surfaces. Both results
			// used to be discarded and `m_written` counted the bytes anyway,
			// so an assembly that could not write reported progress it did
			// not have and handed the player a file with holes in it.
			if (m_file->write(body) != body.size() || !m_file->flush()) {
				emit failed(QString("Could not write %1: %2")
				                .arg(m_path, m_file->errorString()));
				return;
			}
			m_written += body.size();
		}
		++m_index;
		emit progress(m_written, m_index, m_playlist.segments.size());
		next_segment();
	});
}

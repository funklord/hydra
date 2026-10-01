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
	// **Asked about this url, not about the page.** A manifest, its variants,
	// its segments and a separate audio track can each sit on a different host,
	// and the field this used to read was filled once from the page's own
	// address -- so every segment fetch carried the page's cookies wherever it
	// went. An extractor's named header still wins; otherwise the jar is asked
	// what the browser would send here, which is nothing for a stranger.
	const QString jar = (m_ctx.cookies.isEmpty() && m_ctx.cookies_for)
	                      ? m_ctx.cookies_for(url) : m_ctx.cookies;
	if (!jar.isEmpty())
		req.setRawHeader("Cookie", jar.toUtf8());
	if (!range.isEmpty())
		req.setRawHeader("Range", range);
	return m_net->get(req);
}

void hls_assembler::start(const QUrl &manifest, const stream_context &ctx,
                           const QString &output_path,
                           const QString &audio_output_path) {
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
	m_audio_path = audio_output_path;
	m_audio_pending.clear();
	m_audio_done = false;
	m_segments_base = 0;
	m_segments_all  = 0;
	m_live_url      = QUrl();
	m_next_sequence = 0;

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
		// What to ask again, and the number after the last segment this run is
		// about to take. Both are only meaningful for HLS; see the header.
		m_live_url      = url;
		m_next_sequence = m_playlist.media_sequence + m_playlist.segments.size();
		m_live_idle.start();
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
	const dash_representation *a = dash::best_audio(m);
	if (a && m_audio_path.isEmpty()) {
		emit failed("This MPD carries its audio separately from its video, and "
		             "combining the two needs a file to put the audio in, which "
		             "this caller did not ask for. The video alone would play "
		             "silently, so it is refused rather than handed over.");
		return false;
	}
	if (a) {
		// **Kept rather than fetched now.** The video is what a player can
		// start on and what a progress bar is about, so it goes first and the
		// audio follows when its list is exhausted -- the same engine, a second
		// time, into a second file.
		if (!a->init.isEmpty()) {
			hls_segment init;
			init.url = a->init;
			m_audio_pending.push_back(init);
		}
		for (const QUrl &u : a->segments) {
			hls_segment seg;
			seg.url = u;
			m_audio_pending.push_back(seg);
		}
		if (m_audio_pending.isEmpty()) {
			emit failed("The MPD's audio stream listed no segments.");
			return false;
		}
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
	// Both lists are known here and nowhere earlier, which is why the totals are
	// set here rather than in `start`.
	m_segments_all = m_playlist.segments.size() + m_audio_pending.size();
	return true;
}

// Re-read the media playlist and continue into whatever it has grown.
//
// **What makes this terminate is stated rather than hoped for.** Three things
// end it: the playlist saying it is complete (`#EXT-X-ENDLIST`, which the parse
// reports as no longer live), no growth for `k_live_stall_ms`, and `stop()`.
// Nothing here waits on a remote file choosing to stop, which is the shape
// `running-code.md` refuses to let anybody run.
//
// The poll interval is the playlist's own target duration, floored, because a
// list advertising a tenth of a second would otherwise be fetched ten times a
// second and a list advertising nothing would be fetched continuously.
int hls_assembler::live_stall_ms() {
	// Read once and cached: a value that changed under a running assembly would
	// make the bound a property of when it was consulted.
	static const int ms = [] {
		bool ok = false;
		const int v = qEnvironmentVariableIntValue("HYDRA_LIVE_STALL_MS", &ok);
		return (ok && v > 0) ? v : k_live_stall_ms;
	}();
	return ms;
}

void hls_assembler::poll_live() {
	const int wait = qMax(k_live_poll_min_ms,
	                       int(m_playlist.target_duration * 1000.0));
	const int run  = m_run;
	QTimer::singleShot(wait, this, [this, run] {
		// The run counter, for the reason the one above `m_run` gives: a timer
		// pending from a previous press must not drive this one.
		if (m_stopped || run != m_run)
			return;

		m_reply = get(m_live_url);
		QNetworkReply *reply = m_reply;
		connect(reply, &QNetworkReply::finished, this, [this, reply, run] {
			reply->deleteLater();
			if (m_stopped || run != m_run)
				return;
			if (reply->error() != QNetworkReply::NoError) {
				// A poll that fails is not an assembly that failed: everything
				// fetched so far is on disk and playable. Stop and say what is
				// there, rather than throwing away a capture over one refused
				// request.
				finish_live("the playlist could not be re-read: " +
				             reply->errorString());
				return;
			}

			const hls_playlist fresh = hls::parse(reply->readAll(), m_live_url);
			if (!fresh.error.isEmpty()) {
				finish_live("the playlist stopped being understood: " +
				             fresh.error);
				return;
			}

			// **New is a number, not an address.** A live playlist may reuse a
			// URL for different content; `#EXT-X-MEDIA-SEQUENCE` is what the
			// specification makes monotonic, so what has been taken already is
			// `m_next_sequence` and everything at or beyond it is new.
			QList<hls_segment> fresh_segments;
			for (int i = 0; i < fresh.segments.size(); ++i) {
				const int seq = fresh.media_sequence + i;
				if (seq >= m_next_sequence)
					fresh_segments.push_back(fresh.segments.at(i));
			}

			if (fresh_segments.isEmpty()) {
				// **A list that has not grown is not a list that has ended**,
				// so this is where the stall bound earns its place: without it
				// a stream that simply pauses would be polled for ever.
				if (m_live_idle.elapsed() > live_stall_ms()) {
					finish_live(QString());
					return;
				}
				// Still live and still quiet: ask again after the interval.
				m_playlist.is_live        = fresh.is_live;
				m_playlist.target_duration = fresh.target_duration;
				if (!fresh.is_live) {
					finish_live(QString());
					return;
				}
				poll_live();
				return;
			}

			m_live_idle.restart();
			m_segments_base    += m_playlist.segments.size();
			m_playlist          = fresh;
			m_playlist.segments = fresh_segments;
			m_next_sequence    += fresh_segments.size();
			m_index             = 0;
			m_attempt           = 0;
			// The total is not knowable for a growing list, so it is reported as
			// what is known: everything taken so far plus this round. A bar fed
			// this climbs and never walks backwards, which is the property the
			// two-pass case established.
			m_segments_all      = m_segments_base + fresh_segments.size();
			next_segment();
		});
	});
}

// End a live assembly, saying why when there is a reason beyond "it ended".
// Success either way: what was fetched is on disk and playable, and calling a
// captured window a failure would throw away the thing that worked.
void hls_assembler::finish_live(const QString &why) {
	if (m_file) {
		m_file->flush();
		m_file->close();
	}
	m_finished = true;
	if (!why.isEmpty())
		emit status_note(why);
	emit completed();
}

void hls_assembler::next_segment() {
	if (m_stopped)
		return;
	if (m_index >= m_playlist.segments.size()) {
		// **The video list is done; the audio list may not have started.** One
		// assembly, two passes, because a DASH manifest's audio is a second
		// ordered list of segments and this engine walks exactly that.
		if (!m_audio_pending.isEmpty() && !m_audio_done) {
			if (m_file) {
				m_file->flush();
				m_file->close();
				delete m_file;
				m_file = nullptr;
			}
			m_file = new QFile(m_audio_path, this);
			if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				// The video is on disk and playable, silently. Say which half
				// exists rather than reporting a failed assembly, because the
				// caller decides what a video without sound is worth.
				emit failed("Cannot write the audio to " + m_audio_path +
				             " — the video is assembled without it.");
				delete m_file;
				m_file = nullptr;
				return;
			}
			m_segments_base    += m_playlist.segments.size();
			m_playlist.segments = m_audio_pending;
			m_audio_pending.clear();
			m_audio_done = true;
			m_index      = 0;
			m_attempt    = 0;
			next_segment();
			return;
		}

		// **A live playlist keeps growing, so running out of segments is only
		// the end for VOD.** Ask it again rather than calling the window it
		// happened to offer the whole broadcast -- which is what makes the
		// tee-to-disk trick work for live at all: sec 11.3 promises full
		// backward seek over everything captured, and that was true only of
		// whatever one read of the playlist contained.
		if (!m_live_url.isEmpty() && m_playlist.is_live && !m_stopped) {
			poll_live();
			return;
		}

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
		// Reported over the whole job rather than the current list: see
		// `m_segments_base`. `m_segments_all` is zero for a plain HLS assembly,
		// where the current list *is* the whole job, so the fallback keeps that
		// case saying exactly what it always said.
		emit progress(m_written, m_segments_base + m_index,
		               m_segments_all > 0 ? m_segments_all
		                                  : m_playlist.segments.size());
		next_segment();
	});
}

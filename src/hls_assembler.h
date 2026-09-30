#pragma once

#include "hls_playlist.h"
#include "local_proxy.h"

#include <QObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QString>
#include <QUrl>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

// Turns a manifest -- HLS or DASH -- into one growing local file (architecture
// doc sec 11.3).
//
// **The name is narrower than the job.** An MPD is converted into the same
// ordered segment list an HLS playlist becomes, and everything below the parse
// is shared, so there is no second assembler; renaming this class touches every
// user and is a mechanical change of its own rather than one to fold into a
// behavioural one.
//
// This is what "the app compensates in the proxy for what the player lacks"
// means concretely. Classic mplayer is strong on progressive files and weak at
// native HLS, so rather than hand it a manifest and hope, we fetch the segments
// ourselves and append them to a single file the player can seek around in.
//
// It is also the sec 11.3 tee-to-disk trick: because segments are written as they
// arrive, a *live* stream becomes a local VOD -- full backward seek over
// everything captured so far, plus a saved copy, in one step. The same
// mechanism therefore serves both "watch this properly" and "save this".
//
// Deliberately simple: segments are fetched strictly in order, one at a time.
// Concatenated MPEG-TS is directly playable, which is why this works at all
// without a remux.
//
// **fMP4 needs its initialisation segment first, and gets it.** That sentence
// used to say this class did not do it, which DASH made false: a representation
// contributes its init segment as the first entry of the list, so the
// concatenation is a valid fMP4 that ffmpeg can rewrap. What is still not done
// is the *mux* -- combining a separate audio stream with the video -- which
// needs two assembled files and an input more than `media_remux` takes, and is
// why an MPD with separate audio is refused rather than assembled.
class hls_assembler : public QObject {
	Q_OBJECT
public:
	explicit hls_assembler(QObject *parent = nullptr);
	~hls_assembler() override;

	// Fetch `manifest`, pick the best variant if it is a master playlist, then
	// stream its segments into `output_path`.
	// `audio_output_path`, when given, is where a **separately-carried audio
	// stream** is assembled. DASH hands video and audio out apart, and one
	// assembled representation is not the programme: with a path here the audio
	// is fetched into it after the video and the caller muxes the two; without
	// one, a manifest that carries them separately is refused rather than
	// turned into a file that plays perfectly and is silent.
	//
	// **The choice belongs to the caller because only the caller knows what the
	// file is for.** Saving can wait for both streams and mux; watching cannot,
	// because the player has the growing file open from the first segment and a
	// mux needs both complete. So Save passes a path and Watch does not, and the
	// refusal is not a limitation of this class but the honest answer to the
	// question Watch is asking.
	void start(const QUrl &manifest, const stream_context &ctx,
	            const QString &output_path,
	            const QString &audio_output_path = QString());
	void stop();

	QString output_path() const { return m_path; }
	// Where the audio landed, or empty when there was none to assemble -- which
	// is every HLS stream and every MPD that carries one muxed representation.
	// A caller muxes when this is non-empty and does not when it is not, so it
	// needs no separate flag for "was there audio".
	QString audio_path() const { return m_audio_done ? m_audio_path : QString(); }
	qint64  bytes_written() const { return m_written; }
	int     segments_done() const { return m_index; }
	int     segments_total() const { return m_playlist.segments.size(); }
	bool    finished() const { return m_finished; }
	// **Whether the list that was assembled was still growing** -- no
	// `#EXT-X-ENDLIST`. The parse has known this all along and nothing read
	// it, so a capture of a live stream completed in exactly the same words as
	// a whole VOD while the file held whatever window the playlist happened to
	// offer. That is the intended behaviour for watching -- the tee-to-disk
	// trick above turns a live stream into a locally seekable one -- and it is
	// not what "saved" usually means, so the difference is worth a sentence.
	// Only meaningful once `completed()` has been emitted.
	bool    was_live() const { return m_playlist.is_live; }

signals:
	// Emitted as each segment lands, so a reader knows how much is playable.
	void progress(qint64 bytes, int segments_done, int segments_total);
	void completed();
	void failed(const QString &message);
	// **Something a completed assembly still needs to say.** A live capture that
	// ended because the playlist could no longer be read is not a failure --
	// everything fetched is on disk and playable -- and it is not the same event
	// as a stream that ended. `failed` would throw the capture away and
	// `completed` alone would hide the difference, so this carries the reason
	// alongside the success. Emitted before `completed`, and only when there is
	// something to say.
	void status_note(const QString &message);

private:
	void fetch_manifest(const QUrl &url);
	// Fills `m_playlist` from an MPD, or emits `failed` and returns false.
	bool assemble_dash(const QByteArray &body, const QUrl &url);
	void next_segment();
	// **Ask a live playlist for what it has grown**, and continue into whatever
	// it lists beyond what has already been fetched. Called only when the list
	// ran out while still being live; ends the assembly when the playlist stops
	// growing or says it is complete.
	void poll_live();
	// End a live assembly as a success, with an optional reason.
	void finish_live(const QString &why);

	// Attempts per segment, and the step between them. Three is enough for the
	// blip this exists for and small enough that a segment which will never
	// arrive fails in about two seconds rather than hanging the assembly.
	static constexpr int k_segment_attempts = 3;
	static constexpr int k_retry_ms         = 400;

	// **A live capture has to stop on something, and it is not the playlist.**
	// `local_proxy` already holds this shape for the same reason -- no growth
	// for this long, give up -- because a poll loop whose end condition is a
	// remote file's willingness to stop growing is a loop with no end
	// condition. Thirty seconds is several segments at any ordinary target
	// duration, so a stream that is merely slow is not mistaken for one that
	// has ended.
	static constexpr int k_live_stall_ms = 30000;
	// **The bound, overridable, because a bound no test can reach is a bound
	// nobody checks.** Thirty seconds is the right production answer and the
	// wrong suite: a case that waits for it takes thirty seconds, so before this
	// existed the only live cases that could finish were ones ending on
	// `#EXT-X-ENDLIST`, and the stall path was exercised by nothing. Read once
	// per run from `HYDRA_LIVE_STALL_MS`, the same shape as the live-view cap
	// and the scale sizes -- a test knob in the environment rather than an API
	// nobody but a test would call.
	static int live_stall_ms();
	// The floor under the poll interval, which is otherwise the playlist's own
	// target duration. A manifest advertising a tenth of a second would
	// otherwise have this fetching it ten times a second.
	static constexpr int k_live_poll_min_ms = 1000;
	QNetworkReply *get(const QUrl &url, const QByteArray &range = QByteArray());

	QNetworkAccessManager *m_net = nullptr;
	QPointer<QNetworkReply> m_reply;
	// Which run a deferred callback belongs to. `m_stopped` cannot answer
	// that: `start()` sets it true through `stop()` and then false again, so a
	// retry timer pending from the previous press passes its own guard and
	// drives the new run. Counted rather than flagged, because the question is
	// *which* run asked and not whether some run is stopped.
	int                     m_run = 0;
	QFile  *m_file = nullptr;

	stream_context m_ctx;
	hls_playlist   m_playlist;
	// The audio pass: where it goes, the list it will walk, and whether it has
	// run. Three rather than one because "asked for" and "happened" are
	// different questions -- a manifest with no separate audio is asked for and
	// never happens, and `audio_path()` must answer about the second.
	QString        m_audio_path;
	QList<hls_segment> m_audio_pending;
	bool           m_audio_done = false;
	// **So progress does not walk backwards between the two passes.** Each pass
	// replaces `m_playlist.segments`, so `m_index` and the list's size are about
	// the current list alone; a bar fed those would reach the end of the video,
	// drop to zero and start again, which reads as a restarted download rather
	// than as the second half of one. These carry the whole job: what earlier
	// passes finished, and what every pass will amount to.
	int            m_segments_base  = 0;
	int            m_segments_all   = 0;

	// **Live re-polling.** `m_live_url` is the media playlist to ask again, and
	// is set only on the HLS path: an MPD with `type="dynamic"` is also live,
	// and continuing a segment template is a different mechanism from re-reading
	// a growing list, so DASH is deliberately not re-polled and says so rather
	// than being covered by the same flag.
	//
	// `m_next_sequence` is the discriminator for "new". A live playlist can
	// reuse a URL for different content, and `#EXT-X-MEDIA-SEQUENCE` is the
	// number the specification makes monotonic -- so what has already been
	// fetched is a number and not a set of addresses.
	QUrl           m_live_url;
	int            m_next_sequence = 0;
	QElapsedTimer  m_live_idle;
	QString m_path;
	qint64  m_written  = 0;
	int     m_index    = 0;
	bool    m_finished = false;
	bool    m_stopped  = false;
	// Attempts spent on the segment currently being fetched. A single failure
	// used to end the whole assembly, which over a real CDN and hundreds of
	// segments meant one transient error threw away everything already
	// downloaded.
	int     m_attempt  = 0;
	int     m_redirects = 0;   // master -> media playlist hops
};

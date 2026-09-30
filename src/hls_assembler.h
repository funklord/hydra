#pragma once

#include "hls_playlist.h"
#include "local_proxy.h"

#include <QObject>
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

private:
	void fetch_manifest(const QUrl &url);
	// Fills `m_playlist` from an MPD, or emits `failed` and returns false.
	bool assemble_dash(const QByteArray &body, const QUrl &url);
	void next_segment();

	// Attempts per segment, and the step between them. Three is enough for the
	// blip this exists for and small enough that a segment which will never
	// arrive fails in about two seconds rather than hanging the assembly.
	static constexpr int k_segment_attempts = 3;
	static constexpr int k_retry_ms         = 400;
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

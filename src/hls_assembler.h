// SPDX-License-Identifier: GPL-3.0-or-later
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

// Turns an HLS stream into one growing local file (architecture doc sec 11.3).
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
// without a remux; fMP4 segments would need their init segment prepended and a
// real remux to be seekable, and that is the ffmpeg step sec 11.2 describes and
// this does not do.
class hls_assembler : public QObject {
	Q_OBJECT
public:
	explicit hls_assembler(QObject *parent = nullptr);
	~hls_assembler() override;

	// Fetch `manifest`, pick the best variant if it is a master playlist, then
	// stream its segments into `output_path`.
	void start(const QUrl &manifest, const stream_context &ctx,
	            const QString &output_path);
	void stop();

	QString output_path() const { return m_path; }
	qint64  bytes_written() const { return m_written; }
	int     segments_done() const { return m_index; }
	int     segments_total() const { return m_playlist.segments.size(); }
	bool    finished() const { return m_finished; }

signals:
	// Emitted as each segment lands, so a reader knows how much is playable.
	void progress(qint64 bytes, int segments_done, int segments_total);
	void completed();
	void failed(const QString &message);

private:
	void fetch_manifest(const QUrl &url);
	void next_segment();
	QNetworkReply *get(const QUrl &url, const QByteArray &range = QByteArray());

	QNetworkAccessManager *m_net = nullptr;
	QPointer<QNetworkReply> m_reply;
	QFile  *m_file = nullptr;

	stream_context m_ctx;
	hls_playlist   m_playlist;
	QString m_path;
	qint64  m_written  = 0;
	int     m_index    = 0;
	bool    m_finished = false;
	bool    m_stopped  = false;
	int     m_redirects = 0;   // master -> media playlist hops
};

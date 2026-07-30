// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

class QTcpServer;
class QTcpSocket;
class QNetworkAccessManager;

// The request context a CDN expects to see, captured from the page that
// actually loaded the stream (architecture doc §11.3).
struct stream_context {
	QString referer;
	QString user_agent;
	QString cookies;
};

// The local HTTP proxy (architecture doc §10, §11.3).
//
// A naked stream URL frequently returns 403, because the CDN expects the same
// Referer, cookies and User-Agent the page carried. The player-agnostic fix is
// to point the player at localhost and inject that context upstream, rather
// than depending on whichever header flags a given player happens to support —
// which vary, and for mplayer are limited.
//
// Two properties matter more than anything else here:
//
//  * **Range transparency.** When the player sends `Range: bytes=X-Y` this
//    issues the same range upstream and relays the 206 with `Content-Range`
//    and `Accept-Ranges` intact. Without that the player cannot seek, and
//    seekability is most of the point.
//  * **Verbatim relay.** Manifests and segments are passed through unmodified,
//    so an HLS playlist keeps its full segment list and its `#EXTINF`,
//    `#EXT-X-MEDIA-SEQUENCE` and `#EXT-X-BYTERANGE` timing tags. Rewriting a
//    manifest is how seeking silently breaks.
//
// Security: it binds to 127.0.0.1 only, and serves nothing but URLs explicitly
// published to it, each behind an unguessable token. It is a context-injecting
// relay for streams the user chose, not a general forward proxy — a local page
// that guessed the port still cannot make it fetch anything.
//
// Scope: this is the player-facing half of §10. Routing the *browser* through
// it for response inspection is a separate problem, because intercepting HTTPS
// means terminating TLS with a generated certificate the browser must trust,
// which the design does not currently address.
class local_proxy : public QObject {
	Q_OBJECT
public:
	explicit local_proxy(QObject *parent = nullptr);
	~local_proxy() override;

	// Binds to loopback. Port 0 takes an ephemeral one. False if it could not
	// listen; the caller falls back to handing the player the raw URL.
	bool start(quint16 port = 0);
	void stop();
	bool listening() const;
	quint16 port() const;

	// Publish `upstream` and get back the localhost URL to hand a player.
	QUrl publish(const QUrl &upstream, const stream_context &ctx);

	// How many bytes at the start of a published file are genuinely readable.
	// Returning -1 means "trust the file's size".
	using available_length = std::function<qint64()>;

	// What the file will eventually be, or -1 if unknown.
	//
	// This is what makes *holding* possible at all. HTTP wants a length before
	// the body, so a server that only knows what it has right now can promise
	// no more than that — and a player that reaches the end of a complete-
	// looking response stops, which is exactly the "File ended prematurely" a
	// growing file produces. Advertising the eventual size instead lets the
	// response stay open and the player keep waiting.
	using expected_length = std::function<qint64()>;

	// Publish a local file that is still being written — the assembled HLS
	// output (§11.3), or a torrent being watched as it downloads (§11.4).
	// Ranges are served against whatever has landed so far, so a player can
	// seek backwards through a live capture while it grows.
	//
	// `avail` exists because a file's size is not always a statement about what
	// is in it. An appended capture can be trusted (omit it). A torrent's files
	// are allocated **sparse and full-size from the outset**, so the size is
	// right immediately while the content is mostly holes that read as zeros —
	// serving those would hand the player silence instead of an honest short
	// read, and it would look like a corrupt stream rather than a slow one.
	QUrl publish_file(const QString &path, const QString &content_type,
	                   available_length avail = {}, expected_length total = {});
	void unpublish_all();

	// --- capture (architecture doc §11.6) ---------------------------------
	// Open a file for a page to append to, and get back the URL it posts to.
	//
	// The token ends up inside the page, which is worth being explicit about:
	// it grants exactly one thing, appending to a file the user just asked to
	// create, and the bytes going into that file are page-supplied by
	// definition. So a page abusing its own token can only corrupt its own
	// capture — it gains nothing it did not already have. It cannot read, cannot
	// name a path, and cannot reach another capture.
	QUrl open_capture(const QString &path);
	void close_capture(const QUrl &url);
	qint64 captured_bytes(const QUrl &url) const;

signals:
	void failed(const QString &message);

private:
	struct entry {
		QUrl           upstream;
		stream_context ctx;
		QString        local_path;     // set instead of upstream for a file
		QString        content_type;
		available_length avail;        // readable prefix, or null to trust size
		expected_length  expected;     // eventual size, or null if unknown
		bool     capture = false;      // accepts POSTed bytes instead of serving
		qint64   received = 0;
	};

	void on_connection();
	void serve(QTcpSocket *client, const QByteArray &request_head,
	            const QByteArray &body);
	void accept_capture(QTcpSocket *client, const QString &token,
	                     const QByteArray &body);
	void serve_file(QTcpSocket *client, const entry &e, const QByteArray &head,
	                 bool head_only);

	QTcpServer            *m_server = nullptr;
	QNetworkAccessManager *m_net    = nullptr;
	QHash<QString, entry>  m_published;
	// A POST body does not arrive in one read, so each connection accumulates
	// until its headers and declared length are both complete.
	QHash<QTcpSocket *, QByteArray> m_incoming;
};

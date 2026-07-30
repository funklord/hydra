// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>

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
	void unpublish_all();

signals:
	void failed(const QString &message);

private:
	struct entry {
		QUrl           upstream;
		stream_context ctx;
	};

	void on_connection();
	void serve(QTcpSocket *client, const QByteArray &request_head);

	QTcpServer            *m_server = nullptr;
	QNetworkAccessManager *m_net    = nullptr;
	QHash<QString, entry>  m_published;
};

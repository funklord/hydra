// SPDX-License-Identifier: GPL-3.0-or-later
#include "local_proxy.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QFileInfo>

namespace {

QByteArray status_line(int code) {
	const char *reason = "OK";
	switch (code) {
		case 200: reason = "OK"; break;
		case 206: reason = "Partial Content"; break;
		case 302: reason = "Found"; break;
		case 403: reason = "Forbidden"; break;
		case 404: reason = "Not Found"; break;
		case 416: reason = "Range Not Satisfiable"; break;
		case 502: reason = "Bad Gateway"; break;
		default:  reason = "Status"; break;
	}
	return "HTTP/1.1 " + QByteArray::number(code) + " " + reason + "\r\n";
}

void send_simple(QTcpSocket *sock, int code, const QByteArray &body) {
	sock->write(status_line(code));
	sock->write("Content-Type: text/plain\r\n");
	sock->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
	sock->write("Connection: close\r\n\r\n");
	sock->write(body);
	sock->disconnectFromHost();
}

// Header lookup in the raw request head, case-insensitively.
QByteArray header_of(const QByteArray &head, const char *name) {
	const QList<QByteArray> lines = head.split('\n');
	const QByteArray want = QByteArray(name).toLower() + ":";
	for (const QByteArray &raw : lines) {
		const QByteArray line = raw.trimmed();
		if (line.toLower().startsWith(want))
			return line.mid(want.size()).trimmed();
	}
	return {};
}

}  // namespace

local_proxy::local_proxy(QObject *parent) : QObject(parent) {
	m_net = new QNetworkAccessManager(this);
}

local_proxy::~local_proxy() {
	stop();
}

bool local_proxy::start(quint16 port) {
	if (m_server && m_server->isListening())
		return true;
	if (!m_server) {
		m_server = new QTcpServer(this);
		connect(m_server, &QTcpServer::newConnection, this, &local_proxy::on_connection);
	}
	// Loopback only: this must never be reachable from the network.
	if (!m_server->listen(QHostAddress::LocalHost, port)) {
		emit failed("Could not start the local proxy: " + m_server->errorString());
		return false;
	}
	return true;
}

void local_proxy::stop() {
	if (m_server)
		m_server->close();
	m_published.clear();
}

bool local_proxy::listening() const {
	return m_server && m_server->isListening();
}

quint16 local_proxy::port() const {
	return m_server ? m_server->serverPort() : 0;
}

QUrl local_proxy::publish(const QUrl &upstream, const stream_context &ctx) {
	if (!listening())
		return {};
	// 128 bits of unguessable token: the port is easy to find, the path is not.
	QByteArray raw(16, Qt::Uninitialized);
	QRandomGenerator::system()->generate(raw.begin(), raw.end());
	const QString token = QString::fromLatin1(raw.toHex());

	m_published.insert(token, entry{upstream, ctx});

	QUrl local;
	local.setScheme("http");
	local.setHost("127.0.0.1");
	local.setPort(port());
	// Keep the upstream file extension: players sniff it to pick a demuxer.
	QString suffix;
	const int dot = upstream.path().lastIndexOf('.');
	if (dot >= 0)
		suffix = upstream.path().mid(dot);
	local.setPath("/s/" + token + suffix);
	return local;
}

QUrl local_proxy::publish_file(const QString &path, const QString &content_type,
                                available_length avail) {
	if (!listening())
		return {};
	QByteArray raw(16, Qt::Uninitialized);
	QRandomGenerator::system()->generate(raw.begin(), raw.end());
	const QString token = QString::fromLatin1(raw.toHex());

	entry e;
	e.local_path   = path;
	e.content_type = content_type;
	e.avail        = std::move(avail);
	m_published.insert(token, e);

	QUrl local;
	local.setScheme("http");
	local.setHost("127.0.0.1");
	local.setPort(port());
	// Players pick their demuxer from the extension, so it has to be the real
	// one. This was hardcoded to .ts back when the only published file was
	// assembled MPEG-TS; serving a torrent's .mkv under a .ts name told the
	// player the wrong container before it had read a byte.
	const QString ext = QFileInfo(path).suffix();
	local.setPath("/s/" + token + (ext.isEmpty() ? QString(".ts")
	                                             : "." + ext.toLower()));
	return local;
}

void local_proxy::serve_file(QTcpSocket *client, const entry &e,
                              const QByteArray &head, bool head_only) {
	QFile f(e.local_path);
	if (!f.open(QIODevice::ReadOnly)) {
		send_simple(client, 404, "not available yet");
		return;
	}
	// Whatever has landed so far. A live capture grows between requests, which
	// is exactly what makes seeking backwards through it work.
	//
	// For a sparsely-allocated file the size is already the final one while the
	// content is still arriving, so the publisher's own answer wins where it
	// has one — otherwise every read past the real data returns zeros and the
	// player renders them rather than waiting.
	qint64 size = f.size();
	if (e.avail) {
		const qint64 usable = e.avail();
		if (usable >= 0)
			size = qMin(size, usable);
	}
	if (size <= 0) {
		send_simple(client, 503, "not enough downloaded yet");
		return;
	}

	qint64 start = 0, end = size - 1;
	bool partial = false;
	const QByteArray range = header_of(head, "Range");
	if (range.startsWith("bytes=")) {
		const QByteArray spec = range.mid(6);
		const int dash = spec.indexOf('-');
		if (dash >= 0) {
			const QByteArray a = spec.left(dash).trimmed();
			const QByteArray b = spec.mid(dash + 1).trimmed();
			if (!a.isEmpty()) {
				start = a.toLongLong();
				if (!b.isEmpty())
					end = qMin<qint64>(b.toLongLong(), size - 1);
			} else if (!b.isEmpty()) {
				start = qMax<qint64>(0, size - b.toLongLong());   // suffix range
			}
			partial = true;
		}
	}
	if (start >= size || start > end) {
		client->write(status_line(416));
		client->write("Content-Range: bytes */" + QByteArray::number(size) + "\r\n");
		client->write("Connection: close\r\n\r\n");
		client->disconnectFromHost();
		return;
	}

	const qint64 length = end - start + 1;
	client->write(status_line(partial ? 206 : 200));
	client->write("Content-Type: " + e.content_type.toUtf8() + "\r\n");
	client->write("Accept-Ranges: bytes\r\n");
	client->write("Content-Length: " + QByteArray::number(length) + "\r\n");
	if (partial)
		client->write("Content-Range: bytes " + QByteArray::number(start) + "-" +
		               QByteArray::number(end) + "/" + QByteArray::number(size) + "\r\n");
	client->write("Connection: close\r\n\r\n");

	if (!head_only) {
		f.seek(start);
		qint64 left = length;
		while (left > 0 && client->state() == QAbstractSocket::ConnectedState) {
			const QByteArray chunk = f.read(qMin<qint64>(left, 64 * 1024));
			if (chunk.isEmpty())
				break;
			client->write(chunk);
			client->waitForBytesWritten(3000);
			left -= chunk.size();
		}
	}
	client->disconnectFromHost();
}

void local_proxy::unpublish_all() {
	m_published.clear();
}

void local_proxy::on_connection() {
	while (QTcpSocket *client = m_server->nextPendingConnection()) {
		connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
		connect(client, &QTcpSocket::readyRead, this, [this, client] {
			const QByteArray head = client->readAll();
			if (head.isEmpty())
				return;
			serve(client, head);
		});
	}
}

void local_proxy::serve(QTcpSocket *client, const QByteArray &head) {
	const QByteArray request_line = head.left(head.indexOf('\n')).trimmed();
	const QList<QByteArray> parts = request_line.split(' ');
	if (parts.size() < 2) {
		send_simple(client, 400, "bad request");
		return;
	}
	const QByteArray method = parts.at(0);
	const QString    path   = QString::fromLatin1(parts.at(1));

	// Only published tokens are served. This is what keeps the proxy from
	// being usable as a general forward proxy by anything that finds the port.
	QString token = path.section('/', 2, 2);
	const int dot = token.indexOf('.');
	if (dot >= 0)
		token = token.left(dot);
	if (!m_published.contains(token)) {
		send_simple(client, 404, "not published");
		return;
	}
	if (method != "GET" && method != "HEAD") {
		send_simple(client, 405, "method not allowed");
		return;
	}

	const entry e = m_published.value(token);
	if (!e.local_path.isEmpty()) {
		serve_file(client, e, head, method == "HEAD");
		return;
	}

	QNetworkRequest req(e.upstream);
	// The context the CDN expects, replayed from the page that loaded it.
	if (!e.ctx.referer.isEmpty())
		req.setRawHeader("Referer", e.ctx.referer.toUtf8());
	if (!e.ctx.user_agent.isEmpty())
		req.setRawHeader("User-Agent", e.ctx.user_agent.toUtf8());
	if (!e.ctx.cookies.isEmpty())
		req.setRawHeader("Cookie", e.ctx.cookies.toUtf8());

	// Range transparency: forward the player's range verbatim so it can seek.
	const QByteArray range = header_of(head, "Range");
	if (!range.isEmpty())
		req.setRawHeader("Range", range);

	QNetworkReply *reply = m_net->get(req);

	connect(reply, &QNetworkReply::metaDataChanged, client, [reply, client] {
		if (client->state() != QAbstractSocket::ConnectedState)
			return;
		const int code =
			reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		client->write(status_line(code ? code : 200));
		// Relay the headers that make seeking work, unmodified.
		for (const char *name : {"Content-Type", "Content-Length",
		                          "Content-Range", "Accept-Ranges"}) {
			const QByteArray v = reply->rawHeader(name);
			if (!v.isEmpty())
				client->write(QByteArray(name) + ": " + v + "\r\n");
		}
		client->write("Connection: close\r\n\r\n");
	});

	// Stream rather than buffer: a video is not something to hold in memory.
	connect(reply, &QNetworkReply::readyRead, client, [reply, client] {
		if (client->state() == QAbstractSocket::ConnectedState)
			client->write(reply->readAll());
	});

	connect(reply, &QNetworkReply::finished, client, [reply, client] {
		if (client->state() == QAbstractSocket::ConnectedState) {
			client->write(reply->readAll());
			client->disconnectFromHost();
		}
		reply->deleteLater();
	});

	// If the player goes away mid-stream, stop pulling from upstream.
	connect(client, &QTcpSocket::disconnected, reply, [reply] {
		if (reply->isRunning())
			reply->abort();
	});
}

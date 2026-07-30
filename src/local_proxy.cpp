// SPDX-License-Identifier: GPL-3.0-or-later
#include "local_proxy.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>

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

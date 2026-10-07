// A server that answers with the request's own headers, as JSON, so what a
// client sent is readable from what came back.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

// Replaces `test/echohdr.py` and `test/echodl.py`, which had to be started by
// hand on fixed ports and were the only reason the two suites using them sat
// outside `make test`. One class rather than two, because the difference
// between those files was a status code and a Content-Type, and a second copy
// of a behaviour is a second thing to be wrong.
//
// **`range_aware` is not decoration.** With it set, a request carrying a
// `Range` is answered 206 and everything else 200 -- which is what a real CDN
// does, and what `http_download_source` depends on: only 206 means "the rest
// of it", and on anything else it truncates the partial file and starts
// again. A stand-in that answered 200 to a resumed request would make a
// resume test pass for the wrong reason.
class echo_server : public QTcpServer {
public:
	bool       range_aware  = false;
	QByteArray content_type = "application/json";
	// **An opt-in 302, for asking what a consumer does with a redirect.**
	// Empty by default, so every existing user answers exactly as before. When
	// set, a request whose target starts with this prefix gets a 302 to
	// `redirect_to` and nothing else -- which is what a signed CDN url does on
	// its way to a regional edge.
	QByteArray redirect_from;
	QByteArray redirect_to = "/stream";
	int        redirects_served = 0;

	// **An opt-in fixed body, for a consumer that fetches a document rather
	// than asking what it sent.** Empty by default, so every existing user
	// answers exactly as before -- the same shape as `redirect_from` above,
	// and for the same reason: a second fixture class would be a second copy
	// of the socket handling, which this file's own note argues against.
	//
	// `file_status` applies to a matched path only, so a test can ask what a
	// consumer does with a 404 that still carries a body -- which is the
	// shape a moved list server actually answers with.
	QHash<QByteArray, QByteArray> files;
	int        file_status = 200;
	int        files_served = 0;

	// The base url, once listening. Empty when it could not.
	QString start() {
		if (!listen(QHostAddress::LocalHost, 0))
			return QString();
		return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
	}

protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::disconnected, this, [this, s] {
			m_buf.remove(s);
			s->deleteLater();
		});
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			QByteArray &buf = m_buf[s];
			buf += s->readAll();
			// A GET has no body, so the header block is the whole request --
			// and it is not guaranteed to arrive in one read.
			const int end = buf.indexOf("\r\n\r\n");
			if (end < 0)
				return;
			const QByteArray head = buf.left(end);
			m_buf.remove(s);

			if (!redirect_from.isEmpty()) {
				const int sp = head.indexOf(' ');
				const QByteArray target =
				  head.mid(sp + 1, head.indexOf(' ', sp + 1) - sp - 1);
				if (target.startsWith(redirect_from)) {
					++redirects_served;
					s->write("HTTP/1.1 302 Found\r\nLocation: " + redirect_to +
					          "\r\nContent-Length: 0\r\n"
					          "Connection: close\r\n\r\n");
					s->flush();
					s->disconnectFromHost();
					return;
				}
			}

			if (!files.isEmpty()) {
				const int sp = head.indexOf(' ');
				QByteArray target =
				  head.mid(sp + 1, head.indexOf(' ', sp + 1) - sp - 1);
				const int q = target.indexOf('?');
				if (q >= 0)
					target = target.left(q);
				const auto it = files.constFind(target);
				if (it != files.constEnd()) {
					++files_served;
					const QByteArray &out = it.value();
					s->write("HTTP/1.1 " + QByteArray::number(file_status) +
					          " \r\nContent-Type: " + content_type +
					          "\r\nContent-Length: " +
					          QByteArray::number(out.size()) +
					          "\r\nConnection: close\r\n\r\n" + out);
					s->flush();
					s->disconnectFromHost();
					return;
				}
			}

			// Keys lowercased, values as sent, as both python files did: a
			// header name is case-insensitive on the wire and Qt sends them
			// lowercased anyway, so a reader matching on case would be
			// testing the transport rather than the browser.
			QJsonObject o;
			bool ranged = false;
			const QList<QByteArray> lines = head.split('\n');
			for (int i = 1; i < lines.size(); ++i) {   // 0 is the request line
				const QByteArray line = lines.at(i).trimmed();
				const int colon = line.indexOf(':');
				if (colon <= 0)
					continue;
				const QString key =
				  QString::fromUtf8(line.left(colon)).toLower();
				if (key == QLatin1String("range"))
					ranged = true;
				o.insert(key, QString::fromUtf8(line.mid(colon + 1).trimmed()));
			}
			const QByteArray body =
			  QJsonDocument(o).toJson(QJsonDocument::Compact);

			s->write("HTTP/1.1 " +
			          QByteArray(range_aware && ranged ? "206 Partial Content"
			                                            : "200 OK") +
			          "\r\nContent-Type: " + content_type +
			          "\r\nContent-Length: " + QByteArray::number(body.size()) +
			          "\r\nConnection: close\r\n\r\n" + body);
			s->flush();
			s->disconnectFromHost();
		});
	}

private:
	QHash<QTcpSocket *, QByteArray> m_buf;
};

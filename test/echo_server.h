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

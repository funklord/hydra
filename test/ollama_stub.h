// Enough of Ollama to answer a probe, and a host that answers nothing.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

// `ollama_provider::probe` asks for `/api/tags` and reads `models[].name` out
// of the answer; nothing else about Ollama is exercised by a probe, so
// nothing else is here.
//
// `models` is settable rather than fixed because `ready()` distinguishes
// "running" from "running and has the model you asked for" -- a stub naming a
// model the provider under test is not configured for reports reachable and
// then refuses, which is a third state and not the one a probe test wants.
// The suites set it from the provider's own `model()` rather than repeating a
// default that lives in `ollama_provider.h`.
class ollama_stub : public QTcpServer {
public:
	QStringList models = { QStringLiteral("llama3") };
	QStringList seen;   // the paths asked for, so a suite can say what ran

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
			const int end = buf.indexOf("\r\n\r\n");
			if (end < 0)
				return;
			const QByteArray head = buf.left(end);
			m_buf.remove(s);

			const int sp = head.indexOf(' ') + 1;
			const QString path =
			  QString::fromUtf8(head.mid(sp, head.indexOf(' ', sp) - sp));
			seen << path;

			if (!path.startsWith(QLatin1String("/api/tags"))) {
				s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
				          "Connection: close\r\n\r\n");
				s->flush();
				s->disconnectFromHost();
				return;
			}

			QJsonArray list;
			for (const QString &m : models) {
				QJsonObject o;
				o.insert("name", m);
				list.append(o);
			}
			QJsonObject root;
			root.insert("models", list);
			const QByteArray body =
			  QJsonDocument(root).toJson(QJsonDocument::Compact);
			s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json"
			          "\r\nContent-Length: " + QByteArray::number(body.size()) +
			          "\r\nConnection: close\r\n\r\n" + body);
			s->flush();
			s->disconnectFromHost();
		});
	}

private:
	QHash<QTcpSocket *, QByteArray> m_buf;
};

// **A closed port and a silent host are different failures**, and only the
// second costs a timeout. A refused connection comes back at once, so a test
// pointed at one measures how fast the kernel says no; the timeout the probe
// configures is only ever spent on a host that accepts and then says nothing.
//
// The sockets are held rather than dropped: closing one would answer.
class blackhole : public QTcpServer {
public:
	QString start() {
		if (!listen(QHostAddress::LocalHost, 0))
			return QString();
		return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
	}

protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);   // parented, so it dies with this
		s->setSocketDescriptor(fd);
		m_held << s;
	}

private:
	QList<QTcpSocket *> m_held;
};

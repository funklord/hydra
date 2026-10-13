// The local proxy fetching a stream a range at a time, as yt-dlp says
// YouTube must be fetched, and only as fast as the player takes it.
//
// **Why this exists**: Watch on a YouTube set handed mpv the sound through
// this proxy, and YouTube throttles one open-ended request -- measured
// through the proxy 2026-10-13, the sound arrived at 32 KB/s while the
// picture came at 2.2 MB/s. And the proxy queued whatever upstream sent
// for a player that had stopped reading, which for a 4.2 GB stream is
// memory without a bound.
#include "local_proxy.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// Honours `Range: bytes=a-b` with 206 and Content-Range, as googlevideo does,
// and sends /videoplayback on to /real first, as YouTube sends each stream
// to another host. Counts the ranges it was asked for.
class range_server : public QTcpServer {
public:
	explicit range_server(qint64 size) : m_body(size, 0) {
		for (qint64 i = 0; i < size; ++i)
			m_body[i] = char(i % 251);
	}
	QByteArray body() const { return m_body; }
	QList<QByteArray> asked;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		auto head = std::make_shared<QByteArray>();
		connect(s, &QTcpSocket::readyRead, this, [this, s, head] {
			*head += s->readAll();
			if (!head->contains("\r\n\r\n"))
				return;
			if (head->startsWith("GET /videoplayback")) {
				s->write("HTTP/1.1 302 Found\r\nLocation: /real\r\n"
				          "Content-Length: 0\r\nConnection: close\r\n\r\n");
				s->disconnectFromHost();
				return;
			}
			QByteArray range;
			for (const QByteArray &line : head->split('\n'))
				if (line.toLower().startsWith("range:"))
					range = line.mid(6).trimmed();
			asked << range;
			qint64 from = 0, to = m_body.size() - 1;
			if (range.startsWith("bytes=")) {
				const QList<QByteArray> ab = range.mid(6).split('-');
				from = ab.value(0).toLongLong();
				if (!ab.value(1).isEmpty())
					to = qMin<qint64>(ab.value(1).toLongLong(), m_body.size() - 1);
			}
			const QByteArray part = m_body.mid(from, to - from + 1);
			s->write("HTTP/1.1 206 Partial Content\r\nContent-Type: audio/webm\r\n"
			          "Content-Range: bytes " + QByteArray::number(from) + '-' +
			          QByteArray::number(to) + '/' + QByteArray::number(m_body.size()) +
			          "\r\nContent-Length: " + QByteArray::number(part.size()) +
			          "\r\nConnection: close\r\n\r\n" + part);
			s->disconnectFromHost();
		});
	}

private:
	QByteArray m_body;
};

// A player: asks for `range`, and reads everything until the proxy closes.
struct answer {
	QByteArray head, body;
};
static answer play(const QUrl &url, const QByteArray &range, int limit_ms = 15000) {
	QTcpSocket s;
	s.connectToHost(url.host(), quint16(url.port()));
	if (!s.waitForConnected(3000))
		return {};
	s.write("GET " + url.path().toLatin1() + " HTTP/1.1\r\nHost: x\r\n" +
	        (range.isEmpty() ? QByteArray() : "Range: " + range + "\r\n") + "\r\n");
	QByteArray all;
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < limit_ms) {
		spin(10);
		all += s.readAll();
		if (s.state() == QAbstractSocket::UnconnectedState)
			break;
	}
	all += s.readAll();
	const int split = all.indexOf("\r\n\r\n");
	return { all.left(split), split < 0 ? QByteArray() : all.mid(split + 4) };
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	local_proxy proxy;
	check(proxy.start(), "the proxy listens");

	section("a stream fetched a range at a time, through a redirect each");
	{
		range_server up(35000);
		up.listen(QHostAddress::LocalHost, 0);
		const QUrl via = proxy.publish(
		    QUrl(QString("http://127.0.0.1:%1/videoplayback").arg(up.serverPort())),
		    stream_context{}, 10000);
		const answer a = play(via, "bytes=0-");
		check(a.body == up.body(),
		      QString("an open-ended ask gets the whole stream, byte for byte "
		               "(%1 of %2)").arg(a.body.size()).arg(up.body().size()));
		const QList<QByteArray> four = { "bytes=0-9999", "bytes=10000-19999",
		                                 "bytes=20000-29999", "bytes=30000-34999" };
		check(up.asked == four,
		      QString("in ranges of the size asked, the last within the stream (%1)")
		          .arg(QString::fromLatin1(up.asked.join(", "))));
		check(a.head.startsWith("HTTP/1.1 206") &&
		      a.head.contains("Content-Range: bytes 0-34999/35000") &&
		      a.head.contains("Content-Length: 35000"),
		      QString("as one answer, its whole size said up front (%1)")
		          .arg(QString::fromLatin1(a.head).replace("\r\n", " | ")));

		up.asked.clear();
		const answer mid = play(via, "bytes=12345-");
		check(mid.body == up.body().mid(12345) &&
		      mid.head.contains("Content-Range: bytes 12345-34999/35000"),
		      QString("a seek gets the rest from where it asked (%1 bytes)")
		          .arg(mid.body.size()));
		up.asked.clear();
		const answer bounded = play(via, "bytes=100-199");
		check(bounded.body == up.body().mid(100, 100) && up.asked.size() == 1,
		      "and a bounded ask is one range");
		const answer whole = play(via, QByteArray());
		check(whole.body == up.body() && whole.head.startsWith("HTTP/1.1 200"),
		      "no Range gets the whole stream as a 200");
	}

	// **Only as fast as the player takes it.** A player that stops reading
	// holds upstream back to about `k_backlog` plus a range or two, rather
	// than the proxy fetching the whole stream into memory.
	section("a player that stops reading holds upstream back");
	{
		const qint64 chunk = 1024 * 1024;
		range_server up(40 * chunk);
		up.listen(QHostAddress::LocalHost, 0);
		const QUrl via = proxy.publish(
		    QUrl(QString("http://127.0.0.1:%1/real").arg(up.serverPort())),
		    stream_context{}, chunk);
		QTcpSocket s;
		s.setReadBufferSize(64 * 1024);   // the player's own cache, full
		s.connectToHost(via.host(), quint16(via.port()));
		s.waitForConnected(3000);
		s.write("GET " + via.path().toLatin1() + " HTTP/1.1\r\nRange: bytes=0-\r\n\r\n");
		spin(3000);   // reads nothing; the proxy fetches what it will
		const int held = int(up.asked.size());
		const qint64 bound = local_proxy::k_backlog / chunk + 4;
		check(held > 0 && held <= bound,
		      QString("%1 of 40 ranges asked while the player read nothing (at "
		               "most %2)").arg(held).arg(bound));
		QByteArray got;
		s.setReadBufferSize(0);
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < 20000 && s.state() != QAbstractSocket::UnconnectedState) {
			spin(10);
			got += s.readAll();
		}
		got += s.readAll();
		const QByteArray body = got.mid(got.indexOf("\r\n\r\n") + 4);
		check(body == up.body() && up.asked.size() == 40,
		      QString("and reading again resumes it to the end (%1 bytes, %2 ranges)")
		          .arg(body.size()).arg(up.asked.size()));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

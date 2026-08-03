// Turning an HLS stream into one local file (architecture doc §11.3).
//
// The parser's byte-range fix is only half the story: what matters is the file
// that comes out the other end. A playlist that is one resource cut into slices
// used to yield a file of the right length assembled from the same opening slice
// repeated — no error anywhere, just a video that is wrong. So this assembles
// against a real server and compares the bytes.
//
// The server is in-process and speaks enough HTTP to be a CDN for this purpose,
// including honouring `Range`, because a byte-range playlist is exactly the case
// under test and a server that ignored ranges would make it pass for the wrong
// reason.
#include "hls_assembler.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
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

// Three segments' worth of distinguishable bytes, so a wrong order or a repeated
// slice is visible in the result rather than merely a wrong length.
static QByteArray part(char c, int n) { return QByteArray(n, c); }

class cdn : public QTcpServer {
public:
	quint16 port = 0;
	QHash<QString, QByteArray> files;
	QStringList requested;
	QString     fail_path;   // a path that answers 404

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			if (!head.contains("\r\n\r\n"))
				return;
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);
			const QString path = QString::fromUtf8(target);
			requested << path;
			if (qEnvironmentVariableIsSet("HYDRA_CDN_DEBUG"))
				std::printf("        [req] %s\n",
				            head.left(head.indexOf("\r\n\r\n")).replace("\r\n", " | ").constData());

			if (path == fail_path || !files.contains(path)) {
				s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
				          "Connection: close\r\n\r\n");
				s->flush();
				s->disconnectFromHost();
				return;
			}

			QByteArray body = files.value(path);
			QByteArray status = "200 OK";
			QByteArray extra;

			// Range, honoured properly -- the whole point of the byte-range case.
			//
			// Matched case-insensitively, because header names are, and Qt puts
			// them on the wire lowercased. Looking for "Range:" found nothing,
			// this server quietly returned whole files, and the byte-range case
			// failed with three times the expected length -- which looks exactly
			// like the bug it was written to catch. The fixture was wrong, not
			// the assembler: it had sent bytes=0-3999, 4000-6999, 7000-8999,
			// which is precisely right.
			const int r = head.toLower().indexOf("range: bytes=");
			if (r >= 0) {
				const QByteArray spec =
					head.mid(r + 13, head.indexOf('\r', r) - (r + 13));
				const int dash = spec.indexOf('-');
				const qint64 from = spec.left(dash).toLongLong();
				const QByteArray to_s = spec.mid(dash + 1);
				const qint64 to = to_s.isEmpty() ? body.size() - 1 : to_s.toLongLong();
				if (from >= 0 && from < body.size()) {
					const qint64 last = qMin<qint64>(to, body.size() - 1);
					extra = "Content-Range: bytes " + QByteArray::number(from) + "-" +
					        QByteArray::number(last) + "/" +
					        QByteArray::number(body.size()) + "\r\n";
					body   = body.mid(from, last - from + 1);
					status = "206 Partial Content";
				}
			}

			s->write("HTTP/1.1 " + status + "\r\nContent-Type: application/octet-stream"
			          "\r\nContent-Length: " + QByteArray::number(body.size()) +
			          "\r\n" + extra + "Connection: close\r\n\r\n" + body);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

// Runs an assembly to completion (or failure), and says which.
static bool run(hls_assembler &a, const QUrl &manifest, const QString &out,
                 int max_ms = 8000) {
	QSignalSpy done(&a, &hls_assembler::completed);
	QSignalSpy bad(&a, &hls_assembler::failed);
	a.start(manifest, stream_context{}, out);
	QElapsedTimer t;
	t.start();
	while (done.isEmpty() && bad.isEmpty() && t.elapsed() < max_ms)
		spin(25);
	return !done.isEmpty();
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	const QString dir = QDir::tempPath() + "/hydra-asm-test";
	QDir(dir).removeRecursively();
	QDir().mkpath(dir);

	cdn server;
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		std::printf("could not listen\n");
		return 1;
	}
	server.port = server.serverPort();
	const QString base = QString("http://127.0.0.1:%1").arg(server.port);

	const QByteArray s0 = part('A', 4000);
	const QByteArray s1 = part('B', 3000);
	const QByteArray s2 = part('C', 2000);

	section("plain segments, concatenated in order");
	{
		server.files["/media.m3u8"] =
			"#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
			"#EXTINF:4,\nseg0.ts\n#EXTINF:4,\nseg1.ts\n#EXTINF:4,\nseg2.ts\n"
			"#EXT-X-ENDLIST\n";
		server.files["/seg0.ts"] = s0;
		server.files["/seg1.ts"] = s1;
		server.files["/seg2.ts"] = s2;

		hls_assembler a;
		const QString out = dir + "/plain.ts";
		check(run(a, QUrl(base + "/media.m3u8"), out), "the assembly completes");
		QFile f(out);
		f.open(QIODevice::ReadOnly);
		const QByteArray got = f.readAll();
		check(got == s0 + s1 + s2,
		      QString("the file is the segments in order (%1 bytes, wanted %2)")
		          .arg(got.size()).arg(s0.size() + s1.size() + s2.size()));
		check(a.segments_total() == 3 && a.segments_done() == 3,
		      "and it counted all three");
		check(a.bytes_written() == got.size(), "and reports what it wrote");
	}

	section("a byte-range playlist: one file cut into slices");
	{
		// The case the parser fix was for. Only the first sub-range states its
		// offset; the rest continue from where the previous one ended. Assembling
		// it must reproduce the original file exactly -- before the fix every
		// slice after the first was fetched from byte zero, giving a file of the
		// right length made of the same opening bytes over and over.
		const QByteArray whole = s0 + s1 + s2;
		server.files["/all.mp4"] = whole;
		server.files["/ranges.m3u8"] =
			"#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
			"#EXTINF:4,\n#EXT-X-BYTERANGE:4000@0\nall.mp4\n"
			"#EXTINF:4,\n#EXT-X-BYTERANGE:3000\nall.mp4\n"
			"#EXTINF:4,\n#EXT-X-BYTERANGE:2000\nall.mp4\n"
			"#EXT-X-ENDLIST\n";

		hls_assembler a;
		const QString out = dir + "/ranges.ts";
		check(run(a, QUrl(base + "/ranges.m3u8"), out), "the assembly completes");
		QFile f(out);
		f.open(QIODevice::ReadOnly);
		const QByteArray got = f.readAll();
		check(got.size() == whole.size(),
		      QString("the length matches (%1)").arg(got.size()));
		check(got == whole,
		      "and so do the bytes — the slices are the file, not the first slice "
		      "three times");
	}

	section("a master playlist picks the widest variant");
	{
		server.files["/master.m3u8"] =
			"#EXTM3U\n"
			"#EXT-X-STREAM-INF:BANDWIDTH=100000\nlow.m3u8\n"
			"#EXT-X-STREAM-INF:BANDWIDTH=900000\nhigh.m3u8\n";
		server.files["/low.m3u8"] =
			"#EXTM3U\n#EXTINF:4,\nseg2.ts\n#EXT-X-ENDLIST\n";
		server.files["/high.m3u8"] =
			"#EXTM3U\n#EXTINF:4,\nseg0.ts\n#EXTINF:4,\nseg1.ts\n#EXT-X-ENDLIST\n";

		server.requested.clear();
		hls_assembler a;
		const QString out = dir + "/master.ts";
		check(run(a, QUrl(base + "/master.m3u8"), out), "the assembly completes");
		check(server.requested.contains("/high.m3u8"),
		      "the high-bandwidth variant is the one followed");
		check(!server.requested.contains("/low.m3u8"),
		      "and the low one is never fetched");
		QFile f(out);
		f.open(QIODevice::ReadOnly);
		check(f.readAll() == s0 + s1, "with that variant's segments in the file");
	}

	section("a segment that does not exist is a failure, not a short file");
	{
		server.files["/broken.m3u8"] =
			"#EXTM3U\n#EXTINF:4,\nseg0.ts\n#EXTINF:4,\nmissing.ts\n#EXT-X-ENDLIST\n";

		hls_assembler a;
		QSignalSpy bad(&a, &hls_assembler::failed);
		const QString out = dir + "/broken.ts";
		const bool completed = run(a, QUrl(base + "/broken.m3u8"), out, 6000);
		check(!completed, "it does not report success");
		check(bad.count() == 1, "it reports the failure once");
		check(!a.finished(),
		      "and does not claim to have finished — a half file that says it is "
		      "whole is worse than an error");
	}

	section("a manifest that is not there");
	{
		hls_assembler a;
		QSignalSpy bad(&a, &hls_assembler::failed);
		check(!run(a, QUrl(base + "/no-such.m3u8"), dir + "/none.ts", 6000),
		      "no manifest, no assembly");
		check(bad.count() == 1, "and it says so rather than waiting forever");
	}

	QDir(dir).removeRecursively();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

// Downloading a learned stream: does it send what the CDN asked for, does
// resume still own the Range header, and does a write that never reaches the
// disk get reported as a finished download?
//
// **The server is in-process now.** It used to be `test/echodl.py`, started
// by hand on port 8851, and that was the only reason this suite sat in
// NEEDS_MORE -- three checks about which bytes leave this browser, run by
// whoever remembered to read `test/README.md` first. The python file is gone
// rather than kept beside this: two copies of one behaviour is two things to
// be wrong, and nothing else referenced it.
#include "download_manager.h"
#include "http_download_source.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <csignal>
#include <cstdio>
#include <sys/resource.h>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

static const download_job *job_of(const download_manager &m, int id) {
	for (const download_job &j : m.jobs()) if (j.id == id) return &j;
	return nullptr;
}

// The request's own headers, served back as the body, so a completed download
// IS the record of what the server saw.
//
// Faithful to the python it replaces rather than to what seemed reasonable:
// keys lowercased, values exactly as sent, and **206 when the request carried
// a Range, 200 otherwise**. That last one is not decoration -- only 206 means
// "the rest of it" to `http_download_source`, and a 200 makes it truncate the
// partial file and start again, which is the behaviour the resume section
// below is written against.
class echo : public QTcpServer {
public:
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
			// but it is not guaranteed to arrive in one read.
			const int end = buf.indexOf("\r\n\r\n");
			if (end < 0)
				return;
			const QByteArray head = buf.left(end);
			m_buf.remove(s);

			QByteArray body = "{\n";
			const QList<QByteArray> lines = head.split('\n');
			bool ranged = false;
			for (int i = 1; i < lines.size(); ++i) {     // 0 is the request line
				const QByteArray line = lines.at(i).trimmed();
				const int colon = line.indexOf(':');
				if (colon <= 0)
					continue;
				const QByteArray key = line.left(colon).toLower();
				const QByteArray val = line.mid(colon + 1).trimmed();
				if (key == "range")
					ranged = true;
				if (body.size() > 2)
					body += ",\n";
				body += "\"" + key + "\": \"" + val + "\"";
			}
			body += "\n}";

			s->write("HTTP/1.1 " +
			          QByteArray(ranged ? "206 Partial Content" : "200 OK") +
			          "\r\nContent-Type: video/mp4\r\nContent-Length: " +
			          QByteArray::number(body.size()) +
			          "\r\nConnection: close\r\n\r\n" + body);
			s->flush();
			s->disconnectFromHost();
		});
	}

private:
	QHash<QTcpSocket *, QByteArray> m_buf;
};

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	echo server;
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		std::printf("could not listen\n");
		return 1;
	}
	const QString base =
	  QString("http://127.0.0.1:%1").arg(server.serverPort());
	const QString dir  = QDir::temp().filePath("hydra-dlhdr");
	QDir(dir).removeRecursively();
	QDir().mkpath(dir);

	download_manager m;
	m.add_source(new http_download_source);
	m.set_directory(dir);

	section("a download carries the headers it was given");
	{
		QMap<QString, QString> h;
		h.insert("Referer", "https://site.example/watch/1");
		h.insert("User-Agent", "Hydra/1.0 dl");
		h.insert("X-Token", "abc123");

		QString err;
		const int id = m.enqueue(QUrl(base + "/stream.mp4"), "n1", &err, h);
		check(id != 0, QString("accepted (%1)").arg(err));

		QElapsedTimer t; t.start();
		while (!job_of(m, id)->terminal() && t.elapsed() < 15000) spin(50);
		check(job_of(m, id)->status == download_state::done, "it completes");

		QFile f(job_of(m, id)->path);
		f.open(QIODevice::ReadOnly);
		const QString seen = QString::fromUtf8(f.readAll());
		check(seen.contains("https://site.example/watch/1"),
		      "the Referer reached the server");
		check(seen.contains("Hydra/1.0 dl"), "and the User-Agent");
		check(seen.contains("abc123"), "and a header the extractor invented");
	}

	section("resume still owns Range");
	{
		// A partial file on disk means the source must ask for the rest -- and a
		// caller naming Range itself must not be able to move that offset.
		const QString path = QDir(dir).filePath("resume.mp4");
		{ QFile f(path); f.open(QIODevice::WriteOnly); f.write(QByteArray(500, 'x')); }

		QMap<QString, QString> h;
		h.insert("Referer", "https://site.example/2");
		h.insert("Range", "bytes=999999-");     // hostile or careless

		QString err;
		const int id = m.enqueue(QUrl(base + "/resume.mp4"), QString(), &err, h);
		QElapsedTimer t; t.start();
		while (!job_of(m, id)->terminal() && t.elapsed() < 15000) spin(50);

		QFile f(path);
		f.open(QIODevice::ReadOnly);
		const QString seen = QString::fromUtf8(f.readAll());
		check(seen.contains("bytes=500-"),
		      "the source's own resume offset is what was sent");
		check(!seen.contains("999999"),
		      "and the caller's Range was ignored, not merged");
		check(seen.contains("https://site.example/2"),
		      "while its other headers still went");
	}

	section("a download whose bytes never reach the disk is not done");
	{
		// `teardown` used to close the file and report `done` with `received`
		// and `total` both read back from the truncated file -- so they
		// agreed, the row went into the history as finished, and the person
		// opened a short file. QFile buffers, so none of the three `write`
		// calls in this source can answer; the flush in `teardown` is the one
		// point they all pass through.
		//
		// RLIMIT_FSIZE is what produces that here, measured before it was
		// relied on: the open succeeds, the write takes every byte, and the
		// flush fails. It applies to regular files only, so the socket this
		// suite's own server is speaking on is unaffected. SIGXFSZ has to be
		// ignored or the write kills the suite.
		QSignalSpy done(&m, &download_manager::changed);
		QString err;

		void (*was_sig)(int) = ::signal(SIGXFSZ, SIG_IGN);
		rlimit was{};
		::getrlimit(RLIMIT_FSIZE, &was);
		rlimit capped = was;
		capped.rlim_cur = 64;   // below the echoed header block
		const bool set = ::setrlimit(RLIMIT_FSIZE, &capped) == 0;
		int id = 0;
		if (set) {
			id = m.enqueue(QUrl(base + "/nospace.mp4"), "n3", &err);
			QElapsedTimer t; t.start();
			while (id && job_of(m, id) && !job_of(m, id)->terminal() &&
			        t.elapsed() < 15000)
				spin(50);
		}
		::setrlimit(RLIMIT_FSIZE, &was);
		::signal(SIGXFSZ, was_sig);

		if (!set) {
			std::printf("  --    RLIMIT_FSIZE could not be lowered here, so "
			             "the short-write check is skipped\n");
		} else {
			check(id != 0, QString("the download was accepted (%1)").arg(err));
			const download_job *j = id ? job_of(m, id) : nullptr;
			check(j && j->terminal(), "and it ends rather than hanging");
			check(j && j->status == download_state::failed,
			       QString("reporting failure, not done (%1)")
			         .arg(j ? int(j->status) : -1));
			check(j && !j->error.isEmpty(),
			       QString("and saying what went wrong (%1)")
			         .arg(j ? j->error : QString("(no job)")));
		}
	}

	QDir(dir).removeRecursively();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

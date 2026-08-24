// The blocking fetcher behind the helper tier (architecture doc sec 11.5.1).
//
// A real socket, because what is being tested is exactly the part the fake
// fetcher in test_helpers cannot stand in for: that a synchronous call from one
// thread gets a real network reply from another, without deadlocking and
// without re-entering the caller.
#include "network_fetcher.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

class origin : public QTcpServer {
public:
	QByteArray last_head;
	bool       stall = false;   // accept, answer nothing: the timeout case

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			last_head = s->readAll();
			if (stall)
				return;                     // deliberately never answers
			const QByteArray path = last_head.mid(4, last_head.indexOf(' ', 4) - 4);
			QByteArray type = "application/vnd.apple.mpegurl";
			QByteArray body = "#EXTM3U\n#EXT-X-ENDLIST\n";
			if (path.startsWith("/big")) {
				type = "video/mp4";
				body = QByteArray(50000, 'x');   // larger than any cap asked for
			}
			QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: " + type +
			                   "\r\nContent-Length: " + QByteArray::number(body.size()) +
			                   "\r\nConnection: close\r\n\r\n" + body;
			s->write(resp);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

// Run a fetch on another thread and keep this one's event loop turning.
//
// Not a convenience: `fetch` blocks its caller, and the fake origin below lives
// on the main thread, so calling it directly from main stops the server from
// ever answering and every request "times out". That is the whole hazard the
// design warns about, reproduced by accident -- a blocked thread serves nothing,
// which is exactly why the script must not run on the UI thread once this is
// wired into the dialog.
static fetch_result off_thread(network_fetcher &f, const QUrl &url,
                                qint64 cap, int timeout_ms) {
	fetch_result out;
	bool done = false;
	QThread *t = QThread::create([&] { out = f.fetch(url, cap, timeout_ms); done = true; });
	t->start();
	QElapsedTimer clock;
	clock.start();
	while (!done && clock.elapsed() < timeout_ms + 8000)
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
	t->wait(3000);
	delete t;
	return out;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	origin server;
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		std::printf("could not listen\n");
		return 1;
	}
	const QString base = QString("http://127.0.0.1:%1").arg(server.serverPort());

	section("a synchronous call across a thread");
	{
		stream_context ctx;
		ctx.referer    = "https://site.example/watch/1";
		ctx.user_agent = "Hydra/1.0 helper";
		ctx.cookies    = "sid=abc";
		ctx.extra.insert("X-Playback-Session-Id", "sess-42");
		network_fetcher f(ctx);

		const fetch_result r = off_thread(f, QUrl(base + "/manifest.txt"), 2048, 8000);
		check(r.reached && r.status == 200, "it comes back reached");
		check(r.content_type == "application/vnd.apple.mpegurl",
		      QString("with the content type (%1)").arg(r.content_type));
		check(r.body.startsWith("#EXTM3U"), "and the body");

		// The whole reason it goes through the page's context (sec 11.3).
		const QByteArray saw = server.last_head.toLower();
		check(saw.contains("referer: https://site.example/watch/1"),
		      "the page's Referer reached the origin");
		check(saw.contains("user-agent: hydra/1.0 helper"), "and its User-Agent");
		check(saw.contains("cookie: sid=abc"), "and its cookies");
		check(saw.contains("x-playback-session-id: sess-42"),
		      "and anything else the context carried");
		check(saw.contains("range: bytes=0-2047"), "asking only for the cap");
	}

	section("the cap holds even when the server ignores it");
	{
		network_fetcher f;
		const fetch_result r = off_thread(f, QUrl(base + "/big"), 1000, 8000);
		check(r.reached, "the oversized body is still fetched");
		check(r.body.size() <= 1000,
		      QString("but truncated to the cap (%1 bytes)").arg(r.body.size()));
	}

	section("a server that never answers does not hang the caller");
	{
		server.stall = true;
		network_fetcher f;
		QElapsedTimer t;
		t.start();
		const fetch_result r = off_thread(f, QUrl(base + "/silent"), 2048, 1200);
		const qint64 waited = t.elapsed();
		server.stall = false;

		check(!r.reached, "it gives up rather than waiting forever");
		check(r.error.contains("timed out"),
		      QString("saying so (%1)").arg(r.error));
		check(waited >= 1000 && waited < 6000,
		      QString("after about the deadline, not before or long after (%1 ms)")
		          .arg(waited));
	}

	section("a blocked caller is the caller's problem, not a hang");
	{
		// Called straight from the thread that owns the origin's event loop,
		// the request cannot be served -- the server is on the blocked thread.
		// It has to end in a bounded timeout rather than a hang, because that
		// is the difference between a slow review dialog and a dead one.
		network_fetcher f;
		QElapsedTimer t;
		t.start();
		const fetch_result r = f.fetch(QUrl(base + "/manifest.txt"), 2048, 900);
		check(!r.reached && t.elapsed() < 5000,
		      QString("it returns rather than hanging (%1 ms)").arg(t.elapsed()));
		check(r.error.contains("timed out"),
		      "with a timeout, which is what a blocked event loop looks like "
		      "from here");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

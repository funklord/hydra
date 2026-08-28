// The content-type tier (architecture doc sec 10, sec 11.1). Two halves: the
// classification, which is pure and where the subtlety is, and one real fetch
// through a server that answers the way the measured site does.
#include "stream_probe.h"
#include "media_remux.h"

#include <QCoreApplication>
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

// A server that answers by path, and remembers what it was asked.
class fake_origin : public QTcpServer {
public:
	QByteArray last_head;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			last_head = head;
			const QByteArray path = head.mid(4, head.indexOf(' ', 4) - 4);

			QByteArray type, body;
			int status = 200;
			if (path.startsWith("/disguised")) {
				// What the measured site does: a master playlist wearing .txt.
				type = "text/plain; charset=utf-8";
				body = "#EXTM3U\n#EXT-X-VERSION:3\n"
				        "#EXT-X-STREAM-INF:BANDWIDTH=800000\nv1/index.txt\n";
			} else if (path.startsWith("/honest")) {
				type = "application/vnd.apple.mpegurl";
				body = "#EXTM3U\n#EXT-X-ENDLIST\n";
			} else if (path.startsWith("/dash")) {
				type = "application/dash+xml";
				body = "<?xml version=\"1.0\"?>\n<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\"/>";
			} else if (path.startsWith("/mp4")) {
				type = "application/octet-stream";
				body = QByteArray("\0\0\0\x18", 4) + "ftypmp42" + QByteArray(16, '\0');
			} else if (path.startsWith("/page")) {
				type = "text/html";
				body = "<!doctype html><html><body>not a stream</body></html>";
			} else if (path.startsWith("/denied")) {
				type = "text/html";
				body = "denied";
				status = 403;
			} else {
				type = "text/plain";
				body = "nothing here";
				status = 404;
			}

			QByteArray resp = "HTTP/1.1 " + QByteArray::number(status) + " OK\r\n";
			resp += "Content-Type: " + type + "\r\n";
			resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
			resp += "Connection: close\r\n\r\n";
			resp += body;
			s->write(resp);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

static probe_result fetch(stream_probe &p, const QUrl &u,
                           const stream_context &ctx = {}) {
	probe_result out;
	QEventLoop loop;
	p.probe(u, ctx, [&](const probe_result &r) { out = r; loop.quit(); });
	QTimer::singleShot(12000, &loop, &QEventLoop::quit);
	loop.exec();
	return out;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("classification, without a network in sight");
	{
		// The case this tier exists for. Believing the header here is exactly
		// the failure it is meant to fix.
		const probe_result d =
		  stream_probe::classify("text/plain", "#EXTM3U\n#EXT-X-VERSION:3\n");
		check(d.kind == "hls", "a playlist served as text/plain is still HLS");
		check(d.disagreed, "and the disagreement is reported, not smoothed over");
		check(d.reason.contains("text/plain"), "the reason names what it claimed");

		check(stream_probe::classify("application/vnd.apple.mpegurl",
		                              "#EXTM3U\n").kind == "hls",
		      "an honest playlist is HLS");
		check(!stream_probe::classify("application/vnd.apple.mpegurl",
		                               "#EXTM3U\n").disagreed,
		      "with nothing to disagree about");

		check(stream_probe::classify("application/dash+xml",
		                              "<?xml version=\"1.0\"?><MPD/>").kind == "dash",
		      "an MPD is DASH");
		check(stream_probe::classify("application/octet-stream",
		                              QByteArray("\0\0\0\x18", 4) + "ftypmp42").kind
		          == "direct",
		      "an ftyp box is a direct file whatever the header says");
		check(stream_probe::classify("video/mp4", "").kind == "direct",
		      "and a video/* header alone is enough when the bytes are silent");

		const probe_result html =
		  stream_probe::classify("text/html", "<!doctype html><html>");
		check(html.kind.isEmpty(), "a web page is not a stream");
		check(html.reason.contains("text/html"), "and says so with its type");

		// A leading BOM is common in served manifests and means nothing.
		check(stream_probe::classify("text/plain",
		                              QByteArray("\xEF\xBB\xBF") + "#EXTM3U\n").kind
		          == "hls",
		      "a BOM does not hide a playlist");
	}

	section("against a server that answers like the real one");
	{
		fake_origin origin;
		if (!origin.listen(QHostAddress::LocalHost, 0)) {
			std::printf("  FAIL  could not listen\n");
			return 1;
		}
		const QString base =
		  QString("http://127.0.0.1:%1").arg(origin.serverPort());
		stream_probe p;

		const probe_result d = fetch(p, QUrl(base + "/disguised.txt?k=abc"));
		check(d.reached && d.kind == "hls",
		      QString("the disguised manifest is identified (%1)").arg(d.reason));
		check(d.disagreed, "and the .txt/text-plain disguise is called out");

		check(fetch(p, QUrl(base + "/honest.m3u8")).kind == "hls",
		      "an honest manifest still works");
		check(fetch(p, QUrl(base + "/dash.mpd")).kind == "dash", "DASH is DASH");
		check(fetch(p, QUrl(base + "/mp4")).kind == "direct",
		      "an mp4 body is direct");
		check(fetch(p, QUrl(base + "/page")).kind.isEmpty(),
		      "an HTML page is refused");

		// A CDN refusing the context says nothing about the content, and must
		// not be reported as "not a stream".
		const probe_result denied = fetch(p, QUrl(base + "/denied"));
		check(denied.reached && denied.status == 403 && denied.kind.isEmpty(),
		      "a 403 is reported as unestablished, not as a verdict");
		check(denied.reason.contains("403"), "with the status in the reason");

		// The whole reason this goes through the page's context.
		stream_context ctx;
		ctx.referer    = "https://site.example/watch/1";
		ctx.user_agent = "Hydra/1.0 probe";
		ctx.cookies    = "sid=abc123";
		ctx.extra.insert("X-Playback-Session-Id", "sess-42");
		fetch(p, QUrl(base + "/honest.m3u8"), ctx);
		// Compared lowercased: header names are case-insensitive by spec and Qt
		// puts them on the wire in lower case, so matching `Referer:` literally
		// tests Qt's spelling rather than whether the header arrived.
		const QByteArray saw = origin.last_head.toLower();
		check(saw.contains("referer: https://site.example/watch/1"),
		      "the page's Referer reaches the origin");
		check(saw.contains("user-agent: hydra/1.0 probe"), "and its User-Agent");
		check(saw.contains("cookie: sid=abc123"), "and its cookies");
		check(saw.contains("x-playback-session-id: sess-42"),
		      "and whatever else the extractor asked for");
		check(saw.contains("range: bytes=0-"),
		      "and only the opening bytes are asked for");

		const probe_result gone = fetch(p, QUrl("http://127.0.0.1:1/nothing"));
		check(!gone.reached && gone.kind.isEmpty(),
		      "an unreachable address reports that, rather than a verdict");
	}

	section("the remux names its output and says what it asks ffmpeg for");
	{
		check(media_remux::target_for("/tmp/clip.ts") == "/tmp/clip.mp4",
		      "a .ts becomes a .mp4");
		check(media_remux::target_for("/tmp/CLIP.TS") == "/tmp/CLIP.mp4",
		      "and the extension is matched whatever its case");
		// The interesting one: an unrecognised name must not be truncated.
		// Trimming three characters off anything would turn "movie.webm" into
		// "movie.w" and write beside a file nobody asked about.
		check(media_remux::target_for("/tmp/movie.webm") == "/tmp/movie.webm.mp4",
		      "a name this does not recognise gains .mp4 rather than losing what it had");
		check(!media_remux::target_for("/tmp/a.ts").endsWith(".ts"),
		      "and the output is never the input, which would truncate it mid-write");

		const QStringList a = media_remux::arguments("/tmp/in.ts", "/tmp/out.mp4");
		check(a.contains("copy") && a.contains("-c"),
		      "it rewraps rather than re-encodes");
		check(a.indexOf("-i") >= 0 && a.at(a.indexOf("-i") + 1) == "/tmp/in.ts",
		      "the input follows -i");
		check(a.last() == "/tmp/out.mp4",
		      "and the output is last, where ffmpeg expects it");
		check(a.contains("-y"),
		      "a stale target from an interrupted run does not stop it");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

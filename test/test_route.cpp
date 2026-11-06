// Do downloads leave by the connection they were told to, and by no other?
//
// A claim about a route can only be read off the far end, so that is where this
// reads it. Two halves:
//
// **On loopback**, anywhere: the SOCKS5 server's door -- password, refusals, a
// device that is not there -- and that a download through it arrives whole.
//
// **Through a real tunnel**, under `test/route_tunnel.sh`: the suite runs itself
// again in a private namespace with a WireGuard device that has no route, beside
// a server that logs the address of everyone who asks. Going by the tunnel and
// going by the ordinary route are then two different addresses in a file. A
// machine that cannot make the namespace says so with a skip line.
#include "download_manager.h"
#include "download_route.h"
#include "hls_assembler.h"
#include "http_download_source.h"
#include "local_proxy.h"
#include "torrent_download_source.h"
#include "ytdlp_resolver.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

// The test Makefile says where; a build that does not runs from `test/`.
#ifndef HYDRA_SRC_DIR
#define HYDRA_SRC_DIR "../src"
#endif

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &what) {
	std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", qUtf8Printable(what));
	std::fflush(stdout);
	(ok ? g_pass : g_fail)++;
}

static const QString tunnel_address   = QStringLiteral("10.9.0.2");
static const QString ordinary_address = QStringLiteral("10.0.0.2");
static const QString far_end          = QStringLiteral("http://198.51.100.1:8080");

template <typename fn>
static bool wait_for(fn done, int ms) {
	QElapsedTimer t;
	t.start();
	while (!done() && t.elapsed() < ms) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
		QThread::msleep(5);
	}
	return done();
}

// One blocking conversation with the server, as a client that is not Qt.
static QByteArray socks_say(QTcpSocket &s, const QByteArray &words, int want) {
	s.write(words);
	QByteArray got;
	QElapsedTimer t;
	t.start();
	while (got.size() < want && t.elapsed() < 20000) {
		if (s.bytesAvailable() || s.waitForReadyRead(200))
			got += s.readAll();
		if (s.state() != QAbstractSocket::ConnectedState && !s.bytesAvailable())
			break;
	}
	return got;
}

static QByteArray password_message(const QString &user, const QString &password) {
	return QByteArray("\x01") + char(user.size()) + user.toLatin1() +
	       char(password.size()) + password.toLatin1();
}

// A server that answers every GET with the same body, and counts them.
class little_server : public QTcpServer {
public:
	int        asked = 0;
	QByteArray body  = QByteArray(300000, 'b');

	little_server() {
		connect(this, &QTcpServer::newConnection, this, [this] {
			while (QTcpSocket *s = nextPendingConnection()) {
				connect(s, &QTcpSocket::readyRead, s, [this, s] {
					if (!s->readAll().contains("\r\n\r\n"))
						return;
					asked++;
					s->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\n"
					         "Connection: close\r\nContent-Length: " +
					         QByteArray::number(body.size()) + "\r\n\r\n" + body);
					s->disconnectFromHost();
				});
				connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
			}
		});
	}
};

struct outcome {
	bool    ended = false;
	bool    ok    = false;
	QString detail;
};

static outcome fetch(http_download_source &source, const QUrl &url, const QString &dir,
                     const QString &name, int id, int ms) {
	outcome out;
	QObject scope;
	QObject::connect(&source, &download_source::finished, &scope,
	                 [&out, id](int job, bool ok, const QString &detail) {
		if (job != id)
			return;
		out.ended  = true;
		out.ok     = ok;
		out.detail = detail;
	});
	download_request req;
	req.id        = id;
	req.url       = url;
	req.directory = dir;
	req.file_name = name;
	QString error;
	if (!source.start(req, &error)) {
		out.ended  = true;
		out.detail = error;
		return out;
	}
	wait_for([&out] { return out.ended; }, ms);
	return out;
}

static void door() {
	std::printf("-- the door\n");
	download_route &route = download_route::shared();
	check(route.supported(), "this build can bind a device");
	check(!route.bound() && route.ready(), "nothing bound: ready, with nothing to do");
	check(route.proxy().type() == QNetworkProxy::DefaultProxy,
	      "nothing bound leaves Qt's own behaviour alone");
	check(route.proxy_url().isEmpty(), "and gives a child process nothing");

	check(download_route::listen_on("tun0", "") == "tun0:6881",
	      "a torrent with no listen setting takes the usual port");
	check(download_route::listen_on("tun0", "0.0.0.0:7000,[::]:7000") == "tun0:7000",
	      "the setting's port is kept and said once");
	check(download_route::listen_on("tun0", "0.0.0.0:7000s") == "tun0:7000s",
	      "and so is its SSL mark");

	little_server web;
	web.listen(QHostAddress::LocalHost, 0);
	route.set_interface_name("lo");
	check(route.ready(), "a device that is up is ready");
	const QNetworkProxy proxy = route.proxy();
	check(proxy.type() == QNetworkProxy::Socks5Proxy &&
	      proxy.hostName() == "127.0.0.1" && proxy.port() != 0,
	      "the proxy is on loopback");
	check(route.proxy_url().startsWith("socks5h://") &&
	      route.proxy_url().contains(proxy.password()),
	      "a child process gets the same way in");

	const QByteArray connect_to =
	  QByteArray("\x05\x01\x00\x01\x7f\x00\x00\x01", 8) +
	  char(web.serverPort() >> 8) + char(web.serverPort() & 0xff);
	{
		QTcpSocket s;
		s.connectToHost("127.0.0.1", proxy.port());
		s.waitForConnected(2000);
		check(socks_say(s, QByteArray("\x05\x01\x00", 3), 2) == QByteArray("\x05\xff", 2),
		      "a client offering no password is turned away");
	}
	{
		QTcpSocket s;
		s.connectToHost("127.0.0.1", proxy.port());
		s.waitForConnected(2000);
		socks_say(s, QByteArray("\x05\x01\x02", 3), 2);
		const QByteArray said = socks_say(s, password_message(proxy.user(), "wrong"), 2);
		check(said.size() == 2 && said[1] != 0, "a wrong password is refused");
	}
	{
		QTcpSocket s;
		s.connectToHost("127.0.0.1", proxy.port());
		s.waitForConnected(2000);
		socks_say(s, QByteArray("\x05\x01\x02", 3), 2);
		const QByteArray said =
		  socks_say(s, password_message(proxy.user(), proxy.password()), 2);
		check(said == QByteArray("\x01\x00", 2), "the right password opens it");
		const QByteArray answer = socks_say(s, connect_to, 10);
		check(answer.size() == 10 && answer[1] == 0, "and it dials what it is asked");
	}

	QTemporaryDir dir;
	http_download_source source;
	const QUrl url(QString("http://127.0.0.1:%1/a.mp4").arg(web.serverPort()));
	const int before = web.asked;
	outcome got = fetch(source, url, dir.path(), "a.mp4", 1, 20000);
	check(got.ok, QString("a download through it finishes (%1)").arg(got.detail));
	QFile saved(dir.filePath("a.mp4"));
	saved.open(QIODevice::ReadOnly);
	check(saved.readAll() == web.body, "and arrives whole");
	check(web.asked == before + 1, "having asked once");

	const QUrl named(QString("http://localhost:%1/b.mp4").arg(web.serverPort()));
	got = fetch(source, named, dir.path(), "b.mp4", 2, 20000);
	check(got.ok, QString("a host name is looked up at this end (%1)").arg(got.detail));

	route.set_interface_name("hydra-none0");
	QString why;
	check(!route.ready(&why) && why.contains("hydra-none0"),
	      "a device that is not there says so by name");
	const int asked = web.asked;
	got = fetch(source, url, dir.path(), "c.mp4", 3, 5000);
	check(got.ended && !got.ok && got.detail.contains("hydra-none0"),
	      "a download refuses to start, and says why");
	{
		// Past the check, as a transfer already queued would be.
		QTcpSocket s;
		s.connectToHost("127.0.0.1", proxy.port());
		s.waitForConnected(2000);
		socks_say(s, QByteArray("\x05\x01\x02", 3), 2);
		socks_say(s, password_message(proxy.user(), proxy.password()), 2);
		const QByteArray answer = socks_say(s, connect_to, 10);
		check(answer.size() >= 2 && answer[1] != 0, "and the proxy will not dial");
	}
	check(web.asked == asked, "so nothing arrived by any other way");

	route.set_interface_name(QString());
	check(route.proxy().type() == QNetworkProxy::DefaultProxy && route.ready(),
	      "choosing no connection puts it all back");
}

static void torrents() {
	if (!torrent_download_source::available())
		return;
	std::printf("-- torrents\n");
	download_route &route = download_route::shared();
	torrent_download_source tor;
	tor.set_listen_interfaces("127.0.0.1:0");
	check(tor.capabilities().participation_note.contains("Your IP address"),
	      "unbound, the note speaks of your own address");
	tor.start_session();
	check(tor.session_maps_ports(), "and the router is asked");
	check(tor.session_listen() == "127.0.0.1:0", "and the setting is what it listens on");

	route.set_interface_name("lo");
	tor.route_changed();
	check(tor.session_listen() == "lo:0" && tor.session_outgoing() == "lo",
	      "a route moves the session onto its device, both ways");
	check(!tor.session_maps_ports(), "and the ordinary connection's router is left alone");
	check(tor.capabilities().participation_note.contains("address of lo"),
	      "and the note says whose address is shown");

	route.set_interface_name("hydra-none0");
	download_request req;
	req.id  = 1;
	req.url = QUrl("magnet:?xt=urn:btih:0000000000000000000000000000000000000000");
	QString refused;
	check(!tor.start(req, &refused) && refused.contains("hydra-none0"),
	      "a torrent will not start with the device gone, and says why");

	route.set_interface_name(QString());
	tor.route_changed();
	check(tor.session_listen() == "127.0.0.1:0" && tor.session_outgoing() == "127.0.0.1",
	      "and leaving it puts the setting back");
	check(tor.session_maps_ports(), "router and all");
}

static QStringList seen() {
	QFile f(qEnvironmentVariable("HYDRA_ROUTE_SEEN"));
	f.open(QIODevice::ReadOnly);
	return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
}

// Who asked for paths containing `what`.
static QStringList who_asked(const QString &what) {
	QStringList out;
	const QStringList lines = seen();
	for (const QString &line : lines)
		if (line.contains(what))
			out << line.section(' ', 0, 0);
	out.removeDuplicates();
	return out;
}

static void through_tunnel() {
	std::printf("-- through a real tunnel\n");
	download_route &route = download_route::shared();
	QTemporaryDir dir;
	http_download_source source;

	outcome got = fetch(source, QUrl(far_end + "/plain.mp4"), dir.path(), "plain.mp4", 1, 20000);
	check(got.ok && who_asked("/plain.mp4") == QStringList{ ordinary_address },
	      QString("unbound, a download takes the ordinary route (%1: %2)")
	        .arg(got.detail, who_asked("/plain.mp4").join(',')));

	route.set_interface_name("wg0");
	QNetworkAccessManager browsing;
	QNetworkReply *page = browsing.get(QNetworkRequest(QUrl(far_end + "/browsing.ts")));
	wait_for([page] { return page->isFinished(); }, 10000);
	check(who_asked("/browsing.ts") == QStringList{ ordinary_address },
	      "with a route set, what is not a download stays on the ordinary route");

	got = fetch(source, QUrl(far_end + "/bound.mp4"), dir.path(), "bound.mp4", 2, 20000);
	check(got.ok && who_asked("/bound.mp4") == QStringList{ tunnel_address },
	      QString("a file goes by the tunnel (%1: %2)")
	        .arg(got.detail, who_asked("/bound.mp4").join(',')));

	hls_assembler stream;
	stream.start(QUrl(far_end + "/list.m3u8"), {}, dir.filePath("stream.ts"));
	wait_for([&stream] { return stream.finished(); }, 20000);
	check(who_asked("/list.m3u8") == QStringList{ tunnel_address } &&
	      who_asked("/seg") == QStringList{ tunnel_address },
	      QString("a stream's playlist and segments go by the tunnel (%1)")
	        .arg(who_asked("/seg").join(',')));

	// Watch: the player asks the local proxy, and the proxy asks upstream.
	local_proxy watch;
	check(watch.start(), "the local proxy starts");
	const QUrl published = watch.publish(QUrl(far_end + "/watched.mp4"), {});
	QNetworkAccessManager player;
	player.setProxy(QNetworkProxy::NoProxy);
	QNetworkReply *played = player.get(QNetworkRequest(published));
	wait_for([played] { return played->isFinished(); }, 15000);
	check(who_asked("/watched.mp4") == QStringList{ tunnel_address },
	      QString("what Watch fetches goes by the tunnel (%1)")
	        .arg(who_asked("/watched.mp4").join(',')));

	if (torrent_download_source::available()) {
		download_manager manager;
		manager.set_directory(dir.path());
		manager.add_source(new torrent_download_source);
		manager.set_consent("torrent", true);
		QString refused;
		manager.enqueue(QUrl(far_end + "/fetched.torrent"), "node", &refused);
		wait_for([] { return !who_asked("/fetched.torrent").isEmpty(); }, 15000);
		check(who_asked("/fetched.torrent") == QStringList{ tunnel_address },
		      QString("a .torrent fetched from an address goes by the tunnel (%1 %2)")
		        .arg(who_asked("/fetched.torrent").join(','), refused));
	}

	ytdlp_resolver lookup;
	if (lookup.available()) {
		bool done = false;
		QObject::connect(&lookup, &ytdlp_resolver::resolved, &lookup,
		                 [&done](const resolved_media &) { done = true; });
		QObject::connect(&lookup, &ytdlp_resolver::failed, &lookup,
		                 [&done](const QString &) { done = true; });
		lookup.resolve(QUrl(far_end + "/looked-up.mp4"));
		wait_for([&done] { return done; }, 40000);
		check(who_asked("/looked-up.mp4") == QStringList{ tunnel_address },
		      QString("yt-dlp asks by the tunnel too (%1)")
		        .arg(who_asked("/looked-up.mp4").join(',')));
	} else {
		std::printf("--   skipped: no yt-dlp here\n");
	}

	// **The tunnel goes down while a file is coming.** It must end soon, by
	// itself, as a failure, and the rest of the file must not come another way.
	outcome slow;
	QObject scope;
	QObject::connect(&source, &download_source::finished, &scope,
	                 [&slow](int job, bool ok, const QString &detail) {
		if (job == 3) {
			slow.ended  = true;
			slow.ok     = ok;
			slow.detail = detail;
		}
	});
	download_request req;
	req.id        = 3;
	req.url       = QUrl(far_end + "/slow.bin");
	req.directory = dir.path();
	req.file_name = "slow.bin";
	QString error;
	check(source.start(req, &error), "a long download starts");
	const QString part = dir.filePath("slow.bin.part");
	wait_for([&part] { return QFile(part).size() > 200000; }, 20000);
	check(QFile(part).size() > 200000, "and is coming in");
	QProcess::execute("ip", { "link", "del", "wg0" });
	QElapsedTimer since;
	since.start();
	wait_for([&slow] { return slow.ended; }, 15000);
	check(slow.ended && !slow.ok,
	      QString("the tunnel gone, it fails after %1 ms: %2")
	        .arg(since.elapsed()).arg(slow.detail));
	check(slow.detail.contains("wg0"), "naming what went");

	got = fetch(source, QUrl(far_end + "/after.mp4"), dir.path(), "after.mp4", 4, 8000);
	check(got.ended && !got.ok, "a new download will not start");
	check(who_asked("/after.mp4").isEmpty() &&
	      who_asked("/slow.bin") == QStringList{ tunnel_address },
	      "and nothing went out by the ordinary route");
}

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	const bool inside = argc > 1 && QByteArray(argv[1]) == "--tunnel";

	if (inside) {
		through_tunnel();
	} else {
		door();
		torrents();

		// The suite again, in its namespaces. Its lines are its own result.
		QProcess tunnel;
		tunnel.setProcessChannelMode(QProcess::ForwardedChannels);
		const QString script = QString::fromUtf8(HYDRA_SRC_DIR "/../test/route_tunnel.sh");
		tunnel.start("unshare", { "-Urn", "--kill-child", "bash", script,
		                          QCoreApplication::applicationFilePath() });
		const bool ran = tunnel.waitForFinished(170000);
		check(ran && tunnel.exitCode() == 0,
		      QString("the tunnel half ran and passed (%1)").arg(tunnel.exitCode()));
	}

	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

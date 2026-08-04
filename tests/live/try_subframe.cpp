// Does the media badge see a player that lives in a third-party iframe?
//
// This is the normal case on real sites, not a corner: the measured site's
// player is an iframe from an unrelated vendor, and §11.6 already says a tap
// confined to the top frame sees nothing. The hook does run on subframes. What
// this asks is the question after that one — whether what it reports can be
// *found* again by the shell.
//
// The hook reports `location.hostname`, which in a subframe is the iframe's
// host. `main_window` looks the tap up by the view's own URL host. On a page
// whose player is a third-party iframe those are two different names, and a
// stream filed under one cannot be found under the other.
//
// Deterministic and offline: a page on 127.0.0.1 embeds an iframe from
// 127.0.0.2 which feeds a MediaSource. Both loopback, different hosts, which is
// the same trick the interceptor and cookie checks use. The bytes appended are
// not real video — the tap counts what a page hands to `appendBuffer`, and
// whether the decoder then likes it is a different subsystem's problem.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "mse_tap.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTreeView>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

static const char *k_player = R"HTML(<!doctype html><html><body>
<video id="v"></video>
<script>
var ms = new MediaSource();
document.getElementById('v').src = URL.createObjectURL(ms);
// Whichever type this build can open a source buffer for.
//
// It used to be H.264 outright, which made this driver a test of the engine's
// codec licensing rather than of the tap: Qt's own 6.11 binaries ship without
// proprietary codecs, so addSourceBuffer threw NotSupportedError, nothing was
// ever appended, and four checks failed as if the subframe relay were broken.
// The bytes below are not decodable as any of these anyway -- the point is the
// handover, not the decode -- so the only thing the type has to do is exist.
var types = ['video/webm; codecs="vp8"',
             'video/webm; codecs="vp9"',
             'audio/webm; codecs="opus"',
             'video/mp4; codecs="avc1.64001E"'];
var type = null;
for (var t = 0; t < types.length; t++)
  if (window.MediaSource && MediaSource.isTypeSupported(types[t])) { type = types[t]; break; }
// warn, not log: Qt's `js` logging category prints warnings and errors and
// drops info, so a console.log here would be invisible in exactly the run where
// it matters.
console.warn(type ? ('mse fixture using ' + type)
                  : 'mse fixture found NO supported type - this engine can open no source buffer');
ms.addEventListener('sourceopen', function () {
  if (!type) return;
  var sb = ms.addSourceBuffer(type);
  // Past the hook's 256 KiB reporting threshold, deliberately: below it the
  // only report is the one `addSourceBuffer` sends, which carries zero bytes
  // and makes a working tap look idle. Four 128 KiB handovers. The buffer will
  // reject these as media and the later ones throw for being out of state —
  // both fine, because the tap counts the handover and not the decode.
  for (var i = 0; i < 4; i++) {
    try { sb.appendBuffer(new Uint8Array(131072)); } catch (e) {}
  }
});
</script></body></html>)HTML";

class origin : public QTcpServer {
public:
	quint16 port = 0;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);

			QByteArray body;
			if (target.startsWith("/player")) {
				body = k_player;
			} else if (target.startsWith("/plain")) {
				body = "<!doctype html><html><body>no iframe here</body></html>";
			} else if (target.startsWith("/topsame")) {
				// Same-origin iframe, to separate "any iframe" from "cross-origin".
				body = "<!doctype html><html><body>top"
				        "<iframe src=\"http://127.0.0.1:" + QByteArray::number(port) +
				        "/player\" width=\"640\" height=\"360\"></iframe>"
				        "</body></html>";
			} else {
				body = "<!doctype html><html><body>top"
				        "<iframe src=\"http://127.0.0.2:" + QByteArray::number(port) +
				        "/player\" width=\"640\" height=\"360\"></iframe>"
				        "</body></html>";
			}
			QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html"
			                   "\r\nCache-Control: no-store\r\nContent-Length: " +
			                   QByteArray::number(body.size()) +
			                   "\r\nConnection: close\r\n\r\n" + body;
			s->write(resp);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-test");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.ini");
	QFile::remove(out + "/policy.json");   // and the file it migrates from
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Capture\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	origin server;
	if (!server.listen(QHostAddress::AnyIPv4, 0)) {
		std::printf("could not listen\n");
		return 1;
	}
	server.port = server.serverPort();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1000, 700);
	w.show();
	spin(1500);

	auto *tap = w.findChild<mse_tap *>();
	check(tap != nullptr, "the shell has an mse_tap");
	if (!tap) return 1;

	auto *tree_view = w.findChild<QTreeView *>();
	emit tree_view->activated(
	  tree_view->model()->index(0, 0, tree_view->model()->index(0, 0)));
	spin(1500);

	QLineEdit *bar = nullptr;
	for (QLineEdit *e : w.findChildren<QLineEdit *>())
		if (e->placeholderText() == "Address") bar = e;
	if (!bar) { std::printf("NO ADDRESS BAR\n"); return 1; }
	auto go = [&](const QString &url, int ms) {
		bar->setText(url);
		QMetaObject::invokeMethod(bar, "returnPressed");
		spin(ms);
	};
	// Wait for a report rather than for a fixed number of seconds. The relay
	// cannot deliver until its QWebChannel has connected, and on a page with an
	// iframe that took longer than the window this driver used to allow -- which
	// read as "the channel never connects" and sent a whole diagnosis down the
	// wrong path. A deadline that is generous and a wait that ends as soon as
	// the answer arrives costs nothing and cannot lie in that direction.
	auto wait_for_report = [&](int max_ms) {
		for (int waited = 0; waited < max_ms && tap->sites().isEmpty(); waited += 250)
			spin(250);
	};

	// The control, and it comes first: the same player page as its own document.
	// Without it, a tap that reports nothing for the iframe case cannot be told
	// apart from a fixture that never fed a MediaSource — and this driver's
	// fixture is the newest, least trustworthy thing in the room.
	go(QString("http://127.0.0.2:%1/player").arg(server.port), 2000);
	wait_for_report(20000);
	std::printf("\n== control: the same player as the top document ==\n");
	for (const QString &s : tap->sites())
		std::printf("  tap site %s (active=%d)\n", qPrintable(s),
		             int(tap->active_for(s)));
	check(tap->active_for("127.0.0.2"),
	      "the fixture really does feed a MediaSource, and the tap sees it");
	for (const QString &s : tap->sites())
		tap->clear_site(s);

	// Which ingredient stops the channel: a third navigation, an iframe at all,
	// or a cross-origin one. Answered in that order, because the cheapest wrong
	// conclusion here is "iframes break it" when it is only some of them.
	go(QString("http://127.0.0.1:%1/plain").arg(server.port), 4000);
	go(QString("http://127.0.0.1:%1/topsame").arg(server.port), 2000);
	wait_for_report(20000);
	std::printf("\n== a player inside a same-origin iframe ==\n  sites = %s\n",
	             tap->sites().isEmpty() ? "(none)"
	                                     : qPrintable(tap->sites().join(", ")));
	// Not a cross-origin question at all: an iframe of any origin has no relay
	// in it, so this failed too and for the same reason.
	check(tap->active_for("127.0.0.1"),
	      "a same-origin iframe is reported under the page's host too");
	for (const QString &s2 : tap->sites())
		tap->clear_site(s2);

	go(QString("http://127.0.0.1:%1/top").arg(server.port), 2000);
	wait_for_report(20000);

	std::printf("\n== a player inside a third-party iframe ==\n");
	for (const QString &s : tap->sites()) {
		std::printf("  tap site %s\n", qPrintable(s));
		for (const mse_stream &st : tap->streams_for(s))
			std::printf("    %s, %lld bytes, %d appends\n", qPrintable(st.mime),
			             qint64(st.bytes), st.appends);
	}
	if (tap->sites().isEmpty())
		std::printf("  (the tap saw nothing)\n");

	// Asserted as it currently *is*, not as it should be, so this run is green
	// today and fails loudly the moment the tap starts working — which is the
	// signal, not a regression. What is known, in order of how far it was
	// chased:
	//
	//   * the main-world hook does run in the cross-origin iframe;
	//   * a subframe *can* hand its report to the top frame —
	//     `window.top.postMessage` was tried and the top frame's relay received
	//     it, confirmed by console instrumentation;
	//   * and it still did not arrive, because on a page that contains a
	//     cross-origin iframe the top frame's relay never connected its
	//     QWebChannel at all. It sat with `bridge == null` and its queue
	//     unflushed. On the same player loaded as its own document the bridge
	//     connects immediately, so this is not the relay script being wrong.
	//
	// That last point is where the next attempt should start, and it is not the
	// `setRunsOnSubFrames` flag or the transport-is-main-frame-only rule — both
	// were tried and reverted rather than left in for no benefit.
	check(!tap->sites().isEmpty(),
	      "a player in a cross-origin iframe is reported at all");
	check(tap->active_for("127.0.0.1"),
	      "and filed under the page's own host, which is what the shell asks for");
	check(!tap->sites().contains("127.0.0.2"),
	      "not under the iframe's, which nothing would ever look up");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

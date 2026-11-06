// What a page can learn about this machine's network through WebRTC, read from
// the page's side through the real shell. Chromium hides local addresses behind
// mDNS names until a page holds capture permission, and the desktop's camera and
// microphone default to allow, so the page opens a fake microphone first: that is
// the case the default has to survive.
//
// **The allowed case is the control.** Without it, a page that gathered nothing
// for any other reason would read as the block working.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTreeView>
#include <QUrlQuery>
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

static const char *k_page = R"HTML(<!doctype html><html><body><script>
function say(what, value) {
  fetch('/report?what=' + what + '&value=' + encodeURIComponent(value));
}
navigator.mediaDevices.getUserMedia({ audio: true }).then(function (s) {
  say('capture', 'stream');
  var pc = new RTCPeerConnection({ iceServers: [] });
  pc.createDataChannel('x');
  s.getTracks().forEach(function (t) { pc.addTrack(t, s); });
  var seen = [];
  pc.onicecandidate = function (e) {
    if (e.candidate) { seen.push(e.candidate.candidate); return; }
    s.getTracks().forEach(function (t) { t.stop(); });
    say('candidates', seen.join('|') || 'none');
  };
  pc.createOffer().then(function (o) { return pc.setLocalDescription(o); });
}).catch(function (e) { say('capture', e.name); });
</script></body></html>)HTML";

class origin : public QTcpServer {
public:
	QHash<QString, QString> reported;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);
			QByteArray body;
			if (target.startsWith("/report")) {
				const QUrlQuery q(QUrl(QString::fromUtf8(target)).query());
				reported.insert(q.queryItemValue("what"),
				                q.queryItemValue("value", QUrl::FullyDecoded));
			} else if (target.startsWith("/page")) {
				body = k_page;
			}
			s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
			         "Cache-Control: no-store\r\nContent-Length: " +
			         QByteArray::number(body.size()) +
			         "\r\nConnection: close\r\n\r\n" + body);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

// An address rather than an mDNS name, in any candidate.
static QStringList addresses_in(const QString &candidates) {
	static const QRegularExpression address(R"( (\d+\.\d+\.\d+\.\d+|[0-9a-f:]*:[0-9a-f:]+) \d+ typ )");
	QStringList out;
	auto it = address.globalMatch(candidates);
	while (it.hasNext())
		out << it.next().captured(1);
	out.removeDuplicates();
	return out;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--use-fake-device-for-media-stream");
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-test");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.ini");
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Capture\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	origin server;
	if (!server.listen(QHostAddress::LocalHost, 0)) return 1;

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1000, 700);
	w.show();
	spin(1500);

	auto *tree_view = w.findChild<QTreeView *>();
	emit tree_view->activated(
	  tree_view->model()->index(0, 0, tree_view->model()->index(0, 0)));
	spin(1500);

	QLineEdit *bar = nullptr;
	for (QLineEdit *e : w.findChildren<QLineEdit *>())
		if (e->placeholderText() == "Address") bar = e;
	if (!bar) { std::printf("NO ADDRESS BAR\n"); return 1; }

	int visit = 0;
	auto load = [&] {
		server.reported.clear();
		bar->setText(QString("http://127.0.0.1:%1/page?v=%2")
		               .arg(server.serverPort()).arg(++visit));
		QMetaObject::invokeMethod(bar, "returnPressed");
		for (int i = 0; i < 100 && !server.reported.contains("candidates"); ++i)
			spin(100);
	};

	std::printf("\n== the default, with capture granted ==\n");
	load();
	check(server.reported.value("capture") == "stream",
	      QString("the page holds a microphone, so nothing hides its addresses "
	              "but this (%1)").arg(server.reported.value("capture")));
	const QStringList blocked = addresses_in(server.reported.value("candidates"));
	check(server.reported.contains("candidates"), "the page finished gathering");
	check(blocked.isEmpty(), QString("and read no address (%1)").arg(blocked.join(' ')));

	std::printf("\n== the site allowed ==\n");
	policy.set_setting("127.0.0.1", policy::feature::webrtc_local, policy::setting::allow);
	load();
	const QStringList allowed = addresses_in(server.reported.value("candidates"));
	check(!allowed.isEmpty(),
	      QString("the addresses appear, so the page above could have read them "
	              "(%1 of them)").arg(allowed.size()));

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

// The feature-permission callbacks: geolocation, camera, microphone and
// notifications. Wired since step 3.5 and never once exercised, which in this
// project is the same sentence as "probably broken".
//
// Driven end to end and observed from the *page's* side. The page asks for each
// permission and reports what it got by fetching `/report?...`, so what is
// measured is what a site would actually experience -- not what our decider
// returned, which is the thing that could be right while the wiring under it is
// not. `main_window` installs the decider, the backend maps Qt's feature enum
// onto ours, and the policy engine answers; a break anywhere in that chain shows
// up here as the wrong answer reaching the page.
//
// Served from `127.0.0.1`, which Chromium counts as a secure context -- without
// that, geolocation and getUserMedia are refused before any permission is asked
// for and the whole run would measure nothing.
//
// **Reading the results needs one distinction.** "Denied" and "granted but
// unavailable" are different outcomes and this machine produces both: with
// geolocation *granted*, Chromium has no location provider configured and
// answers POSITION_UNAVAILABLE (code 2), while a refusal is PERMISSION_DENIED
// (code 1). Likewise getUserMedia says `NotAllowedError` when refused and
// `NotFoundError` when allowed with no device present. So the codes are what is
// checked, never "did it fail".
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTreeView>
#include <QUrlQuery>
#include <cstdio>

// The decider's own answers, captured in process. The page-visible outcome is
// not always enough: geolocation comes back PERMISSION_DENIED on this build
// even when we grant it, because the engine has no location provider and says
// so in the only vocabulary the API gives it. What we control is what was asked
// and what we answered, so that is what is asserted.
static QStringList g_perm_log;
static void collect(QtMsgType, const QMessageLogContext &, const QString &m) {
	if (m.startsWith("permission:")) g_perm_log << m;
	else std::fprintf(stderr, "%s\n", qPrintable(m));
}

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

static const char *k_page = R"HTML(<!doctype html><html><body>
<script>
function say(what, result) {
  fetch('/report?what=' + encodeURIComponent(what) +
        '&result=' + encodeURIComponent(result));
}
navigator.geolocation.getCurrentPosition(
  function () { say('geo', 'position'); },
  function (e) { say('geo', 'error-' + e.code); });
navigator.mediaDevices.getUserMedia({ audio: true })
  .then(function () { say('mic', 'stream'); })
  .catch(function (e) { say('mic', e.name); });
navigator.mediaDevices.getUserMedia({ video: true })
  .then(function () { say('cam', 'stream'); })
  .catch(function (e) { say('cam', e.name); });
Notification.requestPermission().then(function (p) { say('notify', p); });
</script></body></html>)HTML";

// Serves the page and collects what it reported.
class origin : public QTcpServer {
public:
	QHash<QString, QString> reported;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);

			QByteArray type = "text/html", body;
			if (target.startsWith("/report")) {
				const QUrlQuery q(QUrl(QString::fromUtf8(target)).query());
				reported.insert(q.queryItemValue("what"), q.queryItemValue("result"));
				type = "text/plain";
				body = "ok";
			} else if (target.startsWith("/page")) {
				body = k_page;
			} else {
				body = "";
			}

			QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: " + type +
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
	qputenv("HYDRA_PERM_DEBUG", "1");
	qInstallMessageHandler(collect);
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

	// One origin per case, on its own port. Chromium remembers a permission
	// answer per *origin* for the session, and an origin is host **and port** --
	// so re-asking on the same port would be answered from that memory and the
	// page would never reach our decider a second time. Same host throughout, so
	// the per-host policy still applies to all three.
	origin servers[3];
	quint16 ports[3];
	for (int i = 0; i < 3; ++i) {
		if (!servers[i].listen(QHostAddress::LocalHost, 0)) {
			std::printf("could not listen\n");
			return 1;
		}
		ports[i] = servers[i].serverPort();
	}
	std::printf("origins on 127.0.0.1: %u %u %u\n", ports[0], ports[1], ports[2]);

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
	origin *server = &servers[0];
	auto run_case = [&](int which, const char *title) {
		server = &servers[which];
		server->reported.clear();
		g_perm_log.clear();
		bar->setText(QString("http://127.0.0.1:%1/page?v=%2")
		                 .arg(ports[which]).arg(++visit));
		QMetaObject::invokeMethod(bar, "returnPressed");
		spin(5000);
		std::printf("\n== %s ==\n", title);
		// What the decider would answer, printed beside what the page saw. The
		// two disagreeing is the whole diagnosis: policy saying allow while the
		// page still sees a refusal means the answer never reached the engine.
		std::printf("  policy geo=%d cam=%d mic=%d notify=%d\n",
		             int(policy.is_allowed(policy::feature::geolocation, "127.0.0.1")),
		             int(policy.is_allowed(policy::feature::camera, "127.0.0.1")),
		             int(policy.is_allowed(policy::feature::microphone, "127.0.0.1")),
		             int(policy.is_allowed(policy::feature::notifications, "127.0.0.1")));
		for (auto it = server->reported.cbegin(); it != server->reported.cend(); ++it)
			std::printf("  page  %s -> %s\n", qPrintable(it.key()),
			             qPrintable(it.value()));
		if (server->reported.isEmpty())
			std::printf("  page  (reported nothing — did it load?)\n");
		for (const QString &l : std::as_const(g_perm_log))
			std::printf("  ours  %s\n", qPrintable(l));
	};
	auto got = [&](const char *what) { return server->reported.value(what); };
	// Our own feature names, as the debug line prints them. It used to print
	// Qt's raw enum number, and this matched on that -- until 6.8 introduced
	// QWebEnginePermission with a differently ordered enum, at which point every
	// number here silently meant a different permission. Names do not renumber.
	auto answered = [&](const char *feature, const char *verdict) {
		const QString want = QString("asked for %1 -> %2").arg(feature).arg(verdict);
		for (const QString &l : std::as_const(g_perm_log))
			if (l.contains(want)) return true;
		return false;
	};

	// "Refused", as the page can tell.
	//
	// Chromium says `NotAllowedError` when a permission is denied and
	// `AbortError` when it cannot open the device at all -- and which of the two
	// getUserMedia reports for a denied camera changed between the Chromium in
	// Qt 6.8.2 and the one in 6.11: same machine, same decision, same run,
	// different word. Neither is a grant, and the thing under test is that the
	// page did not get the device. Matching one spelling of a refusal made a Qt
	// upgrade look like a permissions regression.
	auto refused = [&](const char *what) {
		const QString v = got(what);
		return v == "NotAllowedError" || v == "AbortError";
	};

	// 1. The sec 7.2 defaults: every one of these is block.
	run_case(0, "defaults: geo, camera, microphone, notifications all blocked");
	check(answered("notifications", "denied") && answered("geolocation", "denied") &&
	       answered("microphone", "denied") && answered("camera", "denied"),
	      "all four are refused by our decider");
	check(got("geo") == "error-1",
	      "geolocation is refused (PERMISSION_DENIED, not merely unavailable)");
	check(refused("mic"), QString("the microphone is refused (%1)").arg(got("mic")));
	check(refused("cam"), QString("the camera is refused (%1)").arg(got("cam")));
	check(got("notify") == "denied", "notifications are refused");

	// 2. Grant geolocation for this host alone. This is the row that isolates
	//    the callback: nothing else changes, and a refusal that turns into
	//    "granted, no provider" can only have come through our decider.
	policy.set_setting("127.0.0.1", policy::feature::geolocation,
	                    policy::setting::allow);
	run_case(1, "geolocation allowed for this host");
	check(answered("geolocation", "GRANTED"), "geolocation is now granted by our decider");
	check(answered("microphone", "denied") && answered("camera", "denied"),
	      "while microphone and camera are still refused, so the grant is "
	      "per-feature and the enum mapping is right");
	check(refused("mic"),
	      "and the page still cannot open a microphone");
	// Deliberately not asserted: what the page sees for geolocation. This build
	// has no location provider, so a granted request still ends as
	// PERMISSION_DENIED -- the same code a refusal produces. Asserting on it
	// would be asserting on the engine's build options.
	std::printf("  --    page saw geo '%s' (engine has no location provider "
	             "here, so this cannot distinguish grant from refusal)\n",
	             qPrintable(got("geo")));

	// 3. And the notifications path separately, since it is the one feature
	//    here that answers with a word rather than an exception.
	policy.set_setting("127.0.0.1", policy::feature::notifications,
	                    policy::setting::allow);
	run_case(2, "notifications allowed for this host");
	check(answered("notifications", "GRANTED"), "notifications are granted by our decider");
	check(got("notify") == "granted",
	      "and the page really is told granted — the one feature here whose "
	      "end-to-end outcome the engine can actually deliver");
	check(refused("cam"),
	      "while the camera is still refused");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

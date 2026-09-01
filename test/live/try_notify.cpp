// Web notifications, from `new Notification(...)` to the bus and back.
//
// **The presenter this drives was written to close a gap the permission audit
// found**, and the gap is the reason this driver exists rather than a claim in a
// commit message: Chromium treats a missing notification presenter as success.
// The page's notification resolves, and nothing appears. So an untested
// presenter fails in exactly the shape it was written to prevent -- a wrong
// argument in the `Notify` call leaves `reply.isValid()` false, the page is told
// the notification closed, and notifications silently never work again.
//
// The service is a stub, exported on whatever session bus the driver is given.
// That is the point: it is the *arguments* and the round trip being tested, not
// anybody's notification daemon. Run it under `dbus-run-session` to give it a
// private bus, which is also what stops it registering a name a real daemon on
// a developer's desktop already owns.
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "qtwebengine_notifications.h"
#include "request_filter.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QLineEdit>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTreeView>
#include <QUrlQuery>
#include <QVariantMap>
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

static const char *k_page = R"HTML(<!doctype html><html><body>
<script>
function say(what, result) {
  fetch('/report?what=' + encodeURIComponent(what) +
        '&result=' + encodeURIComponent(result));
}
Notification.requestPermission().then(function (p) {
  say('permission', p);
  if (p !== 'granted') return;
  var n = new Notification('Hydra test', { body: 'the body' });
  n.onshow  = function () { say('onshow',  'yes'); };
  n.onclick = function () { say('onclick', 'yes'); };
  n.onclose = function () { say('onclose', 'yes'); };

  // A second one the *page* takes back, which is the other direction and the
  // one nothing was carrying: a chat marking a message read, a countdown that
  // has finished. Without it reaching the service the notification stays on
  // the desktop after the page has withdrawn it.
  var m = new Notification('Withdrawn', { body: 'closed by the page' });
  m.onshow = function () { say('b-onshow', 'yes'); setTimeout(function () {
    m.close();
  }, 800); };
});
</script></body></html>)HTML";

// Serves the page and collects what it reported. The same shape `try_permissions`
// uses, and for the same reason: the page has to say what happened from inside
// itself, because everything interesting here is asynchronous.
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
			}
			const QByteArray resp =
			  "HTTP/1.1 200 OK\r\nContent-Type: " + type +
			  "\r\nCache-Control: no-store\r\nContent-Length: " +
			  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
			s->write(resp);
			s->flush();
			s->disconnectFromHost();
			s->deleteLater();
		});
	}
};

// A notification service that records rather than displays.
//
// Public slots plus `ExportAllSlots`, which is the least machinery that gives a
// real D-Bus object: no adaptor, no generated XML, and the signatures below are
// the freedesktop ones, so a mismatch between what the presenter sends and what
// the specification says shows up here as a failed call rather than as a
// notification nobody sees.
class fake_notifier : public QObject {
	Q_OBJECT
	// **Without this the object is exported under `local.fake_notifier`**, which
	// is what a class name gets you by default, and the presenter's call comes
	// back `UnknownInterface`. That is not a detail of the stub: the interface
	// name is half of what a D-Bus method call names, so a test that got it
	// wrong would be testing that the presenter fails politely. It said so
	// precisely -- "No such interface 'org.freedesktop.Notifications' at object
	// path '/org/freedesktop/Notifications'" -- which is the whole reason the
	// presenter prints the error rather than only closing the notification.
	Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
	int         notify_count = 0;
	int         close_count = 0;
	uint        first_id = 0, last_id = 0, last_closed = 0;
	QString     app_name, app_icon, summary, body;
	QStringList actions;
	QVariantMap hints;
	int         timeout = 0;

public slots:
	void GetServerInformation(QString &name, QString &vendor, QString &version,
	                           QString &spec_version) {
		name = QStringLiteral("hydra-test-stub");
		vendor = QStringLiteral("hydra");
		version = QStringLiteral("1");
		spec_version = QStringLiteral("1.2");
	}

	uint Notify(const QString &a_app_name, uint replaces_id,
	             const QString &a_app_icon, const QString &a_summary,
	             const QString &a_body, const QStringList &a_actions,
	             const QVariantMap &a_hints, int a_timeout) {
		Q_UNUSED(replaces_id)
		// **Distinct ids, because two notifications now exist and which of them
		// gets closed is the whole point of the second.** A fixed id made the
		// close assertion incapable of failing: any withdrawal at all matched.
		last_id = uint(4242 + ++notify_count);
		if (notify_count > 1)
			return last_id;   // only the first one's fields are examined below

		first_id = last_id;
		app_name = a_app_name;
		app_icon = a_app_icon;
		summary  = a_summary;
		body     = a_body;
		actions  = a_actions;
		hints    = a_hints;
		timeout  = a_timeout;
		return last_id;
	}

	void CloseNotification(uint id) {
		++close_count;
		last_closed = id;
	}
};

// The two signals a real service broadcasts. Sent by hand so the driver decides
// when a click and a dismissal happen, rather than waiting on somebody's mouse.
static void emit_signal(const char *name, uint id, uint arg_or_reason,
                         const char *action = nullptr) {
	QDBusMessage sig = QDBusMessage::createSignal(
	  QStringLiteral("/org/freedesktop/Notifications"),
	  QStringLiteral("org.freedesktop.Notifications"),
	  QString::fromLatin1(name));
	if (action)
		sig << id << QString::fromLatin1(action);
	else
		sig << id << arg_or_reason;
	QDBusConnection::sessionBus().send(sig);
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	// **Skipped rather than failed where there is no bus**, which is the state
	// of a headless build machine and of this checkout's own sweep unless it is
	// wrapped. A driver that fails for want of a session bus teaches whoever
	// reads the sweep to ignore it.
	QDBusConnection bus = QDBusConnection::sessionBus();
	if (!bus.isConnected()) {
		std::printf("no session bus; run this under dbus-run-session\n");
		std::printf("\n0 passed, 0 failed\n");
		return 0;
	}

	fake_notifier fake;
	bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &fake,
	                    QDBusConnection::ExportAllSlots);
	if (!bus.registerService(QStringLiteral("org.freedesktop.Notifications"))) {
		// Something already owns the name -- a real daemon, on a real desktop.
		// Taking it away from it would be rude and would also test the wrong
		// service, so this stops instead.
		std::printf("org.freedesktop.Notifications is already owned here; "
		             "run under dbus-run-session for a private bus\n");
		std::printf("\n0 passed, 0 failed\n");
		return 0;
	}

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-test");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.ini");
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Notify\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	origin server;
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		std::printf("could not listen\n");
		return 1;
	}

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);

	std::printf("\n== the presenter installs against a live service ==\n");
	auto *presenter = qtwebengine_notifications::install(factory.profile());
	check(presenter != nullptr,
	      "install() finds the service and takes the profile's presenter slot");
	if (!presenter) {
		std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
		return 1;
	}

	// Allowed for this host: the prompt is `try_permissions`' subject, and what
	// is under test here is what happens *after* a grant.
	policy.set_setting("127.0.0.1", policy::feature::notifications,
	                    policy::setting::allow);

	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(900, 600);
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

	bar->setText(QString("http://127.0.0.1:%1/page").arg(server.serverPort()));
	QMetaObject::invokeMethod(bar, "returnPressed");
	spin(6000);

	auto got = [&](const char *what) { return server.reported.value(what); };

	std::printf("\n== a page posts a notification and it reaches the bus ==\n");
	check(got("permission") == "granted",
	      QString("the page is granted permission (%1)").arg(got("permission")));
	check(fake.notify_count == 2,
	      QString("the service received both Notify calls (%1)")
	        .arg(fake.notify_count));
	check(fake.summary == "Hydra test",
	      QString("with the page's title as the summary (%1)").arg(fake.summary));
	check(fake.body == "the body",
	      QString("and the page's body as the body (%1)").arg(fake.body));
	check(fake.app_name == "Hydra",
	      QString("attributed to this application (%1)").arg(fake.app_name));
	check(fake.hints.value("desktop-entry").toString() == "hydra",
	      "and carrying the desktop-entry hint, so a server can group it");
	check(fake.actions.contains("default"),
	      "with the 'default' action, which is what a web notification means "
	      "by being clicked");
	// **Told after the service took it, not before.** Reporting `show` on a
	// call that then failed would be the one lie that matters here: the page
	// would believe its notification was on screen when nothing was.
	check(got("onshow") == "yes", "and the page is told it was shown");

	std::printf("\n== a page withdrawing its own notification reaches the service ==\n");
	check(got("b-onshow") == "yes", "the second notification was shown too");
	check(fake.close_count == 1,
	      QString("the service was told to close exactly one (%1)")
	        .arg(fake.close_count));
	check(fake.last_closed == uint(4242 + 2),
	      QString("and it is the one the page withdrew, not the one it kept "
	               "(closed %1, kept %2)").arg(fake.last_closed).arg(fake.first_id));

	std::printf("\n== and the service's answers reach the page ==\n");
	emit_signal("ActionInvoked", fake.first_id, 0, "default");
	spin(1200);
	check(got("onclick") == "yes",
	      "clicking the notification reaches the page as onclick");

	emit_signal("NotificationClosed", fake.first_id, 2);
	spin(1200);
	check(got("onclose") == "yes",
	      "and dismissing it reaches the page as onclose");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

#include "try_notify.moc"

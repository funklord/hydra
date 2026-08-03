// The key: the affordance §13.2 asks for, driven through the real shell.
//
// Nothing here needs KeePassXC, and that is the point of choosing this case.
// **Autofill is HTTPS-only by default**, so a login form served over plain HTTP
// is refused for a reason the shell knows on its own -- which makes the whole
// chain observable without a vault, a socket or a human confirming a pairing
// dialog: the page's script finds a password field, asks, the gate refuses, and
// the key has to appear carrying the reason.
//
// That last step is why this is a live driver rather than more assertions in
// `test_autofill`. The controller's decisions are covered there. What is not
// coverable there is whether a `QAction` ever becomes visible on a real toolbar
// and whether its tooltip says anything -- and "a review UI that is correct and
// never clicked" is the defect this project has shipped most often.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <functional>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void note(const QString &w) { std::printf("  --    %s\n", qPrintable(w)); }
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// Waits for the thing to happen rather than for a number of milliseconds. A
// fixed wait is an instrument that invents results -- this file's own project
// notes say so, having paid for it twice.
static bool wait_for(const std::function<bool()> &done, int max_ms = 8000) {
	QElapsedTimer t;
	t.start();
	while (!done() && t.elapsed() < max_ms)
		spin(25);
	return done();
}

// A login form, over plain HTTP on loopback.
class origin : public QTcpServer {
public:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [s] {
			s->readAll();
			const QByteArray body =
				"<!doctype html><html><body><h1>Sign in</h1>"
				"<form><input type=text name=user autocomplete=username>"
				"<input type=password name=pass autocomplete=current-password>"
				"<button type=submit>Go</button></form></body></html>";
			const QByteArray resp =
				"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
				"Content-Length: " + QByteArray::number(body.size()) +
				"\r\nConnection: close\r\n\r\n" + body;
			s->write(resp);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

// **By its placeholder, not by being the first one.** The sidebar has a search
// box, so `findChild<QLineEdit*>()` returns whichever Qt happens to hand back --
// and the first version of this driver navigated by typing a url into the search
// field, which filters the tree and loads nothing. Every check after it failed,
// blaming the feature.
static QLineEdit *address_bar(QWidget *w) {
	for (QLineEdit *e : w->findChildren<QLineEdit *>())
		if (e->placeholderText() == "Address")
			return e;
	return nullptr;
}

// The toolbar action by the text it carries. By name would be better and there
// is no object name to use; by position would break the moment another action
// is added, which is exactly how a harness in this project once reported the
// wrong pane for weeks.
static QAction *key_action(QWidget *w) {
	for (QToolBar *bar : w->findChildren<QToolBar *>())
		for (QAction *a : bar->actions())
			if (a->text().startsWith("Key"))
				return a;
	return nullptr;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-autofill");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.ini");
	QFile::remove(out + "/policy.json");
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Capture\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	origin server;
	if (!server.listen(QHostAddress::LocalHost, 0)) return 1;
	const quint16 port = server.serverPort();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(900, 600);
	w.show();
	spin(1500);

	// A node has to be opened before there is a view to navigate: the shell
	// starts on a placeholder, and an address typed with nothing open goes
	// nowhere. The first version of this driver skipped it and every navigation
	// silently did nothing.
	auto *tree_view = w.findChild<QTreeView *>();
	if (!tree_view) { std::printf("NO TREE\n"); return 1; }
	emit tree_view->activated(
		tree_view->model()->index(0, 0, tree_view->model()->index(0, 0)));
	spin(1500);

	section("before any page has a login form");
	QAction *key = key_action(&w);
	check(key != nullptr, "the key exists on the toolbar");
	if (!key) {
		std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
		return 1;
	}
	check(!key->isVisible(),
	      "and is hidden, because nothing has asked for a credential yet");

	section("a login form raises it, even though the fill is refused");
	{
		QLineEdit *addr = address_bar(&w);
		check(addr != nullptr, "the address bar is reachable");
		if (addr) {
			addr->setText(QString("http://127.0.0.1:%1/login").arg(port));
			QMetaObject::invokeMethod(addr, "returnPressed");
		}
		const bool shown = wait_for([&] { return key->isVisible(); });
		check(shown, "the key appears once the page's script finds a password field");

		// The reason, which is the whole reason the affordance exists. Over
		// plain HTTP the answer is knowable without a vault: filling a password
		// here would put it on the wire.
		check(shown && key->toolTip().contains("HTTPS"),
		      QString("and its tooltip says why it was refused (%1)")
		          .arg(key->toolTip()));
		check(shown && key->text().contains("✕"),
		      QString("and it reads as a refusal rather than a success (%1)")
		          .arg(key->text()));
	}

	section("clicking it asks again, and gets the same honest answer");
	{
		// Clicked, not merely admired. The action is wired to re-run the whole
		// gate rather than to re-open a cached answer, so the same refusal is
		// the correct outcome -- and a click that did nothing at all would be
		// indistinguishable from one that silently succeeded.
		key->setToolTip("cleared, so the next tooltip is this run's");
		key->trigger();
		const bool answered =
			wait_for([&] { return key->toolTip().contains("HTTPS"); });
		check(answered, "the click produces a fresh answer rather than nothing");
	}

	section("navigating away puts it down");
	{
		QLineEdit *addr = address_bar(&w);
		if (addr) {
			addr->setText("about:blank");
			QMetaObject::invokeMethod(addr, "returnPressed");
		}
		const bool hidden = wait_for([&] { return !key->isVisible(); });
		check(hidden,
		      "a page with no login form does not inherit the last page's key");
	}

	note("KeePassXC is not needed for any of the above, and is not exercised by");
	note("it: what a paired vault does with a real reply is try_keepass's job.");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

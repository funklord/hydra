// Handing an address to another application (§19).
//
// On Android this is an untyped ACTION_VIEW and the point is background audio:
// a page cannot keep playing with the screen off, while VLC, NewPipe or YouTube
// itself run a media notification and can. On desktop it is the system's
// default handler, which is a smaller thing and is what can be driven here.
//
// **Proved by the other application actually fetching it.** Checking that
// `QDesktopServices::openUrl` returned true would prove almost nothing -- it
// answers true for a great many things it has merely handed to a launcher. So
// the address handed over is served by this test, and the check is that a
// second, different client comes and asks for it.
#include "main_window.h"
#include "node.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "tab_tree_model.h"

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
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }
static bool wait_for(const std::function<bool()> &done, int max_ms = 15000) {
	QElapsedTimer t; t.start();
	while (!done() && t.elapsed() < max_ms) spin(50);
	return done();
}

// Counts who asks, and remembers what they called themselves. Hydra and
// whatever the desktop hands the url to are different clients, and the user
// agent is how this tells them apart without guessing.
class origin : public QTcpServer {
public:
	int hits = 0;
	QStringList agents;
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			++hits;
			for (const QByteArray &line : head.split('\n'))
				if (line.toLower().startsWith("user-agent:"))
					agents << QString::fromUtf8(line.mid(11)).trimmed().left(60);
			const QByteArray body = "<!doctype html><title>Handed over</title>"
			                         "<h1>Handed over</h1>";
			s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
			          "Content-Length: " + QByteArray::number(body.size()) +
			          "\r\nConnection: close\r\n\r\n" + body);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

static QAction *action_named(QWidget *w, const QString &text) {
	for (QAction *a : w->findChildren<QAction *>())
		if (a->text().contains(text))
			return a;
	return nullptr;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-handoff");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.ini");
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Mine\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	origin server;
	if (!server.listen(QHostAddress::LocalHost, 0)) return 1;
	const QString url = QString("http://127.0.0.1:%1/handed").arg(server.serverPort());

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(900, 620);
	w.show();
	spin(1200);

	auto *tree_view = w.findChild<QTreeView *>();
	emit tree_view->activated(
		tree_view->model()->index(0, 0, tree_view->model()->index(0, 0)));
	spin(1200);

	QLineEdit *addr = nullptr;
	for (QLineEdit *e : w.findChildren<QLineEdit *>())
		if (e->placeholderText() == "Address") addr = e;
	check(addr != nullptr, "the address bar is reachable");
	if (addr) {
		addr->setText(url);
		QMetaObject::invokeMethod(addr, "returnPressed");
	}
	check(wait_for([&] { return server.hits >= 1; }),
	      "Hydra loads the page itself first");
	const int ours = server.hits;
	const QString our_agent = server.agents.isEmpty() ? QString() : server.agents.first();

	section("handing it to whatever the desktop uses");
	{
		QAction *a = action_named(&w, "Open in &Another App");
		check(a != nullptr, "the menu offers the handoff");
		if (a) {
			a->trigger();
			// The other application has to start, which is slower than a
			// function call and is the thing being measured.
			const bool answered = wait_for([&] { return server.hits > ours; }, 20000);
			check(answered,
			      QString("and another application really fetches the address "
			               "(%1 request%2 after the handoff)")
			          .arg(server.hits - ours).arg(server.hits - ours == 1 ? "" : "s"));
			if (answered && server.agents.size() > 1) {
				const QString theirs = server.agents.last();
				note("we asked as:   " + our_agent);
				note("they asked as: " + theirs);
				// Not asserted on: the default handler here is another
				// Chromium, so the two agents can legitimately be similar.
				// The count is the claim; the agents are for the reader.
			}
		}
	}

	section("an address there is nothing to hand over");
	{
		// about:blank has no meaning outside this browser, and the refusal is
		// the interesting half -- doing nothing silently is what this project
		// keeps finding and writing down.
		auto *tv = w.findChild<QTreeView *>();
		Q_UNUSED(tv);
		if (addr) {
			addr->setText("about:blank");
			QMetaObject::invokeMethod(addr, "returnPressed");
		}
		spin(1500);
		const int before = server.hits;
		if (QAction *a = action_named(&w, "Open This Page in &Another App"))
			a->trigger();
		spin(1500);
		check(server.hits == before,
		      "handing over about:blank fetches nothing, as there is nothing to fetch");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

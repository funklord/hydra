// A page that will not run with ads blocked, fixed automatically.
//
// The measured case this comes from: a player whose play button stayed put,
// every click answered by the ad network, and a `fuckadblock.min.js` in the
// request log. Reporting it was tried first and is worse than it sounds -- the
// message is easy to miss and the page stays broken, which reads as a broken
// browser rather than as a blocked one.
//
// So the site's ads setting is relaxed, the page is reloaded, and it is said out
// loud. What this drives at is the three things that keep that from being
// presumptuous, because they are the whole difference between a helpful browser
// and one that quietly stops protecting you:
//
//   * a choice the user already made for that site is never overridden;
//   * the change is an ordinary per-site rule the shield shows and can revert;
//   * it happens once per site per session, so a page that keeps asking cannot
//     put the browser in a reload loop.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "antiadblock_watch.h"

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
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// Serves a page that pulls in a blocker detector, and counts how many times it
// has been asked for -- a reload is the observable half of the fix.
class origin : public QTcpServer {
public:
	int page_loads = 0;
	int detector_loads = 0;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);

			QByteArray type = "text/html", body;
			if (target.contains("fuckadblock")) {
				++detector_loads;
				type = "application/javascript";
				body = "/* pretend detector */";
			} else {
				++page_loads;
				body = "<!doctype html><html><body><p>watch page</p>"
				        "<script src=\"/js/fuckadblock.min.js\"></script>"
				        "</body></html>";
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
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-test");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
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

	auto *tree_view = w.findChild<QTreeView *>();
	emit tree_view->activated(
		tree_view->model()->index(0, 0, tree_view->model()->index(0, 0)));
	spin(1500);

	QLineEdit *bar = nullptr;
	for (QLineEdit *e : w.findChildren<QLineEdit *>())
		if (e->placeholderText() == "Address") bar = e;
	if (!bar) { std::printf("NO ADDRESS BAR\n"); return 1; }
	auto go = [&](int ms) {
		bar->setText(QString("http://127.0.0.1:%1/watch").arg(port));
		QMetaObject::invokeMethod(bar, "returnPressed");
		spin(ms);
	};

	const QString host = "127.0.0.1";

	section("a page that checks for a blocker is fixed, not just reported");
	check(policy.setting_for(host, policy::feature::ads) == policy::setting::unset,
	      "no rule for this site to begin with");
	check(!policy.is_allowed(policy::feature::ads, host),
	      "and ads are blocked by the global default");

	go(6000);
	check(policy.setting_for(host, policy::feature::ads) == policy::setting::allow,
	      "after the detector is seen, ads are allowed for this site");
	check(policy.global_default(policy::feature::ads) == policy::setting::block,
	      "as a per-site rule — the global default is untouched, so this does "
	      "not quietly stop blocking everywhere");
	check(server.page_loads >= 2,
	      QString("and the page is loaded again so the fix is visible (%1 loads)")
	          .arg(server.page_loads));

	section("it does not keep doing it");
	{
		const int before = server.page_loads;
		go(5000);
		// One more load because we asked for one; what must not happen is the
		// browser reloading on its own after that.
		check(server.page_loads <= before + 2,
		      QString("a second visit does not start a reload loop (%1 → %2)")
		          .arg(before).arg(server.page_loads));
	}

	section("a choice the user made is not overridden");
	{
		policy_engine       p2;
		request_filter      f2(&p2);
		qtwebengine_factory fac2(&f2);
		// The user has said: block ads here, even though it may break.
		p2.set_setting(host, policy::feature::ads, policy::setting::block);

		main_window w2(&fac2, &p2, &f2);
		w2.load_tree(tree);
		w2.resize(900, 600);
		w2.show();
		spin(1200);
		auto *tv2 = w2.findChild<QTreeView *>();
		emit tv2->activated(tv2->model()->index(0, 0, tv2->model()->index(0, 0)));
		spin(1200);
		for (QLineEdit *e : w2.findChildren<QLineEdit *>())
			if (e->placeholderText() == "Address") {
				e->setText(QString("http://127.0.0.1:%1/watch").arg(port));
				QMetaObject::invokeMethod(e, "returnPressed");
			}
		spin(6000);
		check(p2.setting_for(host, policy::feature::ads) == policy::setting::block,
		      "someone who blocked ads here on purpose is not second-guessed by "
		      "the thing that noticed it broke");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

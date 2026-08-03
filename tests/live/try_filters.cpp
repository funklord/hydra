// Do accepted filter rules actually stop a request?
//
// For the whole life of the filter-evolution loop they did not. Rules were
// proposed, dry-run, accepted, written to `filters-ai.txt` and listed in the
// settings dialog, and nothing ever asked them about a request:
// `filter_list::blocks()` had no caller in the request path. Everything about
// the loop looked like it worked, because everything except the last step did.
//
// This is the last step, driven end to end through the real interceptor.
//
// **No DNS and no root**, which is what kept this untested. Pointing a name like
// `doubleclick.net` at a local server needs `/etc/hosts` or Chromium's
// `--host-resolver-rules`, and Qt's `QTWEBENGINE_CHROMIUM_FLAGS` mangles the
// latter by splitting on spaces. But the predicate does not care what the name
// is: a page on `127.0.0.1` fetching from `127.0.0.2` is two different hosts,
// both loopback, and a rule can name the second one. That is the same trick
// `try_cookies` and `try_subframe` already use.
//
// Every case uses a **fresh url**, because a cached image would go unrequested
// for reasons that have nothing to do with filtering and look identical in the
// log.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include <QStyleHints>
#include "theme.h"
#include "settings_dialog.h"
#include "qtwebengine_factory.h"

#include <QWebEngineView>

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

// Serves the page on either loopback address and records what it was asked for.
class origin : public QTcpServer {
public:
	quint16     port = 0;
	QStringList asked;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);
			asked << QString::fromUtf8(target);

			QByteArray body, type = "text/html";
			if (target.startsWith("/beacon")) {
				// A 1x1 GIF, so the engine treats it as an image and the request
				// is a real subresource load rather than a failed one.
				static const unsigned char gif[] = {
					0x47,0x49,0x46,0x38,0x39,0x61,0x01,0x00,0x01,0x00,0x80,0x00,
					0x00,0x00,0x00,0x00,0xff,0xff,0xff,0x21,0xf9,0x04,0x01,0x00,
					0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,
					0x00,0x02,0x02,0x44,0x01,0x00,0x3b };
				body = QByteArray(reinterpret_cast<const char *>(gif), sizeof(gif));
				type = "image/gif";
			} else if (target.startsWith("/scheme")) {
				// Reports what the page thinks the browser's colour scheme is.
				body = "<!doctype html><html><body><script>"
				        "new Image().src='/report?scheme='+"
				        "(matchMedia('(prefers-color-scheme: dark)').matches"
				        "?'dark':'light');"
				        "</script></body></html>";
			} else if (target.startsWith("/cosmetic")) {
				body = "<!doctype html><html><body>"
				        "<div class=\"ad-banner\">ADVERT</div>"
				        "<div id=\"keep\">content</div>"
				        "</body></html>";
			} else {
				// The page pulls its beacon from the *other* loopback host, so
				// the request host and the site host differ by name.
				const QByteArray tag = target.mid(target.lastIndexOf('/') + 1);
				body = "<!doctype html><html><body>page"
				        "<img src=\"http://127.0.0.2:" + QByteArray::number(port) +
				        "/beacon" + tag + ".gif\">"
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
	QFile::remove(out + "/policy.ini");
	QFile::remove(out + "/policy.json");   // and the file it migrates from

	// The rules, written where the shell will find them — through the real file
	// it loads at startup, not injected past it. A rule that only works when a
	// test hands it over directly would not be the thing under test.
	{
		QFile f(out + "/filters-ai.txt");
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
		f.write("! written by try_filters\n"
		         "||127.0.0.2^\n"
		         "other.example##.nothing\n");
	}

	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Filters\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	origin server;
	if (!server.listen(QHostAddress::AnyIPv4, 0)) {
		std::printf("could not listen\n");
		return 1;
	}
	server.port = server.serverPort();

	// What main() does, in the order it does it: the engine reads this flag once,
	// at startup, so it has to be set before anything creates a profile.
	const bool want_dark = qEnvironmentVariableIntValue("HYDRA_TEST_DARK") == 1;
	theme::apply(want_dark ? theme::choice::dark : theme::choice::light);
	theme::set_web_engine_scheme(want_dark ? Qt::ColorScheme::Dark
	                                       : Qt::ColorScheme::Light);

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
	spin(1000);

	QLineEdit *bar = nullptr;
	for (QLineEdit *e : w.findChildren<QLineEdit *>())
		if (e->placeholderText() == "Address") bar = e;
	if (!bar) { std::printf("NO ADDRESS BAR\n"); return 1; }

	// Load a page whose beacon url is unique to this case, then say whether the
	// beacon reached the server.
	auto beacon_arrived = [&](const QString &tag) {
		server.asked.clear();
		bar->setText(QString("http://127.0.0.1:%1/page%2").arg(server.port).arg(tag));
		QMetaObject::invokeMethod(bar, "returnPressed");
		spin(2500);
		bool page = false, beacon = false;
		for (const QString &a : std::as_const(server.asked)) {
			if (a.startsWith("/page")) page = true;
			if (a.startsWith("/beacon")) beacon = true;
		}
		if (!page)
			std::printf("  !!    the page itself never loaded — the rest means nothing\n");
		return beacon;
	};

	section("an accepted rule stops a real request");
	check(!beacon_arrived("a"),
	      "the beacon never reaches the server while ||127.0.0.2^ is accepted");

	section("and the shield's escape hatch turns it off");
	{
		policy.set_setting("127.0.0.1", policy::feature::ads, policy::setting::allow);
		check(beacon_arrived("b"),
		      "allowing ads for the site lets the same beacon through");
		policy.set_setting("127.0.0.1", policy::feature::ads, policy::setting::block);
		check(!beacon_arrived("c"), "and blocking again stops it");
	}

	section("a list without the rule blocks nothing");
	{
		// The other half of the pair. Without it, "the beacon did not arrive"
		// could be any of a dozen things -- a typo in the page, a port that moved,
		// an image the engine decided not to fetch -- and the run would look like
		// a pass either way.
		QFile f(out + "/filters-ai.txt");
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
			f.write("! written by try_filters\n"
			         "other.example##.nothing\n");
		f.close();
		// Reload the shell's list the way a restart would.
		w.load_tree(tree);
		spin(500);
		check(beacon_arrived("d"),
		      "with only a cosmetic rule left, the same beacon arrives");
	}

	// --- the cosmetic half, which reaches the page rather than the request ---
	section("an accepted cosmetic rule hides the element");
	{
		QFile f(out + "/filters-ai.txt");
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
			f.write("! written by try_filters\n"
			         "127.0.0.1##.ad-banner\n");
		f.close();
		w.load_tree(tree);
		spin(500);

		bar->setText(QString("http://127.0.0.1:%1/cosmetic").arg(server.port));
		QMetaObject::invokeMethod(bar, "returnPressed");
		spin(3000);

		// Asked of the page itself, in the page's own world: what a user would
		// see. A test that asked the bridge what it would send would be checking
		// the same function the unit tests already cover.
		auto *page_view = w.findChild<QWebEngineView *>();
		QString hidden, kept;
		bool answered = false;
		if (page_view) {
			page_view->page()->runJavaScript(
				"[getComputedStyle(document.querySelector('.ad-banner')).display,"
				" getComputedStyle(document.querySelector('#keep')).display].join('|')",
				[&](const QVariant &v) {
					const QStringList parts = v.toString().split('|');
					if (parts.size() == 2) { hidden = parts[0]; kept = parts[1]; }
					answered = true;
				});
			for (int i = 0; i < 40 && !answered; ++i)
				spin(100);
		}
		check(hidden == "none",
		      QString("the advert is display:none in the page's own view (%1)").arg(hidden));
		check(kept != "none",
		      QString("and the content beside it is untouched (%1)").arg(kept));
	}

	// Does a *page* follow the browser's colour scheme?
	//
	// This is the half of dark mode a user actually looks at, and it is not the
	// palette -- a web page reads `prefers-color-scheme`, which the engine takes
	// from the application's colour scheme rather than from any QPalette. Worth
	// checking rather than assuming, because getting the window dark and leaving
	// every page white is a perfectly plausible way for this to half-work.
	section("web content follows the colour scheme");
	{
		// Set at startup above, because the engine reads it once. The driver is
		// run twice -- once each way -- rather than switching mid-run, which is
		// exactly the limitation the settings page tells the user about.
		server.asked.clear();
		bar->setText(QString("http://127.0.0.1:%1/scheme").arg(server.port));
		QMetaObject::invokeMethod(bar, "returnPressed");
		spin(2500);
		QString said;
		for (const QString &a : std::as_const(server.asked))
			if (a.startsWith("/report?scheme="))
				said = a.section('=', 1);
		check(said == (want_dark ? "dark" : "light"),
		      QString("a page sees prefers-color-scheme: %1 (%2)")
		          .arg(want_dark ? "dark" : "light", said));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

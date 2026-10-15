// What the scriptlet script costs a page, measured in the real engine: a
// local page with 0, 1 and 20 frames, loaded repeatedly, timed by the page's
// own navigation timing.
//
//     QT_QPA_PLATFORM=offscreen ./test/build-make/try_scriptlet_cost [lists-dir [no-scriptlets]]
//
// With `lists-dir` -- a profile directory holding `filters-subscribed.json`
// and `filters-subscribed/`, such as ~/.local/share/Hydra -- those cached
// lists are copied into the scratch profile, so the page gets the scriptlet
// script a real profile builds; without it, none. Run both and compare.
//
// **Why this exists**: the script holds every scriptlet call of every list
// and is injected into every frame, and with uBlock's lists it reached
// 1.3 MB. Cutting it per site is a design change; whether it is worth one
// depends on what the parse costs, which nobody had measured.
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "scriptlets.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <algorithm>
#include <cstdio>

static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

template <typename P> static bool until(P pred, int ms) {
	for (int i = 0; i < ms / 50 && !pred(); ++i)
		spin(50);
	return pred();
}

// `/p?n=N` is a page with N frames, each `/f`, a small document of its own.
class page_server : public QTcpServer {
protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, s, [s] {
			const QByteArray head = s->readAll();
			const QByteArray line = head.left(head.indexOf('\r'));
			QByteArray body = "<!doctype html><html><body><p>A frame.</p></body></html>";
			const int at = line.indexOf("/p?n=");
			if (at >= 0) {
				const int n = line.mid(at + 5, line.indexOf(' ', at) - at - 5).toInt();
				body = "<!doctype html><html><body><p>The page.</p>";
				for (int i = 0; i < n; ++i)
					body += "<iframe src=\"/f?" + QByteArray::number(i) +
					        "\" width=80 height=40></iframe>";
				body += "</body></html>";
			}
			s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: "
			         "no-store\r\nContent-Length: " + QByteArray::number(body.size()) +
			         "\r\nConnection: close\r\n\r\n" + body);
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

static bool copy_tree(const QString &from, const QString &to) {
	QDir().mkpath(to);
	QDirIterator it(from, QDir::Files);
	while (it.hasNext()) {
		const QString f = it.next();
		QFile::remove(to + "/" + QFileInfo(f).fileName());
		if (!QFile::copy(f, to + "/" + QFileInfo(f).fileName()))
			return false;
	}
	return true;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir home;
	const QByteArray base = home.path().toLocal8Bit();
	qputenv("HOME", base);
	qputenv("XDG_CONFIG_HOME", base + "/config");
	qputenv("XDG_DATA_HOME", base + "/data");
	qputenv("XDG_CACHE_HOME", base + "/cache");
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString profile = home.path() + "/profile";
	QDir().mkpath(profile);
	const QString lists = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
	if (!lists.isEmpty()) {
		const bool ok =
		  QFile::copy(lists + "/filters-subscribed.json",
		              profile + "/filters-subscribed.json") &&
		  copy_tree(lists + "/filters-subscribed", profile + "/filters-subscribed");
		// The offer of new lists is a question; this run answers none.
		QFile::copy(lists + "/filters-subscribed.json.offered",
		            profile + "/filters-subscribed.json.offered");
		std::printf("lists copied from %s: %s\n", qPrintable(lists), ok ? "yes" : "NO");
		if (!ok)
			return 1;
	}

	page_server srv;
	srv.listen(QHostAddress::LocalHost, 0);
	const QString origin = QString("http://127.0.0.1:%1").arg(srv.serverPort());
	{
		QFile tf(profile + "/tree.txt");
		tf.open(QIODevice::WriteOnly);
		tf.write(QString("- [f0] folder | Mine\n  - [a1] unopened | A page | %1/p?n=0 | "
		                 "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n")
		             .arg(origin).toUtf8());
	}

	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(profile + "/tree.txt");
	w.resize(1200, 900);
	w.show();
	spin(800);
	auto *tree = w.findChild<QTreeView *>();
	emit tree->activated(tree->model()->index(0, 0, tree->model()->index(0, 0)));
	spin(4000);
	web_view_backend *view = w.findChild<web_view_backend *>();
	if (!view) {
		std::printf("no view\n");
		return 1;
	}
	// A second argument, "no-scriptlets", keeps the lists and takes the
	// script away, so the two runs with lists split the cost between the
	// script and the rest of what the lists bring.
	if (argc > 2 && QByteArray(argv[2]) == "no-scriptlets") {
		view->remove_script(QStringLiteral("hydra-scriptlets"));
		std::printf("scriptlet script removed\n");
	}
	// Two more, to find which part of the script costs: "tiny" is the real
	// catalog with one call, "pad" is 1.3 MB of inert text and nothing else.
	if (argc > 2 && QByteArray(argv[2]) == "tiny") {
		QList<scriptlet_call> one;
		one.push_back({ "nowhere.invalid", "set-constant", { "cfg.a", "true" } });
		const QString src = scriptlets::source_for(one);
		view->inject_main_world_script(QStringLiteral("hydra-scriptlets"), src);
		std::printf("catalog with one call: %lld characters\n", (long long)src.size());
	}
	// "pad-elsewhere" is the same inert text, offered only to a host this
	// page is not: whether the engine honours @include, so a frame is never
	// given a script meant for other sites.
	if (argc > 2 && QByteArray(argv[2]) == "pad-elsewhere") {
		const QString src = "// ==UserScript==\n// @include *://nowhere.invalid/*\n"
		                    "// ==/UserScript==\n(function(){var D='" +
		                    QString(1300000, 'x') + "';})();";
		view->inject_main_world_script(QStringLiteral("hydra-scriptlets"), src);
		std::printf("inert text for another host: %lld characters\n", (long long)src.size());
	}
	// "pad-wide" is the same size in a character outside Latin-1, so the
	// string is two-byte as the real data is: whether that is the cost.
	if (argc > 2 && QByteArray(argv[2]) == "pad-wide") {
		const QString src = "(function(){var D='" + QString(1300000, QChar(0x2014)) + "';})();";
		view->inject_main_world_script(QStringLiteral("hydra-scriptlets"), src);
		std::printf("inert two-byte text: %lld characters\n", (long long)src.size());
	}
	if (argc > 2 && QByteArray(argv[2]) == "pad") {
		const QString src = "(function(){var D='" + QString(1300000, 'x') + "';})();";
		view->inject_main_world_script(QStringLiteral("hydra-scriptlets"), src);
		std::printf("inert text: %lld characters\n", (long long)src.size());
	}

	// Asked in the isolated world; the navigation timing is the page's.
	auto probe = [&](const QString &js) {
		QString out;
		bool got = false;
		view->run_probe(js, [&](const QString &r) { out = r; got = true; });
		until([&] { return got; }, 5000);
		return out;
	};

	std::printf("frames  load(ms) median of 7, then all\n");
	for (int n : { 0, 1, 20 }) {
		QList<double> loads;
		for (int run = 0; run < 8; ++run) {
			bool done = false;
			auto c = QObject::connect(view, &web_view_backend::load_finished,
			                          [&](bool) { done = true; });
			view->load(QUrl(QString("%1/p?n=%2&r=%3").arg(origin).arg(n).arg(run)));
			until([&] { return done; }, 20000);
			QObject::disconnect(c);
			spin(300);
			const QString r = probe(
			    "(function(){var e=performance.getEntriesByType('navigation')[0];"
			    "return JSON.stringify({load:e.loadEventEnd});})()");
			const double load = QJsonDocument::fromJson(r.toUtf8()).object()
			                        .value("load").toDouble();
			if (run > 0)   // the first warms the engine and the cache
				loads << load;
		}
		QList<double> sorted = loads;
		std::sort(sorted.begin(), sorted.end());
		QStringList all;
		for (double d : loads)
			all << QString::number(d, 'f', 0);
		std::printf("%6d  %8.0f    %s\n", n, sorted.value(sorted.size() / 2),
		            qPrintable(all.join(' ')));
	}
	return 0;
}

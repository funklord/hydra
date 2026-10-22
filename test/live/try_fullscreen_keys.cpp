// Leaving a page's fullscreen by key, in the real engine: Esc always, and F
// when the page has no use for it.
//
//     QT_QPA_PLATFORM=offscreen ./test/build-make/try_fullscreen_keys
//
// **Why this exists**: reported 2026-10-22, Esc did not leave a fullscreen
// video, and F -- which leaves fullscreen in mplayer and most players -- did
// nothing. `test_kiosk` covers the controller with a stand-in page; what only
// the real engine can say is whether Chromium hands a key the page did not
// use back up to the window, which is what F relies on, and whether a key
// typed into a field stays in the field.
//
// Fullscreen is entered by emitting the backend's own `fullscreen_requested`,
// as `try_forget` does: a real `requestFullscreen()` needs a user gesture no
// driver here can produce.
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "qtwebengine_view.h"
#include "request_filter.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
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

// A page with a field, so a key can be typed into it.
class page_server : public QTcpServer {
protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, s, [s] {
			s->readAll();
			const QByteArray body = "<!doctype html><title>video</title>"
			                        "<p>A video would be here.</p><input id=q>";
			s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
			         QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

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

	page_server srv;
	srv.listen(QHostAddress::LocalHost, 0);
	const QString profile = home.path() + "/profile";
	QDir().mkpath(profile);
	{
		QFile tf(profile + "/tree.txt");
		tf.open(QIODevice::WriteOnly);
		tf.write(QString("- [f0] folder | Mine\n  - [a1] unopened | Video | "
		                 "http://127.0.0.1:%1/ | created=2026-01-01T00:00:00 | "
		                 "seen=2026-01-01T00:00:00\n").arg(srv.serverPort()).toUtf8());
	}
	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(profile + "/tree.txt");
	w.resize(1000, 700);
	w.show();
	spin(500);
	auto *tree = w.findChild<QTreeView *>();
	emit tree->activated(tree->model()->index(0, 0, tree->model()->index(0, 0)));
	spin(3000);
	auto *view = w.findChild<qtwebengine_view *>();
	if (!view) {
		std::printf("no view\n");
		return 1;
	}

	// A key pressed as a person presses it: to whatever has the focus.
	auto press = [&](int key, const QString &text) {
		QWidget *to = QApplication::focusWidget();
		if (!to)
			to = view->widget();
		QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier, text);
		QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier, text);
		QCoreApplication::sendEvent(to, &down);
		QCoreApplication::sendEvent(to, &up);
		spin(1500);
	};
	auto enter = [&] {
		emit view->fullscreen_requested(true);
		spin(1500);
		view->widget()->setFocus();
		spin(300);
		return !w.isVisible();
	};
	auto field = [&] {
		QString got;
		bool have = false;
		view->run_probe("document.getElementById('q').value",
		                [&](const QString &v) { got = v; have = true; });
		for (int i = 0; i < 60 && !have; ++i)
			spin(50);
		return got;
	};

	check(enter(), "the page has the screen");
	press(Qt::Key_F, "f");
	check(w.isVisible(), "F leaves it, the page having no use for the key");

	check(enter(), "the page has the screen again");
	view->run_probe("document.getElementById('q').focus();'ok'", [](const QString &) {});
	spin(500);
	press(Qt::Key_F, "f");
	check(!w.isVisible() && field() == "f",
	      QString("F typed into a field stays there, and fullscreen stays (field '%1')")
	          .arg(field()));
	press(Qt::Key_Escape, QString());
	check(w.isVisible(), "and Esc leaves, from the field");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

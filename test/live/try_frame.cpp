// Does the player iframe load in a *plain* QWebEngineView, with none of our
// interceptor, scripts or bridges? That is the difference between a browser
// bug of ours and a site behaviour.
#include "media_fixture.h"
#include <QApplication>
#include <QTimer>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <cstdio>

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	// The fixture's page, which holds the player in an iframe -- the shape a
	// mirror has, and the one this driver is about. Pointing it at the
	// player page directly reported "no iframe", correctly: that page is
	// the player rather than a page containing one.
	media_fixture::server fixture;
	const QString url = argc > 1 ? QString::fromLocal8Bit(argv[1])
	                             : fixture.start();

	// **Owned, and owned by something that dies before QApplication.** This was
	// `new QWebEngineView` with no parent and no delete, so the view outlived
	// `app` -- and it is `app` going away that releases the default profile,
	// while a page built on that profile was still alive. Qt says so on the way
	// out: "Release of profile requested but WebEnginePage still not deleted.
	// Expect troubles !"
	//
	// A stack object declared *after* `app` is destroyed *before* it, which is
	// the whole fix; no smart pointer and no include. Everything below still
	// takes a pointer, so nothing else changes.
	QWebEngineView view_owner;
	QWebEngineView *view = &view_owner;
	view->resize(1280, 860);
	view->show();
	view->load(QUrl(url));

	QTimer::singleShot(18000, [view] {
		view->page()->runJavaScript(
		  "(function(){var f=document.querySelector('iframe');"
		  "if(!f) return 'no iframe';"
		  "var st='unknown';"
		  "try { st = f.contentWindow.location.href ? 'same-origin' : 'blank'; }"
		  "catch(e) { st = 'cross-origin (so it loaded)'; }"
		  "return 'src='+f.getAttribute('src')+' state='+st;})()",
		  [](const QVariant &v) { std::printf("PLAIN %s\n", qPrintable(v.toString())); });
	});
	QTimer::singleShot(21000, [] { std::printf("done\n"); qApp->quit(); });
	return app.exec();
}

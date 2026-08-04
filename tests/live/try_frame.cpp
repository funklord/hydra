// Does the player iframe load in a *plain* QWebEngineView, with none of our
// interceptor, scripts or bridges? That is the difference between a browser
// bug of ours and a site behaviour.
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
	const QString url = argc > 1 ? argv[1]
	                             : "https://dramafren.org/watch/born-to-be-tortured/";

	auto *view = new QWebEngineView;
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

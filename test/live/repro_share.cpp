// getDisplayMedia through Qt WebEngine, with nothing of hydra linked.
//
// `try_share` beside this drives the real shell and proves what *this*
// project does with a display-capture request. It cannot answer the question
// that came after it: the request is answered correctly and Chromium still
// refuses with INVALID_STATE, so the fault is below us -- and a driver that
// links the whole browser cannot say whether "below us" means Qt or the way
// Qt was packaged.
//
// This is the smaller instrument. It links Qt and nothing else, so a failure
// here belongs to Qt or to the machine, and it builds against any kit whose
// pkg-config files are on the path -- which is how a Debian build and an
// official Qt binary get compared without changing a line:
//
//     make -C test repro && ./test/build-make/repro_share
//     env PKG_CONFIG_PATH=~/Qt/6.12.0/gcc_64/lib/pkgconfig BUILD_DIR=build-qt612 make -C test repro
//
// It needs a display and a screen to offer. Under Xvfb both are satisfied:
//
//     xvfb-run -a -s "-screen 0 1280x800x24" ./test/build-make/repro_share
//
// Exit 0 means the page got a stream, 1 means it got an error, 2 means
// nothing answered, 3 means the page could not be written.
#include <QAbstractListModel>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QTimer>
#include <QUrl>
#include <QWebEngineDesktopMediaRequest>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QtGlobal>
#include <cstdio>

namespace {

// The page asks **on click**, because `getDisplayMedia` requires transient
// activation: called from a timer it fails with `InvalidStateError` before
// any of Qt's code is reached, which would fail for the wrong reason.
const char k_page[] =
  "<!doctype html><meta charset=\"utf-8\"><title>share</title>"
  "<style>html,body{margin:0;height:100%;background:#222}</style>"
  "<script>window.__r='idle';"
  "document.addEventListener('click',function(){"
  "  if(window.__r!=='idle')return; window.__r='asked';"
  "  navigator.mediaDevices.getDisplayMedia({video:true}).then(function(s){"
  "    window.__r='stream:'+s.getVideoTracks().length;"
  "  }).catch(function(e){ window.__r='error:'+e.name+': '+e.message; });"
  "});</script>";

QString out_dir() {
	if (qEnvironmentVariableIsSet("HYDRA_TEST_OUT"))
		return QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT"));
	return QStringLiteral("/tmp/hydra-repro-share");
}

} // namespace

int main(int argc, char **argv) {
	QApplication app(argc, argv);
	std::printf("qt runtime %s (built against %s)\n", qVersion(), QT_VERSION_STR);

	QWebEngineView view;
	view.resize(640, 480);

	int seen = 0;
	QObject::connect(view.page(), &QWebEnginePage::desktopMediaRequested,
	                  [&](const QWebEngineDesktopMediaRequest &request) {
		++seen;
		QAbstractListModel *screens = request.screensModel();
		QAbstractListModel *windows = request.windowsModel();
		std::printf("request: screens=%d windows=%d\n",
		             screens ? screens->rowCount() : -1,
		             windows ? windows->rowCount() : -1);
		std::fflush(stdout);
		// **Answer, or deliberately do not.** With `HYDRA_REPRO_NOANSWER`
		// set this returns without selecting or cancelling, which hands the
		// request to Qt's own fallback: `~QWebEngineDesktopMediaRequestPrivate`
		// selects screen 0 by itself when nobody answered. That separates "this
		// handler is wrong" from "no handler can be right", and it is the
		// difference between a bug in the caller and a bug in Qt.
		if (qEnvironmentVariableIsSet("HYDRA_REPRO_NOANSWER")) {
			std::printf("answering nothing, leaving it to Qt's destructor\n");
			std::fflush(stdout);
			return;
		}
		// Exactly what the documentation asks for: an index from the
		// request's own model, handed back to the request.
		if (screens && screens->rowCount() > 0)
			request.selectScreen(screens->index(0, 0));
		else
			request.cancel();
	});

	// A `file:` url rather than `setHtml`, because `navigator.mediaDevices`
	// exists only in a secure context and an `about:blank` origin is not one.
	// The first run of this reported "Cannot read properties of undefined"
	// and reached no request at all -- a failure that looks like the one
	// being hunted and is not.
	QDir().mkpath(out_dir());
	const QString path = out_dir() + QStringLiteral("/ask.html");
	{
		QFile page(path);
		if (!page.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			std::printf("cannot write %s\n", qPrintable(path));
			return 3;
		}
		page.write(k_page);
	}
	view.load(QUrl::fromLocalFile(path));
	view.show();

	QTimer::singleShot(2500, [&] {
		QWidget *target = view.focusProxy() ? view.focusProxy()
		                                     : static_cast<QWidget *>(&view);
		const QPoint at(target->width() / 2, target->height() / 2);
		QMouseEvent press(QEvent::MouseButtonPress, at, target->mapToGlobal(at),
		                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
		QMouseEvent release(QEvent::MouseButtonRelease, at, target->mapToGlobal(at),
		                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(target, &press);
		QApplication::sendEvent(target, &release);
	});

	QTimer::singleShot(12000, [&] {
		view.page()->runJavaScript(QStringLiteral("window.__r"),
		                            [&](const QVariant &value) {
			const QString result = value.toString();
			std::printf("page: %s\nrequests: %d\n",
			             qPrintable(result), seen);
			std::fflush(stdout);
			app.exit(result.startsWith(QStringLiteral("stream:")) ? 0 : 1);
		});
	});
	// Its own ceiling, so that a run which never answers still ends.
	QTimer::singleShot(20000, [&] {
		std::printf("page: TIMEOUT\nrequests: %d\n", seen);
		app.exit(2);
	});
	return app.exec();
}

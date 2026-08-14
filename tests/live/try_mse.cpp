// Can a main-world tap see the video bytes, whatever the transport delivered
// them? If MediaSource is being fed, this reports the mime types and totals.
#include "media_fixture.h"
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <cstdio>

static const char *k_tap = R"JS(
(function(){
  if (window.__hydraTap) return; window.__hydraTap = 1;
  var log = function(m){ console.log('HYDRA-MSE ' + m); };
  var totals = {}, mimes = [];

  // Where bytes become video, regardless of how they arrived.
  if (window.MediaSource) {
    var addSB = MediaSource.prototype.addSourceBuffer;
    MediaSource.prototype.addSourceBuffer = function(mime){
      mimes.push(mime); log('addSourceBuffer ' + mime);
      var sb = addSB.apply(this, arguments);
      var ap = sb.appendBuffer;
      sb.appendBuffer = function(buf){
        var n = (buf && (buf.byteLength !== undefined ? buf.byteLength : buf.length)) || 0;
        totals[mime] = (totals[mime]||0) + n;
        if ((totals[mime] / 262144 | 0) !== ((totals[mime]-n) / 262144 | 0))
          log('appended ' + mime + ' total=' + totals[mime]);
        return ap.apply(this, arguments);
      };
      return sb;
    };
    log('MediaSource hooked');
  } else { log('no MediaSource'); }

  // Blob/object URLs handed to a <video> are the other common shape.
  var cou = URL.createObjectURL;
  URL.createObjectURL = function(o){
    var u = cou.apply(this, arguments);
    log('createObjectURL ' + (o && o.constructor ? o.constructor.name : '?') + ' -> ' + u);
    return u;
  };

  setInterval(function(){
    var v = document.querySelectorAll('video');
    for (var i=0;i<v.length;i++)
      if (v[i].currentTime > 0)
        log('video playing t=' + v[i].currentTime.toFixed(1) +
            ' src=' + (v[i].currentSrc||'').slice(0,60));
  }, 4000);
})();
)JS";

// Capture console output explicitly rather than relying on Qt's default sink.
class page : public QWebEnginePage {
public:
	using QWebEnginePage::QWebEnginePage;
protected:
	void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &m,
	                               int, const QString &) override {
		if (m.startsWith("HYDRA-MSE"))
			std::printf("%s\n", qPrintable(m.left(200)));
	}
};

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	// The fixture opens a MediaSource and appends to it, which is what the tap
	// hooks. Whether a real player defeats the tap is a question only a real
	// player answers, and that is what the argument is for.
	media_fixture::server fixture;
	const QString url = argc > 1 ? QString::fromLocal8Bit(argv[1])
	                             : fixture.start();

	QWebEngineScript tap;
	tap.setName("hydra-mse-tap");
	tap.setSourceCode(QString::fromUtf8(k_tap));
	tap.setInjectionPoint(QWebEngineScript::DocumentCreation);
	// MainWorld on purpose: an isolated world has its own globals, so a hook
	// installed there would never see the page's MediaSource.
	tap.setWorldId(QWebEngineScript::MainWorld);
	tap.setRunsOnSubFrames(true);
	QWebEngineProfile::defaultProfile()->scripts()->insert(tap);

	// Stack-owned, and declared after `app` so it is destroyed before it --
	// see try_frame for the reasoning. It matters more here: `pg` is built on
	// the default profile explicitly, and being parented to the view is what
	// gets it destroyed in time. A leaked view took the page with it, and Qt
	// reported the profile being released underneath a live page.
	QWebEngineView view_owner;
	QWebEngineView *view = &view_owner;
	auto *pg = new page(QWebEngineProfile::defaultProfile(), view);
	view->setPage(pg);
	QObject::connect(pg, &QWebEnginePage::loadFinished, [pg](bool ok) {
		std::printf("LOAD %s url=%s\n", ok ? "ok" : "FAILED",
		             qPrintable(pg->url().toString().left(90)));
	});
	view->resize(1280, 860);
	view->show();
	view->load(QUrl(url));

	QTimer::singleShot(16000, [view] {
		QWidget *t = view->focusProxy() ? view->focusProxy() : view;
		const QPoint at(t->width()/2, t->height()/2);
		for (int i = 0; i < 2; ++i) {
			QMouseEvent p(QEvent::MouseButtonPress, at, t->mapToGlobal(at),
			               Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			QMouseEvent r(QEvent::MouseButtonRelease, at, t->mapToGlobal(at),
			               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(t, &p);
			QApplication::sendEvent(t, &r);
		}
		std::printf("CLICK sent\n");
	});
	QTimer::singleShot(50000, [] { std::printf("done\n"); qApp->quit(); });
	return app.exec();
}

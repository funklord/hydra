// What is actually on screen in the moments after a tab is opened?
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "sample_tree.h"
#include "theme.h"
#include "settings_dialog.h"

#include <QApplication>
#include <QPlatformSurfaceEvent>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>
#include <QImage>
#include <QStackedWidget>
#include <QTreeView>
#include <QUrl>
#include <cstdio>

// Where screenshots and captures land. Set HYDRA_TEST_OUT to move it.
static QString test_out() {
	const QByteArray e = qgetenv("HYDRA_TEST_OUT");
	return (e.isEmpty() ? QString("/tmp/hydra-test/")
	                    : QString::fromLocal8Bit(e) + "/");
}

static const QString OUTDIR =
  test_out();

// Mean brightness of a region of a grab, 0..255, or -1 where the region is
// empty. Reported rather than left in the PNGs: the previous record of this
// driver says in as many words that nobody had read what it produces, and a
// number printed beside the timestamp is read where eleven images are not.
static QString mean_of(const QImage &img, const QRect &r) {
	const QRect box = r.intersected(img.rect());
	if (box.isEmpty())
		return QStringLiteral("-");
	qint64 sr = 0, sg = 0, sb = 0;
	int lo = 255, hi = 0;
	for (int y = box.top(); y <= box.bottom(); ++y)
		for (int x = box.left(); x <= box.right(); ++x) {
			const QRgb c = img.pixel(x, y);
			sr += qRed(c); sg += qGreen(c); sb += qBlue(c);
			const int g = qGray(c);
			lo = qMin(lo, g); hi = qMax(hi, g);
		}
	const qint64 n = qint64(box.width()) * box.height();
	// **The channels, not a grey.** A grey cannot separate the two things this
	// driver has to tell apart on a dark desktop: the engine's own background
	// before a page paints, which is neutral, and the fixture page once it has,
	// which is dark blue. Both are dark, and averaged into one number they are
	// the same number -- so "is there still a flash when the chrome is dark"
	// cannot be asked of a grey at all.
	// **The range as well as the mean, because a mean cannot see ink.** Text
	// on a page is a small fraction of its pixels, so a page of dark text on
	// white and a page of dark text on dark have means far apart and are told
	// apart by neither of them alone. The darkest and lightest greys in the
	// region answer the question the mean cannot: whether anything on it
	// stands out from what is behind it.
	return QString("%1/%2/%3 lo%4 hi%5")
	    .arg(sr / n).arg(sg / n).arg(sb / n).arg(lo).arg(hi);
}

// Where the page is, asked of the widget holding it rather than guessed from
// the window's size. main_window keeps its views in a QStackedWidget and that
// is the only one under this window while no dialog is open -- the other four
// in this tree belong to dialogs. Returns an empty rect if it is not there,
// and the caller prints that rather than substituting a number.
static QRect page_rect(QWidget &w) {
	auto *stack = w.findChild<QStackedWidget *>();
	if (!stack)
		return QRect();
	QWidget *cur = stack->currentWidget();
	if (!cur || cur->size().isEmpty())
		return QRect();
	return QRect(cur->mapTo(&w, QPoint(0, 0)), cur->size());
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	QDir().mkpath(OUTDIR);

	// **The theme, because without it this driver measures a case nobody has.**
	// It built a main_window and never applied an appearance, so every grab was
	// taken with Qt's default light palette -- and the open question about the
	// white page area is what it looks like on a DARK desktop, where white is
	// not the colour of the chrome around it. Applied the way main.cpp does, so
	// the window is the one a user gets; with no configuration the choice is
	// `system`, which resolves light here and leaves every previous run
	// comparable.
	//
	// Point HYDRA_TEST_CONFIG at a file holding `appearance=dark` under [ui] to
	// take the other case.
	const theme::choice want = settings_store::appearance();
	theme::watcher appearance;
	appearance.set_choice(want);
	theme::set_web_engine_scheme(theme::resolve(want));
	std::printf("appearance: %s, resolved %s\n",
	             qPrintable(theme::name_of(want)),
	             theme::resolve(want) == Qt::ColorScheme::Dark ? "dark" : "light");

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	// Log exactly when the platform surface goes away and comes back.
	class watcher : public QObject {
	public:
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::PlatformSurface) {
				auto *pe = static_cast<QPlatformSurfaceEvent *>(e);
				std::printf("HYDRA-SURFACE %s\n",
				             pe->surfaceEventType() ==
				                 QPlatformSurfaceEvent::SurfaceCreated
				                 ? "created" : "about-to-be-destroyed");
			} else if (e->type() == QEvent::WinIdChange) {
				std::printf("HYDRA-SURFACE winIdChange\n");
			}
			return QObject::eventFilter(o, e);
		}
	};
	w.installEventFilter(new watcher);

	// **A local page, not the committed example's first tab.** This opened
	// `inert_sample_tree()` and activated its first child, which in the sample
	// is `doc.qt.io` -- so a sweep fetched a real site and pulled amplitude and
	// Simple Analytics with it, which is how this was noticed: the driver's own
	// output carried the Amplitude logger's warnings. See `local_page_tree`.
	//
	// It also makes the measurement mean something. What this reports is how
	// the shell paints in the moments after a tab opens, and while the page
	// came off the network those timings moved with a remote site's week rather
	// than with this code.
	// **A second fixture, chosen by the environment.** The default page sets
	// its own background, which is what makes the flash visible as a change.
	// HYDRA_FLICKER_PLAIN loads one that sets none, which is the page the
	// recorded objection to setBackgroundColor is about -- it is the engine's
	// default that shows through there, and what that default is worth
	// measuring rather than assuming.
	w.load_tree(qEnvironmentVariableIsSet("HYDRA_FLICKER_PLAIN")
	              ? shell::plain_page_tree()
	              : shell::local_page_tree());
	w.resize(1100, 780);
	w.show();

	QElapsedTimer t;
	// Sample densely right after the switch, then thin out.
	const QList<int> at = { 0, 40, 90, 150, 240, 360, 520, 750, 1100, 1700, 2600 };

	QTimer::singleShot(2500, [&] {
		auto *tree = w.findChild<QTreeView *>();
		const QModelIndex first = tree->model()->index(0, 0);
		const QModelIndex kid   = tree->model()->index(0, 0, first);
		std::printf("activating: %s\n", qPrintable(kid.data().toString()));
		t.start();
		emit tree->activated(kid);
		for (int ms : at) {
			QTimer::singleShot(ms, [&w, ms, &t] {
				const QPixmap p = w.grab();
				p.save(OUTDIR + QString("f%1.png").arg(ms, 5, 10, QChar('0')));
				const QImage img = p.toImage();
				const QRect page = page_rect(w);
				// The chrome is sampled as well, and that is the half that
				// makes the page number mean something: "the page area is
				// white" is only a flash if what surrounds it is not. A
				// strip across the toolbar, above wherever the page starts.
				const QRect chrome(0, 0, img.width(),
				                    page.isEmpty() ? 0 : page.top());
				std::printf("t+%-5d grabbed (elapsed %lld) page=%s chrome=%s\n",
				             ms, t.elapsed(), qPrintable(mean_of(img, page)),
				             qPrintable(mean_of(img, chrome)));
			});
		}
		QTimer::singleShot(22000, [] {
			std::printf("done\n");
			qApp->quit();
		});
	});
	return app.exec();
}

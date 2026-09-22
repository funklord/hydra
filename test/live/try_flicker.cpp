// What is actually on screen in the moments after a tab is opened?
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "sample_tree.h"
#include "theme.h"
#include "settings_dialog.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

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
	// The window's own stack. Four dialogs here keep a QStackedWidget and are
	// children of the window, so asking by type alone answers with whichever
	// comes first; `window()` is the top-level widget a widget sits in, which
	// separates them. No dialog is open in this driver today, and a helper
	// that is right only while that holds is one nobody will re-check.
	QStackedWidget *stack = nullptr;
	for (QStackedWidget *st : w.findChildren<QStackedWidget *>())
		if (st->window() == &w) { stack = st; break; }
	if (!stack)
		return QRect();
	QWidget *cur = stack->currentWidget();
	if (!cur || cur->size().isEmpty())
		return QRect();
	return QRect(cur->mapTo(&w, QPoint(0, 0)), cur->size());
}

// Set only when the gap is forced; see the grab loop for why the checks are
// conditional on it.
static bool g_forced = false;
static int  g_checked = 0, g_failed = 0;

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
	// **A third fixture, because the flash is luck on a quiet machine.** Both
	// pages above are `file:` urls off the local disk, so they paint before
	// the first grab whenever the machine is not busy -- five runs on
	// 2026-09-22, one of them under four-way CPU load, every one with the
	// page already painted at `t+0`. The recorded runs that DID catch it
	// caught it because the machine was loaded, which makes the whole
	// measurement a matter of when it is taken.
	//
	// HYDRA_FLICKER_SLOW serves the same background-less page over loopback
	// after a delay, so the gap between `loadStarted` and first paint is
	// forced rather than hoped for. That is what makes "does the page area
	// show white before the document paints" answerable on any machine --
	// and it is the only way to test an arrangement that turns the
	// background dark at `loadStarted`, where the open question is whether
	// that signal comes early enough.
	//
	// Bounded by construction: one connection answered once, no loop, and
	// the server dies with the process.
	QTcpServer slow;
	QString slow_url;
	g_forced = false;
	const int slow_ms = qEnvironmentVariableIntValue("HYDRA_FLICKER_SLOW");
	if (slow_ms > 0 && slow.listen(QHostAddress::LocalHost, 0)) {
		slow_url = QString("http://127.0.0.1:%1/").arg(slow.serverPort());
		QObject::connect(&slow, &QTcpServer::newConnection, [&slow, slow_ms] {
			QTcpSocket *c = slow.nextPendingConnection();
			QObject::connect(c, &QTcpSocket::readyRead, c, [c, slow_ms] {
				c->readAll();
				// The delay is before the response, which is where a real
				// slow site's delay is: the navigation has begun and there
				// is nothing to paint yet.
				// **Styled unless asked otherwise, and that is not a
				// detail.** A background-less page is white before it paints
				// and white after, so it cannot show the flip at all -- the
				// first run of this fixture read 253 from `t+0` to `t+1100`
				// and proved nothing. The flash is only visible against a
				// page that states a colour. HYDRA_FLICKER_PLAIN switches
				// this body to the background-less one, which is the case
				// the setBackgroundColor objection is about.
				const bool plain =
				  qEnvironmentVariableIsSet("HYDRA_FLICKER_PLAIN");
				QTimer::singleShot(slow_ms, c, [c, plain] {
					const QByteArray body = plain
					  ? "<!doctype html><title>slow fixture</title>"
					     "<h1>slow fixture</h1><p>no background, no colour</p>"
					  : "<!doctype html><title>slow fixture</title>"
					     "<style>html,body{background:#123;color:#eee;"
					     "margin:0;height:100%}</style>"
					     "<h1>slow fixture</h1>";
					c->write("HTTP/1.1 200 OK\r\nContent-Type: text/html"
					          "\r\nContent-Length: " +
					          QByteArray::number(body.size()) +
					          "\r\nConnection: close\r\n\r\n" + body);
					c->flush();
					c->disconnectFromHost();
				});
			});
		});
		g_forced = true;
		std::printf("slow fixture: %s, answering after %d ms\n",
		             qPrintable(slow_url), slow_ms);
	}

	w.load_tree(!slow_url.isEmpty()
	              ? shell::single_tab_tree(slow_url)
	              : qEnvironmentVariableIsSet("HYDRA_FLICKER_PLAIN")
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
				const QString page_says = mean_of(img, page);
				std::printf("t+%-5d grabbed (elapsed %lld) page=%s chrome=%s\n",
				             ms, t.elapsed(), qPrintable(page_says),
				             qPrintable(mean_of(img, chrome)));
				// **Judged, but only where the answer is known.** With no
				// slow fixture this driver reports and asserts nothing: what
				// the page area holds at a given millisecond depends on how
				// busy the machine is, and a check on that would fail for
				// the machine rather than for the code.
				//
				// With the gap forced, the early grabs have a defined
				// answer: the page has not painted, so what shows is what
				// the shell put behind it, and flat white is the fault this
				// was written for. `lo255 hi255` rather than a mean, because
				// a pale page and a white ground have the same mean and
				// differ in their range -- which is the distinction this
				// file's own header makes.
				if (g_forced && ms <= 360) {
					++g_checked;
					if (page_says.contains(QLatin1String("lo255 hi255"))) {
						++g_failed;
						std::printf("  FAIL  t+%d the page area is flat white "
						             "before the document painted\n", ms);
					}
				}
			});
		}
		QTimer::singleShot(22000, [] {
			// A tally only in the mode that has expectations, so the sweep's
			// own run stays report-only and is judged as it always was.
			if (g_forced)
				std::printf("\n%d passed, %d failed\n",
				             g_checked - g_failed, g_failed);
			std::printf("done\n");
			qApp->quit();
		});
	});
	return app.exec();
}

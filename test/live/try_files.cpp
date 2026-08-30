//
// The File System Access prompt: a page asking to read or change a real file or
// folder, what the page gets back when the answer is yes and when it is no, and
// an attempt at the re-entry guard that is meant to stop a page stacking two
// questions.
//
// **Compiled and reviewed, never run.** `qtwebengine_view` has answered
// `fileSystemAccessRequested` since the seam sweep and nothing had ever reached
// it, because reaching it needs `showDirectoryPicker()` -- which needs a user
// gesture and then opens a file chooser *before* our question appears. There is
// no `xdotool` on this machine and `test/README.md` records that as the limit
// every driver here works under.
//
// Two things get round it, and neither is a change to the code under test:
//
//   * **A real mouse event, not a script call.** `showDirectoryPicker()` is
//     refused outright without transient activation, and `runJavaScript` grants
//     none. The driver sends `QMouseEvent`s to the render widget the way
//     `try_extract` and `try_media` already click players -- Chromium treats a
//     forwarded press and release as input and grants activation. The page
//     reports `navigator.userActivation.isActive` from inside the handler, so
//     whether the gesture landed is measured rather than assumed: a run where
//     the click missed would otherwise report the same silence as a run where
//     the prompt is broken.
//   * **Both modal surfaces are answered in process.** The engine's file
//     chooser is Qt's own `QFileDialog` -- nothing here overrides
//     `QWebEnginePage::chooseFiles` -- and our question is a `QMessageBox`.
//     Both are `exec()`d, so the driver finds them in `topLevelWidgets()` from
//     a timer, which is the shape `try_forget` and `try_confirm` established.
//
// **`AA_DontUseNativeDialogs` is set, and it is the driver's attribute rather
// than the browser's.** A native file chooser is not a `QWidget` and cannot be
// found or answered from this process, so a run that got one would hang in the
// chooser and report nothing about the prompt behind it. Under
// `QT_QPA_PLATFORM=offscreen` there is no native dialog to get, but a driver
// that depended on that would break the first time somebody ran it on a real
// display.
//
// **The measurement is what the page was given**, never our own log or the
// return of `request.accept()`. A grant is a directory handle whose `name` is
// the folder that was chosen; a refusal is a rejected promise carrying an error
// name. Both are fetched back to the server that served the page -- the shape
// `try_permissions` uses -- so what is recorded is what a site would actually
// experience.
//
// ## The re-entry guard, and why it takes a control, a clock and a branch
//
// `m_prompting` is one flag shared by the two requests that put a modal
// question on screen -- registering a protocol handler, and this one -- because
// each question runs a nested event loop that the page keeps running
// underneath, and a page asking in a loop would otherwise stack modal windows
// over a window nobody can reach.
//
// **The obvious test passes without the guard existing.** Measured, in the
// first version of this file: two `registerProtocolHandler` calls in one script
// produced two questions, one after the other, never two at once -- because the
// browser process delivered the second only after the first had been answered,
// at which point the flag was false again and the guard did nothing. That run
// reports exactly what a working guard reports. It is kept, as section 4c, and
// reported as an observation rather than as evidence.
//
// **The second obvious test cannot even be written the obvious way.** A modal
// `exec()` opens an event loop *deeper* than the one `spin()` is sitting in,
// and Qt unwinds loops in the order they were entered -- so the moment a
// question appears, every sequential line in `main()` stops until that question
// is answered. A `for` loop that waits for a question to be on screen and then
// acts is waiting for something that will already be over when it returns.
// Anything that has to happen *while* a question is up therefore happens in
// `poll_modals`, which is a QTimer and does run inside the nested loop.
//
// So the guard is approached with the second request parked at the server until
// the driver lets it go, a control to prove the route, and a clock:
//
//   * the **control** (4a) releases one parked request with nothing on screen,
//     and a question must appear -- so the route works, the counter counts, and
//     a silence afterwards is a measurement;
//   * the **guarded case** (4b, and section 5 for the file route) releases the
//     second request while a question is up, and asks whether any second
//     question appeared, then or after the first was answered. "Then or after"
//     is what separates refused from merely delayed;
//   * and **every one of them prints a timestamped trace**, because the counts
//     alone cannot tell a request the guard refused from one the browser
//     delivered a moment after the question closed.
//
// **What that measured, on this Qt and this Chromium: the guarded case cannot
// be reached from a driver, and the trace says why.** While one of these
// questions holds the loop, nothing from the page reaches the browser at all --
// not a `fetch`, not the effect of an injected click. In section 4b the page's
// own first reports arrived three milliseconds after the question closed,
// having been sent eight seconds earlier. In section 5 the click goes in at the
// right moment and the page does not report having seen it until the question
// is answered. So the second request cannot be made while the flag is set, the
// branch is not entered, and both sections say so and assert nothing about it
// rather than passing on a silence. They still assert the property a user would
// notice -- that two questions are never on screen together -- and they will
// assert the guard itself, unedited, on any build where the request does get
// through.
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "qtwebengine_view.h"
#include "request_filter.h"
#include "web_view_factory.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QHash>
#include <QHostAddress>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTreeView>
#include <QUrlQuery>
#include <QWebEngineProfile>

#include <cstdio>
#include <memory>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void note(const QString &w) { std::printf("  --    %s\n", qPrintable(w)); }
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// ---------------------------------------------------------------- the clock --
//
// **One clock and one ordered list, written from both sides.** The counters
// answer "how many"; only this answers "in what order", and order is the whole
// question for a guard that is armed exactly as long as a question is up.
// Without it a section cannot tell a request the guard refused from one the
// browser delivered a moment after the question closed -- and those two produce
// identical counts, which is how two earlier versions of this file each read as
// evidence while measuring nothing.
static QElapsedTimer g_clock;
static QStringList   g_trace;
static void trace(const QString &what) {
	g_trace << QString("%1 ms  %2").arg(g_clock.elapsed(), 6).arg(what);
}
static void show_trace(int from) {
	for (int i = from; i < g_trace.size(); ++i)
		std::printf("        %s\n", qPrintable(g_trace.at(i)));
}

// --------------------------------------------------------------- the origin --
//
// `127.0.0.1` rather than a `file:` page, and it is a precondition rather than
// a preference: the File System Access API is refused outside a secure context
// and Chromium counts loopback as one. A `file:` page gets an opaque origin, and
// the whole run would be measuring a refusal that never reached our code.
//
// `/hold` is answered only when the driver says so. Nothing else about the page
// is timed by the page.
static const char *k_page = R"HTML(<!doctype html><html><body style="margin:0">
<button id="go" style="position:fixed;left:0;top:0;width:100%;height:100%;
 font-size:48px">ask</button>
<script>
function say(what, result) {
  fetch('/report?what=' + encodeURIComponent(what) +
        '&result=' + encodeURIComponent(result));
}
function reg(tag) {
  try { navigator.registerProtocolHandler('web+hydra' + tag, '/h' + tag + '?u=%s'); }
  catch (e) { say('error-' + tag, e.name); }
  say('asked-' + tag, 'yes');
}
var mode = (location.search.match(/mode=(\w+)/) || [])[1] || 'pick';
say('api', typeof window.showDirectoryPicker);

// Registering a protocol handler needs no user gesture, which is the whole
// reason it can be the second question: it can be asked for from a fetch
// callback, with no click, at whatever moment the driver releases the fetch.
if (mode === 'control') {
  fetch('/hold').then(function () { reg('control'); });
} else if (mode === 'guarded') {
  reg('first');
  fetch('/hold').then(function () { reg('second'); });
} else if (mode === 'inarow') {
  reg('rowone');
  reg('rowtwo');
}
say('ready', mode);

var clicks = 0;
document.getElementById('go').addEventListener('click', function () {
  var n = ++clicks;
  say('gesture' + n, navigator.userActivation
        ? String(navigator.userActivation.isActive) : 'unknown');
  window.showDirectoryPicker().then(function (h) {
    say('pick' + n, 'granted:' + h.name);
  }, function (e) {
    say('pick' + n, 'refused:' + e.name);
  });
  if (mode === 'double') {
    // A second file question inside the *same* gesture. Reported whatever it
    // answers, because what it answers is the finding.
    window.showDirectoryPicker().then(function (h) {
      say('same-gesture', 'granted:' + h.name);
    }, function (e) {
      say('same-gesture', 'refused:' + e.name + ': ' + e.message);
    });
  }
});
</script></body></html>)HTML";

class local_site : public QTcpServer {
public:
	QHash<QString, QString> reported;
	// **When**, not only what. Sections 4 and 5 turn on whether the page's
	// second request happened before or after the first question was answered,
	// and the two orders are indistinguishable by count.
	QHash<QString, qint64> reported_at;

	// Requests parked until the driver answers them. QPointer because a socket
	// deletes itself when the peer goes away -- a page that navigated while its
	// fetch was parked leaves one behind, and writing into it would be a write
	// into freed memory rather than a request nobody answered.
	QList<QPointer<QTcpSocket>> held;

	void release_holds() {
		trace(QString("driver: releasing %1 parked request(s)").arg(held.size()));
		for (const QPointer<QTcpSocket> &s : std::as_const(held)) {
			if (!s)
				continue;
			s->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
			          "Cache-Control: no-store\r\nConnection: close\r\n\r\nok");
			s->flush();
			s->disconnectFromHost();
		}
		held.clear();
	}

protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		// A `readyRead` can carry a partial header, so the bytes are collected
		// until the blank line rather than assumed to arrive whole.
		struct conn { QByteArray buf; bool served = false; };
		auto c = std::make_shared<conn>();
		connect(s, &QTcpSocket::readyRead, s, [this, s, c] {
			c->buf += s->readAll();
			if (c->served || !c->buf.contains("\r\n\r\n"))
				return;
			c->served = true;
			serve(s, c->buf);
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}

private:
	void serve(QTcpSocket *s, const QByteArray &head) {
		const int first = head.indexOf(' ');
		const int second = head.indexOf(' ', first + 1);
		const QString path = (first < 0 || second < 0)
		                         ? QString()
		                         : QString::fromUtf8(head.mid(first + 1,
		                                                       second - first - 1));
		if (path.startsWith("/hold")) {
			held << QPointer<QTcpSocket>(s);
			trace("server: a request parked at /hold");
			return;                    // answered by release_holds(), not here
		}
		QByteArray body;
		if (path.startsWith("/report")) {
			const QUrlQuery q(QUrl(path).query());
			const QString what = q.queryItemValue("what", QUrl::FullyDecoded);
			const QString said = q.queryItemValue("result", QUrl::FullyDecoded);
			reported.insert(what, said);
			reported_at.insert(what, g_clock.elapsed());
			trace(QString("page: %1 = %2").arg(what, said));
		} else {
			body = k_page;
		}
		const QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html"
		                         "\r\nCache-Control: no-store"
		                           "\r\nContent-Length: "
		                         + QByteArray::number(body.size())
		                         + "\r\nConnection: close\r\n\r\n" + body;
		s->write(resp);
		s->flush();
		s->disconnectFromHost();
	}
};

// ------------------------------------------------------- the modal answerer --
//
// One repeating poller for both surfaces, rather than a shot armed at a guessed
// delay. The reason is `try_evolve_confirm`'s, recorded in `try_forget`: a
// single shot fired before the window it was aiming at finds nothing, does
// nothing, and leaves the driver blocked in `exec()` with no output at all, so
// the run ends as a shell timeout that says nothing about what went wrong.
//
// It also holds every question up for `hold_ms` before answering, and does the
// two things that have to happen while one is up. A question answered on sight
// is a question that was never up when the second request arrived.
struct modal_state {
	QString file_answer  = QStringLiteral("Don't allow");
	QString proto_answer = QStringLiteral("Not now");
	int     hold_ms      = 0;    // leave a question up this long before answering

	// **A queue, not one path.** A section that asks twice needs a different
	// folder each time: a File System Access grant is remembered per origin and
	// path, so asking again for one already answered can be settled out of
	// Chromium's memory without our prompt running at all.
	QStringList give;
	QString     last_give;

	int    boxes       = 0;      // questions answered
	int    max_at_once = 0;      // questions visible in one poll
	int    choosers    = 0;      // file choosers answered
	bool   box_up      = false;  // a question is on screen right now
	bool   chooser_up  = false;
	qint64 first_seen  = -1;

	// One entry per question answered and per chooser opened, in order, so a
	// section can ask "before or after" rather than only "how many".
	QList<qint64> answered_at;
	QList<qint64> chooser_at;

	// Done by the poller, while a question is up, once.
	bool release_when_up   = false;
	bool click_when_up     = false;
	bool released_with_box = false;
	bool clicked_with_box  = false;
};
static modal_state g_modal;

// The poller acts on these while a nested loop is running, which is the only
// time acting on them means anything. See the header for what happens to code
// that tries it from `main()`.
static local_site  *g_site = nullptr;
static main_window *g_window = nullptr;
static void click_page(main_window &w);

static void poll_modals() {
	int visible = 0;
	QMessageBox *answer = nullptr;
	QFileDialog *chooser = nullptr;
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (!w->isVisible())
			continue;
		if (auto *b = qobject_cast<QMessageBox *>(w)) {
			// **By object name, never by caption.** A caption is user-facing
			// text that changes for reasons nothing to do with a test;
			// `try_evolve_confirm` lost two runs to exactly that.
			const QString n = b->objectName();
			if (n != "file_access_box" && n != "protocol_handler_box")
				continue;
			++visible;
			if (!answer)
				answer = b;
		} else if (auto *d = qobject_cast<QFileDialog *>(w)) {
			chooser = d;
		}
	}
	if (visible > g_modal.max_at_once)
		g_modal.max_at_once = visible;
	g_modal.box_up = visible > 0;

	// **Before the hold and before answering anything**, because this is the
	// state the guard is about and it lasts only as long as the question does.
	if (visible > 0 && g_modal.release_when_up && g_site
	     && !g_site->held.isEmpty()) {
		g_site->release_holds();
		g_modal.released_with_box = true;
		g_modal.release_when_up = false;
	}
	if (visible > 0 && g_modal.click_when_up && g_window) {
		trace("driver: clicking the page with a question on screen");
		click_page(*g_window);
		g_modal.clicked_with_box = true;
		g_modal.click_when_up = false;
	}

	if (chooser) {
		// **Counted on the leading edge, not per poll.** Accepting can decline
		// -- Qt refuses a directory whose path does not exist -- and a counter
		// that ticked once per attempt would report a healthy number for a
		// chooser nobody ever got past.
		const bool edge = !g_modal.chooser_up;
		if (edge) {
			g_modal.chooser_up = true;
			++g_modal.choosers;
			g_modal.chooser_at << g_clock.elapsed();
			trace("engine: a file chooser is up");
			if (!g_modal.give.isEmpty())
				g_modal.last_give = g_modal.give.takeFirst();
		}
		// **Both the directory and the name field**, because which of the two
		// `selectedFiles()` reads depends on the mode Qt asked for: with
		// nothing selected it answers the current root, and with text typed it
		// answers that. Setting both means this does not have to know.
		chooser->setDirectory(g_modal.last_give);
		if (auto *e = chooser->findChild<QLineEdit *>("fileNameEdit"))
			e->setText(g_modal.last_give);
		// **`accept()` is protected on QFileDialog**, so it goes through the
		// meta-object: QDialog declares it a public slot and QFileDialog only
		// overrides it, so the slot is still there and the call lands on the
		// override. Clicking the dialog's own accept button is the other route
		// and it is worse here -- that button is disabled until the dialog
		// agrees the selection is valid, and a click on a disabled button is
		// indistinguishable from one that worked.
		QMetaObject::invokeMethod(chooser, "accept", Qt::DirectConnection);
		return;
	}
	g_modal.chooser_up = false;
	if (!answer) {
		g_modal.first_seen = -1;
		return;
	}
	if (g_modal.first_seen < 0) {
		g_modal.first_seen = g_clock.elapsed();
		trace(QString("engine: %1 is up").arg(answer->objectName()));
	}
	if (g_clock.elapsed() - g_modal.first_seen < g_modal.hold_ms)
		return;

	const QString want = answer->objectName() == "file_access_box"
	                         ? g_modal.file_answer : g_modal.proto_answer;
	trace(QString("driver: answering %1 with \"%2\", up for %3 ms")
	          .arg(answer->objectName(), want)
	          .arg(g_clock.elapsed() - g_modal.first_seen));
	g_modal.answered_at << g_clock.elapsed();
	++g_modal.boxes;
	g_modal.first_seen = -1;
	for (QAbstractButton *b : answer->buttons())
		if (b->text().remove('&').startsWith(want)) {
			b->click();
			return;
		}
	answer->reject();
}

// A real press and release at the middle of the page, which is where the button
// is: it is styled to fill the viewport, so this cannot miss it. The same shape
// `try_extract` uses to click a player.
static void click_page(main_window &w) {
	auto *view = w.findChild<qtwebengine_view *>();
	QWidget *widget = view ? view->widget() : nullptr;
	if (!widget)
		return;
	QWidget *t = widget->focusProxy() ? widget->focusProxy() : widget;
	const QPoint at(t->width() / 2, t->height() / 2);
	QMouseEvent p(QEvent::MouseButtonPress, at, t->mapToGlobal(at),
	               Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent r(QEvent::MouseButtonRelease, at, t->mapToGlobal(at),
	               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(t, &p);
	QApplication::sendEvent(t, &r);
}

// Wait, on a bounded count, for the page to have reported `what`.
static QString wait_report(local_site *site, const QString &what, int ms) {
	for (int waited = 0; waited < ms; waited += 100) {
		spin(100);
		if (site->reported.contains(what))
			return site->reported.value(what);
	}
	return QString();
}

// Load a page mode and wait until its script has run. Every report is dropped
// first, so a key read afterwards belongs to this load: `pick1` from the last
// page looks exactly like `pick1` from this one, and a section that read a
// stale key would report the previous page's answer as this page's.
static bool load_mode(local_site *site, QLineEdit *bar, const char *mode) {
	site->reported.clear();
	site->reported_at.clear();
	bar->setText(QString("http://127.0.0.1:%1/page?mode=%2")
	                 .arg(site->serverPort()).arg(QString(mode)));
	QMetaObject::invokeMethod(bar, "returnPressed");
	return wait_report(site, QStringLiteral("ready"), 30000) == QString(mode);
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// See the header: a native chooser is not a QWidget and cannot be answered
	// from in here, so a run that got one would hang in it and report nothing
	// about the prompt behind it.
	QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	g_clock.start();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);

	// ------------------------------------------------------------ isolation --
	//
	// This driver deletes nothing, so the guard is not `try_forget`'s -- but it
	// does hand a page a real folder on a real disk, and the folder it hands
	// over must be one it made. Both halves are checked before anything is
	// offered to anything.
	section("whose profile and whose folder");
	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT"))
	                        : QString("/tmp/hydra-files");
	QDir(out).removeRecursively();
	// **One folder per ask, never reused.** A File System Access grant is
	// remembered per origin and path, so a second ask for a folder already
	// answered can be settled out of Chromium's memory without our prompt
	// running -- and a section measuring the prompt would be measuring a cache.
	QDir().mkpath(out + "/allowed-folder");
	QDir().mkpath(out + "/refused-folder");
	QDir().mkpath(out + "/held-folder");
	QDir().mkpath(out + "/busy-folder");
	QDir().mkpath(out + "/pair-folder");
	QFile marker(out + "/allowed-folder/inside.txt");
	if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		marker.write("a file for the page to find\n");
		marker.close();
	}

	const QString storage = factory.profile()
	                          ? factory.profile()->persistentStoragePath() : QString();
	std::printf("  profile storage   %s\n", qPrintable(storage));
	std::printf("  offered folders   %s/*-folder\n", qPrintable(out));
	const QString qttest = QDir::homePath() + "/.qttest/";
	const bool isolated = QStandardPaths::isTestModeEnabled()
	                       && !storage.isEmpty() && storage.startsWith(qttest);
	check(isolated, "the browsing profile is under ~/.qttest, not the one a "
	                 "real browser uses");
	const bool own_folders = QDir(out).exists("allowed-folder")
	                          && QDir(out).exists("refused-folder")
	                          && QDir(out).exists("held-folder")
	                          && QDir(out).exists("busy-folder")
	                          && QDir(out).exists("pair-folder");
	check(own_folders,
	      "and every folder about to be offered to a page is one this run made, "
	      "under HYDRA_TEST_OUT");
	if (!isolated || !own_folders) {
		std::printf("\nREFUSING TO RUN.\n%d passed, %d failed\n", g_pass, g_fail);
		return 2;
	}

	local_site site;
	if (!site.listen(QHostAddress::LocalHost, 0)) {
		std::printf("could not listen on loopback\n");
		return 1;
	}
	std::printf("  origin            http://127.0.0.1:%u\n", site.serverPort());

	// ------------------------------------------------------------- the shell --
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return 1;
	tf.write("- [f0] folder | Files\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1000, 700);
	w.show();
	spin(1500);

	auto *tree_view = w.findChild<QTreeView *>();
	emit tree_view->activated(
	  tree_view->model()->index(0, 0, tree_view->model()->index(0, 0)));
	spin(1500);

	QLineEdit *bar = nullptr;
	for (QLineEdit *e : w.findChildren<QLineEdit *>())
		if (e->placeholderText() == "Address")
			bar = e;
	if (!bar) {
		std::printf("NO ADDRESS BAR\n");
		return 1;
	}

	g_site = &site;
	g_window = &w;
	QTimer poller;
	QObject::connect(&poller, &QTimer::timeout, &poll_modals);
	poller.start(100);

	// ------------------------------------------ 0. the counter can count two --
	//
	// Sections 4 and 5 turn on "no second question appeared", and "none
	// appeared" is what a counter that cannot count also reports. So the counter
	// is shown two boxes of this driver's own, wearing the object names it looks
	// for, and required to see both.
	section("0. the question counter, proved against two real boxes");
	{
		g_modal.hold_ms = 4000;      // long enough that both are up together
		QMessageBox a(&w), b(&w);
		a.setObjectName("file_access_box");
		b.setObjectName("protocol_handler_box");
		a.setText("control");
		b.setText("control");
		a.setModal(false);
		b.setModal(false);
		a.show();
		b.show();
		spin(1200);
		const int saw = g_modal.max_at_once;
		a.hide();
		b.hide();
		spin(500);
		check(saw >= 2,
		      QString("two questions on screen at once are counted as two "
		               "(saw %1)").arg(saw));
		g_modal.max_at_once = 0;
		g_modal.first_seen = -1;
		g_modal.hold_ms = 0;
	}

	// ------------------------------------ 1. the page can ask, and the click --
	section("1. the page, the API and the gesture");
	{
		g_modal.give = QStringList{ out + "/allowed-folder" };
		g_modal.file_answer = QStringLiteral("Allow");
		check(load_mode(&site, bar, "pick"),
		      "the page loaded from the loopback origin and ran its script");
		check(site.reported.value("api") == "function",
		      QString("showDirectoryPicker exists in this engine (typeof: %1)")
		          .arg(site.reported.value("api", "(never reported)")));
		if (site.reported.value("api") != "function") {
			note("nothing below can reach the prompt without it");
			std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
			return 1;
		}

		click_page(w);
		const QString gesture = wait_report(&site, QStringLiteral("gesture1"), 10000);
		check(!gesture.isEmpty(),
		      "a synthetic press and release reached the page's click handler");
		check(gesture == "true",
		      QString("and Chromium counted it as a user gesture, which is what "
		               "showDirectoryPicker requires (userActivation.isActive: "
		               "%1)").arg(gesture.isEmpty() ? QStringLiteral("(none)")
		                                             : gesture));
	}

	// ----------------------------------------------- 2. the answer being yes --
	section("2. Allow -- the page gets the folder");
	{
		const QString picked = wait_report(&site, QStringLiteral("pick1"), 30000);
		note(QString("choosers answered: %1, questions answered: %2")
		         .arg(g_modal.choosers).arg(g_modal.boxes));
		check(g_modal.choosers > 0,
		      "the engine opened its file chooser and the driver answered it");
		check(g_modal.boxes > 0,
		      "and our own question came up behind it and was answered");
		check(picked.startsWith("granted:"),
		      QString("the page's promise resolved rather than rejecting "
		               "(page saw: %1)")
		          .arg(picked.isEmpty() ? QStringLiteral("(nothing)") : picked));
		// **The folder by name, not merely a handle.** A grant that resolved
		// with the wrong directory is a worse defect than one that refused, and
		// a check for "it resolved" cannot tell the two apart.
		check(picked == "granted:allowed-folder",
		      QString("naming the folder the chooser was given (page saw: %1)")
		          .arg(picked.isEmpty() ? QStringLiteral("(nothing)") : picked));
	}

	// ------------------------------------------------ 3. the answer being no --
	//
	// A folder of its own, for the reason recorded where the folders are made.
	// The question is required to have been asked *again*, which is what would
	// catch a cached answer if one happened anyway.
	section("3. Don't allow -- the page gets a refusal");
	{
		const int boxes_before = g_modal.boxes;
		const int choosers_before = g_modal.choosers;
		g_modal.give = QStringList{ out + "/refused-folder" };
		g_modal.file_answer = QStringLiteral("Don't allow");
		check(load_mode(&site, bar, "pick"), "the page is loaded again");
		click_page(w);
		const QString picked = wait_report(&site, QStringLiteral("pick1"), 30000);
		check(g_modal.choosers > choosers_before,
		      "the engine opened its file chooser again");
		check(g_modal.boxes > boxes_before,
		      "and our question was asked again rather than answered from "
		      "Chromium's memory of the last one");
		check(picked.startsWith("refused:"),
		      QString("the page's promise rejected (page saw: %1)")
		          .arg(picked.isEmpty() ? QStringLiteral("(nothing)") : picked));
	}

	// ------------------------------------------------- 4. the re-entry guard --
	section("4. a second question, asked while the first is on screen");

	int control_boxes = 0;
	{
		note("4a. control -- one parked request, released with nothing on screen");
		const int before = g_modal.boxes;
		g_modal.hold_ms = 0;
		check(load_mode(&site, bar, "control"), "the page is loaded");
		bool parked = false;
		for (int waited = 0; waited < 15000 && !parked; waited += 100) {
			spin(100);
			parked = !site.held.isEmpty();
		}
		check(parked, "the page's request is parked at the server");
		check(!g_modal.box_up, "and no question is on screen when it is let go");
		site.release_holds();
		check(wait_report(&site, QStringLiteral("asked-control"), 10000) == "yes",
		      "the page asked to register a protocol handler");
		spin(6000);
		control_boxes = g_modal.boxes - before;
		check(control_boxes == 1,
		      QString("and exactly one question appeared for it -- so the route "
		               "works, the counter counts, and a silence below is a "
		               "measurement (questions: %1)").arg(control_boxes));
	}

	{
		note("4b. guarded -- the second request let go while the first is up");
		const int before = g_modal.boxes;
		g_modal.max_at_once = 0;
		g_modal.released_with_box = false;
		// **Armed before the page is loaded, and carried out by the poller.**
		// By the time any line here could notice a question, that question has
		// been answered -- see the header.
		g_modal.release_when_up = true;
		g_modal.hold_ms = 8000;
		const int trace_from = g_trace.size();
		check(load_mode(&site, bar, "guarded"), "the page is loaded");
		// Long enough for: the question to appear, the release, the round trip
		// through the page, the second request, the eight-second hold, and a
		// good while afterwards for anything that was merely queued.
		spin(30000);
		g_modal.release_when_up = false;
		g_modal.hold_ms = 0;
		show_trace(trace_from);

		const qint64 closed_at = g_modal.answered_at.value(before, -1);
		const qint64 asked_at  = site.reported_at.value("asked-second", -1);
		const bool entered = g_modal.released_with_box && asked_at >= 0
		                      && closed_at >= 0 && asked_at < closed_at;

		// True either way, and it is the property a user would notice.
		check(g_modal.max_at_once == 1,
		      QString("two questions were never on screen together (most at "
		               "once: %1)").arg(g_modal.max_at_once));

		if (entered) {
			check(true,
			      "the second request was made while the first question was "
			      "still up, so the guard was entered");
			// **Refused, not queued.** A guard that drops the request leaves
			// nothing behind; an engine that merely delayed it would put the
			// question up the moment the first was answered, and the count
			// taken while the first was up would read the same either way. The
			// wait above runs well past that answer, so one question here is
			// one question ever.
			check(g_modal.boxes - before == 1,
			      QString("and two requests produced one question in total, "
			               "counted past the point where a held-over one would "
			               "have appeared (questions: %1, control was %2)")
			          .arg(g_modal.boxes - before).arg(control_boxes));
		} else {
			// **Said plainly, and nothing asserted.** The trace above is the
			// evidence: between the question appearing and being answered,
			// nothing from the page reached the server at all -- its own
			// `ready` and its parked request both landed within milliseconds of
			// the question closing, having been sent eight seconds earlier. So
			// the request could not be let go into a live question, the branch
			// was never entered, and a check over that silence would be exactly
			// the vacuous pass this section was rebuilt to avoid.
			note(QString("INCONCLUSIVE: the second request could not be made "
			              "while the question was up. Released into a live "
			              "question: %1; the page reported making it at %2 ms, "
			              "and the question was answered at %3 ms.")
			         .arg(g_modal.released_with_box ? "yes" : "no")
			         .arg(asked_at).arg(closed_at));
			note("Nothing here is asserted about the guard. On this Qt and "
			      "Chromium a modal question run from inside the request's own "
			      "signal stops the page's traffic reaching the browser until "
			      "it is answered -- which is the condition the guard exists "
			      "for, and also the condition that makes it unobservable.");
		}
	}

	{
		note("4c. observation -- two requests in one script, nothing parked");
		const int before = g_modal.boxes;
		g_modal.max_at_once = 0;
		g_modal.hold_ms = 4000;
		const int trace_from = g_trace.size();
		check(load_mode(&site, bar, "inarow"), "the page is loaded");
		check(wait_report(&site, QStringLiteral("asked-rowtwo"), 10000) == "yes",
		      "the page made both requests inside one script");
		spin(14000);
		g_modal.hold_ms = 0;
		spin(3000);
		show_trace(trace_from);
		// Reported, not asserted. Two questions here is not a defect and one is
		// not a pass: it says only whether the browser process delivered the
		// second request before or after the first was answered, which is what
		// makes this shape useless as evidence about the guard.
		note(QString("questions answered: %1, most on screen at once: %2 -- "
		              "which of those comes out depends on when the second "
		              "request was delivered, and that is the browser process's "
		              "scheduling rather than the guard")
		         .arg(g_modal.boxes - before).arg(g_modal.max_at_once));
		check(g_modal.max_at_once <= 1,
		      QString("two questions were never on screen together (most at "
		               "once: %1)").arg(g_modal.max_at_once));
	}

	// -------------------------------------------- 5. the guard on this route --
	//
	// 4b aimed the flag at a *protocol handler* request. This aims it at a
	// **file** request -- the branch in the `fileSystemAccessRequested` handler
	// itself -- which needs a second user gesture while a modal box holds a
	// nested event loop. Only the driver can produce that, and only if Qt lets a
	// sent mouse event through to a widget behind a modal. Both halves are
	// measured rather than assumed.
	section("5. a file request arriving while our own question is up");
	{
		const int boxes_before = g_modal.boxes;
		const int choosers_before = g_modal.choosers;
		g_modal.max_at_once = 0;
		// One folder for each of the two asks, in the order they happen.
		g_modal.give = QStringList{ out + "/held-folder", out + "/busy-folder" };
		g_modal.file_answer = QStringLiteral("Don't allow");
		g_modal.clicked_with_box = false;
		g_modal.click_when_up = true;    // the poller does it, not this code
		g_modal.hold_ms = 15000;
		const int trace_from = g_trace.size();
		check(load_mode(&site, bar, "pick"), "the page is loaded");
		click_page(w);
		spin(45000);
		g_modal.click_when_up = false;
		g_modal.hold_ms = 0;
		spin(6000);
		show_trace(trace_from);

		check(!site.reported.value("gesture1").isEmpty(),
		      "the first click landed and the page asked");
		check(g_modal.clicked_with_box,
		      "and a second click was sent while our question was on screen");

		const QString gesture = site.reported.value("gesture2");
		const qint64 closed_at = g_modal.answered_at.value(boxes_before, -1);
		const qint64 second_chooser_at =
		  g_modal.chooser_at.value(choosers_before + 1, -1);
		// The chooser comes first and `fileSystemAccessRequested` follows it, so
		// a second chooser opening before the first question was answered is
		// what would say a second file request reached the handler while the
		// flag was set.
		const bool entered = !gesture.isEmpty() && second_chooser_at >= 0
		                      && closed_at >= 0 && second_chooser_at < closed_at;

		note(QString("choosers this section: %1, questions: %2, most at once: "
		              "%3, second click's activation: %4")
		         .arg(g_modal.choosers - choosers_before)
		         .arg(g_modal.boxes - boxes_before)
		         .arg(g_modal.max_at_once)
		         .arg(gesture.isEmpty() ? QStringLiteral("(never reported)")
		                                 : gesture));
		check(g_modal.max_at_once == 1,
		      QString("two questions were never on screen together (most at "
		               "once: %1)").arg(g_modal.max_at_once));

		if (entered) {
			check(gesture == "true",
			      QString("the second click counted as a user gesture, which is "
			               "what the second ask needed (userActivation.isActive: "
			               "%1)").arg(gesture));
			check(site.reported.value("pick2").startsWith("refused:"),
			      QString("the page's second ask was refused rather than left "
			               "waiting (page saw: %1)")
			          .arg(site.reported.value("pick2", "(nothing)")));
			check(g_modal.boxes - boxes_before == 1,
			      QString("one question for two file requests, counted past the "
			               "answer to the first -- so the second was refused by "
			               "the guard rather than held over (questions: %1)")
			          .arg(g_modal.boxes - boxes_before));
		} else {
			note(QString("INCONCLUSIVE: the second file request did not reach "
			              "the engine while our question was up. The click went "
			              "in at the right moment -- the trace shows it -- but "
			              "the page did not report having seen it until %1 ms, "
			              "the second chooser opened at %2 ms, and the first "
			              "question had been answered at %3 ms.")
			         .arg(site.reported_at.value("gesture2", -1))
			         .arg(second_chooser_at).arg(closed_at));
			note("So the branch in the file handler was not entered and nothing "
			      "here is asserted about it. What the trace does establish is "
			      "that a page cannot ask again while the question is up: "
			      "everything it tried to send arrived within milliseconds of "
			      "the question closing.");
		}
	}

	// ------------------------------------------------ what Chromium refuses --
	//
	// The obvious way to ask twice, recorded because it is the first thing
	// anybody will try and it does not work: a second `showDirectoryPicker()`
	// inside the same gesture never reaches the browser at all.
	section("6. two file pickers inside one gesture");
	{
		g_modal.give = QStringList{ out + "/pair-folder" };
		g_modal.file_answer = QStringLiteral("Don't allow");
		g_modal.hold_ms = 0;
		check(load_mode(&site, bar, "double"), "the page is loaded");
		click_page(w);
		const QString same = wait_report(&site, QStringLiteral("same-gesture"), 30000);
		check(!same.isEmpty(),
		      "the second call answered the page rather than hanging");
		note(QString("it answered: %1")
		         .arg(same.isEmpty() ? QStringLiteral("(nothing)") : same));
		wait_report(&site, QStringLiteral("pick1"), 20000);
		spin(3000);
	}

	poller.stop();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

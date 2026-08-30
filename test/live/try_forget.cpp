//
// Clearing browsing data, driven from the button somebody presses down to the
// cookie a server stops being sent -- and the one path that must clear
// nothing.
//
// The factory half of this feature was already measured against a real engine.
// Four things around it were not, and they are what this file is for: the
// settings dialog's wiring to the factory, kiosk's `clear_between_sessions` in
// a live session, the idle timer that is the third and least-watched of the
// three moments that setting clears on, and the guard in
// `main_window::toggle_kiosk()` that switches that flag off again when the
// request came from a *page* asking for fullscreen rather than from an
// operator setting up a screen.
//
// **The measurement is the `Cookie:` header a local server was sent**, never a
// return value, a `clear_report` or the label the dialog writes. Every one of
// those is the browser reporting on itself, and this whole feature exists
// because a store that quietly did not empty looks exactly like one that did.
// A cookie the server stops receiving is the browser's claim checked against
// somebody else's record.
//
// **`shell_fixture` does not suit this one**, and the reason is not style: it
// serves its pages from `file://`, which Chromium gives an opaque origin and
// no cookie jar at all. Anything about cookies needs an http origin, so the
// server below is the fixture.
//
// ## The negative is the valuable check, so it is built to be able to fail
//
// Section 3 asserts that nothing was cleared, and an assertion that nothing
// happened passes just as loudly when the code under it never ran, when the
// probe cannot see, and when the cookie was never there to begin with. So it
// is arranged so that each of those would show up as a failure somewhere else
// in the same run:
//
//   * the cookie is re-established and *observed coming back* immediately
//     before the section, so "no cookie died" is not "no cookie existed";
//   * section 2 runs the same probe over the same server against a path that
//     genuinely clears, so the probe is known to be capable of a positive
//     minutes earlier in the same process;
//   * kiosk logs which moment it cleared on, and section 2 must be seen to
//     produce those lines before section 3's silence is allowed to count as
//     evidence -- a log nobody is reading is silent whatever happens;
//   * and the fullscreen path is asserted to have actually entered kiosk
//     (the shell hides its window when it does), because a guard that was
//     never reached is not a guard that held.
//
// ## Which clear was observed is a different question from whether one happened
//
// Section 4 is the idle timer, and all three of kiosk's clearing moments call
// the same function on the same stores, so a section that cannot say which of
// them it saw says nothing about the timer. Three things keep them apart there,
// and none of them is a delay chosen by eye:
//
//   * the cookie is put back only after the *entering* clear has been seen to
//     finish -- waited for by its own log line -- so the clear that ran before
//     the measurement cannot be the one that ends it;
//   * every reading is taken while kiosk is still up, so the leaving clear has
//     not happened yet;
//   * and the log slice covering the measurement is required to hold
//     `cleared on idle` and neither of the other two.
//
// ## How the modal surfaces are driven
//
// Both the settings dialog and its confirmation are `exec()`d, so each blocks
// whoever opened it. The established shape here -- `try_confirm`, `try_look`,
// `try_downloads` -- is to arm a `QTimer::singleShot` *before* the call that
// blocks and find the window in `QApplication::topLevelWidgets()`. That
// nests: the timer that drives the settings dialog arms another one for the
// confirmation before it presses Clear, because pressing Clear blocks too.
//
// The page-fullscreen path is entered by emitting the backend's own
// `fullscreen_requested` signal, which is the line the engine emits when a
// page asks. That covers the connection in `main_window` and everything below
// it; what it does not cover is Chromium deciding to emit it, since a real
// `requestFullscreen()` needs a user gesture and there is no synthetic input
// here -- the same limit `test/README.md` records for every driver.
#include "kiosk_controller.h"
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "qtwebengine_view.h"
#include "request_filter.h"
#include "settings_dialog.h"   // settings_store
#include "web_view_factory.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTreeView>
#include <QWebEngineProfile>

#include <cstdio>
#include <functional>
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

// ------------------------------------------------------------ the log probe --
//
// `kiosk_controller::forget_session` names the moment it cleared on --
// entering, idle, leaving -- and sends it to the log, because nobody is
// watching a kiosk. That makes the log the only place the *moment* is
// observable from outside, which is exactly what section 3 needs: the cookie
// header says whether anything went, and this says which of the three
// occasions asked.
//
// **It is only trusted after it has been seen to fire.** A handler that is
// never called and a kiosk that never cleared produce the same empty list, so
// section 2 asserts the lines arrive before section 3 reads anything into
// their absence.
static QStringList  g_kiosk_log;
static QtMessageHandler g_previous_handler = nullptr;

// Wait, on a bounded count, for a kiosk line containing `what` to arrive
// **after** `from`. The index is not decoration: every section leaves its lines
// behind, so a search over the whole list finds section 2's `cleared on
// entering` and reports section 4's as having arrived before it was asked for.
//
// Polled rather than slept for the reason the modal waits below give, and one
// more that belongs to this feature: the clear is asynchronous and its report
// is written from the callback, so a fixed delay is a guess about how long
// emptying three stores takes on a machine under load. Guessed short it reports
// an absence that is impatience; guessed long it is time added to every run.
static bool wait_for_kiosk_line(int from, const QString &what, int ms) {
	for (int waited = 0; waited < ms; waited += 100) {
		spin(100);
		if (!g_kiosk_log.mid(from).filter(what).isEmpty())
			return true;
	}
	return false;
}

static void capture_kiosk_log(QtMsgType type, const QMessageLogContext &ctx,
                               const QString &msg) {
	if (msg.startsWith("kiosk:"))
		g_kiosk_log << msg;
	// **Passed on, never swallowed.** `qInstallMessageHandler` answers with a
	// null pointer when what it replaced was Qt's own default, so a handler
	// that only forwards to its predecessor silences every warning the engine
	// produces -- and a driver that has gone quiet about the engine looks
	// exactly like one whose engine had nothing to say.
	if (g_previous_handler)
		g_previous_handler(type, ctx, msg);
	else
		std::fprintf(stderr, "%s\n",
		              qPrintable(qFormatLogMessage(type, ctx, msg)));
}

// --------------------------------------------------------------- the origin --
//
// One loopback host, two paths. `/set` hands out the cookie; `/check` sets
// nothing and exists only to report the `Cookie:` header it was sent. Keeping
// them apart is what makes a reading unambiguous: a path that both sets and
// reports cannot distinguish "the jar still has it" from "this response just
// put it back".
class local_site : public QTcpServer {
public:
	struct request {
		QString path;
		QString cookie;      // "none" when the browser sent no Cookie header
	};
	QList<request> seen;

protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		// One request per connection, and the state dies with the socket. A
		// `readyRead` can deliver a partial header, so the bytes are collected
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
		const QByteArray path = (first < 0 || second < 0)
		                            ? QByteArray() : head.mid(first + 1,
		                                                       second - first - 1);

		QByteArray cookie = "none";
		for (const QByteArray &line : head.split('\n'))
			if (line.toLower().startsWith("cookie:"))
				cookie = line.mid(7).trimmed();
		seen << request{ QString::fromUtf8(path), QString::fromUtf8(cookie) };

		// Persistent rather than a session cookie, because what this feature
		// deletes is "every login the browser is holding" and those survive a
		// restart. A session cookie would be cleared by things other than the
		// clear.
		const QByteArray set = path.startsWith("/set")
		    ? QByteArray("\r\nSet-Cookie: sess=1; Path=/; Max-Age=3600") : QByteArray();
		const QByteArray body = "<!doctype html><html><body>" + path
		                         + "</body></html>";
		const QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html"
		                         + set
		                         + "\r\nCache-Control: no-store"
		                           "\r\nContent-Length: "
		                         + QByteArray::number(body.size())
		                         + "\r\nConnection: close\r\n\r\n" + body;
		s->write(resp);
		s->flush();
		s->disconnectFromHost();
	}
};

// Navigate the address bar and answer with the `Cookie:` header the server was
// sent **for this exact request**.
//
// Every visit carries its own marker in the query, and the answer is looked up
// by it. Without that a reading could come from a request the shell made for
// its own reasons -- kiosk reloads its home page on entering, which is the
// same url -- and the driver would be reporting on a navigation it did not
// make.
static int g_visit = 0;
static QString next_marker() { return QString("v%1").arg(++g_visit); }

static QUrl marked_url(local_site *site, const char *path,
                        const QString &marker) {
	return QUrl(QString("http://127.0.0.1:%1%2?%3")
	                .arg(site->serverPort()).arg(QString(path), marker));
}

static QString await_request(local_site *site, const QString &marker) {
	for (int waited = 0; waited < 15000; waited += 100) {
		spin(100);
		// `endsWith`, not `contains`: the marker is the whole query and the
		// tenth visit's `v1` is a prefix of the eleventh's `v11`.
		for (const local_site::request &r : std::as_const(site->seen))
			if (r.path.endsWith(marker))
				return r.cookie;
	}
	return QStringLiteral("(no request arrived)");
}

static QString visit(local_site *site, QLineEdit *bar, const char *path) {
	const QString marker = next_marker();
	bar->setText(marked_url(site, path, marker).toString());
	QMetaObject::invokeMethod(bar, "returnPressed");
	return await_request(site, marker);
}

// The same reading, taken by loading the view directly, and it exists for one
// reason: **the address bar cannot be used while kiosk holds the view.**
// Entering removes the widget from the stack, so `main_window::current_view()`
// answers nothing, and `navigate_to_address` deliberately opens a *new tab* in
// that case. A section that measured cookies through a tab kiosk is not
// presenting would be reporting on the wrong page, and would look exactly like
// one that worked.
static QString visit_view(local_site *site, web_view_backend *view,
                           const char *path) {
	const QString marker = next_marker();
	view->load(marked_url(site, path, marker));
	return await_request(site, marker);
}

// Put the cookie back and confirm it comes back, so that a later "it is gone"
// is a deletion rather than an absence. Answers whether the jar really held it.
static bool establish_cookie(local_site *site, QLineEdit *bar) {
	visit(site, bar, "/set");
	return visit(site, bar, "/check").contains("sess=1");
}

static bool establish_cookie(local_site *site, web_view_backend *view) {
	visit_view(site, view, "/set");
	return visit_view(site, view, "/check").contains("sess=1");
}

// The dialog the shell opens, found by what it carries rather than by its
// title: this is the one with a Clear button. A caption is user-facing text and
// changes for reasons that have nothing to do with a test -- `try_evolve_confirm`
// lost two runs to exactly that.
static QDialog *clear_dialog() {
	for (QWidget *w : QApplication::topLevelWidgets()) {
		auto *d = qobject_cast<QDialog *>(w);
		if (!d || !d->isVisible() || qobject_cast<QMessageBox *>(d))
			continue;
		if (d->findChild<QPushButton *>("clear_now"))
			return d;
	}
	return nullptr;
}

// Both of the waits below **poll for a bounded number of tries and then give
// up by closing whatever is up**, rather than firing once at a guessed delay.
//
// The reason is a specific failure and not tidiness: a driver that arms a
// single shot too early finds nothing, does nothing, and is then blocked in
// `exec()` for ever with no output at all, so the run ends as a shell timeout
// that says nothing about what went wrong. `try_evolve_confirm` records losing
// two runs to exactly that. The give-up branch turns a missed window into a
// failed check, which is a result.

// The settings dialog, once the shell has it on screen. Captured by value:
// this returns long before the dialog exists.
static void when_clear_dialog_is_up(const std::function<void(QDialog *)> &action,
                                     int tries = 40) {
	QTimer::singleShot(500, [action, tries] {
		if (QDialog *d = clear_dialog()) {
			action(d);
			return;
		}
		if (tries > 0) {
			when_clear_dialog_is_up(action, tries - 1);
			return;
		}
		for (QWidget *w : QApplication::topLevelWidgets()) {
			auto *d = qobject_cast<QDialog *>(w);
			if (d && d->isVisible()) {
				d->reject();
				return;
			}
		}
	});
}

// Answer the confirmation, which is up only while the press that raised it is
// still on the stack.
static void answer_confirmation(bool *saw, const QString &button_text,
                                 int tries = 40) {
	QTimer::singleShot(250, [saw, button_text, tries] {
		for (QWidget *w : QApplication::topLevelWidgets()) {
			auto *box = qobject_cast<QMessageBox *>(w);
			if (!box || !box->isVisible())
				continue;
			if (saw)
				*saw = true;
			for (QAbstractButton *b : box->buttons())
				if (b->text().remove('&').startsWith(button_text)) {
					b->click();
					return;
				}
			box->reject();
			return;
		}
		if (tries > 0)
			answer_confirmation(saw, button_text, tries - 1);
	});
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// The kiosk report is `qInfo`, and an info line the logging rules have
	// switched off never reaches a message handler at all. Asking for it
	// explicitly means the probe's silence in section 3 is about the kiosk
	// rather than about the filter -- and section 2 proves the ask worked.
	QLoggingCategory::setFilterRules(QStringLiteral("default.info=true"));
	g_previous_handler = qInstallMessageHandler(capture_kiosk_log);

	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	// ------------------------------------------------------------ isolation --
	//
	// **Checked before anything is asked to delete, and fatal when it fails.**
	// This driver's whole job is to empty cookie jars, and the person running
	// it has a real browser with real logins in one. `live/live_paths.cpp` is
	// linked into every live driver and puts `QStandardPaths` into test mode
	// before `main()`, which moves the profile under `~/.qttest` -- but a
	// driver that trusted that without looking would delete somebody's logins
	// on the day the linking rule was changed. So it is measured here, on the
	// profile object that is actually about to be cleared, and a run that
	// cannot prove it is pointed somewhere disposable does not start.
	section("whose profile this is");
	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);

	const QString app_data = QStandardPaths::writableLocation(
	  QStandardPaths::AppDataLocation);
	const QString storage = factory.profile()
	                          ? factory.profile()->persistentStoragePath() : QString();
	const QString cache = factory.profile()
	                        ? factory.profile()->cachePath() : QString();
	std::printf("  application data  %s\n", qPrintable(app_data));
	std::printf("  profile storage   %s\n", qPrintable(storage));
	std::printf("  profile cache     %s\n", qPrintable(cache));

	const QString qttest = QDir::homePath() + "/.qttest/";
	const bool isolated = QStandardPaths::isTestModeEnabled()
	                       && !storage.isEmpty() && storage.startsWith(qttest)
	                       && !cache.isEmpty() && cache.startsWith(qttest);
	check(isolated,
	      "the profile about to be cleared is under ~/.qttest, not the one a "
	      "real browser uses");
	if (!isolated) {
		std::printf("\nREFUSING TO RUN: this would clear cookies out of a "
		             "profile that is not disposable.\n"
		             "%d passed, %d failed\n", g_pass, g_fail);
		return 2;
	}

	// -------------------------------------------------------------- the shell --
	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT"))
	                        : QString("/tmp/hydra-forget");
	QDir(out).removeRecursively();
	QDir().mkpath(out);

	// A kiosk configuration of this driver's own, written the way the settings
	// dialog writes it. `clear_between_sessions` is left ON for both kiosk
	// sections: sections 2 and 3 differ only in which route reaches the
	// controller, which is the whole claim being checked.
	kiosk_config kiosk = settings_store::kiosk();
	kiosk.clear_between_sessions = true;
	kiosk.home = QUrl();          // blank home means "whatever tab you were on"
	// **Off for sections 2 and 3, and section 4 turns it on.** It is the third
	// clearing moment, and one firing underneath the other two would leave
	// neither of them attributable -- which is the same reason section 4 has to
	// work to exclude the other two.
	kiosk.idle_reset_seconds = 0;
	settings_store::set_kiosk(kiosk);

	local_site site;
	if (!site.listen(QHostAddress::LocalHost, 0)) {
		std::printf("could not listen on loopback\n");
		return 1;
	}
	std::printf("  origin            http://127.0.0.1:%u\n", site.serverPort());

	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return 1;
	tf.write("- [f0] folder | Forget\n"
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

	// ------------------------------------------------- 0. the positive control --
	//
	// Everything after this is "the cookie is gone" or "the cookie is still
	// here", and neither means anything until the cookie is known to make the
	// return journey at all.
	section("0. a cookie is stored and comes back");
	{
		const QString on_set = visit(&site, bar, "/set");
		note(QString("the request that set it carried: %1").arg(on_set));
		const QString on_check = visit(&site, bar, "/check");
		check(on_check.contains("sess=1"),
		      QString("the next request to the same origin carries it back "
		               "(server saw: %1)").arg(on_check));
		if (!on_check.contains("sess=1")) {
			std::printf("\nnothing below can measure anything without this.\n"
			             "%d passed, %d failed\n", g_pass, g_fail);
			return 1;
		}
	}

	// ------------------------------------- 1. the button, end to end --
	section("1. the settings dialog's Clear now button");
	{
		bool saw_dialog = false, saw_confirmation = false, pressed = false;
		bool ticked_all = false;
		QString reported;

		// Armed before the call that blocks; `open_settings` exec()s.
		when_clear_dialog_is_up([&](QDialog *d) {
			saw_dialog = true;

			auto *cookies = d->findChild<QCheckBox *>("clear_cookies");
			auto *cache_box = d->findChild<QCheckBox *>("clear_cache");
			auto *links = d->findChild<QCheckBox *>("clear_visited_links");
			auto *go = d->findChild<QPushButton *>("clear_now");
			ticked_all = cookies && cache_box && links && go;
			if (!ticked_all) {
				d->reject();
				return;
			}
			cookies->setChecked(true);
			cache_box->setChecked(true);
			links->setChecked(true);

			// Arm the answer first: the press below does not return until the
			// confirmation has been dealt with.
			answer_confirmation(&saw_confirmation, "Clear");
			pressed = true;
			go->click();

			// None of the three stores empties synchronously, so the report
			// lands after the press returns. Wait for it here, while the
			// dialog is still up and its label can still be read.
			spin(6000);
			for (QLabel *l : d->findChildren<QLabel *>())
				if (l->text().startsWith("Cookies:"))
					reported = l->text();

			// Cancel rather than OK: everything else on this page is pending
			// until OK and this driver changed a checkbox, so leaving by the
			// undo is what keeps the clear the only thing that happened.
			d->reject();
		});
		QMetaObject::invokeMethod(&w, "open_settings");
		spin(500);

		check(saw_dialog, "the shell's settings dialog offers a Clear now button");
		check(ticked_all,
		      "with the three checkboxes the privacy page is documented to have");
		check(saw_confirmation,
		      "pressing it asks for confirmation rather than deleting on a click");
		check(pressed, "and the button was actually pressed");
		if (!reported.isEmpty())
			note(QString("the dialog reported: %1")
			         .arg(QString(reported).replace('\n', " | ")));
		else
			note("the dialog wrote no report into its label");

		const QString after = visit(&site, bar, "/check");
		check(!after.contains("sess=1"),
		      QString("and the server is no longer sent the cookie "
		               "(server saw: %1)").arg(after));
	}

	// ------------------------------------------- 2. kiosk, configured to forget --
	section("2. kiosk with clear_between_sessions on");
	int log_before_kiosk = 0;
	{
		check(establish_cookie(&site, bar),
		      "the cookie is back, so there is something for kiosk to delete");
		log_before_kiosk = g_kiosk_log.size();

		QMetaObject::invokeMethod(&w, "toggle_kiosk");
		spin(5000);
		// The shell hides itself when the stage goes up. If this is false the
		// controller was never entered and nothing below measures kiosk.
		const bool entered = !w.isVisible();
		check(entered, "kiosk was entered (the shell hides for the stage)");

		QMetaObject::invokeMethod(&w, "toggle_kiosk");
		spin(5000);
		check(w.isVisible(), "and left again, with the window back");

		const QStringList said = g_kiosk_log.mid(log_before_kiosk);
		for (const QString &line : said)
			note(line);
		check(said.filter("cleared on entering").size() > 0,
		      "the controller says it cleared on entering");
		check(said.filter("cleared on leaving").size() > 0,
		      "and again on leaving");

		const QString after = visit(&site, bar, "/check");
		check(!after.contains("sess=1"),
		      QString("and the server is no longer sent the cookie "
		               "(server saw: %1)").arg(after));
	}

	// The two things section 3 is about to read silence from. Neither is
	// allowed to be believed on its own.
	const QStringList kiosk_said = g_kiosk_log.mid(log_before_kiosk);
	const bool probe_saw_a_clear = kiosk_said.filter("cleared on").size() > 0;

	// ---------------------------- 3. a page asking for fullscreen must not clear --
	section("3. page-requested fullscreen, with the same setting still on");
	{
		note("the stored kiosk config still says clear_between_sessions=true; "
		      "only the route into the controller differs from section 2");
		check(settings_store::kiosk().clear_between_sessions,
		      "confirmed by reading it back out of the store");
		check(establish_cookie(&site, bar),
		      "the cookie is back, so silence below is not an empty jar");

		auto *view = w.findChild<qtwebengine_view *>();
		check(view != nullptr, "the page's backend is reachable");
		if (!view) {
			std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
			return 1;
		}

		const int log_before = g_kiosk_log.size();

		// The engine's own signal, which is the line `qtwebengine_view` emits
		// when a page calls requestFullscreen. Emitting it here exercises the
		// connection in main_window, present_fullscreen, and the guard inside
		// toggle_kiosk; what it does not exercise is Chromium deciding to emit
		// it, which needs a user gesture no driver here can produce.
		emit view->fullscreen_requested(true);
		spin(5000);
		const bool entered = !w.isVisible();
		check(entered,
		      "the page got the screen -- so the guard was reached rather than "
		      "the whole path being skipped");

		emit view->fullscreen_requested(false);
		spin(5000);
		check(w.isVisible(), "and the window came back when the page let go");

		const QStringList said = g_kiosk_log.mid(log_before);
		for (const QString &line : said)
			note(line);
		if (probe_saw_a_clear)
			check(said.filter("cleared on").isEmpty(),
			      "the controller cleared nothing on this route -- and the same "
			      "probe reported two clears in section 2, so its silence here "
			      "is a measurement");
		else
			note("INCONCLUSIVE: the log probe never fired in section 2 either, "
			      "so its silence here measures the probe rather than the guard");

		const QString after = visit(&site, bar, "/check");
		check(after.contains("sess=1"),
		      QString("and the cookie survived a page-requested fullscreen "
		               "(server saw: %1)").arg(after));
	}

	// ---------------------------- 4. kiosk's idle timer, the third moment --
	//
	// Entering and leaving are section 2's. This is the one between them, and it
	// is the one an unattended screen actually runs on: nobody presses anything
	// when they walk away, so the timer is the only signal a kiosk gets that a
	// session has ended, and every other moment needs an operator standing
	// there. It had never been run.
	//
	// **Two halves, and either one alone looks like a working kiosk.** The
	// controller issues the navigation home first and clears beside it, on the
	// stated ground that a screen must not sit on a stranger's page while a
	// store empties. A timer that walks home and forgets nothing looks right
	// from in front of the screen and leaves the last person's logins on the
	// disk; one that forgets and does not walk home leaves their page up. So
	// both are measured -- the navigation as a request the server was sent for
	// a path used nowhere else in this run, the forgetting as the `Cookie:`
	// header the same server stops being sent.
	section("4. kiosk's idle timer, the moment nobody presses anything");
	{
		// Long enough that the entering clear and the re-establishment below
		// finish inside it with room to spare, and the room is asserted rather
		// than assumed a few lines down. Short enough to be worth waiting for.
		const int idle_seconds = 20;

		// A home of kiosk's own, on the same server. `/home` is asked for
		// nowhere else in this driver, so a request for it is the timer's
		// doing: the only other thing that loads home is the watchdog, and that
		// wants a dead render process.
		const QString home_mark = QStringLiteral("idle-home");
		kiosk_config idle_kiosk = settings_store::kiosk();
		idle_kiosk.clear_between_sessions = true;
		idle_kiosk.idle_reset_seconds = idle_seconds;
		idle_kiosk.home = QUrl(QString("http://127.0.0.1:%1/home?%2")
		                           .arg(site.serverPort()).arg(home_mark));
		settings_store::set_kiosk(idle_kiosk);

		auto *view = w.findChild<qtwebengine_view *>();
		check(view != nullptr, "the presented view is reachable");
		if (!view) {
			std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
			return 1;
		}

		int home_seen = 0;
		for (const local_site::request &r : std::as_const(site.seen))
			if (r.path.endsWith(home_mark))
				++home_seen;
		check(home_seen == 0,
		      "nothing has asked the server for the kiosk home page yet, so a "
		      "request for it below is new");

		const int log_before = g_kiosk_log.size();
		QElapsedTimer since_entry;
		since_entry.start();
		QMetaObject::invokeMethod(&w, "toggle_kiosk");
		spin(2000);
		check(!w.isVisible(), "kiosk was entered (the shell hides for the stage)");

		// **The setting reached the controller by the route it ships on** --
		// the store, then `toggle_kiosk`. Without this the section could arm
		// nothing and spend its whole wait measuring a timer that was never
		// started, which is indistinguishable from one that never fires.
		auto *controller = w.findChild<kiosk_controller *>();
		check(controller
		       && controller->config().idle_reset_seconds == idle_seconds,
		      QString("the controller is holding the idle reset the store was "
		               "given (%1 s)").arg(idle_seconds));

		// **Waited for, not slept through, and it is the load-bearing step of
		// the whole section.** Entering clears too. Until that clear has been
		// seen to *finish*, a cookie set afterwards might still be inside its
		// reach, and the deletion measured below would be attributable to
		// entering rather than to the timer.
		//
		// It is also this section's own proof that the log probe can see a
		// clear: the silence tests further down are worth nothing without a
		// positive from the same probe in the same section.
		check(wait_for_kiosk_line(log_before, "cleared on entering", 25000),
		      "the entering clear ran and finished -- so the probe below can "
		      "see a clear, and nothing it reports later is this one");

		check(establish_cookie(&site, view),
		      "the cookie is back, set inside kiosk and after the entering "
		      "clear finished, so its disappearance can only be later");

		const qint64 spent = since_entry.elapsed();
		note(QString("%1 ms of the %2 ms idle window spent reaching that point")
		         .arg(spent).arg(idle_seconds * 1000));
		check(spent + 5000 < idle_seconds * 1000,
		      "with at least five seconds of the window still to run, so the "
		      "clear below is the timer firing and not a race with it");

		// Everything from here is read out of the two slices these mark.
		const int log_at_mark  = g_kiosk_log.size();
		const int seen_at_mark = site.seen.size();

		check(wait_for_kiosk_line(log_at_mark, "cleared on idle",
		                           idle_seconds * 1000 + 20000),
		      "the idle timer fired and the controller cleared on it");

		const QStringList said = g_kiosk_log.mid(log_at_mark);
		for (const QString &line : said)
			note(line);
		check(said.filter("cleared on entering").isEmpty()
		       && said.filter("cleared on leaving").isEmpty(),
		      "and neither of the other two moments cleared in the same "
		      "window, so what follows has one candidate");

		// Half one: the screen walked back to the home page. Polled, because
		// the navigation is issued first but the request lands when it lands,
		// and the clear's report can beat it.
		int home_requests = 0;
		QString home_cookie;
		for (int waited = 0; waited < 15000 && home_requests == 0; waited += 100) {
			spin(100);
			for (int i = seen_at_mark; i < site.seen.size(); ++i)
				if (site.seen[i].path.endsWith(home_mark)) {
					++home_requests;
					if (home_cookie.isEmpty())
						home_cookie = site.seen[i].cookie;
				}
		}
		check(home_requests > 0,
		      QString("the screen walked back to the kiosk home page, measured "
		               "as a request the server was sent (%1 of them)")
		          .arg(home_requests));
		// **Not a check, and the measurement contradicted the guess**, which is
		// why it is written down. "The navigation is issued first" is about
		// what the screen *shows* -- `load()` posts to the render process and
		// returns -- and it promises nothing about which of two asynchronous
		// things reaches the wire first. Measured here the clear won: the walk
		// home went out carrying no cookie, so the kiosk's own request to its
		// home page is not authenticated as the person who has just left. That
		// is the better of the two outcomes and it is still a race, so it is
		// reported rather than asserted; a driver that asserted it would fail
		// on a slower machine for no defect.
		if (home_requests > 0)
			note(QString("that request carried: %1").arg(home_cookie));

		// Half two: the store was emptied. Read while kiosk is still up, which
		// is what keeps the leaving clear out of it.
		const QString after = visit_view(&site, view, "/check");
		check(!after.contains("sess=1"),
		      QString("and the server is no longer sent the cookie, with kiosk "
		               "still up and only the idle clear behind us "
		               "(server saw: %1)").arg(after));

		QMetaObject::invokeMethod(&w, "toggle_kiosk");
		spin(5000);
		check(w.isVisible(), "and the window came back when kiosk was left");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

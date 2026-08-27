// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clearing browsing data, driven from the button somebody presses down to the
// cookie a server stops being sent -- and the one path that must clear
// nothing.
//
// The factory half of this feature was already measured against a real engine.
// Three things around it were not, and they are what this file is for: the
// settings dialog's wiring to the factory, kiosk's `clear_between_sessions` in
// a live session, and the guard in `main_window::toggle_kiosk()` that switches
// that flag off again when the request came from a *page* asking for
// fullscreen rather than from an operator setting up a screen.
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
static QString visit(local_site *site, QLineEdit *bar, const char *path) {
	const QString marker = QString("v%1").arg(++g_visit);
	bar->setText(QString("http://127.0.0.1:%1%2?%3")
	                 .arg(site->serverPort()).arg(QString(path), marker));
	QMetaObject::invokeMethod(bar, "returnPressed");
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

// Put the cookie back and confirm it comes back, so that a later "it is gone"
// is a deletion rather than an absence. Answers whether the jar really held it.
static bool establish_cookie(local_site *site, QLineEdit *bar) {
	visit(site, bar, "/set");
	return visit(site, bar, "/check").contains("sess=1");
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
	kiosk.idle_reset_seconds = 0;  // the idle timer is a third clearing moment
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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

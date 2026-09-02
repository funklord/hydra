// Driving the screen-share path end to end, without a person at a screen.
//
// The fault this was written for was reported as "desktop sharing at least
// shows the picker, but then, no error, nothing" -- and the reason it took a
// round trip to the copyright holder to learn anything is that there was no
// way to run it here. That is the actual defect: a path with four places to
// stop, three of which look identical from the page, and no driver over any
// of them.
//
// So the cases are cut where the *dialogs* differ rather than where the
// outcomes do. `screen_share` at `allow` reaches the picker with no prompt in
// front of it; at `ask` it opens the prompt first and the picker inside it.
// Same request, same selection, one modal event loop or two -- which makes
// the pair a measurement of the nesting rather than an argument about it. If
// `allow` hands over a stream and `ask` does not, the difference is the only
// thing that changed.
#include "shell_fixture.h"
#include "policy.h"
#include "policy_engine.h"
#include "qtwebengine_view.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QFile>
#include <QListView>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <cstdio>

using shell::check;
using shell::section;
using shell::spin;

namespace {

// A page that asks on click and writes down what it got. **On click**, because
// `getDisplayMedia` requires transient activation: called from a timer it
// fails with `InvalidStateError` before any of this code is reached, which
// would have made every case below pass for the wrong reason.
const char k_page[] =
  "<!doctype html><meta charset=\"utf-8\"><title>share</title>\n"
  "<style>html,body{margin:0;height:100%;background:#222}</style>\n"
  "<script>\n"
  "window.__r = \"idle\";\n"
  "document.addEventListener(\"click\", function () {\n"
  "  if (window.__r !== \"idle\") return;\n"
  "  window.__r = \"asked\";\n"
  "  navigator.mediaDevices.getDisplayMedia({ video: true }).then(function (s) {\n"
  "    window.__s = s;\n"
  "    var t = s.getVideoTracks();\n"
  "    window.__r = \"stream:\" + t.length;\n"
  "  }).catch(function (e) {\n"
  "    window.__r = \"error:\" + e.name;\n"
  "  });\n"
  "});\n"
  "</script>\n";

enum class on_prompt { allow, refuse };
enum class on_picker { share, cancel };

// What each case saw, so a failure can say which half of the path it was in.
struct tally {
	int prompts = 0;
	int pickers = 0;
	int screens = -1;
	int windows = -1;
};

void click_page(main_window &w) {
	auto *view = w.findChild<QWebEngineView *>();
	QWidget *widget = view ? view->focusProxy() : nullptr;
	QWidget *t = widget ? widget : view;
	if (!t)
		return;
	const QPoint at(t->width() / 2, t->height() / 2);
	QMouseEvent p(QEvent::MouseButtonPress, at, t->mapToGlobal(at),
	               Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent r(QEvent::MouseButtonRelease, at, t->mapToGlobal(at),
	               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(t, &p);
	QApplication::sendEvent(t, &r);
}

QString page_result(main_window &w) {
	const auto views = w.findChildren<QWebEngineView *>();
	if (views.isEmpty())
		return QString();
	QString out;
	QEventLoop loop;
	views.last()->page()->runJavaScript(
	  QStringLiteral("window.__r"), [&](const QVariant &v) {
		out = v.toString();
		loop.quit();
	});
	// Bounded, because a page wedged behind a modal loop is the very thing
	// under test and must not hang the sweep.
	QTimer::singleShot(5000, &loop, &QEventLoop::quit);
	loop.exec();
	return out;
}

// Back to "idle", and any stream from the previous case stopped.
//
// **Not a reload**, which is what this did first and why the second case would
// have passed for the first case's reason: activating a row that is already
// open switches to the tab without fetching it again, so `window.__r` still
// held the previous answer and `wait_settled` returned it before the new
// request had gone anywhere. Resetting the variable is the thing actually
// wanted, and it cannot be satisfied by a stale value.
void reset_page(main_window &w) {
	const auto views = w.findChildren<QWebEngineView *>();
	if (views.isEmpty())
		return;
	QEventLoop loop;
	views.last()->page()->runJavaScript(QStringLiteral(
	  "(function(){if(window.__s){try{window.__s.getTracks()"
	  ".forEach(function(t){t.stop();});}catch(e){}window.__s=null;}"
	  "window.__r=\"idle\";return \"ok\";})()"),
	  [&](const QVariant &) { loop.quit(); });
	QTimer::singleShot(5000, &loop, &QEventLoop::quit);
	loop.exec();
}

// Until the promise settles, or until it is clear it will not.
QString wait_settled(main_window &w, int ms = 25000) {
	QString r;
	for (int waited = 0; waited < ms; waited += 250) {
		r = page_result(w);
		if (r != "idle" && r != "asked" && !r.isEmpty())
			return r;
		spin(250);
	}
	return r.isEmpty() ? QStringLiteral("(no answer)") : r;
}

// What the engine did with a selection we handed over correctly.
//
// **Reported rather than asserted, and the asymmetry is deliberate.** Qt
// discards the selection on every version available here -- 6.8.2 and 6.10.2
// both, `INVALID_STATE` underneath a `NotAllowedError` and an `AbortError`
// respectively -- while Chromium's own picker, on the same display and the
// same XRandR source, hands back a stream. Asserting a stream would leave this
// driver permanently red for somebody else's defect, which is how a sweep
// stops being read at all; asserting the failure would lock the bug in as
// expected behaviour and go quiet on the day it is fixed. So the failure is a
// note and only a *stream* is a check -- the sweep stays honest, and the news
// arrives by itself when Qt starts working.
void engine_answer(const QString &r, const char *how) {
	if (r.startsWith("stream:"))
		check(true, QStringLiteral("Qt accepts a selection made %1 -- "
		                            "the upstream defect is gone")
		              .arg(QLatin1String(how)));
	else
		std::printf("  known  Qt discards a selection made %s (%s); "
		             "Chromium's own picker does not\n", how, qPrintable(r));
}

}  // namespace

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	shell::fixture f("/tmp/hydra-share");

	// The fixture's own page is a title and a paragraph; this one has to ask.
	{
		QFile one(f.one);
		if (!one.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			std::printf("cannot write the asking page\n");
			return 1;
		}
		one.write(k_page);
	}

	tally      t;
	on_prompt  prompt_answer = on_prompt::allow;
	on_picker  picker_answer = on_picker::share;

	// One watcher for the whole run. Timers fire inside nested `exec()` loops,
	// which is exactly why this works on the case it was written to catch.
	QTimer watcher;
	watcher.setInterval(150);
	QObject::connect(&watcher, &QTimer::timeout, [&] {
		for (QWidget *tl : QApplication::topLevelWidgets()) {
			if (!tl->isVisible())
				continue;
			const QString n = tl->objectName();
			if (n == "permission_dialog") {
				++t.prompts;
				if (prompt_answer == on_prompt::allow)
					if (auto *b = tl->findChild<QPushButton *>("permission_allow")) {
						b->click();
						return;
					}
				qobject_cast<QDialog *>(tl)->reject();
				return;
			}
			if (n == "screen_picker") {
				++t.pickers;
				auto *sc = tl->findChild<QListView *>("screen_picker_screens");
				auto *wi = tl->findChild<QListView *>("screen_picker_windows");
				t.screens = sc && sc->model() ? sc->model()->rowCount() : -1;
				t.windows = wi && wi->model() ? wi->model()->rowCount() : -1;
				if (picker_answer == on_picker::share) {
					QListView *use = nullptr;
					if (t.screens > 0)      use = sc;
					else if (t.windows > 0) use = wi;
					if (use) {
						use->setCurrentIndex(use->model()->index(0, 0));
						auto *b = tl->findChild<QPushButton *>("screen_picker_share");
						if (b && b->isEnabled()) {
							b->click();
							return;
						}
					}
				}
				qobject_cast<QDialog *>(tl)->reject();
				return;
			}
		}
	});
	watcher.start();

	// Ask once, from a clean page, under a named rule.
	auto run_case = [&](const char *what, policy::setting rule,
	                     on_prompt pa, on_picker ka) -> QString {
		t = tally();
		prompt_answer = pa;
		picker_answer = ka;
		f.policy.set_global_default(policy::feature::screen_share, rule);
		reset_page(f.window);
		click_page(f.window);
		const QString r = wait_settled(f.window);
		std::printf("  %-22s -> %-18s prompts=%d pickers=%d screens=%d windows=%d\n",
		             what, qPrintable(r), t.prompts, t.pickers, t.screens, t.windows);
		return r;
	};

	// Once, before any of them: every case below resets this page rather than
	// fetching it again.
	if (!f.open_tab(0, "one.html")) {
		std::printf("the asking page never loaded; "
		             "nothing below would mean anything\n");
		return 1;
	}
	spin(800);

	section("refused outright");
	{
		const QString r = run_case("block", policy::setting::block,
		                            on_prompt::refuse, on_picker::cancel);
		check(r.startsWith("error:"), "a blocked rule refuses the page");
		check(t.prompts == 0, "a blocked rule asks nobody");
		check(t.pickers == 0, "a blocked rule opens no picker");
	}

	// Whether this desktop offers anything to share at all. Under the offscreen
	// platform it may not, and an empty picker is a fact about the machine
	// rather than a fault in the code -- so it is reported, and the cases that
	// need something to select say why they were not decided.
	bool anything_to_share = false;

	section("allowed: the picker alone, no prompt in front of it");
	{
		const QString r = run_case("allow + share", policy::setting::allow,
		                            on_prompt::refuse, on_picker::share);
		check(t.prompts == 0, "an allowed rule does not ask again");
		check(t.pickers == 1, "an allowed rule opens the picker");
		anything_to_share = t.screens > 0 || t.windows > 0;
		if (anything_to_share)
			engine_answer(r, "inside one modal loop");
		else
			std::printf("  note  this display offers nothing to share; "
			             "the two share cases are not decided\n");
	}

	section("allowed, then changed their mind");
	{
		const QString r = run_case("allow + cancel", policy::setting::allow,
		                            on_prompt::refuse, on_picker::cancel);
		check(t.pickers == 1, "cancelling still opens the picker");
		check(r.startsWith("error:"), "cancelling refuses the page rather than hanging");
	}

	section("ask: the prompt, and the picker inside it");
	{
		const QString r = run_case("ask + allow + share", policy::setting::ask,
		                            on_prompt::allow, on_picker::share);
		check(t.prompts == 1, "an ask rule prompts once");
		check(t.pickers == 1, "an ask rule reaches the picker");
		if (anything_to_share)
			engine_answer(r, "inside two nested modal loops");
	}

	// **The nesting question, asked in a way an empty display can still
	// answer.** Whether the engine accepts a *selection* made after two modal
	// loops needs something shareable to select; whether the request is still
	// listening after those same two loops does not, because `cancel()` travels
	// the identical path and its effect is visible from the page as a rejected
	// promise.
	//
	// **And it is not asked again**, which is the shell's doing rather than a
	// missed prompt: the decider answers once per site per feature per run, so
	// that a page calling `getDisplayMedia` in a loop cannot put a dialog on
	// the screen as fast as one can be dismissed. This case inherits the grant
	// the case above gave, and asserting `prompts == 0` here is asserting that
	// protection rather than excusing its absence.
	section("asked once per run, not once per request");
	{
		const QString r = run_case("ask again + cancel", policy::setting::ask,
		                            on_prompt::allow, on_picker::cancel);
		check(t.prompts == 0, "the same site and feature is not asked twice");
		check(t.pickers == 1, "the remembered grant still reaches the picker");
		check(r.startsWith("error:"),
		       "the request survives two nested modal loops and still refuses");
	}

	// **The experiment the rest of this file exists to set up.** Every case so
	// far answered the engine from inside a modal event loop, because that is
	// what a picker is. This one answers from the callback directly, with no
	// dialog anywhere: same request, same row, same `selectScreen`, one less
	// nested loop. If a stream arrives here and nowhere else, the modal loop is
	// the fault and the fix is to stop answering from inside one. If this fails
	// too, the loop was never the problem and the fault is in the handing over
	// itself -- which is a different repair, and worth not guessing at.
	//
	// Installed last: it replaces the shell's own chooser for the rest of the
	// run, and every case above needs the real one.
	section("no dialog at all: answered straight from the callback");
	{
		auto *v = f.window.findChild<qtwebengine_view *>();
		if (!v) {
			std::printf("  note  no view to install a chooser on\n");
		} else {
			v->set_capture_chooser(
			  [](const QUrl &, QAbstractListModel *screens,
			      QAbstractListModel *,
			      web_view_backend::capture_answer answer) {
				answer(true, screens && screens->rowCount() > 0 ? 0 : -1);
			});
			const QString r = run_case("allow + no dialog", policy::setting::allow,
			                            on_prompt::refuse, on_picker::share);
			check(t.pickers == 0, "the replacement chooser opens no dialog");
			if (anything_to_share)
				engine_answer(r, "outside any modal loop");
		}
	}

	watcher.stop();
	return shell::report();
}

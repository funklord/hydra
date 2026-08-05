// Back, Forward and Reload: do they say what they can do?
//
// All three used to be permanently enabled, including on an empty tab where
// none of them did anything at all. That is the same defect the Media button
// had, with a better cure available: a navigation button does not need a status
// message explaining why it did nothing, it needs to look unavailable, which is
// what every browser does and what somebody is already reading the toolbar for.
//
// **Driven with real pages, from local files.** History is Chromium's, not
// ours, and the only way to find out whether `canGoBack` says what the toolbar
// claims is to navigate. Two `file://` documents need no network and no server,
// and they build exactly the history a person builds by following a link.
#include "main_window.h"
#include "node.h"
#include "policy.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "qtwebengine_view.h"
#include "request_filter.h"
#include "tab_tree_model.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QStatusBar>
#include <QTimer>
#include <QTreeView>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

// The toolbar actions, found the way anything else finds them: by the tooltip
// somebody reads. Nothing here reaches into the window's private members.
static QAction *toolbar_action(QWidget *w, const QString &tip) {
	for (QAction *a : w->findChildren<QAction *>())
		if (a->toolTip() == tip)
			return a;
	return nullptr;
}

static bool write_page(const QString &path, const QString &body) {
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	f.write(QString("<!doctype html><title>%1</title><p>%1</p>\n")
	            .arg(body).toUtf8());
	return true;
}

// Wait until the address bar shows the page asked for, or give up. Load times
// vary and a fixed sleep either wastes seconds or fails on a busy machine.
static bool wait_for(QLineEdit *address, const QString &fragment, int ms = 12000) {
	for (int waited = 0; waited < ms; waited += 200) {
		spin(200);
		if (address->text().contains(fragment))
			return true;
	}
	return false;
}

// The enabled state follows `loadFinished`, which can land a beat after the url
// changes, so a check made the instant the address bar updates is a race.
static void settle(QAction *a, bool want) {
	for (int i = 0; i < 25 && a->isEnabled() != want; ++i)
		spin(200);
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT"))
	                        : QString("/tmp/hydra-navigate");
	QDir(out).removeRecursively();
	QDir().mkpath(out);

	const QString one = out + "/one.html", two = out + "/two.html";
	if (!write_page(one, "one") || !write_page(two, "two")) {
		std::printf("could not write the pages\n");
		return 1;
	}

	const QString tree = out + "/tree.txt";
	{
		QFile tf(tree);
		if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate))
			return 1;
		tf.write(QString("- [f0] folder | Work\n"
		                  "  - [a1] unopened | One | %1\n"
		                  "  - [a2] unopened | Two | %2\n")
		             .arg(QUrl::fromLocalFile(one).toString(),
		                   QUrl::fromLocalFile(two).toString()).toUtf8());
	}

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1000, 700);
	w.show();
	spin(1200);

	QAction   *back    = toolbar_action(&w, "Back");
	QAction   *fwd     = toolbar_action(&w, "Forward");
	QAction   *reload  = toolbar_action(&w, "Reload");
	QLineEdit *address = nullptr;
	for (QLineEdit *e : w.findChildren<QLineEdit *>())
		if (e->placeholderText() == "Address")
			address = e;

	section("the three buttons exist and can be read");
	check(back && fwd && reload && address,
	      "Back, Forward, Reload and the address bar are all there");
	if (!back || !fwd || !reload || !address) {
		std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
		return 1;
	}

	section("with no page open");
	{
		// The case that started this. All three were enabled on an empty tab,
		// and pressing any of them did nothing whatsoever.
		check(!back->isEnabled(),   "Back is greyed");
		check(!fwd->isEnabled(),    "Forward is greyed");
		check(!reload->isEnabled(), "Reload is greyed -- there is nothing to reload");
	}

	auto *tv = w.findChild<QTreeView *>();
	emit tv->activated(tv->model()->index(0, 0, tv->model()->index(0, 0)));
	check(wait_for(address, "one.html"), "the first page loads");

	section("the window says which page it is showing");
	{
		// A window called "Hydra" and nothing else is indistinguishable from
		// every other one in a task switcher.
		check(w.windowTitle().contains("one") || w.windowTitle().contains("One"),
		      QString("the title names the page (%1)").arg(w.windowTitle()));
		check(w.windowTitle().endsWith("Hydra"),
		      "and still says which browser it is");
	}

	section("on the first page of a tab");
	{
		settle(reload, true);
		check(reload->isEnabled(), "Reload works now that there is a page");
		check(!back->isEnabled(),  "Back is still greyed -- nothing precedes it");
		check(!fwd->isEnabled(),   "and so is Forward");
	}

	section("after going somewhere else");
	{
		address->setText(QUrl::fromLocalFile(two).toString());
		emit address->returnPressed();
		check(wait_for(address, "two.html"), "the second page loads");
		settle(back, true);
		check(back->isEnabled(), "Back comes alive");
		check(!fwd->isEnabled(), "Forward stays greyed -- nothing was undone yet");
	}

	section("after going back");
	{
		back->trigger();
		check(wait_for(address, "one.html"), "the first page returns");
		settle(fwd, true);
		check(fwd->isEnabled(),   "Forward comes alive");
		check(!back->isEnabled(), "and Back goes grey at the start of history");
	}

	section("finding text on the page");
	{
		// Through the menu action, the way somebody reaches it, rather than by
		// calling the slot: the action is the part that can go missing.
		QAction *find_act = nullptr;
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().contains("Find on &Page"))
				find_act = a;
		check(find_act, "there is a Find on Page action");
		// Page search owns Ctrl+F, the way it does in every browser, and the
		// tree filter moved to Ctrl+Shift+F. Only one action may hold a
		// sequence: two is an ambiguous overload and Qt fires neither
		// reliably, which briefly broke both of these.
		int on_ctrl_f = 0;
		for (QAction *a : w.findChildren<QAction *>())
			if (a->shortcut() == QKeySequence("Ctrl+F"))
				++on_ctrl_f;
		check(on_ctrl_f == 1,
		      QString("exactly one action holds Ctrl+F (%1)").arg(on_ctrl_f));
		check(find_act && find_act->shortcut() == QKeySequence("Ctrl+F"),
		      "and it is the one that searches the page");

		QAction *tree_find = nullptr;
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().contains("Find in Tree"))
				tree_find = a;
		check(tree_find && tree_find->shortcut() == QKeySequence("Ctrl+Shift+F"),
		      "the tree filter still has a shortcut of its own");

		QWidget *bar = w.findChild<QWidget *>("find_bar");
		check(bar && !bar->isVisible(), "and the bar stays out of the way until asked");

		if (find_act && bar) {
			find_act->trigger();
			spin(300);
			check(bar->isVisible(), "the bar appears");

			auto *input = w.findChild<QLineEdit *>("find_input");
			auto *count = w.findChild<QLabel *>("find_count");
			check(input && count, "with somewhere to type and somewhere to report");
			if (input && count) {
				input->setText("one");
				for (int i = 0; i < 30 && count->text().isEmpty(); ++i)
					spin(200);
				check(count->text().contains("1"),
				      QString("a word on the page is found (%1)").arg(count->text()));

				input->setText("zzzznotonthispage");
				for (int i = 0; i < 30 && !count->text().contains("No"); ++i)
					spin(200);
				check(count->text() == "No matches",
				      QString("and one that is not says so (%1)").arg(count->text()));
			}

			auto *closer = w.findChild<QWidget *>("find_close");
			if (auto *b = qobject_cast<QAbstractButton *>(closer)) {
				b->click();
				spin(300);
				check(!bar->isVisible(), "closing it puts it away again");
			}
		}
	}

	section("where a link would take you");
	{
		// Driven through the window's own method rather than by hovering: what
		// is worth checking is the eliding and the clearing, and synthesising a
		// mouse move over an offscreen page tests Qt rather than this.
		QStatusBar *sb = w.findChild<QStatusBar *>();
		check(sb, "the status bar is reachable");
		if (sb) {
			w.show_link_target(QUrl("https://example.test/a/page"));
			check(sb->currentMessage() == "https://example.test/a/page",
			      QString("a short target is shown whole (%1)")
			          .arg(sb->currentMessage()));

			// **The host has to survive.** A url elided from the right keeps
			// the scheme and loses the only part that answers "where does this
			// go", which is the entire question being asked.
			const QString host = "https://example.test/";
			w.show_link_target(QUrl(host + QString("x").repeated(400)));
			const QString shown = sb->currentMessage();
			check(shown.size() < 140,
			      QString("a long one is cut down (%1 chars)").arg(shown.size()));
			check(shown.startsWith("https://example.test/"),
			      QString("and keeps the host (%1)").arg(shown.left(40)));

			w.show_link_target(QUrl());
			check(sb->currentMessage().isEmpty(),
			      "leaving the link clears it rather than leaving a stale claim");
		}
	}

	section("the Reload button becomes Stop while a page is arriving");
	{
		// **Watched rather than sampled.** Even a local file goes through
		// loadStarted and loadFinished, so the button is Stop for a moment
		// too short to catch by looking; recording every change to the action
		// catches it however fast the page arrives.
		check(reload->text().contains("Reload"),
		      QString("it is Reload while nothing is loading (%1)").arg(reload->text()));

		QStringList seen;
		auto conn = QObject::connect(reload, &QAction::changed, reload,
		                              [&seen, reload] { seen << reload->text(); });
		address->setText(QUrl::fromLocalFile(two).toString());
		emit address->returnPressed();
		check(wait_for(address, "two.html"), "a page loads");
		spin(600);
		QObject::disconnect(conn);

		check(seen.filter("Stop").size() > 0,
		      QString("it offered Stop while the page was on its way (%1)")
		          .arg(seen.join(", ").left(48)));
		check(reload->text().contains("Reload"),
		      QString("and it is Reload again once the page arrived (%1)")
		          .arg(reload->text()));

		// Back to the first page, so the sections after this one still find
		// what they expect.
		back->trigger();
		wait_for(address, "one.html");
		spin(300);
	}

	section("zooming the page");
	{
		// Through the actions, and read back through the seam rather than from
		// anything this window remembers -- a level the window believes and the
		// page does not have is exactly the bug worth catching.
		QAction *zin = nullptr, *zout = nullptr, *zoff = nullptr;
		for (QAction *a : w.findChildren<QAction *>()) {
			if (a->text().contains("Zoom &In"))   zin  = a;
			if (a->text().contains("Zoom &Out"))  zout = a;
			if (a->text().contains("Actual Size")) zoff = a;
		}
		check(zin && zout && zoff, "the three zoom actions exist");

		auto *view = w.findChild<qtwebengine_view *>();
		check(view, "and the page can be asked what it is at");
		if (zin && zout && zoff && view) {
			check(qFuzzyCompare(view->zoom_factor(), 1.0), "a page starts at 100%");

			zin->trigger();
			spin(200);
			check(view->zoom_factor() > 1.0,
			      QString("zooming in enlarges it (%1)").arg(view->zoom_factor()));

			// The ladder, not a multiplier: two steps up and two down is
			// exactly where it started, whatever route it took.
			zin->trigger();
			spin(150);
			zout->trigger();
			zout->trigger();
			spin(200);
			check(qFuzzyCompare(view->zoom_factor(), 1.0),
			      QString("and stepping back lands on 100% exactly (%1)")
			          .arg(view->zoom_factor()));

			zin->trigger();
			spin(150);
			zoff->trigger();
			spin(200);
			check(qFuzzyCompare(view->zoom_factor(), 1.0),
			      "Actual Size is an absolute, not an undo");
		}
	}

	section("a page that does not arrive says so");
	{
		// The bar is only ever on screen while something is loading, and a
		// failure used to be entirely silent: the bar would have sat at
		// whatever it reached and nothing would have said why.
		QProgressBar *bar = w.findChild<QProgressBar *>("load_progress");
		QStatusBar   *sb  = w.findChild<QStatusBar *>();
		check(bar && sb, "the loading bar and the status bar are reachable");
		if (bar && sb) {
			check(!bar->isVisible(), "no bar once the page has arrived");

			const QString missing =
			    QUrl::fromLocalFile(out + "/there-is-no-such-page.html").toString();
			address->setText(missing);
			emit address->returnPressed();
			for (int i = 0; i < 40 && !sb->currentMessage().contains("could not"); ++i)
				spin(200);
			check(sb->currentMessage().contains("could not be loaded"),
			      QString("a failed load is reported (%1)").arg(sb->currentMessage()));
			check(!bar->isVisible(), "and the bar goes away rather than sticking");
		}
	}

	section("a certificate that could not be trusted");
	{
		// The refusal itself is not the change -- Qt rejects an unhandled
		// certificate error already, which is correct. What was missing is any
		// account of it: the page just failed, and "could not be loaded" is
		// what a site being down says too. Those want different responses.
		QStatusBar *sb = w.findChild<QStatusBar *>();
		if (sb) {
			w.report_certificate_rejected(QUrl("https://expired.example.test/x"),
			                               "The certificate has expired");
			const QString msg = sb->currentMessage();
			check(msg.contains("expired.example.test"),
			      QString("it names the site (%1)").arg(msg.left(40)));
			check(msg.contains("certificate"),
			      "and says the certificate was the problem");
			check(msg.contains("expired"), "including what was wrong with it");

			// **The flag must not stick.** A later ordinary failure has to
			// report itself, or one bad certificate silences every failure
			// after it for the life of the window.
			address->setText(QUrl::fromLocalFile(out + "/still-not-there.html")
			                     .toString());
			emit address->returnPressed();
			for (int i = 0; i < 40 && !sb->currentMessage().contains("could not"); ++i)
				spin(200);
			check(sb->currentMessage().contains("could not be loaded"),
			      QString("an ordinary failure after it still speaks (%1)")
			          .arg(sb->currentMessage()));
		}
	}

	section("a page asking for another window");
	{
		// **Unhandled is not the same as refused.** With nothing implementing
		// this, a target="_blank" link did nothing whatsoever -- and a blocked
		// popup looked exactly the same, which is how the gap survived beside
		// a popup setting that appeared to work.
		//
		// Driven directly: what is worth checking is the decision and where
		// the tab lands, and Qt's own signal is one line of wiring.
		auto *model = w.findChild<tab_tree_model *>();
		node *folder = model->root()->children.first();
		const int before = folder->children.size();

		// A click. Chromium's rule and the right one: the popup setting exists
		// to stop pages opening windows nobody asked for, not to break links.
		node *made = w.open_new_window(QUrl("https://example.test/clicked"), true);
		spin(400);
		check(made != nullptr, "a clicked link opens even with popups blocked");
		check(folder->children.size() == before + 1,
		      QString("as a tab in the tree (%1 -> %2)")
		          .arg(before).arg(folder->children.size()));
		if (made)
			check(made->parent == folder,
			      "under the tab that asked, where the tree shows the relation");

		// A script, with the default policy, which blocks popups.
		const int after_click = folder->children.size();
		QStatusBar *sb = w.findChild<QStatusBar *>();
		node *blocked = w.open_new_window(QUrl("https://example.test/popup"), false);
		spin(300);
		check(blocked == nullptr, "a script-opened window is refused");
		check(folder->children.size() == after_click,
		      "and leaves no tab behind");
		check(sb && sb->currentMessage().contains("Blocked"),
		      QString("out loud, not silently (%1)")
		          .arg(sb ? sb->currentMessage() : QString()));

		// The same request once the site is allowed popups.
		policy.set_setting("*", policy::feature::popups, policy::setting::allow);
		node *allowed = w.open_new_window(QUrl("https://example.test/allowed"), false);
		spin(300);
		check(allowed != nullptr, "allowing popups lets one through");
		policy.set_setting("*", policy::feature::popups, policy::setting::block);
	}

	section("a page whose renderer dies says so");
	{
		// Driven directly, because a render process cannot be killed from here
		// on purpose and the wiring is one line. What is worth checking is what
		// gets said, that it names the site, and that it does not expire.
		QStatusBar *sb = w.findChild<QStatusBar *>();
		if (sb) {
			w.report_render_crash("example.test");
			check(sb->currentMessage().contains("example.test"),
			      QString("it names the site (%1)").arg(sb->currentMessage()));
			check(sb->currentMessage().contains("Reload"),
			      "and says what to do about it");

			// **No timeout.** This describes a state the window is still in;
			// one that expires leaves somebody in front of a blank page
			// wondering what they missed.
			spin(1200);
			check(!sb->currentMessage().isEmpty(),
			      "and it is still there a moment later");

			// Loading something is what makes it untrue.
			address->setText(QUrl::fromLocalFile(one).toString());
			emit address->returnPressed();
			check(wait_for(address, "one.html"), "loading a page again");
			spin(500);
			check(!sb->currentMessage().contains("stopped responding"),
			      QString("clears it (%1)").arg(sb->currentMessage()));
		}
	}

	section("switching tabs drops what belonged to the old page");
	{
		// Two live bugs, both of the same shape: a piece of chrome that was
		// true about the page it was set from and is a lie about the next one.
		// Neither had anything clearing it, because each was hooked into the
		// four places that matter one addition at a time.
		auto *count = w.findChild<QLabel *>("find_count");
		QAction *find_act = nullptr;
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().contains("Find on &Page"))
				find_act = a;
		QStatusBar *sb = w.findChild<QStatusBar *>();

		if (count && find_act && sb && tv->model()->rowCount(tv->model()->index(0, 0)) > 1) {
			find_act->trigger();
			auto *input = w.findChild<QLineEdit *>("find_input");
			input->setText("one");
			for (int i = 0; i < 30 && count->text().isEmpty(); ++i)
				spin(200);
			check(!count->text().isEmpty(),
			      QString("a count is showing before the switch (%1)").arg(count->text()));

			w.show_link_target(QUrl("https://example.test/somewhere"));
			check(!sb->currentMessage().isEmpty(), "and a link target with it");

			// The second tab.
			emit tv->activated(tv->model()->index(1, 0, tv->model()->index(0, 0)));
			check(wait_for(address, "two.html"), "the other tab opens");
			spin(400);

			check(count->text().isEmpty(),
			      QString("the match count does not follow (%1)").arg(count->text()));
			check(!sb->currentMessage().contains("example.test"),
			      QString("nor does the link target (%1)").arg(sb->currentMessage()));
			check(w.windowTitle().contains("two") || w.windowTitle().contains("Two"),
			      QString("and the title is the new page's (%1)").arg(w.windowTitle()));
		}
	}

	section("and when the page goes away again");
	{
		// Deleting the open tab takes its view out of the stack and leaves the
		// window on its placeholder -- the state it started in, so the buttons
		// have to go back with it rather than keep the last page's answers.
		// **Every tab, not just one.** The tree holds two now, and deleting one
		// while the other is still open leaves a page in the window -- so the
		// checks below would be asserting that the chrome forgets a page which
		// is still there, which is the opposite of what they mean.
		auto *model = w.findChild<tab_tree_model *>();
		node *folder = model->root()->children.first();
		while (!folder->children.isEmpty())
			model->remove_node(folder->children.first());
		spin(900);
		check(!back->isEnabled() && !fwd->isEnabled() && !reload->isEnabled(),
		      "all three go grey again once the page is gone");
		check(w.windowTitle() == "Hydra",
		      QString("and the window drops the page's name (%1)")
		          .arg(w.windowTitle()));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

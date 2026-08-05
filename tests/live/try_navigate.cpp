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
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "tab_tree_model.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
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
		                  "  - [a1] unopened | One | %1\n")
		             .arg(QUrl::fromLocalFile(one).toString()).toUtf8());
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

	section("and when the page goes away again");
	{
		// Deleting the open tab takes its view out of the stack and leaves the
		// window on its placeholder -- the state it started in, so the buttons
		// have to go back with it rather than keep the last page's answers.
		auto *model = w.findChild<tab_tree_model *>();
		node *tab   = model->root()->children.first()->children.first();
		model->remove_node(tab);
		spin(900);
		check(!back->isEnabled() && !fwd->isEnabled() && !reload->isEnabled(),
		      "all three go grey again once the page is gone");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

// The one-click "something got through here" report, through the shell.
//
// The claim worth testing is not that a button exists. It is that **pressing it
// records something even when the dialog that follows is dismissed** -- because
// the whole point is that complaining costs one click, and a report lost
// because somebody did not pick one of three tools would make this a worse
// version of the tools it feeds.
#include "annoyance_log.h"
#include "main_window.h"
#include "node.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QPushButton>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

// Answer the modal while it is up. **Captured by value**, because this returns
// before the dialog exists and a reference would be dead stack by the time the
// timer fires -- the defect this project has now paid for twice.
static void answer(bool *saw, QString button_text, bool just_close) {
	QTimer::singleShot(500, [saw, button_text, just_close] {
		for (QWidget *w : QApplication::topLevelWidgets()) {
			auto *d = qobject_cast<QDialog *>(w);
			if (!d || !d->isVisible() || d->objectName() != "annoyed_dialog")
				continue;
			if (saw) *saw = true;
			if (just_close) { d->reject(); return; }
			for (QPushButton *b : d->findChildren<QPushButton *>())
				if (b->text().remove('&').startsWith(button_text)) { b->click(); return; }
			d->reject();
			return;
		}
	});
}

static QAction *toolbar_action(QWidget *w, const QString &text) {
	for (QToolBar *bar : w->findChildren<QToolBar *>())
		for (QAction *a : bar->actions())
			if (a->text() == text)
				return a;
	return nullptr;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-annoyed");
	QDir(out).removeRecursively();
	QDir().mkpath(out);
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Mine\n"
	          "  - [a1] unopened | A page | https://example.com/ | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1000, 700);
	w.show();
	spin(1200);

	section("the affordance itself");
	QAction *annoyed = toolbar_action(&w, "Annoyed");
	check(annoyed != nullptr, "it is on the toolbar, not buried in a menu");
	if (!annoyed) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
	check(!annoyed->toolTip().isEmpty(),
	      QString("and says what it is for (%1)").arg(annoyed->toolTip()));

	section("with no page open it declines rather than filing nothing");
	{
		bool saw = false;
		answer(&saw, "", true);
		annoyed->trigger();
		spin(900);
		check(!saw, "no dialog appears for an empty tab");
		check(!QFile::exists(out + "/annoyances.ini"),
		      "and nothing is written");
	}

	section("filing one");
	{
		// Open the tab so there is a page and a host to file against.
		auto *tree_view = w.findChild<QTreeView *>();
		emit tree_view->activated(
			tree_view->model()->index(0, 0, tree_view->model()->index(0, 0)));
		spin(2500);

		bool saw = false;
		answer(&saw, "Just Record It", false);
		annoyed->trigger();
		spin(900);
		check(saw, "pressing it opens the report");

		annoyance_log log;
		check(log.load(out + "/annoyances.ini"), "and a report file exists");
		check(log.count_for("example.com") == 1,
		      QString("with one report against the site (%1)")
		          .arg(log.count_for("example.com")));
		if (log.count_for("example.com") == 1) {
			const annoyance_report r = log.for_host("example.com").first();
			check(r.when.isValid(), "carrying when it was filed");
			check(r.page.contains("example.com"), "and what was open");
			check(r.outcome == "recorded",
			      QString("and what came of it (%1)").arg(r.outcome));
		}
	}

	section("dismissing the dialog still leaves the complaint");
	{
		// **The claim this driver exists for.** The report is written before
		// the dialog opens, so closing it is not the same as not having
		// complained.
		bool saw = false;
		answer(&saw, "", true);        // rejected, no button pressed
		annoyed->trigger();
		spin(900);
		check(saw, "the dialog opened");

		annoyance_log log;
		log.load(out + "/annoyances.ini");
		check(log.count_for("example.com") == 2,
		      QString("and the dismissed press was filed anyway (%1)")
		          .arg(log.count_for("example.com")));
	}

	section("forgetting it");
	{
		annoyance_log log;
		log.load(out + "/annoyances.ini");
		log.clear_host("example.com");
		check(log.save(out + "/annoyances.ini"), "a site's reports can be cleared");
		annoyance_log back;
		back.load(out + "/annoyances.ini");
		check(back.count_for("example.com") == 0,
		      "and stay cleared, since this is a record of where somebody has been");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

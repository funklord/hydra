// What it costs to come back to a tab, live against evicted.
//
// **The question this answers is a user's**, reported as "it takes a longer
// time to load a tab than other browsers". The shell keeps at most
// `k_max_live_views` engine views alive; beyond that, `enforce_live_cap`
// writes the least-recently-used tab's history into a state blob and destroys
// its view. Coming back to that tab is not a switch, it is a fresh
// `QWebEnginePage`, the whole callback wiring, every injected script, and a
// `restore_state` that re-runs the page load. Every other browser keeps a
// background tab resident and switching costs nothing.
//
// So the measurement is the difference between those two paths, taken on the
// same window, the same pages and the same run:
//
//   * a tab whose view is still in `m_views_by_id` -- what a switch costs;
//   * a tab whose view the cap destroyed -- what a switch costs after eviction.
//
// **The discriminator is the map, not the clock.** A time on its own cannot
// say which path was taken -- a fast restore and a slow switch would read the
// same -- so each case asserts what it is before it times anything.
//
// Local `file://` pages, so the number is the shell's and the engine's rather
// than a network's.
#include "shell_fixture.h"

#include "node.h"
#include "tab_tree_model.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <cstdio>

namespace {
using shell::spin;

// Activate a node the way a person does -- through the tree -- and wait until
// the page it names is actually showing.
//
// **Waiting on the title rather than on a timer**, because the cost being
// measured is the one somebody feels: the moment the page is there. A live
// switch reaches it immediately; a restore reaches it after the engine has
// re-run the load.
qint64 activate_and_wait(shell::fixture &f, node *n, const QString &want_title,
                          bool *timed_out) {
	auto *model = f.window.findChild<tab_tree_model *>();
	auto *proxy = qobject_cast<QSortFilterProxyModel *>(f.tv->model());
	const QModelIndex idx =
	  proxy->mapFromSource(model->index_for_node(n));

	QElapsedTimer clock;
	clock.start();
	emit f.tv->activated(idx);

	while (clock.elapsed() < 20000) {
		web_view_backend *v = f.window.m_views_by_id.value(n->id, nullptr);
		if (v && v->page_title() == want_title)
			break;
		spin(10);
	}
	*timed_out = clock.elapsed() >= 20000;
	return clock.elapsed();
}

}  // namespace

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	using namespace shell;

	fixture f("/tmp/hydra-tabswitch");

	const int cap = f.window.live_view_cap();
	std::printf("  live view cap: %d\n", cap);

	// Two more than the cap, so the earliest are certain to be evicted.
	const int n_tabs = cap + 2;
	QList<node *> tabs;
	QStringList titles;
	auto *model = f.window.findChild<tab_tree_model *>();
	for (int i = 0; i < n_tabs; ++i) {
		const QString title = QString("page %1").arg(i);
		const QString path  = QString("%1/p%2.html").arg(f.out).arg(i);
		// `write_page` builds the whole document and uses its argument as the
		// title. Passing markup here made the title the markup, and the wait
		// below never matched -- every open "timed out" at 20 seconds while
		// the pages were in fact loading fine.
		if (!write_page(path, title)) {
			std::printf("could not write %s\n", qPrintable(path));
			return 2;
		}
		titles << title;
		tabs << model->add_tab(nullptr, title, QUrl::fromLocalFile(path).toString());
	}
	f.tv->expandAll();

	section("open every tab once, so the cap has something to evict");
	for (int i = 0; i < n_tabs; ++i) {
		bool late = false;
		const qint64 ms = activate_and_wait(f, tabs[i], titles[i], &late);
		std::printf("  %-10s first open %5lld ms%s\n",
		             qPrintable(titles[i]), (long long)ms, late ? "  (TIMED OUT)" : "");
	}

	check(f.window.m_views_by_id.size() <= cap,
	       QString("no more than %1 views are live (%2)")
	           .arg(cap).arg(f.window.m_views_by_id.size()));
	check(!f.window.m_views_by_id.contains(tabs[0]->id),
	       "the first tab's view was evicted, which is what makes this a test");
	check(f.window.m_views_by_id.contains(tabs[n_tabs - 1]->id),
	       "and the last tab's view is still live");

	section("switching to a tab that is still live");
	// The one before the last: still inside the cap, and not the tab that is
	// already showing.
	node *live = tabs[n_tabs - 2];
	check(f.window.m_views_by_id.contains(live->id),
	       "its view is in the map before the switch");
	bool late = false;
	const qint64 live_ms = activate_and_wait(f, live, titles[n_tabs - 2], &late);
	check(!late, "it arrived");

	section("switching to a tab the cap evicted");
	node *cold = tabs[0];
	check(!f.window.m_views_by_id.contains(cold->id),
	       "its view is not in the map before the switch");
	bool cold_late = false;
	const qint64 cold_ms = activate_and_wait(f, cold, titles[0], &cold_late);
	check(!cold_late, "it arrived");

	std::printf("\n  live switch   %5lld ms\n", (long long)live_ms);
	std::printf("  after eviction %4lld ms\n", (long long)cold_ms);
	std::printf("  difference     %4lld ms\n", (long long)(cold_ms - live_ms));

	// **Not asserted as a threshold.** What a restore costs depends on the
	// page, the machine and what else is running, so a number pinned here
	// would fail for reasons that are not this project's. What is asserted is
	// the shape: coming back to an evicted tab is not free, and coming back to
	// a live one is.
	check(live_ms < 250,
	       QString("a live switch is immediate (%1 ms)").arg(live_ms));
	check(cold_ms > live_ms,
	       QString("and an evicted one is not (%1 ms against %2)")
	           .arg(cold_ms).arg(live_ms));

	return report();
}

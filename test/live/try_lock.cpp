//
// Locking a tab, and the sub-tab that comes of navigating one (sec 5.5).
//
// The unit tests already prove the model half -- the flag persists, a locked
// node refuses to move, a tab can hold children. What they cannot reach is the
// half that only exists once there is an engine: a real navigation, refused
// before it commits, with the page that was asked for arriving in a new tab
// underneath instead of in the one that was pinned.
//
// **Photographed with `QWidget::grab()`**, which renders in-process and never
// touches the X server. Never `import`: it grabs the X pointer and keeps the
// grab when it cannot do what it was asked, which froze this desktop once
// already (see `shoot.sh`).
#include "shell_fixture.h"

#include "node.h"
#include "tab_tree_model.h"
#include "tab_tree_view.h"

#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QTreeView>

static int g_shots = 0;
static QString g_out;

static void save(QWidget *w, const QString &name) {
	const QString path = QString("%1/%2-%3.png")
	                         .arg(g_out).arg(g_shots, 2, 10, QChar('0')).arg(name);
	if (w->grab().save(path)) {
		std::printf("  shot  %s\n", qPrintable(path));
		++g_shots;
	} else {
		std::printf("  %-28s could not be grabbed\n", qPrintable(name));
	}
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	using namespace shell;

	fixture f("/tmp/hydra-lock");
	g_out = f.out;
	main_window &w = f.window;

	tab_tree_model *model = w.findChild<tab_tree_model *>();
	tab_tree_view  *tree  = w.findChild<tab_tree_view *>();
	if (!model || !tree || !f.address || !f.back || !f.fwd) {
		std::printf("the window did not come up as expected\n");
		return 1;
	}

	section("a tab on a page, before anything is pinned");
	if (!f.open_tab(0, "one.html")) {
		std::printf("the first page never arrived\n");
		return 1;
	}
	node *pinned = model->node_by_id("a1");
	check(pinned && !pinned->locked, "the tab starts unlocked");
	check(f.address->text().contains("one.html"), "showing page one");
	tree->expandAll();
	spin(300);
	save(&w, "unlocked");

	section("locking it");
	// The same signal the context menu emits, so this drives the real path --
	// the shell reads the live view's address and writes it as the pin, which
	// is the part the tree cannot do for itself.
	emit tree->lock_requested(pinned);
	spin(400);
	check(pinned->locked, "the tab is locked");
	check(pinned->url.contains("one.html"),
	      "pinned to the page that was showing, not the one it was created with");
	settle(f.back, false);
	check(!f.back->isEnabled() && !f.fwd->isEnabled(),
	      "Back and Forward grey out: a tab that can be walked backwards out of "
	      "its page is not pinned");
	save(&w, "locked");

	section("browsing away from it opens a sub-tab instead");
	const int children_before = pinned->children.size();
	f.address->setText(QUrl::fromLocalFile(f.two).toString());
	emit f.address->returnPressed();

	// The sub-tab is queued one turn of the event loop later, on purpose, so
	// this waits for the tree to grow rather than asserting immediately.
	for (int i = 0; i < 60 && pinned->children.size() == children_before; ++i)
		spin(200);
	f.wait_idle();
	tree->expandAll();
	spin(500);

	check(pinned->children.size() == children_before + 1,
	      "a sub-tab appeared below the locked tab");
	node *sub = pinned->children.isEmpty() ? nullptr : pinned->children.last();
	check(sub && sub->url.contains("two.html"),
	      "carrying the address that was asked for");
	check(sub && sub->parent == pinned,
	      "as a child of the tab it came from, which is the relationship");
	check(pinned->url.contains("one.html"),
	      "and the locked tab is still pinned to its own page");
	check(f.address->text().contains("two.html"),
	      "with the browsing continuing in the sub-tab");
	save(&w, "sub-tab");

	section("the sub-tab is an ordinary tab");
	check(sub && !sub->locked, "it is not locked itself");
	const QModelIndex si = model->index_for_node(sub);
	check(model->flags(si) & Qt::ItemIsDragEnabled, "so it can be moved");
	// And it navigates freely: browsing on from a sub-tab stays in it.
	const int grandchildren = sub ? sub->children.size() : -1;
	f.address->setText(QUrl::fromLocalFile(f.one).toString());
	emit f.address->returnPressed();
	if (!wait_for(f.address, "one.html"))
		std::printf("  (the sub-tab's navigation did not settle)\n");
	f.wait_idle();
	check(sub && sub->children.size() == grandchildren,
	      "navigating it spawns nothing: an unlocked tab changes address freely");
	tree->expandAll();
	spin(300);
	save(&w, "sub-tab-browsed-on");

	section("unlocking releases both halves");
	emit tree->lock_requested(pinned);
	spin(400);
	check(!pinned->locked, "the tab is unlocked");
	check(model->flags(model->index_for_node(pinned)) & Qt::ItemIsDragEnabled,
	      "and can be moved again");
	save(&w, "unlocked-again");

	std::printf("\n%d image(s) in %s\n", g_shots, qPrintable(g_out));
	return report();
}

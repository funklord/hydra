// Deleting tabs and folders, and what has to be cleaned up when they go (sec 4).
//
// The model's own removal is covered offline in `test_model`. What is not
// coverable there is everything the *shell* keeps beside the tree: a live view
// in the stack, an entry in the map keyed by node id, a place in the LRU, and a
// saved state blob on disk. The model knows about none of it, so deletion used
// to leave all four behind.
//
// The leak was not the interesting part. `enforce_live_cap` picks its victim
// from the LRU and then resolves it against the tree, and when that resolution
// failed it gave up on the whole loop -- so a single deleted-but-still-live tab
// stopped the four-view cap being enforced *for the rest of the session*. The
// symptom would have been memory, hours later, with nothing pointing at the
// delete that caused it. That is what the last section here pins down.
#include "main_window.h"
#include "node.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "state_store.h"
#include "tab_tree_model.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QRegularExpression>
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

// What the status bar says is live. Read rather than inferred: `m_views_by_id`
// is private, and the count in the corner is the same number the person sees.
static int live_count(QWidget *w) {
	QLabel *l = w->findChild<QLabel *>("tab_counts");
	if (!l)
		return -1;
	const QRegularExpressionMatch m =
	    QRegularExpression("^(\\d+)\\s*/").match(l->text());
	return m.hasMatch() ? m.captured(1).toInt() : -1;
}

// Open the tab at `row` under the first folder, the way a click does.
static void open_row(QTreeView *tree, int row) {
	const QModelIndex folder = tree->model()->index(0, 0);
	emit tree->activated(tree->model()->index(row, 0, folder));
	spin(900);
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-delete");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.ini");
	const QString tree_path = out + "/tree.txt";
	QFile tf(tree_path);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	QString doc = "- [f0] folder | Mine\n";
	// Six, because the cap is four: opening them all forces suspensions, and a
	// suspension is what writes the state blob this test then expects to be
	// deleted along with its node.
	for (int i = 1; i <= 6; ++i)
		doc += QString("  - [a%1] unopened | Tab %1 | about:blank | "
		                "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n").arg(i);
	tf.write(doc.toUtf8());
	tf.close();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree_path);
	w.resize(1000, 700);
	w.show();
	spin(1200);

	auto *model = w.findChild<tab_tree_model *>();
	auto *tree  = w.findChild<QTreeView *>();
	check(model && tree, "the tree and its view are reachable");
	check(live_count(&w) == 0, "nothing is live before anything is opened");
	if (!model || !tree) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
	node *folder = model->root()->children.first();

	section("the cap holds while tabs are opened");
	for (int row = 0; row < 5; ++row)
		open_row(tree, row);
	check(live_count(&w) == 4,
	      QString("five opened, four live (%1)").arg(live_count(&w)));

	state_store store(out + "/state");
	// The first tab opened is the first suspended, and suspending is what saves.
	check(store.has_state("a1"), "the suspended tab left a state blob behind");

	section("deleting a suspended tab takes its blob");
	{
		node *a1 = nullptr;
		for (node *c : folder->children)
			if (c->id == "a1") a1 = c;
		check(a1 != nullptr, "the suspended tab is still in the tree");
		if (a1) {
			model->remove_node(a1);
			spin(300);
			check(!store.has_state("a1"),
			      "its saved state goes with it, so a reused id cannot inherit it");
			check(model->node_by_id("a1") == nullptr, "and it is out of the tree");
		}
	}

	section("deleting a folder closes the live tabs inside it");
	{
		const int before = live_count(&w);
		check(before == 4, QString("four live going in (%1)").arg(before));
		model->remove_node(folder);
		spin(500);
		check(live_count(&w) == 0,
		      QString("the whole subtree's views are closed (%1 live)").arg(live_count(&w)));
		check(model->root()->children.isEmpty(), "and the folder is gone");
	}

	section("the cap still works afterwards");
	{
		// The point of the whole test. Before the fix, the deletions above left
		// live views under ids the tree could no longer resolve, and the cap's
		// victim loop hit one and gave up -- permanently. Opening six tabs would
		// then leave six live, and nothing anywhere would say why.
		node *f2 = model->add_folder(model->root(), "Again");
		for (int i = 1; i <= 6; ++i)
			model->add_tab(f2, QString("Second %1").arg(i), "about:blank");
		spin(300);
		for (int row = 0; row < 6; ++row)
			open_row(tree, row);
		const int live = live_count(&w);
		check(live == 4,
		      QString("six more opened after the deletions, still four live (%1)").arg(live));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

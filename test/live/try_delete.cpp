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

#include <QMessageBox>
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
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
// A row's index in the view's coordinates, through whatever proxy is in the
// way. Written once because the alternative is `index(0, 0)` and a comment
// about which row that is today.
static QModelIndex index_for(QTreeView *tree, tab_tree_model *model, node *n) {
	const QModelIndex src = model->index_for_node(n);
	if (auto *proxy = qobject_cast<QSortFilterProxyModel *>(tree->model()))
		return proxy->mapFromSource(src);
	return src;
}

static void open_row(QTreeView *tree, int row) {
	const QModelIndex folder = tree->model()->index(0, 0);
	emit tree->activated(tree->model()->index(row, 0, folder));
	spin(900);
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	// **The cap this driver asserts against, stated rather than assumed.**
	// Every count below -- four live out of five opened, four again out of
	// eleven -- is a claim about the live-view cap, and the cap was a constant
	// when they were written. It is a setting now, defaulting to eight for
	// measured reasons about how long a tab takes to come back, so a driver
	// that assumed four went from passing to reporting three defects the day
	// the default moved, and nothing was wrong with the browser.
	//
	// Forced rather than read, because these assertions are about *specific*
	// counts and a relationship would say much less: what is being tested is
	// that the cap is enforced at all, that a suspension writes a blob, and
	// that deleting a node takes the blob with it.
	//
	// Set before the window exists. `live_view_cap()` reads the environment on
	// every call, so later would work too and would be a worse habit.
	qputenv("HYDRA_MAX_LIVE_VIEWS", "4");

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

	section("the Delete menu entry asks before it takes a folder");
	{
		// **The defect this covers destroyed data silently.** `remove_node`
		// deletes the whole subtree and its comment says the caller must have
		// asked first; the context menu asked, and the Edit menu -- which also
		// binds the Delete key -- called it directly. A selected folder and
		// every tab in it went without a word, and the only undo in this
		// window is for Reorganize.
		//
		// The tree view's own `keyPressEvent` declines to bind Delete "because
		// the menu entry asks first", so the safety one file stated was
		// supposed to be provided by another, and was not.
		//
		// **Answered from a timer, because the question is modal.** The driver
		// cannot reach the box any other way: `QMessageBox::question` runs its
		// own event loop, so the answer has to be posted from inside it.
		auto answer_modal = [](QMessageBox::StandardButton with) {
			QTimer::singleShot(400, [with] {
				auto *box = qobject_cast<QMessageBox *>(
				  QApplication::activeModalWidget());
				if (!box)
					return;
				if (QAbstractButton *b = box->button(with))
					b->click();
			});
		};

		QAction *del = nullptr;
		for (QAction *a : w.findChildren<QAction *>())
			if (QString(a->text()).remove('&') == "Delete")
				del = a;
		check(del != nullptr, "the Edit menu offers Delete");

		// **Its own folder, because the yes-case destroys what it deletes and
		// every section after this one needs the fixture.** The first draft
		// deleted the fixture's folder and the driver segfaulted in the next
		// section on a node that was gone -- and the two checks here had
		// already printed `ok`, so reading them alone said the section passed.
		// A tally and an exit status are halves of one result; that is the
		// rule this driver's own sweep now enforces, skipped here by its
		// author.
		node *victim = model->add_folder(nullptr, "Scratch");
		model->add_tab(victim, "Inside", "about:blank");
		spin(100);
		const int before = model->root()->children.size();
		const QString name = victim->title;

		if (del) {
			// Refused: the folder must survive. If nothing asks, it does not,
			// which is the whole assertion -- a silent delete cannot pass this
			// by luck.
			tree->setCurrentIndex(index_for(tree, model, victim));
			answer_modal(QMessageBox::No);
			del->trigger();
			spin(900);
			check(model->root()->children.size() == before,
			       QString("answering no leaves \"%1\" where it was (%2 -> %3)")
			           .arg(name).arg(before)
			           .arg(model->root()->children.size()));

			// And accepted, so the check above is not passing because the
			// entry does nothing at all.
			tree->setCurrentIndex(index_for(tree, model, victim));
			answer_modal(QMessageBox::Yes);
			del->trigger();
			spin(900);
			check(model->root()->children.size() == before - 1,
			       QString("and answering yes takes it (%1 -> %2)")
			           .arg(before).arg(model->root()->children.size()));
		}
	}

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

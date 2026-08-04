// The tree model and its sort/filter proxy (architecture doc §5.2/§5.3).
//
// Everything the user sees of the tree goes through these two, and a model that
// lies about its own shape does not produce a wrong answer — it produces a crash
// inside Qt's view code, somewhere with no stack frames of ours in it.
//
// So this does two different things. Qt's own `QAbstractItemModelTester` walks
// the model and checks the contract — parent/index round trips, row counts,
// signal ordering — which is the part no hand-written assertion covers well.
// Then the rest is behaviour: sorting, and a search that has to keep the
// ancestors of a hit or the hit is invisible inside a collapsed folder.
#include "tab_tree_model.h"
#include "tree_sort_proxy.h"
#include "node.h"

#include <QAbstractItemModelTester>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMimeData>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// The tester reports through the Qt message handler in Warning mode; anything
// it says is a contract violation, so they are counted rather than watched.
static int g_warnings = 0;
static void counting_handler(QtMsgType type, const QMessageLogContext &,
                              const QString &msg) {
	if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
		++g_warnings;
		std::printf("        [qt] %s\n", qPrintable(msg));
	}
}

// Rows visible in a proxy, depth-first, as titles.
static QStringList visible(const QAbstractItemModel *m,
                            const QModelIndex &parent = QModelIndex()) {
	QStringList out;
	for (int i = 0; i < m->rowCount(parent); ++i) {
		const QModelIndex idx = m->index(i, 0, parent);
		out << m->data(idx, tab_tree_model::title_role).toString();
		out += visible(m, idx);
	}
	return out;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// QApplication, not QCoreApplication: this is a *GUI* model — it answers
	// DecorationRole with a style icon and FontRole with a font — so it needs a
	// style to exist. Testing it under QCoreApplication segfaults inside
	// QApplication::style(), which is the test being wrong about what it is
	// testing rather than the model being wrong.
	QApplication app(argc, argv);

	const QString dir = QDir::tempPath() + "/hydra-model-test";
	QDir().mkpath(dir);
	const QString path = dir + "/tree.txt";
	{
		QFile f(path);
		f.open(QIODevice::WriteOnly | QIODevice::Truncate);
		f.write("- [f1] folder | Work\n"
		         "  - [a1] unopened | Zebra notes | https://z.example/notes | "
		         "created=2026-01-01T00:00:00 | seen=2026-03-01T00:00:00\n"
		         "  - [a2] unopened | Apple docs | https://a.example/docs | "
		         "created=2026-02-01T00:00:00 | seen=2026-01-15T00:00:00\n"
		         "- [f2] folder | Play\n"
		         "  - [a3] unopened | Music | https://m.example/tunes | "
		         "created=2026-03-01T00:00:00 | seen=2026-02-01T00:00:00\n"
		         "- [a4] unopened | Loose page | https://loose.example/ | "
		         "created=2026-01-15T00:00:00 | seen=2026-03-15T00:00:00\n");
	}

	tab_tree_model model;
	check(model.load(path), "the model loads the outline");

	section("Qt's own model contract");
	{
		QtMessageHandler prev = qInstallMessageHandler(counting_handler);
		g_warnings = 0;
		{
			QAbstractItemModelTester tester(
				&model, QAbstractItemModelTester::FailureReportingMode::Warning);
			tree_sort_proxy proxy;
			proxy.setSourceModel(&model);
			QAbstractItemModelTester proxy_tester(
				&proxy, QAbstractItemModelTester::FailureReportingMode::Warning);
			proxy.set_sort_mode(tree_sort_proxy::sort_mode::title_asc);
			proxy.set_search_text("a");
			proxy.set_search_text("");
			proxy.set_sort_mode(tree_sort_proxy::sort_mode::tree_order);
		}
		qInstallMessageHandler(prev);
		check(g_warnings == 0,
		      QString("the model and proxy satisfy QAbstractItemModelTester "
		               "through sorting and filtering (%1 complaint(s))")
		          .arg(g_warnings));
	}

	section("shape");
	{
		check(model.rowCount(QModelIndex()) == 3,
		      QString("three top-level rows (%1)").arg(model.rowCount(QModelIndex())));
		const QModelIndex work = model.index(0, 0, QModelIndex());
		check(model.rowCount(work) == 2, "Work holds two");
		check(model.data(work, tab_tree_model::title_role).toString() == "Work",
		      "titles come through the role");
		check(model.rowCount(model.index(2, 0, QModelIndex())) == 0, "and a leaf holds nothing");

		node *n = model.node_by_id("a3");
		check(n && n->title == "Music", "a node can be found by id");
		const QModelIndex idx = model.index_for_node(n);
		check(idx.isValid() && model.data(idx, tab_tree_model::title_role) == "Music",
		      "and turned back into an index that points at it");
		check(model.node_for_index(idx) == n, "which maps back to the same node");
		check(!model.index_for_node(nullptr).isValid(),
		      "a null node has no index, rather than the root's");
	}

	section("sorting keeps the nesting and groups folders first");
	{
		tree_sort_proxy proxy;
		proxy.setSourceModel(&model);
		proxy.set_sort_mode(tree_sort_proxy::sort_mode::title_asc);

		const QStringList seen = visible(&proxy);
		check(seen.value(0) == "Play" || seen.value(0) == "Work",
		      QString("a folder is first, not the loose page (%1)").arg(seen.value(0)));
		check(seen.indexOf("Loose page") > seen.indexOf("Work") &&
		          seen.indexOf("Loose page") > seen.indexOf("Play"),
		      "the loose leaf sorts after both folders, whatever its title");
		check(seen.indexOf("Apple docs") < seen.indexOf("Zebra notes"),
		      "and siblings are alphabetical within their folder");
		check(seen.contains("Music") && seen.size() == 6,
		      QString("every node is still present — sorting is not filtering (%1)")
		          .arg(seen.join(", ")));
	}

	section("search keeps a hit's ancestors, or the hit is invisible");
	{
		tree_sort_proxy proxy;
		proxy.setSourceModel(&model);
		proxy.set_search_text("zebra");

		const QStringList seen = visible(&proxy);
		check(seen.contains("Zebra notes"), "the matching leaf is shown");
		check(seen.contains("Work"),
		      "and the folder containing it, or it would be inside something hidden");
		check(!seen.contains("Play") && !seen.contains("Music"),
		      QString("while an unrelated branch is gone entirely (%1)")
		          .arg(seen.join(", ")));
		check(!seen.contains("Apple docs"),
		      "and a sibling that does not match is not carried along");
	}

	section("search looks at the url too, and ignores case");
	{
		tree_sort_proxy proxy;
		proxy.setSourceModel(&model);

		proxy.set_search_text("m.example");
		check(visible(&proxy).contains("Music"),
		      "a url match counts, since that is what the user typed to find it");

		proxy.set_search_text("ZEBRA");
		check(visible(&proxy).contains("Zebra notes"), "case is ignored");

		proxy.set_search_text("nothing here matches");
		check(visible(&proxy).isEmpty(),
		      QString("a search with no hits shows nothing rather than everything (%1)")
		          .arg(visible(&proxy).join(", ")));

		proxy.set_search_text("");
		check(visible(&proxy).size() == 6, "and clearing it brings the tree back");
	}

	section("a folder that matches by name");
	{
		// Worth pinning because it surprises: the search keeps a node and its
		// ancestors, not its descendants, so matching a folder shows the folder
		// without its contents. That is what the header describes; a test says so
		// out loud, so changing it later is a decision rather than a slip.
		tree_sort_proxy proxy;
		proxy.setSourceModel(&model);
		proxy.set_search_text("Play");
		const QStringList seen = visible(&proxy);
		check(seen.contains("Play"), "the folder is shown");
		check(!seen.contains("Music"),
		      QString("and its non-matching children are not (%1)").arg(seen.join(", ")));
	}

	section("dragging a tab about, which the tree could not do at all");
	{
		// The model implemented the read-only half of QAbstractItemModel and
		// nothing else, so the view refused every drag: a tab could be moved
		// only by the AI reorganizer or by editing the outline file by hand.
		tab_tree_model m;
		check(m.load(path), "a tree loads");
		node *root = m.root();
		check(root->children.size() >= 1,
		      QString("with a folder in it (%1 children)").arg(root->children.size()));
		node *folder = root->children.first();
		check(folder->is_folder(), "which is a folder");
		node *tab = folder->children.isEmpty() ? nullptr : folder->children.first();
		check(tab != nullptr, "holding a tab");
		if (!tab) return 1;

		// The flags a view asks about before it will start a drag at all.
		const QModelIndex fi = m.index_for_node(folder);
		const QModelIndex ti = m.index_for_node(tab);
		check(m.flags(ti) & Qt::ItemIsDragEnabled, "a tab can be dragged");
		check(m.flags(fi) & Qt::ItemIsDropEnabled, "a folder accepts a drop");
		check(!(m.flags(ti) & Qt::ItemIsDropEnabled),
		      "a tab does not -- dropping onto one would have to mean beside it, "
		      "and a gesture meaning two things is one people stop trusting");
		check(m.flags(QModelIndex()) & Qt::ItemIsDropEnabled,
		      "and the root does, so a tab can be dragged out to the top level");

		// A second folder to move things between.
		node *other = m.add_folder(root, "Elsewhere");
		check(other && other->is_folder(), "a folder can be made");
		check(other->id != folder->id, "with an id of its own");

		const QString moved_id = tab->id;
		QMimeData *md = m.mimeData({ ti });
		check(md && md->hasFormat("application/x-hydra-node-ids"),
		      "a drag carries node ids");
		// By id, not by url: the id is what state/<id>.blob and the outline file
		// are keyed by, so moving by url would silently produce a tab that had
		// forgotten where it had been.
		check(md && QString::fromUtf8(md->data("application/x-hydra-node-ids"))
		                .contains(moved_id),
		      "naming the node that was picked up");

		const int before = folder->children.size();
		check(m.dropMimeData(md, Qt::MoveAction, -1, 0, m.index_for_node(other)),
		      "and dropping it on another folder is accepted");
		check(folder->children.size() == before - 1, "it leaves where it was");
		check(!other->children.isEmpty() &&
		          other->children.last()->id == moved_id,
		      "and arrives where it was dropped, keeping its id");
		delete md;
	}

	section("the move that would eat the tree");
	{
		tab_tree_model m;
		m.load(path);
		node *root = m.root();
		node *outer = m.add_folder(root, "Outer");
		node *inner = m.add_folder(outer, "Inner");
		check(inner->parent == outer, "a folder inside a folder");

		QMimeData *md = m.mimeData({ m.index_for_node(outer) });
		// A ring: the outline writer would recurse forever and everything below
		// the drag would vanish from the file. The reorganizer refuses the same
		// move (§9.4); this is that rule one gesture closer to the user.
		check(!m.dropMimeData(md, Qt::MoveAction, -1, 0, m.index_for_node(inner)),
		      "cannot be dropped inside its own child");
		check(outer->parent == root, "and is left where it was");
		check(!m.dropMimeData(md, Qt::MoveAction, -1, 0, m.index_for_node(outer)),
		      "nor onto itself");
		delete md;
	}

	section("copying gives the copy an id of its own");
	{
		tab_tree_model m;
		m.load(path);
		node *root = m.root();
		node *folder = root->children.first();
		node *tab = folder->children.first();
		const QString original = tab->id;

		node *copy = m.duplicate_node(tab);
		check(copy != nullptr, "a tab duplicates");
		check(copy->id != original,
		      QString("with a new id (%1 vs %2)").arg(copy->id, original));
		check(m.node_by_id(original) == tab && m.node_by_id(copy->id) == copy,
		      "and both are findable, so nothing was overwritten in the index");
		check(copy->url == tab->url && copy->title == tab->title,
		      "carrying the same address and title");
		// Two nodes sharing an id would share a state blob, so one tab's scroll
		// position and form contents would be restored into the other.
		check(copy->type != node_type::open_tab &&
		          copy->type != node_type::suspended_tab,
		      "and is not claimed to be open or suspended, since the state blob "
		      "belongs to the id it was written under");
	}

	section("what the properties editor is allowed to change");
	{
		tab_tree_model m;
		m.load(path);
		node *tab = m.root()->children.first()->children.first();
		const QString id_before = tab->id;
		m.update_node(tab, "Renamed", "https://example.test/x", { "a", "b" });
		check(tab->title == "Renamed", "the title changes");
		check(tab->url == "https://example.test/x", "the address changes");
		check(tab->tags == QStringList({ "a", "b" }), "the tags change");
		check(tab->id == id_before,
		      "and the id does not -- it keys the saved state, and retyping it "
		      "would orphan a tab's history with no warning");
		check(m.node_by_id(id_before) == tab, "so the index still finds it");
	}

	section("deleting takes the subtree, and refuses the root");
	{
		tab_tree_model m;
		m.load(path);
		node *root = m.root();
		node *doomed = m.add_folder(root, "Doomed");
		m.add_folder(doomed, "Child");
		const int before = root->children.size();
		check(!m.remove_node(root), "the root cannot be removed");
		check(m.remove_node(doomed), "a folder can");
		check(root->children.size() == before - 1, "and is gone from its parent");
		check(m.node_by_id("Child") == nullptr,
		      "with what was inside it, rather than leaving orphans in the index");
	}

	QDir(dir).removeRecursively();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

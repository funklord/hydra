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
#include "tree_invariants.h"
#include "tree_sort_proxy.h"
#include "node.h"

#include <QAbstractItemModelTester>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMimeData>
#include <QSet>
#include <functional>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// **Called at the end of every section below, whatever that section was
// testing.** The point is not to check the operation just performed -- each
// section already does that -- but to catch an operation leaving the tree
// wrong in a way its own assertions were never going to look at. A future
// mutation added to this file inherits the check by being written here, and a
// future mutation added *elsewhere* fails in these tests without anybody
// having anticipated it.
static void holds(tab_tree_model &m, const char *where) {
	const auto r = tree_invariants::check(m.root());
	check(r.ok, QString("%1: the tree is still well formed (%2)")
	                .arg(where, r.summary().left(60)));
}

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
		holds(model, "Qt's own model contract");
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
		holds(model, "shape");
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
		holds(model, "sorting keeps the nesting and groups f");
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
		holds(model, "search keeps a hit's ancestors, or the");
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
		holds(model, "search looks at the url too, and ignor");
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
		holds(model, "a folder that matches by name");
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
		holds(m, "dragging a tab about, which the tree c");
	}

	section("Ctrl-drag copies, which is a different branch of the same drop");
	{
		// **The path Ctrl actually takes.** `supportedDropActions` offers
		// Move|Copy and the view leaves `startDrag` alone, so Qt hands both to
		// the drag and the platform draws the plus badge while Ctrl is held.
		// That badge is a *promise*, and what makes it true is `dropMimeData`
		// branching on CopyAction -- which was covered only indirectly, through
		// a test of `duplicate_node`, the function that branch happens to call.
		// A drop that ignored the action would still have passed it.
		tab_tree_model m;
		check(m.load(path), "a tree loads");
		check(m.supportedDropActions() & Qt::CopyAction,
		      "the model offers a copy action, which is what puts the plus on "
		      "the cursor");
		node *root   = m.root();
		node *folder = root->children.first();
		node *tab    = folder->children.first();
		node *other  = m.add_folder(root, "Elsewhere");

		const QString source_id = tab->id;
		const QString source_url = tab->url;
		const int before = folder->children.size();

		QMimeData *md = m.mimeData({ m.index_for_node(tab) });
		check(m.dropMimeData(md, Qt::CopyAction, -1, 0, m.index_for_node(other)),
		      "a copy drop is accepted");
		check(folder->children.size() == before,
		      "the original stays where it was -- which is the whole difference "
		      "from a move");
		check(m.node_by_id(source_id) == tab, "and is still itself");
		check(!other->children.isEmpty(), "something arrived");
		if (!other->children.isEmpty()) {
			node *copy = other->children.last();
			check(copy->id != source_id,
			      QString("the arrival has an id of its own (%1 vs %2)")
			          .arg(copy->id, source_id));
			check(copy->url == source_url, "carrying the same address");
			// Two nodes sharing an id would each claim the same state blob and
			// the same line of the tree file, which the invariant check below
			// would also catch -- stated here because it is the specific thing
			// a copy must not do.
			check(m.node_by_id(copy->id) == copy,
			      "and both are findable, so the index holds two distinct nodes");
		}
		delete md;
		holds(m, "Ctrl-drag copies");
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
		holds(m, "the move that would eat the tree");
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
		holds(m, "copying gives the copy an id of its ow");
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
		holds(m, "what the properties editor is allowed ");
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
		holds(m, "deleting takes the subtree, and refuse");
	}

	section("a mirror is shown and never written");
	{
		// The invariant the whole design rests on. A mirror is another browser's
		// session; saving it would resurrect a stale copy of somebody else's
		// tabs on the next launch, indistinguishable from tabs the user had
		// filed themselves -- and they would keep coming back, because nothing
		// would ever delete them.
		tab_tree_model m;
		check(m.load(path), "a tree loads");
		const int real_top = m.root()->children.size();

		QList<node *> tabs;
		for (int i = 0; i < 3; ++i) {
			node *n = new node;
			n->id    = QString("fx-%1").arg(i);
			n->type  = node_type::unopened_tab;
			n->title = QString("Elsewhere %1").arg(i);
			n->url   = QString("https://elsewhere.test/%1").arg(i);
			tabs << n;
		}
		node *mirror = m.replace_mirror("firefox", "Firefox (3 tabs)", tabs);
		check(mirror != nullptr, "a mirror folder appears");
		check(m.root()->children.size() == real_top + 1, "beside the real tree");
		check(m.root()->children.first() == mirror,
		      "at the top, where a thing that is not yours is easiest to spot");
		check(!mirror->mirror.isEmpty() &&
		          mirror->children.first()->mirror == "firefox",
		      "and everything under it is marked as belonging to the source");

		const QString saved = dir + "/with-mirror.txt";
		check(m.save(saved), "the tree saves");
		tab_tree_model reloaded;
		check(reloaded.load(saved), "and loads again");
		bool found_mirror = false;
		for (node *c : reloaded.root()->children)
			if (c->title.startsWith("Firefox"))
				found_mirror = true;
		check(!found_mirror,
		      "with no trace of the mirror in it -- the file is this tree, not "
		      "a copy of another browser's");
		check(reloaded.root()->children.size() == real_top,
		      "and the real tree is intact around where it was");

		// Re-reading replaces rather than accumulating: a merge would leave
		// tabs the user closed in the other browser sitting here for ever.
		QList<node *> again;
		node *one = new node;
		one->id = "fx-0"; one->type = node_type::unopened_tab;
		one->title = "Only one now"; one->url = "https://elsewhere.test/0";
		again << one;
		m.replace_mirror("firefox", "Firefox (1 tab)", again);
		check(m.root()->children.size() == real_top + 1,
		      "a second import does not add a second folder");
		check(m.root()->children.first()->children.size() == 1,
		      "and holds what the source has now, not the union of both reads");
		holds(m, "a mirror is shown and never written");
	}

	section("dropping while the tree is filtered");
	{
		// The hazard: a drop between two rows is a *position*, the view hands
		// that position through the proxy, and while a search is active proxy
		// row N is not source row N. If nothing maps it, a drop lands somewhere
		// other than where it was aimed -- and silently, since the row it
		// displaces looks plausible.
		tab_tree_model m;
		check(m.load(path), "a tree loads");
		tree_sort_proxy proxy;
		proxy.setSourceModel(&m);
		proxy.set_sort_mode(tree_sort_proxy::sort_mode::tree_order);

		node *work = m.root()->children.first();
		check(work->children.size() >= 2, "a folder with several tabs");
		// Titles in the fixture: "Zebra notes", "Apple docs", ...
		QStringList before;
		for (node *c : work->children)
			before << c->title;

		// Hide everything but one of them.
		proxy.set_search_text("Apple");
		const QModelIndex pf = proxy.mapFromSource(m.index_for_node(work));
		check(pf.isValid(), "the folder is still visible while filtered");
		check(proxy.rowCount(pf) == 1,
		      QString("and shows only what matched (%1 of %2)")
		          .arg(proxy.rowCount(pf)).arg(before.size()));

		// Take a tab from elsewhere and drop it at proxy row 0 of that folder --
		// which, filtered, is a different source row from 0.
		node *loose = nullptr;
		for (node *c : m.root()->children)
			if (!c->is_folder()) loose = c;
		check(loose != nullptr, "there is a tab outside that folder to move");
		if (loose) {
			const QString moved = loose->id;
			QMimeData *md = m.mimeData({ m.index_for_node(loose) });
			// Through the *proxy*, which is what the view talks to.
			const bool dropped = proxy.dropMimeData(md, Qt::MoveAction, 0, 0, pf);
			check(dropped, "the drop is accepted through the proxy");
			delete md;

			node *landed = m.node_by_id(moved);
			check(landed && landed->parent == work,
			      "and it lands in the folder that was aimed at");
			if (landed && landed->parent == work) {
				const int at = work->children.indexOf(landed);
				// Proxy row 0 is the *matching* row -- "Apple docs" -- which sits
				// at source index 1. Landing at source 0 would mean the position
				// was passed through unmapped.
				const int apple = [&] {
					for (int i = 0; i < work->children.size(); ++i)
						if (work->children.at(i)->title.startsWith("Apple"))
							return i;
					return -1;
				}();
				check(at >= 0 && apple >= 0,
				      QString("both are placed (dropped at %1, Apple at %2)")
				          .arg(at).arg(apple));
				// The claim: it went next to what the user could actually see,
				// not to the raw row number the view happened to use.
				check(qAbs(at - apple) <= 1,
				      QString("beside the row it was dropped on, not at the "
				               "unmapped index (at %1, Apple at %2)")
				          .arg(at).arg(apple));
			}
		}
		proxy.set_search_text("");
		holds(m, "dropping while the tree is filtered");
	}

	section("who decides whether a between-rows drop means anything");
	{
		// This used to be a bool on the model that the shell set from the sort
		// combo -- two calls that had to be kept in step, so any other route to
		// changing the sort left the model believing a stale answer. The proxy
		// already encodes it in its sort role, so the question is asked rather
		// than mirrored and there is nothing to keep in sync.
		tab_tree_model m;
		m.load(path);
		tree_sort_proxy proxy;
		proxy.setSourceModel(&m);

		proxy.set_sort_mode(tree_sort_proxy::sort_mode::tree_order);
		check(proxy.in_tree_order(), "tree order says so");
		proxy.set_sort_mode(tree_sort_proxy::sort_mode::title_asc);
		check(!proxy.in_tree_order(), "sorting by title says otherwise");
		proxy.set_sort_mode(tree_sort_proxy::sort_mode::newest_created);
		check(!proxy.in_tree_order(), "and so does sorting by date");
		proxy.set_sort_mode(tree_sort_proxy::sort_mode::tree_order);
		check(proxy.in_tree_order(), "and it comes back");

		// The property the old arrangement could not have: nobody had to be
		// told. The answer follows the sort with no second call anywhere.
		proxy.set_sort_mode(tree_sort_proxy::sort_mode::recently_seen);
		check(!proxy.in_tree_order(),
		      "a sort changed by any route at all is reflected immediately, "
		      "because nothing is storing a copy of it");
		holds(m, "who decides whether a between-rows dro");
	}

	section("making a tab, which nothing could do");
	{
		// Until this existed a tab could arrive only from the tree file, a
		// duplicate, a browser mirror or the AI reorganizer. A browser that
		// cannot open a new tab is a gap worth a check of its own.
		tab_tree_model m;
		m.load(path);
		node *folder = m.root()->children.first();
		const int before = folder->children.size();

		node *t = m.add_tab(folder, QString(), QString());
		check(t != nullptr, "a tab can be made");
		check(t && t->parent == folder && folder->children.size() == before + 1,
		      "inside the folder it was asked for");
		check(t && !t->is_folder() && t->type == node_type::unopened_tab,
		      "as an unopened tab");
		check(t && !t->title.isEmpty(),
		      QString("with a label rather than a blank row (%1)").arg(t->title));
		check(t && m.node_by_id(t->id) == t, "and an id the index knows");

		// Asked for beside a *tab* it goes next to it, not inside: a tab holds
		// no children, so "in here" has no meaning.
		node *sibling = m.add_tab(t, QString(), "https://x.test/");
		check(sibling && sibling->parent == folder,
		      "a tab asked for beside a tab lands next to it, not within it");
		check(sibling && sibling->url == "https://x.test/", "keeping its address");

		// Ids do not collide with anything, including a second new tab.
		check(t && sibling && t->id != sibling->id, "two new tabs differ");
		QSet<QString> ids;
		std::function<void(node *)> walk = [&](node *n) {
			for (node *c : n->children) { ids.insert(c->id); walk(c); }
		};
		walk(m.root());
		int count = 0;
		std::function<void(node *)> tally = [&](node *n) {
			for (node *c : n->children) { ++count; tally(c); }
		};
		tally(m.root());
		check(ids.size() == count,
		      QString("and every id in the tree is still unique (%1 of %2)")
		          .arg(ids.size()).arg(count));

		// It survives a save, unlike a mirror.
		const QString saved = dir + "/with-new-tab.txt";
		check(m.save(saved), "the tree saves");
		tab_tree_model again;
		again.load(saved);
		check(again.node_by_id(t->id) != nullptr,
		      "and the new tab is in it -- this one is the user's, so it keeps");
		holds(m, "making a tab, which nothing could do");
	}

	section("a tab's name follows the page, unless somebody chose it");
	{
		// Two different things wearing one field. A title that arrived from the
		// page should follow the page; a title a person typed should not be
		// quietly replaced the next time that tab loads something. Before this,
		// the seam carried no title at all -- a tab wore whatever the file said
		// for ever.
		tab_tree_model m;
		m.load(path);
		node *t = m.root()->children.first()->children.first();

		check(m.set_page_title(t, "First page"), "a page title is taken");
		check(t->title == "First page", "and shows");
		check(m.set_page_title(t, "Second page"), "and is replaced on the next page");
		check(t->title == "Second page", "by the newer one");
		check(!m.set_page_title(t, "Second page"),
		      "while the same title again changes nothing, so nothing is saved");
		check(!t->renamed, "and none of that counts as being named");

		// Now a person names it.
		m.update_node(t, "My bank", t->url, t->tags);
		check(t->renamed, "typing a name marks it as chosen");
		check(!m.set_page_title(t, "Bank plc — log in"),
		      "and the page can no longer rename it");
		check(t->title == "My bank", "so the chosen name stays");

		// Opening the editor and pressing OK without touching the name must not
		// pin a title nobody chose.
		tab_tree_model m2;
		m2.load(path);
		node *u = m2.root()->children.first()->children.first();
		m2.set_page_title(u, "From the page");
		m2.update_node(u, u->title, u->url, u->tags);
		check(!u->renamed,
		      "pressing OK without changing the name does not pin it");
		check(m2.set_page_title(u, "Still following"),
		      "so it goes on following the page");

		// Clearing the name hands it back.
		m.update_node(t, QString(), t->url, t->tags);
		check(!t->renamed, "clearing the name un-chooses it");
		check(!t->title.isEmpty(),
		      QString("leaving a label rather than a blank row (%1)").arg(t->title));
		check(m.set_page_title(t, "Bank plc — log in"),
		      "and the page may name it again");
		check(t->title == "Bank plc — log in", "which it does");
		holds(m, "a tab's name follows the page, unless ");
	}

	section("being named survives a save, or it was not worth recording");
	{
		tab_tree_model m;
		m.load(path);
		node *a = m.root()->children.first()->children.first();
		node *b = m.root()->children.first()->children.last();
		m.update_node(a, "Chosen name", a->url, a->tags);
		m.set_page_title(b, "Page name");
		const QString saved = dir + "/named.txt";
		check(m.save(saved), "the tree saves");

		tab_tree_model r;
		check(r.load(saved), "and loads");
		node *ra = r.node_by_id(a->id);
		node *rb = r.node_by_id(b->id);
		check(ra && ra->renamed, "the chosen name is still chosen");
		check(rb && !rb->renamed, "and the page's name is still the page's");
		check(ra && ra->title == "Chosen name", "with the names intact");
		// The flag is only written when true, so an ordinary tree does not grow
		// a column of `named=0`.
		QFile f(saved);
		f.open(QIODevice::ReadOnly);
		const QString text = QString::fromUtf8(f.readAll());
		f.close();
		check(text.count("named=1") == 1 && !text.contains("named=0"),
		      "and only the chosen one carries a marker in the file");

		// A file written before this existed says nothing, and that must read
		// as "not chosen" rather than as a parse failure.
		const QString old = dir + "/old-format.txt";
		QFile o(old);
		o.open(QIODevice::WriteOnly | QIODevice::Truncate);
		o.write("- [z1] unopened | Old tab | https://old.test/ | "
		         "created=2026-01-01T00:00:00 | seen=2026-01-02T00:00:00\n");
		o.close();
		tab_tree_model legacy;
		check(legacy.load(old), "a tree from before the flag loads");
		node *z = legacy.node_by_id("z1");
		check(z && !z->renamed, "as not chosen");
		check(z && z->title == "Old tab" && z->url == "https://old.test/",
		      "with its title and address where they were");
		check(z && z->last_seen.isValid(),
		      "and its dates still parsed, which a new trailing key could have "
		      "broken by stopping the reader early");
		holds(m, "being named survives a save, or it was");
	}

	QDir(dir).removeRecursively();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

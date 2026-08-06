// What must be true of the tab tree, and what happens to a file that breaks it.
#include "tree_invariants.h"
#include "tree_outline.h"
#include "node.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void note(const QString &w) { std::printf("     %s\n", qPrintable(w)); }
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static node *make(const QString &id, node_type t, node *parent) {
	node *n = new node;
	n->id = id;
	n->type = t;
	n->title = id;
	if (!n->is_folder())
		n->url = "https://example.test/" + id;
	n->parent = parent;
	if (parent)
		parent->children.append(n);
	return n;
}

// A tree of `folders` folders, each holding `per` tabs, nested `depth` deep.
// One generator rather than a pile of fixtures: the shapes that break a tree
// are combinations of these three numbers, and a future operation gets the
// same coverage without anybody writing a new fixture for it.
static node *generate(int folders, int per, int depth) {
	node *root = new node;
	root->id = "root";
	root->type = node_type::folder;
	node *at = root;
	for (int d = 0; d < depth; ++d)
		at = make(QString("d%1").arg(d), node_type::folder, at);
	for (int f = 0; f < folders; ++f) {
		node *fo = make(QString("f%1").arg(f), node_type::folder, at);
		for (int i = 0; i < per; ++i)
			make(QString("t%1_%2").arg(f).arg(i), node_type::unopened_tab, fo);
	}
	return root;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("a tree that is fine");
	{
		node *root = generate(3, 4, 2);
		const auto r = tree_invariants::check(root);
		check(r.ok, QString("passes (%1)").arg(r.summary()));
		check(r.nodes == 2 + 3 + 12,
		      QString("and counts every node (%1)").arg(r.nodes));
		check(r.depth == 4, QString("and reports the deepest level (%1)").arg(r.depth));
		delete root;
	}

	section("each violation, one at a time");
	{
		node *root = generate(1, 1, 0);
		make("f0", node_type::folder, root);   // duplicate of the generated f0
		const auto r = tree_invariants::check(root);
		check(!r.ok && r.summary().contains("used more than once"),
		      "a repeated id is caught");
		delete root;
	}
	{
		node *root = generate(1, 1, 0);
		node *stray = new node;
		stray->id = "x";
		stray->type = node_type::unopened_tab;
		stray->parent = nullptr;               // listed here, points nowhere
		root->children.first()->children.append(stray);
		const auto r = tree_invariants::check(root);
		check(!r.ok && r.summary().contains("parent points elsewhere"),
		      "a child whose parent pointer disagrees is caught");
		delete root;
	}
	{
		node *root = generate(1, 1, 0);
		node *f = root->children.first();
		f->children.append(f);                 // its own child
		const auto r = tree_invariants::check(root);
		check(!r.ok && r.summary().contains("more than once"),
		      "a cycle is caught rather than hung on");
		// Broken before the tree is deleted: `~node` deletes its children, and
		// a cycle would take the same node twice.
		f->children.removeAll(f);
		delete root;
	}
	{
		node *root = generate(0, 0, tree_limits::max_depth + 3);
		const auto r = tree_invariants::check(root);
		check(!r.ok && r.summary().contains("past the limit"),
		      QString("nesting past %1 is caught").arg(tree_limits::max_depth));
		delete root;
	}
	{
		node *root = generate(1, 1, 0);
		node *tab = root->children.first()->children.first();
		make("under_a_tab", node_type::unopened_tab, tab);
		const auto r = tree_invariants::check(root);
		// **This assertion was the opposite one**, and it was wrong on its own
		// terms: it required a tab with children to be reported, "since the
		// file cannot hold it". The file always could -- `write_node` recurses
		// into any node's children and the reader nests by indentation without
		// consulting the type -- so the rule was enforcing a model restriction
		// while citing a format limit that did not exist. Sub-tabs (§5.5)
		// removed the restriction, and a tab with a child below it is now the
		// shape the feature produces rather than a violation.
		check(r.ok,
		      "a tab with children is well formed: that is what a sub-tab is");
		delete root;
	}
	{
		node *root = generate(1, 1, 0);
		node *f = root->children.first();
		f->mirror = "firefox";                 // the child is left unmarked
		const auto r = tree_invariants::check(root);
		check(!r.ok && r.summary().contains("not marked as mirrored"),
		      "half a mirror is caught, which is the half that reaches the file");
		delete root;
	}

	section("a file nested past the limit is flattened, not refused");
	{
		const QString path = QDir::temp().filePath("hydra-deep-test.txt");
		QFile f(path);
		f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
		QTextStream out(&f);
		const int too_deep = tree_limits::max_depth + 40;
		for (int i = 0; i < too_deep; ++i)
			out << QString(i * 2, ' ') << "- [f" << i << "] folder | L" << i << "\n";
		f.close();

		int flattened = -1;
		node *root = tree_outline::load(path, &flattened);
		check(flattened == 40,
		      QString("it says how many it moved up (%1)").arg(flattened));
		const auto r = tree_invariants::check(root);
		check(r.ok, QString("and the result is a legal tree (%1)").arg(r.summary()));
		check(r.nodes == too_deep,
		      QString("with every node kept -- nesting is lost, tabs are not (%1)")
		          .arg(r.nodes));
		check(r.depth == tree_limits::max_depth,
		      QString("and nothing past the limit (%1)").arg(r.depth));
		delete root;
		QFile::remove(path);
	}

	section("scale, bounded so this stays a unit test");
	{
		// Deliberately modest. The shapes that hurt are covered by the extreme
		// suite, which is opt-in and memory-capped; what matters here is that
		// the invariants hold on a tree far larger than anyone files by hand,
		// and that checking one is cheap enough to do after every mutation.
		QElapsedTimer t; t.start();
		node *root = generate(200, 50, 8);     // 10,208 nodes
		const qint64 built = t.elapsed();
		t.restart();
		const auto r = tree_invariants::check(root);
		const qint64 checked = t.elapsed();
		check(r.ok, QString("%1 nodes pass (%2)").arg(r.nodes).arg(r.summary().left(48)));
		note(QString("built in %1 ms, checked in %2 ms").arg(built).arg(checked));
		check(checked < 2000,
		      QString("and checking is cheap enough to do constantly (%1 ms)")
		          .arg(checked));

		// A round trip through the file must not change the shape.
		const QString path = QDir::temp().filePath("hydra-scale-test.txt");
		check(tree_outline::save(path, root), "it saves");
		int flattened = -1;
		node *back = tree_outline::load(path, &flattened);
		const auto r2 = tree_invariants::check(back);
		check(flattened == 0, "loading it back flattens nothing");
		check(r2.ok, "and the reloaded tree is legal");
		check(r2.nodes == r.nodes && r2.depth == r.depth,
		      QString("with the same shape (%1/%2 nodes, %3/%4 deep)")
		          .arg(r2.nodes).arg(r.nodes).arg(r2.depth).arg(r.depth));
		delete back;
		delete root;
		QFile::remove(path);
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

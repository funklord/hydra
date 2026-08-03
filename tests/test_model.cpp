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

	QDir(dir).removeRecursively();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

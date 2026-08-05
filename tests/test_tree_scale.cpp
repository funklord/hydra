// The tab tree at scale: how it behaves when there is far more of it than
// anyone files by hand, and what it costs.
//
// **This suite caps its own memory.** A stress test that can take the machine
// down is worse than no stress test, and this one deliberately builds shapes
// designed to be expensive. `setrlimit(RLIMIT_AS)` at startup means a runaway
// allocation fails inside this process rather than pushing the desktop into
// the OOM killer -- which has happened here twice, from other causes, and is
// not a thing to risk for a test.
//
// **And it cleans up as it goes.** The tree file format is O(depth^2) in bytes,
// so a deep shape writes a startlingly large file; one of these probes filled a
// 16 GB tmpfs while it was being written. Each case removes its file
// immediately rather than at the end.
//
// Default sizes are modest so this belongs in `make test`. The genuinely
// extreme shapes are behind HYDRA_SCALE_EXTREME=1, because they take minutes
// and their value is answering "where does it stop" rather than "does it work".
#include "tree_gen.h"
#include "tree_invariants.h"
#include "tree_outline.h"
#include "tab_tree_model.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <sys/resource.h>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void note(const QString &w) { std::printf("     %s\n", qPrintable(w)); }
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// A path that deletes itself, so a case that fails still leaves nothing behind.
struct scratch {
	QString path;
	explicit scratch(const QString &name)
	    : path(QDir::temp().filePath("hydra-scale-" + name + ".txt")) {
		QFile::remove(path);
	}
	~scratch() { QFile::remove(path); }
};

static qint64 file_kb(const QString &path) {
	return QFileInfo(path).size() / 1024;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	const bool extreme = qEnvironmentVariableIntValue("HYDRA_SCALE_EXTREME") == 1;

	// Two gigabytes of address space. Generous for anything sane here and far
	// below what would trouble the machine; a shape that wants more fails as a
	// bad_alloc in this process, which is a test result rather than an
	// incident.
	{
		struct rlimit lim { 2ull << 30, 2ull << 30 };
		if (setrlimit(RLIMIT_AS, &lim) != 0)
			note("could not cap address space; continuing uncapped");
	}
	note(extreme ? "extreme shapes enabled"
	              : "modest sizes; HYDRA_SCALE_EXTREME=1 for the rest");

	section("many tabs, flat -- the ordinary large tree");
	{
		const int per = extreme ? 50000 : 5000;
		node *root = tree_gen::wide(per);
		const auto r = tree_invariants::check(root);
		check(r.ok, QString("%1 nodes are well formed").arg(r.nodes));

		scratch s("wide");
		QElapsedTimer t; t.start();
		check(tree_outline::save(s.path, root), "they save");
		const qint64 save_ms = t.elapsed();
		t.restart();
		int flattened = -1;
		node *back = tree_outline::load(s.path, &flattened);
		const qint64 load_ms = t.elapsed();

		const auto r2 = tree_invariants::check(back);
		check(r2.ok && r2.nodes == r.nodes,
		      QString("and come back identical (%1 nodes)").arg(r2.nodes));
		check(flattened == 0, "with nothing flattened");
		note(QString("save %1 ms, load %2 ms, file %3 KB")
		         .arg(save_ms).arg(load_ms).arg(file_kb(s.path)));
		// Linear-ish is the claim; a wildly superlinear result is the finding.
		check(load_ms < (extreme ? 30000 : 5000),
		      QString("loading stays proportionate (%1 ms)").arg(load_ms));
		delete back;
		delete root;
	}

	section("at the depth limit exactly");
	{
		// `max_depth - 2`, because `build` adds a folder *and* a tab level below
		// the nesting it is asked for: depth + 1 + 1. Written as `- 1` first,
		// which put the tabs one past the limit, and the invariant checker
		// refused the tree the test had just called legal.
		node *root = tree_gen::build(1, 4, tree_limits::max_depth - 2);
		const auto r = tree_invariants::check(root);
		check(r.ok, QString("a tree at the limit is legal (depth %1)").arg(r.depth));

		scratch s("deep");
		check(tree_outline::save(s.path, root), "it saves");
		int flattened = -1;
		node *back = tree_outline::load(s.path, &flattened);
		check(flattened == 0, "and loads back with nothing flattened");
		const auto r2 = tree_invariants::check(back);
		check(r2.ok && r2.depth == r.depth,
		      QString("at the same depth (%1)").arg(r2.depth));
		// The quadratic, bounded: this is what 64 levels costs, and it is the
		// number that justifies the limit.
		note(QString("a tree at the limit costs %1 KB").arg(file_kb(s.path)));
		delete back;
		delete root;
	}

	section("deeper than the limit is flattened, at scale");
	{
		const int over = extreme ? 4000 : 400;
		scratch s("over");
		{
			QFile f(s.path);
			f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
			QTextStream out(&f);
			// Written by hand rather than generated and saved: the point is a
			// *file* that nests too deep, which `save` would never produce.
			for (int i = 0; i < over; ++i)
				out << QString(i * 2, ' ') << "- [f" << i << "] folder | L" << i << "\n";
		}
		QElapsedTimer t; t.start();
		int flattened = -1;
		node *back = tree_outline::load(s.path, &flattened);
		const qint64 ms = t.elapsed();
		const auto r = tree_invariants::check(back);
		check(r.ok, QString("the result is legal (%1)").arg(r.summary().left(52)));
		check(r.nodes == over,
		      QString("every node survives -- nesting is lost, tabs are not (%1)")
		          .arg(r.nodes));
		check(flattened == over - tree_limits::max_depth,
		      QString("and it says how many it moved (%1)").arg(flattened));
		note(QString("loaded a %1 KB over-deep file in %2 ms")
		         .arg(file_kb(s.path)).arg(ms));
		delete back;
	}

	section("the model's own operations, on a large tree");
	{
		const int per = extreme ? 20000 : 2000;
		scratch s("model");
		{
			node *root = tree_gen::build(4, per / 4, 2);
			tree_outline::save(s.path, root);
			delete root;
		}
		tab_tree_model model;
		QElapsedTimer t; t.start();
		check(model.load(s.path), "the model loads it");
		note(QString("model load %1 ms").arg(t.elapsed()));
		check(tree_invariants::check(model.root()).ok, "and it is well formed");

		// Deleting a folder takes its whole subtree, which is the operation
		// most likely to leave dangling parents behind at scale.
		node *victim = model.root()->children.first()->children.first()
		                    ->children.first();
		const int before = tree_invariants::check(model.root()).nodes;
		t.restart();
		model.remove_node(victim);
		const qint64 del_ms = t.elapsed();
		const auto after = tree_invariants::check(model.root());
		check(after.ok, "deleting a large subtree leaves it well formed");
		check(after.nodes < before,
		      QString("and the tree shrank (%1 -> %2)").arg(before).arg(after.nodes));
		note(QString("subtree delete %1 ms").arg(del_ms));
	}

	section("duplicating the same node again and again");
	{
		// `unused_id` counts upward from 2 looking for a free id, so the k-th
		// copy of one node costs O(k) lookups and k copies cost O(k^2). Cheap
		// at any believable number of copies; measured so that a change making
		// it worse is visible rather than inferred.
		scratch s("dup");
		{
			node *root = tree_gen::build(1, 4, 0);
			tree_outline::save(s.path, root);
			delete root;
		}
		tab_tree_model model;
		check(model.load(s.path), "a small tree loads");
		node *seed = model.root()->children.first()->children.first();

		const int copies = extreme ? 4000 : 400;
		QElapsedTimer t; t.start();
		for (int i = 0; i < copies; ++i)
			model.duplicate_node(seed);
		const qint64 ms = t.elapsed();
		const auto r = tree_invariants::check(model.root());
		check(r.ok, QString("%1 copies leave the tree well formed").arg(copies));
		note(QString("%1 duplicates in %2 ms").arg(copies).arg(ms));
		check(ms < (extreme ? 60000 : 5000),
		      QString("and it stays usable (%1 ms)").arg(ms));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

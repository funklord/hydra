// "No node left behind" (architecture doc sec 9.4/sec 9.5).
//
// The reorganizer hands a language model the whole tree and asks it to rearrange
// it. What comes back is text, and this is the gate that decides whether it is
// safe to show a diff for -- so a bug here does not produce a wrong pixel, it
// loses somebody's tabs to a machine that hallucinated.
//
// The invariant, from the header: every original *leaf* id appears exactly once
// in the proposal. Folders are the model's to invent, rename and drop; leaves
// are the user's tabs and are not. Dropped leaves are re-attached and duplicates
// collapsed, because either could lose a tab; an invented leaf id fails the
// whole proposal, because there is no safe repair for a tab that never existed.
//
// Nothing checked any of it.
#include "tree_diff.h"
#include "tab_history.h"
#include "node.h"

#include <QCoreApplication>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// --- tiny tree builder ----------------------------------------------------
static node *mk(const QString &id, bool folder, const QString &title,
                 const QString &url = QString()) {
	auto *n = new node;
	n->id    = id;
	n->type  = folder ? node_type::folder : node_type::unopened_tab;
	n->title = title;
	n->url   = url;
	return n;
}
static node *add(node *parent, node *child) {
	child->parent = parent;
	child->order  = parent->children.size();
	parent->children.push_back(child);
	return child;
}
static node *root_of() { return mk("root", true, "root"); }

// Find by id, for assertions.
static node *find(node *n, const QString &id) {
	if (n->id == id)
		return n;
	for (node *c : n->children)
		if (node *f = find(c, id))
			return f;
	return nullptr;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	// The original, used by most sections:  Work/[a1,a2]  Play/[a3]
	auto build_original = [] {
		node *r = root_of();
		node *work = add(r, mk("f1", true, "Work"));
		add(work, mk("a1", false, "One", "https://x.example/1"));
		add(work, mk("a2", false, "Two", "https://x.example/2"));
		node *play = add(r, mk("f2", true, "Play"));
		add(play, mk("a3", false, "Three", "https://x.example/3"));
		return r;
	};

	section("leaf ids, in document order");
	{
		node *r = build_original();
		check(tree_diff::leaf_ids(r) == QStringList({"a1", "a2", "a3"}),
		      QString("the leaves and only the leaves (%1)")
		          .arg(tree_diff::leaf_ids(r).join(",")));
		delete r;
	}

	section("a faithful proposal passes untouched");
	{
		node *orig = build_original();
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a2", false, "Two"));          // reordered within the folder
		add(f, mk("a1", false, "One"));
		node *g = add(prop, mk("f2", true, "Play"));
		add(g, mk("a3", false, "Three"));

		const proposal_report rep = tree_diff::check_and_repair(orig, prop);
		check(rep.usable, "it is usable");
		check(rep.dropped_ids.isEmpty() && rep.duplicated_ids.isEmpty() &&
		          rep.invented_ids.isEmpty(),
		      "with nothing dropped, duplicated or invented");
		check(tree_diff::leaf_ids(prop) == QStringList({"a2", "a1", "a3"}),
		      "and the proposed order is left as proposed");
		delete orig;
		delete prop;
	}

	section("a leaf the model forgot is put back");
	{
		node *orig = build_original();
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a1", false, "One"));
		// a2 and a3 simply absent.

		const proposal_report rep = tree_diff::check_and_repair(orig, prop);
		check(rep.usable, "the proposal is still usable — this is repairable");
		check(rep.dropped_ids.contains("a2") && rep.dropped_ids.contains("a3"),
		      QString("both omissions are reported (%1)").arg(rep.dropped_ids.join(",")));
		check(tree_diff::leaf_ids(prop).contains("a2") &&
		          tree_diff::leaf_ids(prop).contains("a3"),
		      "and both are back in the tree");
		node *a2 = find(prop, "a2");
		check(a2 && a2->parent && a2->parent->id == "f1",
		      "a2 returns to the parent it had, which survived");
		node *a3 = find(prop, "a3");
		check(a3 && a3->parent && a3->parent->id == "root",
		      QString("a3's parent did not survive, so it goes to the root rather "
		               "than nowhere (%1)")
		          .arg(a3 && a3->parent ? a3->parent->id : QString("none")));
		check(a3 && a3->url == "https://x.example/3",
		      "and it comes back with its url, not as an empty shell");
		delete orig;
		delete prop;
	}

	section("a leaf listed twice is collapsed");
	{
		node *orig = build_original();
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a1", false, "One"));
		add(f, mk("a2", false, "Two"));
		node *g = add(prop, mk("f2", true, "Play"));
		add(g, mk("a3", false, "Three"));
		add(g, mk("a1", false, "One again"));    // the same tab, twice

		const proposal_report rep = tree_diff::check_and_repair(orig, prop);
		check(rep.usable, "still usable");
		check(rep.duplicated_ids.contains("a1"), "the duplicate is reported");
		check(tree_diff::leaf_ids(prop).count("a1") == 1,
		      QString("and only one a1 remains (%1)")
		          .arg(tree_diff::leaf_ids(prop).join(",")));
		check(find(prop, "a1") && find(prop, "a1")->parent->id == "f1",
		      "the first occurrence is the one kept");
		delete orig;
		delete prop;
	}

	section("a duplicate folder takes its subtree, and the leaves come back");
	{
		// The case the implementation calls out: a leaf that existed *only*
		// inside a duplicated subtree would vanish with it, so recovery has to
		// look at what survived the cull rather than at the proposal as given.
		node *orig = build_original();
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a1", false, "One"));
		node *dup = add(prop, mk("f1", true, "Work again"));   // duplicate folder
		add(dup, mk("a2", false, "Two"));                      // only copy of a2
		node *g = add(prop, mk("f2", true, "Play"));
		add(g, mk("a3", false, "Three"));

		const proposal_report rep = tree_diff::check_and_repair(orig, prop);
		check(rep.usable, "usable");
		check(rep.duplicated_ids.contains("f1"), "the duplicated folder is reported");
		check(tree_diff::leaf_ids(prop).contains("a2"),
		      "and the leaf that lived only inside it is recovered, not lost");
		delete orig;
		delete prop;
	}

	section("an invented tab fails the whole proposal");
	{
		node *orig = build_original();
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a1", false, "One"));
		add(f, mk("a2", false, "Two"));
		add(f, mk("a99", false, "A tab nobody has", "https://made.up/"));
		node *g = add(prop, mk("f2", true, "Play"));
		add(g, mk("a3", false, "Three"));

		const proposal_report rep = tree_diff::check_and_repair(orig, prop);
		check(!rep.usable, "it is not usable — there is no safe repair");
		check(rep.invented_ids == QStringList({"a99"}),
		      QString("and the fabricated id is named (%1)")
		          .arg(rep.invented_ids.join(",")));
		check(rep.message.contains("a99"),
		      "the message says which, since the user is being told why");
		delete orig;
		delete prop;
	}

	section("a folder the model invents is allowed");
	{
		node *orig = build_original();
		node *prop = root_of();
		node *newf = add(prop, mk("f-new", true, "Everything"));
		add(newf, mk("a1", false, "One"));
		add(newf, mk("a2", false, "Two"));
		add(newf, mk("a3", false, "Three"));

		const proposal_report rep = tree_diff::check_and_repair(orig, prop);
		check(rep.usable, "usable — inventing folders is the whole job");
		check(rep.new_folders == 1,
		      QString("and the new folder is counted (%1)").arg(rep.new_folders));
		check(rep.invented_ids.isEmpty(), "not treated as an invented node");
		delete orig;
		delete prop;
	}

	section("a tab turned into a folder is an invention, not a rename");
	{
		// Same id, different kind. Silently accepting it would convert somebody's
		// tab into a folder, or a folder full of tabs into a single tab.
		node *orig = build_original();
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a1", false, "One"));
		add(f, mk("a2", false, "Two"));
		add(prop, mk("a3", true, "Three, now a folder"));   // was a leaf

		const proposal_report rep = tree_diff::check_and_repair(orig, prop);
		check(!rep.usable, "the proposal is refused");
		check(rep.invented_ids.contains("a3"), "and the id that changed kind is named");
		delete orig;
		delete prop;
	}

	section("undo: a snapshot puts the tree back");
	{
		node *orig = build_original();
		const tree_snapshot snap = tree_diff::snapshot(orig);
		check(snap.valid(), "a snapshot is taken");

		// Rearrange as an accepted proposal would: move a1 into Play, invent a
		// folder, and rename one.
		node *a1 = find(orig, "a1");
		node *play = find(orig, "f2");
		a1->parent->children.removeAll(a1);
		a1->parent = play;
		play->children.push_back(a1);
		node *invented = add(orig, mk("f-new", true, "Invented"));
		node *a2 = find(orig, "a2");
		a2->parent->children.removeAll(a2);
		a2->parent = invented;
		invented->children.push_back(a2);
		find(orig, "f1")->title = "Renamed";

		const int restored = tree_diff::restore(orig, snap);
		check(restored > 0, QString("restore reports what it did (%1)").arg(restored));
		check(find(orig, "a1") && find(orig, "a1")->parent->id == "f1",
		      "the moved tab is back where it was");
		check(find(orig, "f1") && find(orig, "f1")->title == "Work",
		      "the renamed folder has its name back");
		check(find(orig, "f-new") == nullptr,
		      "the invented folder is gone, since the snapshot never knew it");
		check(find(orig, "a2") != nullptr,
		      "and the tab that was inside it is not gone with it");
		check(find(orig, "a2")->parent->id == "f1",
		      "it is back in its own folder");
		check(tree_diff::leaf_ids(orig) == QStringList({"a1", "a2", "a3"}),
		      QString("every leaf is present, in the original order (%1)")
		          .arg(tree_diff::leaf_ids(orig).join(",")));
		delete orig;
	}

	// **The net was dropping what it was there to save.** The section above
	// checks that a forgotten leaf comes back and lands in the right parent.
	// It said nothing about what came back *with* it, and the answer was: id,
	// type, title, url and tags, because the copy listed five fields by hand.
	//
	// So a tab the model omitted returned **unlocked**, un-renamed, with no
	// dates and no history -- each of those either a decision the person made
	// or content the tab arrived with. In a safety net, which runs exactly
	// when something has already gone wrong.
	section("a leaf put back keeps what the person decided about it");
	{
		node *orig = build_original();
		node *a2 = find(orig, "a2");
		check(a2 != nullptr, "the original has the tab this is about");
		a2->locked   = true;
		a2->renamed  = true;
		a2->tags     = QStringList{ "keep" };
		a2->created  = QDateTime(QDate(2020, 1, 2), QTime(3, 4, 5));
		a2->last_seen = QDateTime(QDate(2021, 6, 7), QTime(8, 9, 10));
		a2->history.entries << history_entry{ "https://example.com/old",
		                                       "Where it had been" };
		a2->history.index = 0;

		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a1", false, "One"));
		// a2 dropped by the model, exactly as in the section above.

		const proposal_report rep = tree_diff::check_and_repair(orig, prop);
		check(rep.dropped_ids.contains("a2"), "the tab is reported dropped");

		node *back = find(prop, "a2");
		check(back != nullptr, "and put back");
		if (back) {
			check(back->locked,
			      "it is still locked — a pin the person set, not the model's");
			check(back->renamed,
			      "still marked renamed, so the page title cannot overwrite "
			      "the title they chose");
			check(back->tags == (QStringList{ "keep" }), "with its tags");
			check(back->created == a2->created, "the date it was created");
			check(back->last_seen == a2->last_seen, "and when it was last seen");
			check(back->history.entries.size() == 1 && back->history.index == 0,
			      QString("and the past it arrived with (%1 entr(y/ies))")
			          .arg(back->history.entries.size()));
			// The two the section above already covers, asserted here as well
			// because a copy made wholesale could get the structure wrong in
			// the other direction.
			check(back->parent && back->parent->id == "f1",
			      "and it is still in the parent it had");
			check(back->children.isEmpty(),
			      "with no subtree dragged along by the copy");
		}
		delete orig;
		delete prop;
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

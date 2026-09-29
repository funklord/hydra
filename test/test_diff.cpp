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
#include <algorithm>
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

// The whole arrangement as one string: "root[f1[a1,a2],f2[a3]]". Asserting a
// shape rather than one node's parent is what catches a change that lands in
// the right family and the wrong place -- which is the defect this file's
// apply() sections were written for.
static QString shape(node *n) {
	QString s = n->id;
	if (n->children.isEmpty())
		return s;
	QStringList kids;
	for (node *c : n->children)
		kids << shape(c);
	return s + "[" + kids.join(",") + "]";
}

static int count_of(const QList<tree_change> &ch, change_kind k) {
	int n = 0;
	for (const tree_change &c : ch)
		if (c.kind == k)
			++n;
	return n;
}

static const tree_change *change_for(const QList<tree_change> &ch,
                                      const QString &id, change_kind k) {
	for (const tree_change &c : ch)
		if (c.node_id == id && c.kind == k)
			return &c;
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
		tree_snapshot snap = tree_diff::snapshot(orig);
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
		snap.invented_folders << "f-new";   // what the accepted changes created

		const int restored = tree_diff::restore(orig, snap);
		check(restored > 0, QString("restore reports what it did (%1)").arg(restored));
		check(find(orig, "a1") && find(orig, "a1")->parent->id == "f1",
		      "the moved tab is back where it was");
		check(find(orig, "f1") && find(orig, "f1")->title == "Work",
		      "the renamed folder has its name back");
		check(find(orig, "f-new") == nullptr,
		      "the invented folder is gone, since the record names it");
		check(find(orig, "a2") != nullptr,
		      "and the tab that was inside it is not gone with it");
		check(find(orig, "a2")->parent->id == "f1",
		      "it is back in its own folder");
		check(tree_diff::leaf_ids(orig) == QStringList({"a1", "a2", "a3"}),
		      QString("every leaf is present, in the original order (%1)")
		          .arg(tree_diff::leaf_ids(orig).join(",")));
		delete orig;
	}

	section("the tree is untouched until a proposal is applied");
	{
		// **Two places promise this and nothing checked it.** The menu entry
		// says "nothing changes until you accept" and the dialog says "the tree
		// is unchanged until you apply", and between them sits a repair pass
		// that writes into a tree -- `check_and_repair` re-attaches leaves the
		// model dropped, which is a mutation, and the question is only which
		// tree it mutates. `apply_reorganization` runs from `on_accept` alone.
		//
		// Pointer identity as well as shape: the repair copies a dropped leaf
		// into the proposal with `new node(*orig)`, and a version that moved it
		// instead would leave the same shape here with the original's node
		// living in somebody else's tree.
		node *orig = build_original();
		const QString before = shape(orig);
		node *p_a1 = find(orig, "a1"), *p_a2 = find(orig, "a2"),
		     *p_a3 = find(orig, "a3"), *p_f1 = find(orig, "f1");
		const QString t_f1 = p_f1->title;

		// A proposal that needs every kind of repair: a1 listed twice, a2
		// dropped, a folder invented, and f1 renamed.
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Renamed by the model"));
		add(f, mk("a1", false, "One"));
		node *g = add(prop, mk("f2", true, "Play"));
		add(g, mk("a3", false, "Three"));
		add(g, mk("a1", false, "One again"));
		node *nf = add(prop, mk("f-new", true, "Invented"));
		add(nf, mk("a9", false, "This one does not exist"));

		const proposal_report rep = tree_diff::check_and_repair(orig, prop);
		check(!rep.usable,
		      "a proposal inventing a tab id is refused, as it should be");
		check(shape(orig) == before,
		      QString("and the original is untouched by the refusal (%1)")
		          .arg(shape(orig)));

		// Now a repairable one, which is the case that writes.
		node *prop2 = root_of();
		node *f2 = add(prop2, mk("f1", true, "Renamed by the model"));
		add(f2, mk("a1", false, "One"));
		add(f2, mk("a1", false, "One again"));     // duplicate
		node *nf2 = add(prop2, mk("f-new", true, "Invented"));
		add(nf2, mk("a3", false, "Three"));
		// a2 simply absent.
		const proposal_report rep2 = tree_diff::check_and_repair(orig, prop2);
		check(rep2.usable, "a repairable proposal is usable");
		check(!rep2.dropped_ids.isEmpty() && !rep2.duplicated_ids.isEmpty(),
		      "and it did have to repair something");
		const QList<tree_change> ch = tree_diff::compute(orig, prop2);
		check(!ch.isEmpty(), "and a change list is derived from it");

		check(shape(orig) == before,
		      QString("the tree still has its own shape (%1)").arg(shape(orig)));
		check(find(orig, "a1") == p_a1 && find(orig, "a2") == p_a2 &&
		          find(orig, "a3") == p_a3 && find(orig, "f1") == p_f1,
		      "and the very same nodes, not copies put back");
		check(p_f1->title == t_f1,
		      QString("the folder the model renamed still has its own title "
		               "(%1)").arg(p_f1->title));
		check(tree_diff::leaf_ids(orig) == QStringList({"a1", "a2", "a3"}),
		      QString("with every tab where it was (%1)")
		          .arg(tree_diff::leaf_ids(orig).join(",")));
		delete orig;
		delete prop;
		delete prop2;
	}

	section("undo does not delete a tab opened since the snapshot");
	{
		// **The menu entry promises to "put the tree back the way it was before
		// the last accepted reorganization", and the undo stays available until
		// it is pressed.** Nothing invalidates the snapshot when the tree
		// changes in between -- so between accepting a reorganization and
		// pressing Undo, a person can open a tab. Its id is not in the
		// snapshot.
		//
		// `restore` deleted everything the snapshot did not know, on the
		// strength of a comment saying "whatever the snapshot never knew about
		// is a folder the reorganization invented". That is true only if
		// nothing has been added since, and it is a `delete` rather than a
		// move: the ordinary Reopen Closed Tab net does not cover it, because
		// nothing went through the deletion path.
		//
		// The invariant that makes the distinction safe is already in this
		// file: `check_and_repair` REJECTS a proposal that invents a leaf id,
		// so an unknown leaf cannot be the model's -- it is somebody's tab.
		node *orig = build_original();
		tree_snapshot snap = tree_diff::snapshot(orig);

		// The reorganization: a1 into Play, and an invented folder holding a2.
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

		// And then the person opens three tabs: one at the top level, one
		// inside a folder that survived, and one inside the folder the model
		// invented -- which is the arrangement with nowhere obvious to put it
		// back, since its parent is about to be deleted.
		add(orig, mk("t9", false, "Opened afterwards",
		              "https://example.com/after"));
		add(find(orig, "f1"), mk("t10", false, "And another",
		                          "https://example.com/after2"));
		add(invented, mk("t11", false, "Opened in the new folder",
		                  "https://example.com/after3"));

		// **What the model invented is named, not inferred.** The undo record
		// carries the ids the accepted changes created, which is what lets the
		// section below keep a folder the person made -- indistinguishable
		// from this one by any property of the node.
		snap.invented_folders << "f-new";

		const int restored = tree_diff::restore(orig, snap);
		check(restored > 0, QString("restore reports what it did (%1)").arg(restored));
		check(find(orig, "f-new") == nullptr,
		      "the invented folder is still gone");
		check(find(orig, "t9") != nullptr,
		      "a tab opened at the top level since the snapshot survives");
		check(find(orig, "t10") != nullptr,
		      "and so does one opened inside a folder");
		check(find(orig, "t9") && find(orig, "t9")->parent == orig,
		      QString("the top-level one is still at the top level (%1)")
		          .arg(find(orig, "t9") && find(orig, "t9")->parent
		                 ? find(orig, "t9")->parent->id : QString("nowhere")));
		check(find(orig, "t10") && find(orig, "t10")->parent &&
		          find(orig, "t10")->parent->id == "f1",
		      QString("and the other is still in the folder it was opened in "
		               "(%1)")
		          .arg(find(orig, "t10") && find(orig, "t10")->parent
		                 ? find(orig, "t10")->parent->id : QString("nowhere")));
		// Reachable, not merely alive: a node whose parent no longer lists it
		// is invisible to the tree and to every save, which is a leak wearing
		// the appearance of a survivor.
		check(shape(orig).contains("t9") && shape(orig).contains("t10"),
		      QString("both are reachable from the root (%1)").arg(shape(orig)));
		check(find(orig, "t11") != nullptr,
		      "a tab opened inside the invented folder survives the folder");
		check(find(orig, "t11") && find(orig, "t11")->parent == orig,
		      QString("and goes to the root, since the folder it was in is the "
		               "one being removed (%1)")
		          .arg(find(orig, "t11") && find(orig, "t11")->parent
		                 ? find(orig, "t11")->parent->id : QString("nowhere")));
		check(shape(orig).contains("t11"),
		      QString("reachable there rather than merely alive (%1)")
		          .arg(shape(orig)));
		check(tree_diff::leaf_ids(orig).size() == 6,
		      QString("six leaves: the three that were there and the three that "
		               "were opened (%1)")
		          .arg(tree_diff::leaf_ids(orig).join(",")));
		delete orig;
	}

	section("undo does not delete a folder made since the snapshot");
	{
		// **The same defect one fix later, and the same cost.** Unknown leaves
		// were kept and unknown folders were still deleted, because nothing
		// about a node says who made it: a folder the model invented and a
		// folder the person made between accepting and pressing Undo are the
		// same node with a different author.
		//
		// So the undo record names the ids the accepted changes created, and
		// `restore` deletes those. That is delete-by-name rather than
		// delete-by-absence, which is the rule this workspace already holds
		// for removing files.
		node *orig = build_original();
		tree_snapshot snap = tree_diff::snapshot(orig);

		// The reorganization invents one folder and moves a2 into it.
		node *invented = add(orig, mk("f-model", true, "Invented"));
		node *a2 = find(orig, "a2");
		a2->parent->children.removeAll(a2);
		a2->parent = invented;
		invented->children.push_back(a2);
		snap.invented_folders << "f-model";

		// Then the person makes a folder of their own and puts a new tab in
		// it -- and another folder inside the model's, which is the case with
		// nowhere obvious to go back to.
		node *mine = add(orig, mk("f-mine", true, "Mine"));
		add(mine, mk("t20", false, "In my folder", "https://example.com/mine"));
		node *nested = add(invented, mk("f-nested", true, "Mine, inside theirs"));

		tree_diff::restore(orig, snap);

		check(find(orig, "f-model") == nullptr,
		      "the folder the record names is gone");
		check(find(orig, "f-mine") != nullptr,
		      "the folder the person made is not");
		check(shape(orig).contains("f-mine"),
		      QString("and is reachable from the root (%1)").arg(shape(orig)));
		check(find(orig, "t20") != nullptr &&
		          find(orig, "t20")->parent == mine,
		      "the tab in it is still in it");
		check(shape(orig).contains("t20"),
		      QString("reachable there rather than merely alive (%1)")
		          .arg(shape(orig)));
		check(find(orig, "f-nested") != nullptr &&
		          find(orig, "f-nested")->parent == orig,
		      "a folder made inside the model's goes to the root rather than "
		      "away with it");
		check(shape(orig).contains("f-nested"),
		      QString("reachable there too (%1)").arg(shape(orig)));
		check(nested->parent == orig, "and its parent pointer agrees");
		delete orig;
	}

	// **An empty list deletes nothing, which is the safe direction.** A
	// reorganization that invented no folder names none, and every folder in
	// the tree is then somebody's.
	section("undo with nothing named deletes no folder");
	{
		node *orig = build_original();
		const tree_snapshot snap = tree_diff::snapshot(orig);
		node *mine = add(orig, mk("f-only-mine", true, "Mine"));
		add(mine, mk("t21", false, "In it", "https://example.com/only"));

		tree_diff::restore(orig, snap);
		check(find(orig, "f-only-mine") != nullptr,
		      "a folder made after the snapshot survives an undo that names "
		      "nothing");
		check(shape(orig).contains("f-only-mine") && shape(orig).contains("t21"),
		      QString("with its tab, both reachable (%1)").arg(shape(orig)));
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

	// --- compute() and apply() ------------------------------------------
	//
	// Everything above is check_and_repair and the undo snapshot. The two steps
	// between them -- deriving the change list the user ticks, and applying what
	// they ticked to their live tree -- had no test at all, in either this file
	// or any other: `tree_diff::compute` is called once, from
	// `reorganize_dialog`, and `tree_diff::apply` once, from `tab_tree_model`,
	// and nothing else in the tree named either. That is the step that actually
	// moves somebody's tabs.

	section("compute names each kind of change, and only where there is one");
	{
		node *orig = build_original();          // f1 Work/[a1,a2]  f2 Play/[a3]
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Jobs"));    // same folder, new title
		add(f, mk("a1", false, "One"));                 // unmoved
		node *nf = add(prop, mk("f9", true, "News"));   // invented
		add(nf, mk("a2", false, "Two"));                // moved into it
		node *g = add(prop, mk("f2", true, "Play"));    // pushed down one place
		add(g, mk("a3", false, "Three"));               // unmoved

		const proposal_report rep = tree_diff::check_and_repair(orig, prop);
		check(rep.usable, "the proposal is usable");

		const QList<tree_change> ch = tree_diff::compute(orig, prop);
		check(count_of(ch, change_kind::folder_new) == 1, "one folder invented");
		check(count_of(ch, change_kind::folder_renamed) == 1, "one folder renamed");
		check(count_of(ch, change_kind::reparented) == 1, "one leaf reparented");
		check(count_of(ch, change_kind::reordered) == 1,
		      "one sibling reordered, because inserting a folder moved it down");
		check(count_of(ch, change_kind::duplicate_url) == 0,
		      "and no duplicate urls, since every leaf has its own");
		check(ch.size() == 4,
		      QString("four changes and no more -- a1 and a3 did not move, and a "
		               "change list that names them would be asking the person to "
		               "tick nothing (%1)")
		          .arg(ch.size()));
		check(change_for(ch, "f9", change_kind::folder_new) != nullptr &&
		          change_for(ch, "f1", change_kind::folder_renamed) != nullptr &&
		          change_for(ch, "a2", change_kind::reparented) != nullptr &&
		          change_for(ch, "f2", change_kind::reordered) != nullptr,
		      "each one against the node it is about");

		bool all_summarised = true, all_default_accepted = true;
		for (const tree_change &c : ch) {
			if (c.summary.isEmpty())
				all_summarised = false;
			if (c.kind != change_kind::duplicate_url && !c.accepted)
				all_default_accepted = false;
		}
		check(all_summarised,
		      "every change says something, since the list is what the person reads");
		check(all_default_accepted, "and arrives ticked");

		const tree_change *rename = change_for(ch, "f1", change_kind::folder_renamed);
		check(rename && rename->new_title == "Jobs",
		      "the rename carries the new title rather than only the fact of it");
		const tree_change *move = change_for(ch, "a2", change_kind::reparented);
		check(move && move->new_parent_id == "f9",
		      "and the move carries where it is going");
		delete orig;
		delete prop;
	}

	section("a folder a leaf is moved into exists before the move, in any order");
	{
		// apply() runs the new folders in a pass of their own, and the reason is
		// that a change list is a list: nothing says the folder comes before the
		// move, and a person ticking changes in a dialog can hand back any
		// order at all. So the list is deliberately reversed here.
		node *orig = build_original();
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a1", false, "One"));
		node *nf = add(prop, mk("f9", true, "News"));
		add(nf, mk("a2", false, "Two"));
		node *g = add(prop, mk("f2", true, "Play"));
		add(g, mk("a3", false, "Three"));
		tree_diff::check_and_repair(orig, prop);

		QList<tree_change> ch = tree_diff::compute(orig, prop);
		std::reverse(ch.begin(), ch.end());

		const int applied = tree_diff::apply(orig, ch);
		check(applied == ch.size(),
		      QString("every change applied (%1 of %2)").arg(applied).arg(ch.size()));
		node *a2 = find(orig, "a2");
		check(a2 && a2->parent && a2->parent->id == "f9",
		      "the leaf is in the folder that did not exist when the list was made");
		check(shape(orig) == "root[f1[a1],f9[a2],f2[a3]]",
		      QString("and the whole arrangement is the proposed one (%1)")
		          .arg(shape(orig)));
		delete orig;
		delete prop;
	}

	section("a new folder lands where it was proposed, not merely somewhere");
	{
		// The invented folder goes FIRST in the proposal, which is the case the
		// surviving reorder changes cannot repair: they say where the existing
		// siblings go, and none of them says where the new folder goes.
		node *orig = build_original();
		node *prop = root_of();
		node *nf = add(prop, mk("f9", true, "News"));
		add(nf, mk("a2", false, "Two"));
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a1", false, "One"));
		node *g = add(prop, mk("f2", true, "Play"));
		add(g, mk("a3", false, "Three"));
		tree_diff::check_and_repair(orig, prop);

		const QList<tree_change> ch = tree_diff::compute(orig, prop);
		const tree_change *nfc = change_for(ch, "f9", change_kind::folder_new);
		check(nfc && nfc->new_order == 0,
		      QString("compute records the position the folder was proposed at (%1)")
		          .arg(nfc ? nfc->new_order : -1));
		tree_diff::apply(orig, ch);
		check(shape(orig) == "root[f9[a2],f1[a1],f2[a3]]",
		      QString("and apply puts it there (%1)").arg(shape(orig)));
		delete orig;
		delete prop;
	}

	section("two invented folders, interleaved with the ones that survive");
	{
		// One folder in the right place could be a coincidence of a single
		// insertion; two, with existing siblings between them, cannot.
		node *orig = build_original();
		node *prop = root_of();
		node *n1 = add(prop, mk("g1", true, "First"));
		add(n1, mk("a1", false, "One"));
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a2", false, "Two"));
		node *n2 = add(prop, mk("g2", true, "Third"));
		add(n2, mk("a3", false, "Three"));
		add(prop, mk("f2", true, "Play"));   // empty now, and kept
		tree_diff::check_and_repair(orig, prop);

		const QList<tree_change> ch = tree_diff::compute(orig, prop);
		check(count_of(ch, change_kind::folder_new) == 2, "both are invented");
		tree_diff::apply(orig, ch);
		check(shape(orig) == "root[g1[a1],f1[a2],g2[a3],f2]",
		      QString("and the arrangement is the proposed one (%1)")
		          .arg(shape(orig)));
		delete orig;
		delete prop;
	}

	section("a reorder is not a reparent");
	{
		node *orig = build_original();
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a2", false, "Two"));     // swapped with a1
		add(f, mk("a1", false, "One"));
		node *g = add(prop, mk("f2", true, "Play"));
		add(g, mk("a3", false, "Three"));
		tree_diff::check_and_repair(orig, prop);

		const QList<tree_change> ch = tree_diff::compute(orig, prop);
		check(count_of(ch, change_kind::reparented) == 0,
		      "nothing changed parent, so nothing is called a move");
		check(count_of(ch, change_kind::reordered) == 2,
		      QString("both of the swapped leaves are reorders (%1)")
		          .arg(count_of(ch, change_kind::reordered)));
		const int applied = tree_diff::apply(orig, ch);
		check(applied == 2, QString("both applied (%1)").arg(applied));
		check(shape(orig) == "root[f1[a2,a1],f2[a3]]",
		      QString("and the swap happened (%1)").arg(shape(orig)));
		node *a1 = find(orig, "a1"), *a2 = find(orig, "a2");
		check(a1 && a2 && a2->order == 0 && a1->order == 1,
		      "with the order fields renumbered to match the arrangement");
		delete orig;
		delete prop;
	}

	section("two tabs on one page are advisory, and never pre-ticked");
	{
		// Two leaves sharing a url is legitimate -- the same page open twice --
		// so a merge is offered rather than done. `accepted = false` is the
		// whole safety property here: a person who ticks everything and presses
		// apply must not lose a tab to it.
		node *orig = root_of();
		node *w = add(orig, mk("f1", true, "Work"));
		add(w, mk("a1", false, "One", "https://x.example/same"));
		add(w, mk("a2", false, "Two", "https://x.example/same"));

		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a1", false, "One", "https://x.example/same"));
		add(f, mk("a2", false, "Two", "https://x.example/same"));
		tree_diff::check_and_repair(orig, prop);

		const QList<tree_change> ch = tree_diff::compute(orig, prop);
		check(count_of(ch, change_kind::duplicate_url) == 1,
		      QString("the second one is reported (%1)")
		          .arg(count_of(ch, change_kind::duplicate_url)));
		const tree_change *d = change_for(ch, "a2", change_kind::duplicate_url);
		check(d && !d->accepted,
		      "unticked, because merging is destructive and this is a suggestion");
		check(d && !d->summary.isEmpty() && d->summary.contains("a1"),
		      "and it names the other tab, which is the only way to judge it");

		QList<tree_change> ticked = ch;
		for (tree_change &c : ticked)
			c.accepted = true;   // the person ticks everything, including this
		const int applied = tree_diff::apply(orig, ticked);
		check(applied == 0,
		      QString("ticking it applies nothing, since no merge is implemented "
		               "(%1 applied)").arg(applied));
		check(shape(orig) == "root[f1[a1,a2]]",
		      QString("and both tabs are still there (%1)").arg(shape(orig)));
		delete orig;
		delete prop;
	}

	section("a locked node does not move, however the proposal is written");
	{
		// sec 5.5. The refusal is in apply() rather than in the model, because
		// the reorganizer proposes from a serialized tree and a lock has to
		// survive that round trip without being trusted to.
		node *orig = build_original();
		node *a1 = find(orig, "a1");
		check(a1 != nullptr, "the tab this is about exists");
		a1->locked = true;

		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a2", false, "Two"));
		node *g = add(prop, mk("f2", true, "Play"));
		add(g, mk("a1", false, "One"));    // the model moves the locked tab
		add(g, mk("a3", false, "Three"));
		tree_diff::check_and_repair(orig, prop);

		const QList<tree_change> ch = tree_diff::compute(orig, prop);
		check(change_for(ch, "a1", change_kind::reparented) != nullptr,
		      "the move is proposed, since the proposal is allowed to ask");
		const int applied = tree_diff::apply(orig, ch);
		node *moved = find(orig, "a1");
		check(moved && moved->parent && moved->parent->id == "f1",
		      QString("and refused, so the tab is where the person pinned it (%1)")
		          .arg(moved && moved->parent ? moved->parent->id : "gone"));
		check(applied == count_of(ch, change_kind::reordered) +
		          count_of(ch, change_kind::reparented) - 1,
		      QString("and the refusal is not counted as applied (%1 of %2)")
		          .arg(applied).arg(ch.size()));
		delete orig;
		delete prop;
	}

	section("nothing moves inside itself");
	{
		// Not reachable from compute(), which derives changes from a tree and
		// so cannot describe a cycle. It is reachable from a change list, which
		// is what apply() takes -- and the guard is in apply() for that reason.
		node *orig = root_of();
		node *f1 = add(orig, mk("f1", true, "Work"));
		add(f1, mk("f1a", true, "Inner"));
		add(orig, mk("a1", false, "One", "https://x.example/1"));

		tree_change into_self;
		into_self.kind          = change_kind::reparented;
		into_self.node_id       = "f1";
		into_self.new_parent_id = "f1";
		tree_change into_child;
		into_child.kind          = change_kind::reparented;
		into_child.node_id       = "f1";
		into_child.new_parent_id = "f1a";

		const int a = tree_diff::apply(orig, QList<tree_change>{ into_self });
		check(a == 0, QString("a node cannot become its own parent (%1)").arg(a));
		const int b = tree_diff::apply(orig, QList<tree_change>{ into_child });
		check(b == 0, QString("nor a child of its own subtree (%1)").arg(b));
		check(shape(orig) == "root[f1[f1a],a1]",
		      QString("and the tree is untouched by either (%1)").arg(shape(orig)));
		delete orig;
	}

	section("a change the person unticked does not happen");
	{
		node *orig = build_original();
		node *prop = root_of();
		node *f = add(prop, mk("f1", true, "Work"));
		add(f, mk("a1", false, "One"));
		node *g = add(prop, mk("f2", true, "Play"));
		add(g, mk("a2", false, "Two"));     // proposed move
		add(g, mk("a3", false, "Three"));
		tree_diff::check_and_repair(orig, prop);

		QList<tree_change> ch = tree_diff::compute(orig, prop);
		check(!ch.isEmpty(), "there is something to untick");
		for (tree_change &c : ch)
			c.accepted = false;
		const int applied = tree_diff::apply(orig, ch);
		check(applied == 0, QString("nothing is applied (%1)").arg(applied));
		check(shape(orig) == "root[f1[a1,a2],f2[a3]]",
		      QString("and the tree is exactly as it was (%1)").arg(shape(orig)));
		delete orig;
		delete prop;
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

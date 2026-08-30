#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

struct node;

// What the model did to one node.
enum class change_kind {
	reparented,      // same node, different parent
	reordered,       // same parent, different position among siblings
	folder_new,      // a folder the model invented
	folder_renamed,  // an existing folder with a new title
	duplicate_url,   // two leaves share a URL -- offered as a merge candidate
};

struct tree_change {
	change_kind kind;
	QString     node_id;
	QString     summary;         // one line, for the diff list
	QString     new_parent_id;   // reparented / folder_new
	int         new_order = 0;
	QString     new_title;       // folder_renamed / folder_new
	bool        accepted = true; // cherry-pick state
};

// --- Undo ----------------------------------------------------------------
// sec 9.4 asks for "a single undo snapshot [that] makes any accepted change one
// keystroke to revert". This is that snapshot.
//
// It records structure only -- id, parent, order, and folder titles -- because
// structure is the only thing a reorganization changes. Live views and state
// blobs are keyed by id and were never stored on the node, so restoring the
// structure puts every tab back where it was without touching its payload,
// for exactly the same reason applying the change could not disturb them.
struct tree_snapshot_entry {
	QString id;
	QString parent_id;   // "root" for a top-level node
	QString title;
	int     order  = 0;
	bool    folder = false;
};

struct tree_snapshot {
	QList<tree_snapshot_entry> entries;
	bool valid() const { return !entries.isEmpty(); }
};

// The result of checking a proposal against the original -- the
// "no node left behind" gate (architecture doc sec 9.4). Nothing is shown to the
// user until this has run and, where possible, repaired.
struct proposal_report {
	bool        usable = false;   // false = reject outright, do not show a diff
	QStringList dropped_ids;      // original leaves the model omitted (repaired)
	QStringList duplicated_ids;   // leaves it listed more than once (repaired)
	QStringList invented_ids;     // leaf ids that never existed (rejected)
	int         new_folders = 0;
	QString     message;          // human-readable summary of the above
};

// Verifies and repairs a proposal in place, then derives the change list.
//
// The invariant: every original *leaf* id appears exactly once in the proposal.
// Folders are the model's to invent, rename, and drop; leaves are the user's
// tabs and are not. So dropped leaves are re-attached and duplicates collapsed
// rather than surfaced as a diff that could lose a tab, while an invented leaf
// id -- a tab the model made up -- fails the whole proposal, because there is no
// safe repair for it and accepting one would put a fabricated entry in the tree.
namespace tree_diff {

// Repairs `proposal` against `original`. Both are synthetic roots.
proposal_report check_and_repair(node *original, node *proposal);

// Atomic changes derived from two id-keyed trees (architecture doc sec 9.5).
// Call only on a proposal that passed check_and_repair.
QList<tree_change> compute(node *original, node *proposal);

// Applies the accepted changes to `original`, in place. Returns how many were
// applied. Because live views and state blobs are keyed by node id and never
// stored on the node, re-parenting cannot disturb them (sec 4.2).
int apply(node *original, const QList<tree_change> &changes);

// Every leaf id in a tree, in document order.
QStringList leaf_ids(node *root);

tree_snapshot snapshot(node *root);

// Puts `root` back into the recorded shape. Nodes the snapshot does not know
// about are folders the reorganization invented, and are deleted -- their
// children are re-attached first, so nothing is lost with them. Returns the
// number of nodes restored.
int restore(node *root, const tree_snapshot &snap);

}  // namespace tree_diff

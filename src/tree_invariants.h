#pragma once

#include "node.h"

#include <QString>
#include <QStringList>

// What must be true of the tab tree, always, whatever put it in that state.
//
// **This exists to be called after operations nobody has written yet.** The
// tree is restructured by things that cannot enumerate their own failure
// modes -- drag and drop, an AI reorganisation, a mirror refresh replacing a
// folder while a view inside it is live -- and each of those arrived with its
// own tests for its own behaviour. What none of them can do is notice that
// they left the tree subtly wrong in a way that only shows up in a different
// feature much later.
//
// So the checks are stated once, here, and called after every mutation in
// every suite. A future feature that corrupts the tree then fails in tests
// that were written before it existed and know nothing about it, which is the
// only kind of coverage that survives the thing it was meant to catch being
// replaced.
//
// Cheap enough to call constantly: one walk, a hash of ids, no allocation per
// node beyond that.
namespace tree_invariants {

struct report {
	bool        ok = true;
	QStringList problems;   // every violation found, not just the first
	int         nodes = 0;
	int         depth = 0;  // deepest level below the root

	QString summary() const;
};

// Walks the whole tree and checks:
//
//   - ids are unique, and non-empty
//   - `parent` agrees with the parent's `children` list, both ways
//   - no cycles, and no node reachable twice
//   - depth is within `tree_limits::max_depth`
//   - a child of a mirror is itself marked as that mirror, since a mirror is
//     a subtree and half of one would be written to the tree file
//
// **A tab with children is NOT a violation**, and this list said it was long
// after the check had gone. Sub-tabs are the model's own feature (architecture
// doc sec 5.5), the rule was removed deliberately, and the reason it gave --
// that the tree file cannot express a tab with children -- was never true:
// `write_node` recurses into any node's children and the reader nests by
// indentation without consulting the type. See the note where the check used
// to be, which is where that is argued out.
//
// It is corrected here rather than quietly deleted because this list is what
// a reader consults to find out what the tree may look like, so a retired
// rule left standing in it answers a design question the wrong way.
//
// `root` is the synthetic root, which is exempt from the id and parent rules
// because nothing owns it.
report check(node *root);

}  // namespace tree_invariants

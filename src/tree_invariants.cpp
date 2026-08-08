// SPDX-License-Identifier: GPL-3.0-or-later
#include "tree_invariants.h"

#include <QHash>
#include <QSet>

namespace tree_invariants {

QString report::summary() const {
	if (ok)
		return QString("%1 nodes, depth %2, no violations").arg(nodes).arg(depth);
	return QString("%1 nodes, depth %2, %3 violation(s): %4")
		.arg(nodes).arg(depth).arg(problems.size())
		.arg(problems.join("; "));
}

namespace {

// Iterative, not recursive, and that is the point rather than a style
// preference: this is the one function that has to survive a tree somebody
// else built badly. A recursive checker blows the stack on exactly the input
// it exists to reject, and a crash is a worse diagnosis than a report.
void walk(node *root, report &r) {
	QHash<QString, int> seen_id;
	QSet<const node *>  visited;

	struct frame { node *n; int depth; };
	QList<frame> stack;
	stack.append({ root, 0 });

	while (!stack.isEmpty()) {
		const frame f = stack.takeLast();
		node *n = f.n;
		if (!n) {
			r.problems << "a null child";
			continue;
		}

		// Reached twice means the graph is not a tree: either a cycle, or one
		// node listed by two parents. Either way the walk must stop descending
		// here or it does not terminate.
		if (visited.contains(n)) {
			r.problems << QString("node '%1' is reachable more than once "
					"(cycle or shared child)").arg(n->id);
			continue;
		}
		visited.insert(n);

		if (n != root) {
			++r.nodes;
			r.depth = qMax(r.depth, f.depth);

			if (n->id.isEmpty())
				r.problems << QString("a node at depth %1 has no id").arg(f.depth);
			else if (++seen_id[n->id] == 2)
				r.problems << QString("id '%1' is used more than once").arg(n->id);

			if (f.depth > tree_limits::max_depth)
				r.problems << QString("node '%1' is at depth %2, past the limit "
						"of %3")
						.arg(n->id).arg(f.depth).arg(tree_limits::max_depth);

			// **A tab with children was a violation here** and is not one now:
			// that is what a sub-tab is (architecture doc §5.5). The rule gave
			// its reason as "the tree file cannot express that", and the file
			// always could -- `write_node` recurses into any node's children
			// and the reader nests by indentation without consulting the type.
			// The rule was enforcing a model restriction while citing a format
			// limit that did not exist, which is why removing the restriction
			// left nothing here to keep.
		}

		for (int i = 0; i < n->children.size(); ++i) {
			node *c = n->children.at(i);
			if (!c)
				continue;
			if (c->parent != n)
				r.problems << QString("'%1' is listed under '%2' but its parent "
						"points elsewhere")
						.arg(c->id, n->id);
			// `order` is the position in this list, written down. It is what
			// tree-order sorting compares on and what the reorganizer diffs, so
			// a stored value that disagrees with the list is two answers to one
			// question -- and the sort has no defined result for a tie.
			//
			// Checked here rather than trusted at each mutation because it is a
			// property of the tree rather than of any one edit: a new node took
			// `children.size()` as its order, which is one past the highest only
			// while nothing has left the list. A delete and a drag-out both
			// leave a gap in the numbering without leaving one in the count, so
			// the next node added collides with a sibling still sitting there.
			// Three siblings were measured holding order 2.
			if (c->order != i)
				r.problems << QString("'%1' is at position %2 under '%3' but "
						"records order %4")
						.arg(c->id).arg(i).arg(n->id).arg(c->order);
			// A mirror is a subtree, and half a mirror is the dangerous shape:
			// the unmarked half would be written to the tree file, resurrecting
			// somebody else's tabs as though they had been filed.
			if (!n->mirror.isEmpty() && c->mirror != n->mirror)
				r.problems << QString("'%1' is inside mirror '%2' but is not "
						"marked as mirrored")
						.arg(c->id, n->mirror);
			stack.append({ c, f.depth + 1 });
		}
	}
}

}  // namespace

report check(node *root) {
	report r;
	if (!root) {
		r.ok = false;
		r.problems << "the tree has no root";
		return r;
	}
	walk(root, r);
	r.ok = r.problems.isEmpty();
	return r;
}

}  // namespace tree_invariants

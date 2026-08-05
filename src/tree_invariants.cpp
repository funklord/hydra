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

			if (!n->is_folder() && !n->children.isEmpty())
				r.problems << QString("'%1' is a tab with %2 child(ren); the "
						"tree file cannot express that")
						.arg(n->id).arg(n->children.size());
		}

		for (node *c : n->children) {
			if (!c)
				continue;
			if (c->parent != n)
				r.problems << QString("'%1' is listed under '%2' but its parent "
						"points elsewhere")
						.arg(c->id, n->id);
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

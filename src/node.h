// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QList>

// A single entry in the tab/link tree. Folder or leaf.
// Runtime weight (a live web view backend, or a suspended state blob) is attached
// separately by `id` in the shell — never stored on the node itself — so moving
// a node never disturbs its payload. See architecture doc §4.2.
enum class node_type { folder, open_tab, unopened_tab, suspended_tab };

struct node {
	QString   id;                 // short, opaque, stable for the node's lifetime
	node_type type = node_type::unopened_tab;
	QString   title;
	QString   url;                // empty for pure folders
	QDateTime created;
	QDateTime last_seen;
	QStringList tags;

	int       order = 0;          // canonical sibling order as loaded from disk

	// Non-empty when this node is a *mirror* of somewhere else -- the name of
	// the source, e.g. "firefox". Set on the folder and on everything under it.
	//
	// Two things follow, and both are the point. A mirror is **not written to
	// the tree file**: it is a view of another browser's session, and saving it
	// would resurrect a stale copy of somebody else's tabs as real ones on the
	// next launch, indistinguishable from tabs the user had filed themselves.
	// And a mirror can be *replaced* wholesale when the source is re-read,
	// which is what makes the polled version later the same mechanism as the
	// one-shot one rather than a second.
	//
	// Dragging a mirrored tab into the tree copies it, and the copy has no
	// `mirror` -- which is exactly what "keep this one" means.
	QString   mirror;

	// Somebody named this, so browsing must not rename it back.
	//
	// The two cases are genuinely different and only the node can tell them
	// apart: a title that arrived from the page should follow the page, and a
	// title a person typed should not be quietly replaced the next time that
	// tab loads something. This is stored rather than derived because there is
	// nothing to derive it from -- a title is just a string, and "did a human
	// choose this" is not recoverable from the string afterwards.
	bool      renamed = false;

	// Pinned: this node keeps its page and its place (architecture doc §5.5).
	//
	// Two effects, and they are one idea rather than two features bolted
	// together -- "this row stays as it is". Navigating a locked tab does not
	// change its url; the navigation opens a **sub-tab** below it and browsing
	// continues there. And the node cannot be moved: not dragged to another
	// parent, not reordered among its siblings, not moved by the reorganizer.
	//
	// A locked tab is an anchor -- a search result page, a forum index, a
	// reference being worked from -- and an anchor that can drift is not one.
	// The flag lives on the node rather than on the view because a suspended
	// tab is still locked, and because it has to survive being written to the
	// outline file and read back.
	bool      locked = false;

	node        *parent = nullptr;
	QList<node *> children;

	~node() { qDeleteAll(children); }

	bool is_folder() const { return type == node_type::folder; }

	int row() const {
		return parent ? parent->children.indexOf(const_cast<node *>(this)) : 0;
	}
};

// How deep the tree may nest.
//
// **A bound on a quadratic, not a taste in filing.** The tree file expresses
// nesting as two spaces of indent per level, so a chain of `d` folders writes
// `2 + 4 + ... + 2d` spaces: the file is O(d^2) in bytes for O(d) tabs.
// Measured, on this format:
//
//     16,000 folders nested   ->  245 MB file, 526 MB resident
//     50,000 tabs flat        ->  a small file, 28 MB resident
//
// So the shape that hurts is depth, and it is reachable by dragging a folder
// into a folder repeatedly. 64 is far past any filing anyone does by hand --
// deeper than most filesystems are used to -- and it bounds the quadratic term
// at nothing.
//
// Exceeding it is not an error: a tree that is too deep is **flattened** at the
// depth limit rather than refused, because refusing a file loses tabs and
// flattening loses only nesting. Whatever flattens says so rather than doing it
// quietly.
namespace tree_limits {
constexpr int max_depth = 64;
}

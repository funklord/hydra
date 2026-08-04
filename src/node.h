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

	node        *parent = nullptr;
	QList<node *> children;

	~node() { qDeleteAll(children); }

	bool is_folder() const { return type == node_type::folder; }

	int row() const {
		return parent ? parent->children.indexOf(const_cast<node *>(this)) : 0;
	}
};

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

	node        *parent = nullptr;
	QList<node *> children;

	~node() { qDeleteAll(children); }

	bool is_folder() const { return type == node_type::folder; }

	int row() const {
		return parent ? parent->children.indexOf(const_cast<node *>(this)) : 0;
	}
};

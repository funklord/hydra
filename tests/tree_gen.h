// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "node.h"

#include <QString>

// One tree generator, shared by the suites that need trees.
//
// **Parameterised rather than fixed**, because the shapes that break a tab tree
// are combinations of three numbers -- how many nodes, how wide a folder gets,
// how deep the nesting goes -- and a pile of hand-written fixtures covers the
// three shapes somebody thought of on the day. A future operation gets the same
// coverage by being handed these, without anybody writing a fixture for it.
//
// Header-only and in `tests/` because it is test scaffolding: it builds
// deliberately extreme trees, which is not something the application should
// have a function for.
namespace tree_gen {

inline node *leaf(const QString &id, node *parent) {
	node *n   = new node;
	n->id     = id;
	n->type   = node_type::unopened_tab;
	n->title  = "Tab " + id;
	n->url    = "https://example.test/" + id;
	n->parent = parent;
	if (parent)
		parent->children.append(n);
	return n;
}

inline node *folder(const QString &id, node *parent) {
	node *n   = new node;
	n->id     = id;
	n->type   = node_type::folder;
	n->title  = "Folder " + id;
	n->parent = parent;
	if (parent)
		parent->children.append(n);
	return n;
}

// `depth` nested folders, then `folders` folders side by side at the bottom,
// each holding `per` tabs. Total nodes: depth + folders * (1 + per).
inline node *build(int folders, int per, int depth) {
	node *root = new node;
	root->id   = "root";
	root->type = node_type::folder;

	node *at = root;
	for (int d = 0; d < depth; ++d)
		at = folder(QString("d%1").arg(d), at);
	for (int f = 0; f < folders; ++f) {
		node *fo = folder(QString("f%1").arg(f), at);
		for (int i = 0; i < per; ++i)
			leaf(QString("t%1_%2").arg(f).arg(i), fo);
	}
	return root;
}

// One folder holding `n` tabs, which is the shape a view has to draw and a
// proxy has to sort. Kept separate from `build` because "wide" and "many" are
// different questions and conflating them hides which one is slow.
inline node *wide(int n) {
	return build(1, n, 0);
}

}  // namespace tree_gen

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "node.h"

#include <QString>

// One tree generator, shared by the suites that need trees --
// `test_invariants` and `test_tree_scale`. **Add a third caller here rather
// than a third generator**, which is not a style preference: this file and
// `test_invariants` each held a copy of it, with the same three parameters,
// the same node ids and the same docstring arguing for one generator rather
// than a pile of fixtures. Both copies made that argument while being two of
// them, and neither said the other existed.
//
// What the duplication cost is recorded under `attach` below.
//
// **Parameterised rather than fixed**, because the shapes that break a tab tree
// are combinations of three numbers -- how many nodes, how wide a folder gets,
// how deep the nesting goes -- and a pile of hand-written fixtures covers the
// three shapes somebody thought of on the day. A future operation gets the same
// coverage by being handed these, without anybody writing a fixture for it.
//
// Header-only and in `test/` because it is test scaffolding: it builds
// deliberately extreme trees, which is not something the application should
// have a function for.
namespace tree_gen {

// **`order` is set here, and it was not** -- in either copy, and that is what
// the duplication cost. Every node came out holding the default 0, so a
// generated tree had ten thousand siblings all claiming position zero: a shape
// no code path in the application can produce, since the outline reader numbers
// as it nests, the model renumbers on every mutation and `tree_diff` renumbers
// after a restore.
//
// A fixture that builds something the product cannot is testing a tree nobody
// will ever have, and this one hid the invariant that says so -- the checker
// could not be given the rule about `order` until the generator obeyed it. Then
// it had to be fixed twice, and the second copy was found only because the
// first fix left its suite still failing.
inline node *attach(node *n, node *parent) {
	n->parent = parent;
	if (parent) {
		n->order = parent->children.size();
		parent->children.append(n);
	}
	return n;
}

inline node *leaf(const QString &id, node *parent) {
	node *n   = new node;
	n->id     = id;
	n->type   = node_type::unopened_tab;
	n->title  = "Tab " + id;
	n->url    = "https://example.test/" + id;
	return attach(n, parent);
}

inline node *folder(const QString &id, node *parent) {
	node *n   = new node;
	n->id     = id;
	n->type   = node_type::folder;
	n->title  = "Folder " + id;
	return attach(n, parent);
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

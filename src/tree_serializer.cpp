// SPDX-License-Identifier: GPL-3.0-or-later
#include "tree_serializer.h"
#include "tree_outline.h"
#include "node.h"

#include <QStringList>

namespace {

void write_node(QString &out, node *n, int depth) {
	const QString indent(depth * 2, ' ');
	out += indent + "- [" + n->id + "] " + tree_outline::type_to_string(n->type)
	     + " | " + n->title;
	if (!n->is_folder())
		out += " | " + n->url;
	if (!n->tags.isEmpty())
		out += " | tags=" + n->tags.join(',');
	out += "\n";
	for (node *c : n->children)
		write_node(out, c, depth + 1);
}

int leading_spaces(const QString &line) {
	int n = 0;
	while (n < line.size() && line.at(n) == ' ') ++n;
	return n;
}

}  // namespace

namespace tree_serializer {

QString to_payload(node *root) {
	QString out;
	if (!root)
		return out;
	for (node *c : root->children)
		write_node(out, c, 0);
	return out;
}

node *parse_proposal(const QString &text) {
	node *root = new node;
	root->id   = "root";
	root->type = node_type::folder;

	// A model will often wrap the outline in prose or a fenced code block.
	// Rather than demand clean output, take the node lines and ignore the rest;
	// the invariant check downstream is what actually guards correctness.
	struct frame { int depth; node *n; };
	QVector<frame> stack;
	stack.push_back({-1, root});

	bool any = false;
	const QStringList lines = text.split('\n');
	for (const QString &raw : lines) {
		const QString line = raw.trimmed();
		if (!line.startsWith("- ["))
			continue;
		const int close = line.indexOf(']');
		if (close < 0)
			continue;

		const int depth = leading_spaces(raw) / 2;
		const QString id = line.mid(3, close - 3).trimmed();
		if (id.isEmpty())
			continue;

		const QStringList fields = line.mid(close + 1).trimmed().split(" | ");

		node *n  = new node;
		n->id    = id;
		n->type  = tree_outline::type_from_string(fields.value(0).trimmed());

		// From the right, for the same reason as the tree file: the title is the
		// one field that may contain " | ", and "Article | Site" is ordinary.
		// Here the loss went further than a reload -- a proposal parsed this way
		// carries the wrong url into the diff the user is asked to accept.
		QStringList rest_fields = fields.mid(1);
		for (auto &f : rest_fields)
			f = f.trimmed();
		while (!rest_fields.isEmpty() && rest_fields.last().startsWith("tags=")) {
			n->tags = rest_fields.last().mid(5).split(',', Qt::SkipEmptyParts);
			rest_fields.removeLast();
		}
		if (!n->is_folder() && rest_fields.size() >= 2)
			n->url = rest_fields.takeLast();
		n->title = rest_fields.join(" | ");

		while (stack.size() > 1 && stack.last().depth >= depth)
			stack.pop_back();

		node *parent = stack.last().n;
		n->parent = parent;
		n->order  = parent->children.size();
		parent->children.push_back(n);
		stack.push_back({depth, n});
		any = true;
	}

	if (!any) {
		delete root;
		return nullptr;
	}
	return root;
}

}  // namespace tree_serializer

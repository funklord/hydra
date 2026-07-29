// SPDX-License-Identifier: GPL-3.0-or-later
#include "tree_outline.h"
#include "node.h"

#include <QFile>
#include <QTextStream>
#include <QVector>

namespace tree_outline {

QString type_to_string(node_type t) {
	switch (t) {
		case node_type::folder:        return "folder";
		case node_type::open_tab:      return "open";
		case node_type::unopened_tab:  return "unopened";
		case node_type::suspended_tab: return "suspended";
	}
	return "unopened";
}

node_type type_from_string(const QString &s) {
	if (s == "folder")    return node_type::folder;
	if (s == "open")      return node_type::open_tab;
	if (s == "suspended") return node_type::suspended_tab;
	return node_type::unopened_tab;
}

}  // namespace tree_outline

namespace {

int leading_spaces(const QString &line) {
	int n = 0;
	while (n < line.size() && line.at(n) == ' ') ++n;
	return n;
}

}  // namespace

namespace tree_outline {

node *load(const QString &path) {
	node *root = new node;
	root->id   = "root";
	root->type = node_type::folder;

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return root;

	QTextStream in(&f);
	// Stack of (depth, node) used to resolve each line's parent.
	struct frame { int depth; node *n; };
	QVector<frame> stack;
	stack.push_back({-1, root});

	int counter = 0;
	while (!in.atEnd()) {
		const QString raw = in.readLine();
		if (raw.trimmed().isEmpty())
			continue;

		const int depth = leading_spaces(raw) / 2;
		QString line = raw.trimmed();
		if (!line.startsWith("- ["))
			continue;

		const int close = line.indexOf(']');
		if (close < 0)
			continue;

		const QString id = line.mid(3, close - 3);
		QString rest = line.mid(close + 1).trimmed();
		const QStringList fields = rest.split(" | ");
		if (fields.isEmpty())
			continue;

		node *n   = new node;
		n->id     = id.isEmpty() ? QString("n%1").arg(counter) : id;
		n->type   = type_from_string(fields.value(0).trimmed());
		n->title  = fields.value(1).trimmed();
		if (!n->is_folder())
			n->url = fields.value(2).trimmed();

		for (const QString &field : fields) {
			const QString t = field.trimmed();
			if (t.startsWith("created="))
				n->created = QDateTime::fromString(t.mid(8), Qt::ISODate);
			else if (t.startsWith("seen="))
				n->last_seen = QDateTime::fromString(t.mid(5), Qt::ISODate);
		}
		if (!n->created.isValid())   n->created   = QDateTime::currentDateTime();
		if (!n->last_seen.isValid()) n->last_seen = n->created;

		// Pop until the stack top is a shallower node; that becomes the parent.
		while (stack.size() > 1 && stack.last().depth >= depth)
			stack.pop_back();

		node *parent = stack.last().n;
		n->parent = parent;
		n->order  = parent->children.size();
		parent->children.push_back(n);

		stack.push_back({depth, n});
		++counter;
	}
	return root;
}

static void write_node(QTextStream &out, node *n, int depth) {
	const QString indent(depth * 2, ' ');
	if (n->is_folder()) {
		out << indent << "- [" << n->id << "] folder | " << n->title << "\n";
	} else {
		out << indent << "- [" << n->id << "] " << type_to_string(n->type)
		     << " | " << n->title
		     << " | " << n->url
		     << " | created=" << n->created.toString(Qt::ISODate)
		     << " | seen="    << n->last_seen.toString(Qt::ISODate) << "\n";
	}
	for (node *c : n->children)
		write_node(out, c, depth + 1);
}

bool save(const QString &path, node *root) {
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
		return false;
	QTextStream out(&f);
	for (node *c : root->children)
		write_node(out, c, 0);
	return true;
}

}  // namespace tree_outline

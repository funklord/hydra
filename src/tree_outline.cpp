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

node *load(const QString &path, int *flattened) {
	if (flattened)
		*flattened = 0;
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

		// Clamped, not rejected. A node deeper than the limit becomes a sibling
		// at the limit, so its tabs survive and only its nesting is lost.
		//
		// `max_depth - 1`, and the off-by-one is worth naming: indentation in
		// the file is 0-based and counts *below* the synthetic root, while
		// depth in the tree counts the root as level 0. So a line indented `d`
		// lands at tree depth `d + 1`, and clamping the indentation to the
		// limit produces a tree one level past it. Caught by the invariant
		// checker on its first run, which is the argument for having one.
		const int raw_depth = leading_spaces(raw) / 2;
		const int depth     = qMin(raw_depth, tree_limits::max_depth - 1);
		if (raw_depth > depth && flattened)
			++*flattened;
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

		// Fields are read **from the right**, and the title is whatever is left.
		//
		// Reading them left to right -- type, title, url -- assumes a title with
		// no " | " in it, and "Article Title | Site Name" is one of the commonest
		// shapes a page title takes on the web. Such a title used to shift every
		// field after it: the url became the tail of the title and the real url
		// landed in a position nothing read, so **it was lost on the next
		// reload**. The title is the one free-form field here; the url cannot
		// contain a space unencoded and the trailing keys cannot either, so
		// working inwards from the end is unambiguous where working outwards
		// from the start is not.
		QStringList rest_fields = fields.mid(1);
		for (auto &f : rest_fields)
			f = f.trimmed();
		while (!rest_fields.isEmpty()) {
			const QString &t = rest_fields.last();
			if (t.startsWith("created="))
				n->created = QDateTime::fromString(t.mid(8), Qt::ISODate);
			else if (t.startsWith("seen="))
				n->last_seen = QDateTime::fromString(t.mid(5), Qt::ISODate);
			else if (t.startsWith("named="))
				n->renamed = t.mid(6) == "1";
			else if (t.startsWith("locked="))
				n->locked = t.mid(7) == "1";
			else
				break;
			rest_fields.removeLast();
		}
		// A page's url is the last field before the metadata -- but only when
		// there is something in front of it to be the title, so a node written
		// with a title and no url does not lose the title instead.
		if (!n->is_folder() && rest_fields.size() >= 2)
			n->url = rest_fields.takeLast();
		n->title = rest_fields.join(" | ");
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
	// A mirror is a view of another browser's session, not part of this tree.
	// Writing it would resurrect a stale copy of somebody else's tabs on the
	// next launch, indistinguishable from tabs the user had filed themselves --
	// and the whole subtree goes with it, since a child of a mirror is
	// mirrored too.
	if (!n->mirror.isEmpty())
		return;
	const QString indent(depth * 2, ' ');
	if (n->is_folder()) {
		out << indent << "- [" << n->id << "] folder | " << n->title;
		// A folder can be locked too. The url half of a lock means nothing to
		// one, but the half that pins it beside its siblings means exactly what
		// it means for a tab, and a folder that would not stay where it was put
		// is the same complaint.
		if (n->locked)
			out << " | locked=1";
		out << "\n";
	} else {
		out << indent << "- [" << n->id << "] " << type_to_string(n->type)
			   << " | " << n->title
			   << " | " << n->url
			   << " | created=" << n->created.toString(Qt::ISODate)
			   << " | seen="    << n->last_seen.toString(Qt::ISODate);
		// Only when true, so a file full of ordinary tabs does not grow a column
		// of `named=0`. The reader treats absence as false, which is also what
		// every file written before this existed says.
		if (n->renamed)
			out << " | named=1";
		if (n->locked)
			out << " | locked=1";
		out << "\n";
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

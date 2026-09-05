#include "tree_outline.h"
#include "node.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
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

node *load(const QString &path, int *flattened, int *unparsed) {
	if (flattened)
		*flattened = 0;
	if (unparsed)
		*unparsed = 0;
	node *root = new node;
	root->id   = "root";
	root->type = node_type::folder;

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		// **"There is no file yet" and "there is a file and I could not read
		// it" are opposite answers, and returning an empty tree for both is
		// how the second one destroyed the first one's data.**
		//
		// An empty tree is exactly what a first run legitimately produces, so
		// a caller given one has no way to tell a fresh install from a tree it
		// has just failed to read -- and the next save wrote that empty tree
		// back over the file. The write lands even though the read did not:
		// an atomic save renames a new file over the old, and `rename` needs
		// write permission on the *directory*, not on the file it replaces.
		// Measured as an ordinary user; a mode-000 file in a writable
		// directory is replaced without complaint.
		//
		// So only the second returns nothing, and the caller is expected to
		// stop rather than carry on with a tree it invented.
		if (QFileInfo::exists(path)) {
			qCritical("tree: %s exists but could not be opened (%s); refusing "
			           "to treat it as an empty tree, because the next save "
			           "would write that back over it",
			           qPrintable(path), qPrintable(f.errorString()));
			delete root;
			return nullptr;
		}
		return root;
	}

	QTextStream in(&f);
	// Stack of (depth, node) used to resolve each line's parent.
	struct frame { int depth; node *n; };
	QVector<frame> stack;
	stack.push_back({-1, root});

	int counter = 0;
	// Lines with something on them, and lines that became a node. A file with
	// the first and none of the second was not read, whatever the reason.
	int content = 0, made = 0, lost = 0;
	while (!in.atEnd()) {
		const QString raw = in.readLine();
		if (raw.trimmed().isEmpty())
			continue;
		++content;

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
		if (!line.startsWith("- [")) {
			++lost;
			continue;
		}

		const int close = line.indexOf(']');
		if (close < 0) {
			++lost;
			continue;
		}

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
			else if (t.startsWith("tags="))
				n->tags = t.mid(5).split(QLatin1Char(','), Qt::SkipEmptyParts);
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
		++made;
	}

	// **A file with content and no nodes was not read.** Every reason it can
	// happen -- a different format, a truncation, an encoding that turned the
	// markers into something else, a file that was never a tree -- produces
	// the same bytes-in-nothing-out, and the same empty root a first run
	// produces legitimately. That indistinguishability is what let the next
	// save write the empty tree back, so the two are told apart here instead.
	//
	// A partial read is deliberately NOT refused, for the reason the depth
	// clamp above is not: refusing loses every tab, where carrying on loses
	// only the lines that would not parse. `unparsed` is how the caller
	// learns it happened, and it must do something with it -- what it writes
	// back is what parsed.
	if (content > 0 && made == 0) {
		qCritical("tree: %s has %d line(s) and none of them is a node; "
		           "refusing to read it as an empty tree, because the next "
		           "save would write that back over it",
		           qPrintable(path), content);
		delete root;
		return nullptr;
	}
	if (unparsed)
		*unparsed = lost;
	return root;
}

// The tags as one field, or nothing when there are none.
//
// **`|` is stripped, because the line format has no escape.** Fields are
// separated by " | " and read from the right, so a tag containing one would
// split the line and the loader would take part of it as the url -- which is
// the same fault that once ate a real address when a title held " | ". The
// dialog already splits on commas, so a comma cannot reach here; a bar can.
static QString tags_field(node *n) {
	if (n->tags.isEmpty())
		return QString();
	QStringList clean;
	for (QString t : n->tags) {
		// `simplified` rather than `remove` then `trimmed`: taking the bar out
		// of "a | b" leaves "a  b" with the gap it used to separate, and a tag
		// carrying a double space is a different tag from the one beside it.
		t = t.replace(QLatin1Char('|'), QLatin1Char(' ')).simplified();
		if (!t.isEmpty())
			clean << t;
	}
	return clean.isEmpty() ? QString()
	                        : " | tags=" + clean.join(QLatin1Char(','));
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
		out << tags_field(n) << "\n";
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
		out << tags_field(n) << "\n";
	}
	for (node *c : n->children)
		write_node(out, c, depth + 1);
}

// **Atomic, because this file is now written while the browser is running.**
// `main_window` flushes the tree on a debounce timer rather than only on the
// way out, so the moment between truncating and finishing the last line is a
// moment the process can be killed in -- and what is left then is a tree file
// that parses, with the tabs from the top of the tree in it and none of the
// rest. A stale tree is a tree; a half-written one is a loss that reads as a
// successful load.
//
// The second fault was quieter and is the reason this used to return true
// unconditionally. `QTextStream` buffers, so every write here landed after the
// last statement of the function: the stream flushed in its destructor, the
// file closed in its own, and a full disk or a failed write was discovered by
// nobody. `commit()` is the first thing in this file that can report that the
// bytes actually arrived.
//
// The stream is scoped so that it flushes into the temporary file *before*
// commit() renames it over the target. Left to the end of the function the
// order reverses, and commit() would publish a file missing its tail.
bool save(const QString &path, node *root) {
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
		return false;
	{
		QTextStream out(&f);
		for (node *c : root->children)
			write_node(out, c, 0);
	}
	return f.commit();
}

}  // namespace tree_outline

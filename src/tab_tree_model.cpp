// SPDX-License-Identifier: GPL-3.0-or-later
#include "tab_tree_model.h"
#include "node.h"
#include "tree_outline.h"
#include "tree_diff.h"

#include <QMimeData>
#include <functional>
#include <QApplication>
#include <QStyle>
#include <QFont>
#include <QBrush>
#include <QColor>

tab_tree_model::tab_tree_model(QObject *parent)
	: QAbstractItemModel(parent) {
	m_root = new node;
	m_root->id   = "root";
	m_root->type = node_type::folder;
}

tab_tree_model::~tab_tree_model() {
	delete m_root;  // deletes the whole tree recursively (node dtor)
}

static void index_subtree(node *n, QHash<QString, node *> &map) {
	for (node *c : n->children) {
		map.insert(c->id, c);
		index_subtree(c, map);
	}
}

void tab_tree_model::reindex() {
	m_id_index.clear();
	index_subtree(m_root, m_id_index);
}

bool tab_tree_model::load(const QString &path) {
	node *fresh = tree_outline::load(path);
	if (!fresh)
		return false;
	beginResetModel();
	delete m_root;
	m_root = fresh;
	m_path = path;
	reindex();
	endResetModel();
	return true;
}

bool tab_tree_model::save(const QString &path) const {
	return tree_outline::save(path.isEmpty() ? m_path : path, m_root);
}

node *tab_tree_model::node_for_index(const QModelIndex &index) const {
	if (!index.isValid())
		return m_root;
	return static_cast<node *>(index.internalPointer());
}

node *tab_tree_model::node_by_id(const QString &id) const {
	return m_id_index.value(id, nullptr);
}

QModelIndex tab_tree_model::index_for_node(node *n) const {
	if (!n || n == m_root)
		return {};
	return createIndex(n->row(), 0, n);
}

void tab_tree_model::refresh_node(node *n) {
	const QModelIndex idx = index_for_node(n);
	if (idx.isValid())
		emit dataChanged(idx, idx);
}

QModelIndex tab_tree_model::index(int row, int column, const QModelIndex &parent) const {
	if (!hasIndex(row, column, parent))
		return {};
	node *parent_node = node_for_index(parent);
	if (row < 0 || row >= parent_node->children.size())
		return {};
	return createIndex(row, column, parent_node->children.at(row));
}

QModelIndex tab_tree_model::parent(const QModelIndex &index) const {
	if (!index.isValid())
		return {};
	node *n = static_cast<node *>(index.internalPointer());
	node *parent_node = n ? n->parent : nullptr;
	if (!parent_node || parent_node == m_root)
		return {};
	return createIndex(parent_node->row(), 0, parent_node);
}

int tab_tree_model::rowCount(const QModelIndex &parent) const {
	if (parent.column() > 0)
		return 0;
	return node_for_index(parent)->children.size();
}

int tab_tree_model::columnCount(const QModelIndex &) const {
	return 1;
}

QVariant tab_tree_model::data(const QModelIndex &index, int role) const {
	if (!index.isValid())
		return {};
	node *n = static_cast<node *>(index.internalPointer());
	if (!n)
		return {};

	switch (role) {
		case Qt::DisplayRole:
			return n->title.isEmpty() ? n->url : n->title;
		case Qt::ToolTipRole:
			return n->url;
		case Qt::DecorationRole: {
			QStyle *s = QApplication::style();
			return s->standardIcon(n->is_folder() ? QStyle::SP_DirIcon
			                                      : QStyle::SP_FileIcon);
		}
		case Qt::FontRole: {
			// Open (live) tabs are bold; suspended are italic.
			QFont f;
			if (n->type == node_type::open_tab)      f.setBold(true);
			if (n->type == node_type::suspended_tab) f.setItalic(true);
			return f;
		}
		case Qt::ForegroundRole:
			// Unopened links are muted; everything else default.
			if (n->type == node_type::unopened_tab)
				return QBrush(QColor(140, 140, 140));
			return {};
		case title_role:      return n->title;
		case url_role:        return n->url;
		case created_role:    return n->created;
		case last_seen_role:  return n->last_seen;
		case tree_order_role: return n->order;
		case node_type_role:  return static_cast<int>(n->type);
		default:              return {};
	}
}

tree_snapshot tab_tree_model::take_snapshot() const {
	return tree_diff::snapshot(m_root);
}

int tab_tree_model::restore_snapshot(const tree_snapshot &snap) {
	beginResetModel();
	const int n = tree_diff::restore(m_root, snap);
	reindex();
	endResetModel();
	return n;
}

int tab_tree_model::apply_reorganization(const QList<tree_change> &changes) {
	// Moves and new folders restructure whole subtrees at once, so a reset is
	// both simpler and safer here than a sequence of begin/endMoveRows calls —
	// and this runs once, on an explicit user action, not on a hot path.
	beginResetModel();
	const int applied = tree_diff::apply(m_root, changes);
	reindex();
	endResetModel();
	return applied;
}

// --- Moving nodes about ---------------------------------------------------

Qt::ItemFlags tab_tree_model::flags(const QModelIndex &index) const {
	Qt::ItemFlags f = QAbstractItemModel::flags(index);
	if (!index.isValid())
		return f | Qt::ItemIsDropEnabled;   // the root accepts drops
	f |= Qt::ItemIsDragEnabled;
	// Only a folder can contain something. Dropping *onto* a tab would have to
	// mean "beside it", and a gesture that means one thing on one row and
	// another thing on the next is a gesture people stop trusting.
	if (node *n = node_for_index(index))
		if (n->is_folder())
			f |= Qt::ItemIsDropEnabled;
	return f;
}

Qt::DropActions tab_tree_model::supportedDropActions() const {
	return Qt::MoveAction | Qt::CopyAction;
}

QStringList tab_tree_model::mimeTypes() const {
	// Our own type, carrying ids rather than urls. An id is what the state
	// blob and the outline file are keyed by, so moving by id keeps a tab's
	// history and suspended state attached to it; moving by url would quietly
	// produce a new tab that had forgotten where it had been.
	return { QStringLiteral("application/x-hydra-node-ids"),
	         QStringLiteral("text/uri-list") };
}

QMimeData *tab_tree_model::mimeData(const QModelIndexList &indexes) const {
	QStringList ids, urls;
	for (const QModelIndex &i : indexes) {
		if (i.column() != 0)
			continue;   // one entry per row, not per column
		if (node *n = node_for_index(i)) {
			ids << n->id;
			if (!n->url.isEmpty())
				urls << n->url;
		}
	}
	if (ids.isEmpty())
		return nullptr;
	auto *m = new QMimeData;
	m->setData("application/x-hydra-node-ids", ids.join('\n').toUtf8());
	// And as plain urls, so a tab can be dragged to any other application that
	// takes one. Costs two lines and is the difference between a tree that
	// talks to the desktop and one that only talks to itself.
	if (!urls.isEmpty())
		m->setText(urls.join('\n'));
	return m;
}

bool tab_tree_model::is_ancestor_of(const node *maybe_ancestor, const node *n) const {
	for (const node *p = n; p; p = p->parent)
		if (p == maybe_ancestor)
			return true;
	return false;
}

QString tab_tree_model::unused_id(const QString &like) const {
	// Keep the shape of the id it came from -- they are short and opaque, and a
	// copy of `a1` reading `a1-2` stays readable in the outline file a person
	// may well open in an editor.
	for (int n = 2; ; ++n) {
		const QString candidate = QString("%1-%2").arg(like).arg(n);
		if (!m_id_index.contains(candidate))
			return candidate;
	}
}

static node *deep_copy(const node *src, tab_tree_model *model,
                        const std::function<QString(const QString &)> &fresh) {
	node *c = new node;
	c->id       = fresh(src->id);
	c->type     = src->type;
	c->title    = src->title;
	c->url      = src->url;
	c->created  = src->created;
	c->last_seen = src->last_seen;
	c->tags     = src->tags;
	// **Not** a copy of the open/suspended state. A copied tab is a second
	// bookmark of the same address, not a second live view of it: the state
	// blob belongs to the id it was written under, and duplicating the id is
	// exactly what `unused_id` exists to prevent.
	if (c->type == node_type::open_tab || c->type == node_type::suspended_tab)
		c->type = node_type::unopened_tab;
	for (const node *k : src->children) {
		node *kid = deep_copy(k, model, fresh);
		kid->parent = c;
		c->children << kid;
	}
	return c;
}

bool tab_tree_model::dropMimeData(const QMimeData *data, Qt::DropAction action,
                                   int row, int column, const QModelIndex &parent) {
	Q_UNUSED(column);
	if (!data || action == Qt::IgnoreAction)
		return false;
	if (!data->hasFormat("application/x-hydra-node-ids"))
		return false;

	node *target = parent.isValid() ? node_for_index(parent) : m_root;
	if (!target || !target->is_folder())
		return false;

	const QStringList ids =
		QString::fromUtf8(data->data("application/x-hydra-node-ids")).split('\n');

	QList<node *> moving;
	for (const QString &id : ids) {
		node *n = m_id_index.value(id);
		if (!n)
			continue;
		// A folder cannot be dropped inside itself: the tree would become a
		// ring, the outline writer would recurse forever, and every node below
		// the drag would vanish from the file. The reorganizer refuses the same
		// move for the same reason (§9.4) and this is that rule again, one
		// gesture closer to the user.
		if (is_ancestor_of(n, target))
			return false;
		if (n == target)
			return false;
		moving << n;
	}
	if (moving.isEmpty())
		return false;

	// Reset rather than fine-grained move signals, matching what `load` and
	// `apply_reorganization` already do here. It costs the view's expansion
	// state on a drop, which is worth revisiting; it is not worth risking a
	// begin/endMoveRows index mistake for, since those corrupt the view in ways
	// that show up much later than they happen.
	beginResetModel();
	for (node *n : moving) {
		if (action == Qt::CopyAction) {
			node *c = deep_copy(n, this, [this](const QString &like) {
				return unused_id(like);
			});
			c->parent = target;
			if (row >= 0 && row <= target->children.size())
				target->children.insert(row++, c);
			else
				target->children << c;
		} else {
			if (n->parent)
				n->parent->children.removeOne(n);
			n->parent = target;
			if (row >= 0 && row <= target->children.size())
				target->children.insert(row++, n);
			else
				target->children << n;
		}
	}
	// Sibling order is what the outline file records, so it has to agree with
	// the list we just rearranged or the next save undoes the drag.
	for (int i = 0; i < target->children.size(); ++i)
		target->children[i]->order = i;
	reindex();
	endResetModel();
	emit structure_changed();
	return true;
}

// --- Operations the context menu offers -----------------------------------

node *tab_tree_model::add_folder(node *parent, const QString &title) {
	if (!parent)
		parent = m_root;
	if (!parent->is_folder())
		parent = parent->parent ? parent->parent : m_root;
	beginResetModel();
	node *f = new node;
	f->id      = unused_id("f");
	f->type    = node_type::folder;
	f->title   = title.isEmpty() ? QStringLiteral("New folder") : title;
	f->created = QDateTime::currentDateTime();
	f->last_seen = f->created;
	f->parent  = parent;
	f->order   = parent->children.size();
	parent->children << f;
	reindex();
	endResetModel();
	emit structure_changed();
	return f;
}

node *tab_tree_model::add_tab(node *parent, const QString &title,
                               const QString &url) {
	if (!parent)
		parent = m_root;
	// Dropped beside a tab rather than inside it, the way a folder is: a tab
	// holds no children, so "in here" has no meaning and the nearest sensible
	// reading is "next to this".
	if (!parent->is_folder())
		parent = parent->parent ? parent->parent : m_root;
	beginResetModel();
	node *t = new node;
	t->id        = unused_id("t");
	t->type      = node_type::unopened_tab;
	t->title     = title.isEmpty() ? QStringLiteral("New tab") : title;
	t->url       = url;
	t->created   = QDateTime::currentDateTime();
	t->last_seen = t->created;
	t->parent    = parent;
	t->order     = parent->children.size();
	parent->children << t;
	reindex();
	endResetModel();
	emit structure_changed();
	return t;
}

bool tab_tree_model::remove_node(node *n) {
	// The root is the tree; removing it would leave the model pointing at
	// nothing and the outline writer with no document to write.
	if (!n || n == m_root || !n->parent)
		return false;
	beginResetModel();
	n->parent->children.removeOne(n);
	// Deletes the whole subtree through node's destructor, which is what the
	// gesture means -- deleting a folder in a file manager takes what is in it.
	// The caller is responsible for having asked first.
	delete n;
	reindex();
	endResetModel();
	emit structure_changed();
	return true;
}

void tab_tree_model::update_node(node *n, const QString &title,
                                  const QString &url, const QStringList &tags) {
	if (!n)
		return;
	n->title = title;
	n->url   = url;
	n->tags  = tags;
	refresh_node(n);
	// Saved like a move is: what a tab is called is as much a part of the
	// canonical file as where it sits.
	emit structure_changed();
}

node *tab_tree_model::duplicate_node(node *n) {
	if (!n || !n->parent)
		return nullptr;
	beginResetModel();
	node *c = deep_copy(n, this, [this](const QString &like) {
		return unused_id(like);
	});
	c->parent = n->parent;
	n->parent->children.insert(n->parent->children.indexOf(n) + 1, c);
	for (int i = 0; i < n->parent->children.size(); ++i)
		n->parent->children[i]->order = i;
	reindex();
	endResetModel();
	emit structure_changed();
	return c;
}

// --- Mirrors of other browsers --------------------------------------------

void tab_tree_model::mark_mirror(node *n, const QString &source) {
	if (!n)
		return;
	n->mirror = source;
	for (node *c : n->children)
		mark_mirror(c, source);
}

node *tab_tree_model::replace_mirror(const QString &source, const QString &title,
                                      const QList<node *> &tabs) {
	if (source.isEmpty())
		return nullptr;

	// Row signals rather than a full reset, and that matters once this is on a
	// timer: a reset collapses every folder in the tree, so a background
	// refresh would fold up the user's own work every time the other browser
	// happened to write its file. Only the root's own children change here, so
	// the fine-grained calls are tractable -- one row out, one row in.
	for (int i = m_root->children.size() - 1; i >= 0; --i) {
		node *c = m_root->children.at(i);
		if (c->mirror == source) {
			beginRemoveRows(QModelIndex(), i, i);
			m_root->children.removeAt(i);
			delete c;
			endRemoveRows();
		}
	}

	node *folder = new node;
	folder->id        = unused_id("m");
	folder->type      = node_type::folder;
	folder->title     = title;
	folder->created   = QDateTime::currentDateTime();
	folder->last_seen = folder->created;
	folder->parent    = m_root;
	for (node *t : tabs) {
		t->parent = folder;
		t->order  = folder->children.size();
		folder->children << t;
	}
	mark_mirror(folder, source);

	// Mirrors sit at the top, where a thing that is not yours is easiest to
	// tell apart from the tree you built.
	beginInsertRows(QModelIndex(), 0, 0);
	m_root->children.prepend(folder);
	for (int i = 0; i < m_root->children.size(); ++i)
		m_root->children[i]->order = i;
	reindex();
	endInsertRows();

	// **Not** `structure_changed`: that signal means "save the tree", and a
	// mirror is the one thing that must not be saved. The shell shows it and
	// forgets it, which is what makes it safe to replace on every refresh.
	return folder;
}

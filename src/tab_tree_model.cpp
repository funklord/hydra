// SPDX-License-Identifier: GPL-3.0-or-later
#include "tab_tree_model.h"
#include "node.h"
#include "tree_outline.h"
#include "tree_diff.h"

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

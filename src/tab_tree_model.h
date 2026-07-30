// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tree_diff.h"   // for tree_change in apply_reorganization()

#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QString>

struct node;

// The single source of truth for the tree (architecture doc §5.1). Exposes
// node attributes through custom roles so the sort/filter proxy can order and
// filter without the model caring how.
class tab_tree_model : public QAbstractItemModel {
	Q_OBJECT
public:
	enum roles {
		title_role = Qt::UserRole + 1,
		url_role,
		created_role,
		last_seen_role,
		tree_order_role,
		node_type_role,
	};

	explicit tab_tree_model(QObject *parent = nullptr);
	~tab_tree_model() override;

	// Load/save the canonical outline file. Replaces the current tree.
	bool load(const QString &path);
	bool save(const QString &path) const;

	node *node_for_index(const QModelIndex &index) const;
	node *node_by_id(const QString &id) const;    // O(1) lookup for lifecycle/AI
	node *root() const { return m_root; }

	QModelIndex index_for_node(node *n) const;
	void        refresh_node(node *n);            // emit dataChanged for one node

	// Apply an accepted AI reorganization (architecture doc §9.5). Structural,
	// so it goes through a model reset rather than per-row moves; returns the
	// number of changes applied. Payloads follow ids, so nothing else moves.
	int apply_reorganization(const QList<tree_change> &changes);

	// The §9.4 undo snapshot: take one before applying, restore it to revert.
	tree_snapshot take_snapshot() const;
	int restore_snapshot(const tree_snapshot &snap);

	// QAbstractItemModel
	QModelIndex index(int row, int column, const QModelIndex &parent) const override;
	QModelIndex parent(const QModelIndex &index) const override;
	int         rowCount(const QModelIndex &parent) const override;
	int         columnCount(const QModelIndex &parent) const override;
	QVariant    data(const QModelIndex &index, int role) const override;

private:
	void reindex();  // rebuild m_id_index from the current tree

	QString               m_path;
	node                 *m_root = nullptr;
	QHash<QString, node *> m_id_index;
};

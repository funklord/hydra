// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSortFilterProxyModel>
#include <QString>

// Hierarchical sort + filter over tab_tree_model (architecture doc sec 5.2/sec 5.3).
// Sorts siblings while preserving nesting; folders are grouped ahead of leaves.
// A search string keeps any node whose title/url matches, plus its ancestors.
class tree_sort_proxy : public QSortFilterProxyModel {
	Q_OBJECT
public:
	enum class sort_mode { tree_order, title_asc, newest_created, recently_seen };

	explicit tree_sort_proxy(QObject *parent = nullptr);

	void set_sort_mode(sort_mode mode);

	// Whether the rows are in the tree's own order right now.
	//
	// **Derived, not stored.** The view needs this to decide whether a drop
	// between two rows means anything, and the obvious implementations are both
	// worse: a flag on the model that the shell must remember to update, or a
	// copy of the mode kept here beside the sort role that already encodes it.
	// `setSortRole` is the state; asking it cannot go stale, and there is
	// nothing to keep in step.
	bool in_tree_order() const;
	void set_search_text(const QString &text);

protected:
	bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;
	bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
	bool node_matches(const QModelIndex &source_index) const;

	QString m_search;
};

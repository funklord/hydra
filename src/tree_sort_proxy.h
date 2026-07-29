// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSortFilterProxyModel>
#include <QString>

// Hierarchical sort + filter over tab_tree_model (architecture doc §5.2/§5.3).
// Sorts siblings while preserving nesting; folders are grouped ahead of leaves.
// A search string keeps any node whose title/url matches, plus its ancestors.
class tree_sort_proxy : public QSortFilterProxyModel {
	Q_OBJECT
public:
	enum class sort_mode { tree_order, title_asc, newest_created, recently_seen };

	explicit tree_sort_proxy(QObject *parent = nullptr);

	void set_sort_mode(sort_mode mode);
	void set_search_text(const QString &text);

protected:
	bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;
	bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
	bool node_matches(const QModelIndex &source_index) const;

	QString m_search;
};

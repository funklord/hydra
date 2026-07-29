// SPDX-License-Identifier: GPL-3.0-or-later
#include "tree_sort_proxy.h"
#include "tab_tree_model.h"
#include "node.h"

#include <QDateTime>

tree_sort_proxy::tree_sort_proxy(QObject *parent)
	: QSortFilterProxyModel(parent) {
	setDynamicSortFilter(true);
	set_sort_mode(sort_mode::tree_order);
}

void tree_sort_proxy::set_sort_mode(sort_mode mode) {
	switch (mode) {
		case sort_mode::tree_order:
			setSortRole(tab_tree_model::tree_order_role);
			sort(0, Qt::AscendingOrder);
			break;
		case sort_mode::title_asc:
			setSortRole(tab_tree_model::title_role);
			sort(0, Qt::AscendingOrder);
			break;
		case sort_mode::newest_created:
			setSortRole(tab_tree_model::created_role);
			sort(0, Qt::DescendingOrder);
			break;
		case sort_mode::recently_seen:
			setSortRole(tab_tree_model::last_seen_role);
			sort(0, Qt::DescendingOrder);
			break;
	}
}

void tree_sort_proxy::set_search_text(const QString &text) {
	m_search = text.trimmed();
	invalidateFilter();
}

bool tree_sort_proxy::lessThan(const QModelIndex &left, const QModelIndex &right) const {
	node *l = static_cast<node *>(left.internalPointer());
	node *r = static_cast<node *>(right.internalPointer());
	if (l && r && l->is_folder() != r->is_folder())
		return l->is_folder();  // folders grouped first

	const QVariant lv = sourceModel()->data(left, sortRole());
	const QVariant rv = sourceModel()->data(right, sortRole());

	if (lv.typeId() == QMetaType::QDateTime)
		return lv.toDateTime() < rv.toDateTime();
	if (lv.typeId() == QMetaType::Int)
		return lv.toInt() < rv.toInt();
	return QString::localeAwareCompare(lv.toString(), rv.toString()) < 0;
}

bool tree_sort_proxy::node_matches(const QModelIndex &source_index) const {
	if (m_search.isEmpty())
		return true;
	node *n = static_cast<node *>(source_index.internalPointer());
	if (!n)
		return false;
	if (n->title.contains(m_search, Qt::CaseInsensitive) ||
	    n->url.contains(m_search, Qt::CaseInsensitive))
		return true;
	// Keep a node if any descendant matches (so parents of hits stay visible).
	const int rows = sourceModel()->rowCount(source_index);
	for (int i = 0; i < rows; ++i)
		if (node_matches(sourceModel()->index(i, 0, source_index)))
			return true;
	return false;
}

bool tree_sort_proxy::filterAcceptsRow(int row, const QModelIndex &parent) const {
	if (m_search.isEmpty())
		return true;
	return node_matches(sourceModel()->index(row, 0, parent));
}

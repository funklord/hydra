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

bool tree_sort_proxy::in_tree_order() const {
	return sortRole() == tab_tree_model::tree_order_role;
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
	// `invalidateFilter()` is deprecated from 6.9 in favour of a begin/end pair,
	// and the pair does not exist before it, so both spellings are here.
	//
	// **Measured before changed**: the deprecated call still filters correctly on
	// 6.11 — `test_model`'s twenty-four checks pass there — so this is
	// forward-compatibility rather than a repair. That distinction is worth
	// making because the *other* deprecation this project met, the WebEngine
	// permission API, was documented as functional and was not: geolocation
	// silently stopped arriving. A deprecation warning says nothing about
	// whether the thing still works; only running it does.
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
	beginFilterChange();
	endFilterChange();
#else
	invalidateFilter();
#endif
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

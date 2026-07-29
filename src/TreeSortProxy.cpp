#include "TreeSortProxy.h"
#include "TabTreeModel.h"
#include "Node.h"

#include <QDateTime>

TreeSortProxy::TreeSortProxy(QObject* parent)
    : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
    setSortMode(SortMode::TreeOrder);
}

void TreeSortProxy::setSortMode(SortMode mode) {
    switch (mode) {
        case SortMode::TreeOrder:
            setSortRole(TabTreeModel::TreeOrderRole);
            sort(0, Qt::AscendingOrder);
            break;
        case SortMode::TitleAsc:
            setSortRole(TabTreeModel::TitleRole);
            sort(0, Qt::AscendingOrder);
            break;
        case SortMode::NewestCreated:
            setSortRole(TabTreeModel::CreatedRole);
            sort(0, Qt::DescendingOrder);
            break;
        case SortMode::RecentlySeen:
            setSortRole(TabTreeModel::LastSeenRole);
            sort(0, Qt::DescendingOrder);
            break;
    }
}

void TreeSortProxy::setSearchText(const QString& text) {
    search_ = text.trimmed();
    invalidateFilter();
}

bool TreeSortProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const {
    Node* l = static_cast<Node*>(left.internalPointer());
    Node* r = static_cast<Node*>(right.internalPointer());
    if (l && r && l->isFolder() != r->isFolder())
        return l->isFolder();  // folders grouped first

    const QVariant lv = sourceModel()->data(left, sortRole());
    const QVariant rv = sourceModel()->data(right, sortRole());

    if (lv.typeId() == QMetaType::QDateTime)
        return lv.toDateTime() < rv.toDateTime();
    if (lv.typeId() == QMetaType::Int)
        return lv.toInt() < rv.toInt();
    return QString::localeAwareCompare(lv.toString(), rv.toString()) < 0;
}

bool TreeSortProxy::nodeMatches(const QModelIndex& sourceIndex) const {
    if (search_.isEmpty())
        return true;
    Node* n = static_cast<Node*>(sourceIndex.internalPointer());
    if (!n)
        return false;
    if (n->title.contains(search_, Qt::CaseInsensitive) ||
        n->url.contains(search_, Qt::CaseInsensitive))
        return true;
    // Keep a node if any descendant matches (so parents of hits stay visible).
    const int rows = sourceModel()->rowCount(sourceIndex);
    for (int i = 0; i < rows; ++i)
        if (nodeMatches(sourceModel()->index(i, 0, sourceIndex)))
            return true;
    return false;
}

bool TreeSortProxy::filterAcceptsRow(int row, const QModelIndex& parent) const {
    if (search_.isEmpty())
        return true;
    return nodeMatches(sourceModel()->index(row, 0, parent));
}

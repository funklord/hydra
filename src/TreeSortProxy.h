#pragma once

#include <QSortFilterProxyModel>
#include <QString>

// Hierarchical sort + filter over TabTreeModel (architecture doc §5.2/§5.3).
// Sorts siblings while preserving nesting; folders are grouped ahead of leaves.
// A search string keeps any node whose title/url matches, plus its ancestors.
class TreeSortProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    enum class SortMode { TreeOrder, TitleAsc, NewestCreated, RecentlySeen };

    explicit TreeSortProxy(QObject* parent = nullptr);

    void setSortMode(SortMode mode);
    void setSearchText(const QString& text);

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

private:
    bool nodeMatches(const QModelIndex& sourceIndex) const;

    QString search_;
};

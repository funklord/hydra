#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QString>

struct Node;

// The single source of truth for the tree (architecture doc §5.1). Exposes
// node attributes through custom roles so the sort/filter proxy can order and
// filter without the model caring how.
class TabTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        UrlRole,
        CreatedRole,
        LastSeenRole,
        TreeOrderRole,
        NodeTypeRole,
    };

    explicit TabTreeModel(QObject* parent = nullptr);
    ~TabTreeModel() override;

    // Load/save the canonical outline file. Replaces the current tree.
    bool load(const QString& path);
    bool save(const QString& path) const;

    Node* nodeForIndex(const QModelIndex& index) const;
    Node* nodeById(const QString& id) const;      // O(1) lookup for lifecycle/AI
    Node* root() const { return root_; }

    QModelIndex indexForNode(Node* node) const;
    void        refreshNode(Node* node);          // emit dataChanged for one node

    // QAbstractItemModel
    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int         rowCount(const QModelIndex& parent) const override;
    int         columnCount(const QModelIndex& parent) const override;
    QVariant    data(const QModelIndex& index, int role) const override;

private:
    void reindex();  // rebuild idIndex_ from the current tree

    QString                  path_;
    Node*                    root_ = nullptr;
    QHash<QString, Node*>    idIndex_;
};

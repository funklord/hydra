#include "TabTreeModel.h"
#include "Node.h"
#include "TreeOutline.h"

#include <QApplication>
#include <QStyle>
#include <QFont>
#include <QBrush>
#include <QColor>

TabTreeModel::TabTreeModel(QObject* parent)
    : QAbstractItemModel(parent) {
    root_ = new Node;
    root_->id   = "root";
    root_->type = NodeType::Folder;
}

TabTreeModel::~TabTreeModel() {
    delete root_;  // deletes the whole tree recursively (Node dtor)
}

static void indexSubtree(Node* n, QHash<QString, Node*>& map) {
    for (Node* c : n->children) {
        map.insert(c->id, c);
        indexSubtree(c, map);
    }
}

void TabTreeModel::reindex() {
    idIndex_.clear();
    indexSubtree(root_, idIndex_);
}

bool TabTreeModel::load(const QString& path) {
    Node* fresh = TreeOutline::load(path);
    if (!fresh)
        return false;
    beginResetModel();
    delete root_;
    root_ = fresh;
    path_ = path;
    reindex();
    endResetModel();
    return true;
}

bool TabTreeModel::save(const QString& path) const {
    return TreeOutline::save(path.isEmpty() ? path_ : path, root_);
}

Node* TabTreeModel::nodeForIndex(const QModelIndex& index) const {
    if (!index.isValid())
        return root_;
    return static_cast<Node*>(index.internalPointer());
}

Node* TabTreeModel::nodeById(const QString& id) const {
    return idIndex_.value(id, nullptr);
}

QModelIndex TabTreeModel::indexForNode(Node* node) const {
    if (!node || node == root_)
        return {};
    return createIndex(node->row(), 0, node);
}

void TabTreeModel::refreshNode(Node* node) {
    const QModelIndex idx = indexForNode(node);
    if (idx.isValid())
        emit dataChanged(idx, idx);
}

QModelIndex TabTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (!hasIndex(row, column, parent))
        return {};
    Node* parentNode = nodeForIndex(parent);
    if (row < 0 || row >= parentNode->children.size())
        return {};
    return createIndex(row, column, parentNode->children.at(row));
}

QModelIndex TabTreeModel::parent(const QModelIndex& index) const {
    if (!index.isValid())
        return {};
    Node* node = static_cast<Node*>(index.internalPointer());
    Node* parentNode = node ? node->parent : nullptr;
    if (!parentNode || parentNode == root_)
        return {};
    return createIndex(parentNode->row(), 0, parentNode);
}

int TabTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0)
        return 0;
    return nodeForIndex(parent)->children.size();
}

int TabTreeModel::columnCount(const QModelIndex&) const {
    return 1;
}

QVariant TabTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    Node* n = static_cast<Node*>(index.internalPointer());
    if (!n)
        return {};

    switch (role) {
        case Qt::DisplayRole:
            return n->title.isEmpty() ? n->url : n->title;
        case Qt::ToolTipRole:
            return n->url;
        case Qt::DecorationRole: {
            QStyle* s = QApplication::style();
            return s->standardIcon(n->isFolder() ? QStyle::SP_DirIcon
                                                  : QStyle::SP_FileIcon);
        }
        case Qt::FontRole: {
            // Open (live) tabs are bold; suspended are italic.
            QFont f;
            if (n->type == NodeType::OpenTab)      f.setBold(true);
            if (n->type == NodeType::SuspendedTab) f.setItalic(true);
            return f;
        }
        case Qt::ForegroundRole:
            // Unopened links are muted; everything else default.
            if (n->type == NodeType::UnopenedTab)
                return QBrush(QColor(140, 140, 140));
            return {};
        case TitleRole:     return n->title;
        case UrlRole:       return n->url;
        case CreatedRole:   return n->created;
        case LastSeenRole:  return n->lastSeen;
        case TreeOrderRole: return n->order;
        case NodeTypeRole:  return static_cast<int>(n->type);
        default:            return {};
    }
}

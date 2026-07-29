#pragma once

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QList>

// A single entry in the tab/link tree. Folder or leaf.
// Runtime weight (a live QWebEngineView, or a suspended state blob) is attached
// separately by `id` in the shell — never stored on the Node itself — so moving
// a node never disturbs its payload. See architecture doc §4.2.
enum class NodeType { Folder, OpenTab, UnopenedTab, SuspendedTab };

struct Node {
    QString   id;                 // short, opaque, stable for the node's lifetime
    NodeType  type = NodeType::UnopenedTab;
    QString   title;
    QString   url;                // empty for pure folders
    QDateTime created;
    QDateTime lastSeen;
    QStringList tags;

    int       order = 0;          // canonical sibling order as loaded from disk

    Node*         parent = nullptr;
    QList<Node*>  children;

    ~Node() { qDeleteAll(children); }

    bool isFolder() const { return type == NodeType::Folder; }

    int row() const {
        return parent ? parent->children.indexOf(const_cast<Node*>(this)) : 0;
    }
};

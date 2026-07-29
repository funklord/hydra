#include "TreeOutline.h"
#include "Node.h"

#include <QFile>
#include <QTextStream>
#include <QVector>

namespace {

QString typeToString(NodeType t) {
    switch (t) {
        case NodeType::Folder:       return "folder";
        case NodeType::OpenTab:      return "open";
        case NodeType::UnopenedTab:  return "unopened";
        case NodeType::SuspendedTab: return "suspended";
    }
    return "unopened";
}

NodeType typeFromString(const QString& s) {
    if (s == "folder")    return NodeType::Folder;
    if (s == "open")      return NodeType::OpenTab;
    if (s == "suspended") return NodeType::SuspendedTab;
    return NodeType::UnopenedTab;
}

int leadingSpaces(const QString& line) {
    int n = 0;
    while (n < line.size() && line.at(n) == ' ') ++n;
    return n;
}

}  // namespace

namespace TreeOutline {

Node* load(const QString& path) {
    Node* root = new Node;
    root->id   = "root";
    root->type = NodeType::Folder;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return root;

    QTextStream in(&f);
    // Stack of (depth, node) used to resolve each line's parent.
    struct Frame { int depth; Node* node; };
    QVector<Frame> stack;
    stack.push_back({-1, root});

    int counter = 0;
    while (!in.atEnd()) {
        const QString raw = in.readLine();
        if (raw.trimmed().isEmpty())
            continue;

        const int depth = leadingSpaces(raw) / 2;
        QString line = raw.trimmed();
        if (!line.startsWith("- ["))
            continue;

        const int close = line.indexOf(']');
        if (close < 0)
            continue;

        const QString id = line.mid(3, close - 3);
        QString rest = line.mid(close + 1).trimmed();
        const QStringList fields = rest.split(" | ");
        if (fields.isEmpty())
            continue;

        Node* n   = new Node;
        n->id     = id.isEmpty() ? QString("n%1").arg(counter) : id;
        n->type   = typeFromString(fields.value(0).trimmed());
        n->title  = fields.value(1).trimmed();
        if (!n->isFolder())
            n->url = fields.value(2).trimmed();

        for (const QString& field : fields) {
            const QString t = field.trimmed();
            if (t.startsWith("created="))
                n->created = QDateTime::fromString(t.mid(8), Qt::ISODate);
            else if (t.startsWith("seen="))
                n->lastSeen = QDateTime::fromString(t.mid(5), Qt::ISODate);
        }
        if (!n->created.isValid())  n->created  = QDateTime::currentDateTime();
        if (!n->lastSeen.isValid()) n->lastSeen = n->created;

        // Pop until the stack top is a shallower node; that becomes the parent.
        while (stack.size() > 1 && stack.last().depth >= depth)
            stack.pop_back();

        Node* parent = stack.last().node;
        n->parent = parent;
        n->order  = parent->children.size();
        parent->children.push_back(n);

        stack.push_back({depth, n});
        ++counter;
    }
    return root;
}

static void writeNode(QTextStream& out, Node* n, int depth) {
    const QString indent(depth * 2, ' ');
    if (n->isFolder()) {
        out << indent << "- [" << n->id << "] folder | " << n->title << "\n";
    } else {
        out << indent << "- [" << n->id << "] " << typeToString(n->type)
            << " | " << n->title
            << " | " << n->url
            << " | created=" << n->created.toString(Qt::ISODate)
            << " | seen="    << n->lastSeen.toString(Qt::ISODate) << "\n";
    }
    for (Node* c : n->children)
        writeNode(out, c, depth + 1);
}

bool save(const QString& path, Node* root) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;
    QTextStream out(&f);
    for (Node* c : root->children)
        writeNode(out, c, 0);
    return true;
}

}  // namespace TreeOutline

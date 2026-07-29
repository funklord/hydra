#pragma once

#include <QString>
#include <QByteArray>

// Per-node suspended-state blobs, keyed by node id (architecture doc §4.2/§5.4).
// A blob is the serialized navigation history of a suspended tab; it lives in a
// sidecar directory next to the tree file, never inside the canonical outline.
class StateStore {
public:
    explicit StateStore(const QString& dir);

    bool       hasState(const QString& id) const;
    QByteArray load(const QString& id) const;
    bool       save(const QString& id, const QByteArray& blob);
    bool       remove(const QString& id);

private:
    QString pathFor(const QString& id) const;
    QString dir_;
};

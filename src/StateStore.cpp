#include "StateStore.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

StateStore::StateStore(const QString& dir) : dir_(dir) {
    QDir().mkpath(dir_);
}

QString StateStore::pathFor(const QString& id) const {
    // Ids are short opaque tokens; keep the filename safe regardless.
    QString safe = id;
    safe.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    return dir_ + "/" + safe + ".blob";
}

bool StateStore::hasState(const QString& id) const {
    return QFile::exists(pathFor(id));
}

QByteArray StateStore::load(const QString& id) const {
    QFile f(pathFor(id));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

bool StateStore::save(const QString& id, const QByteArray& blob) {
    QFile f(pathFor(id));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(blob) == blob.size();
}

bool StateStore::remove(const QString& id) {
    return QFile::remove(pathFor(id));
}

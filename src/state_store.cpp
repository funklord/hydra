// SPDX-License-Identifier: GPL-3.0-or-later
#include "state_store.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

state_store::state_store(const QString &dir) : m_dir(dir) {
	QDir().mkpath(m_dir);
}

QString state_store::path_for(const QString &id) const {
	// Ids are short opaque tokens; keep the filename safe regardless.
	QString safe = id;
	safe.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
	return m_dir + "/" + safe + ".blob";
}

bool state_store::has_state(const QString &id) const {
	return QFile::exists(path_for(id));
}

QByteArray state_store::load(const QString &id) const {
	QFile f(path_for(id));
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return f.readAll();
}

bool state_store::save(const QString &id, const QByteArray &blob) {
	QFile f(path_for(id));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	return f.write(blob) == blob.size();
}

bool state_store::remove(const QString &id) {
	return QFile::remove(path_for(id));
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "state_store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRegularExpression>

namespace {

// Enough of a hash to tell two ids apart, not enough to be a checksum: this
// distinguishes names, it does not verify contents.
QString digest(const QString &id) {
	return QString::fromLatin1(
		QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1)
		  .toHex()
		  .left(8));
}

}  // namespace

state_store::state_store(const QString &dir) : m_dir(dir) {
	QDir().mkpath(m_dir);
}

QString state_store::path_for(const QString &id) const {
	// Ids are short opaque tokens, but they come from the tree file, which is
	// documented as human-editable -- so "regardless" has to mean it.
	static const QRegularExpression unsafe("[^A-Za-z0-9._-]");
	QString safe = id;
	safe.replace(unsafe, "_");

	// Replacing every unsafe character with '_' is **not injective**: "a b" and
	// "a_b" both become "a_b", so two suspended tabs shared one file and the
	// second to be restored came back wearing the first one's history. A tab
	// claiming a past that is not its own is worse than one that has forgotten.
	//
	// The suffix is added only when sanitising changed something, so every id
	// the application itself generates keeps the filename it already had and no
	// blob on disk is orphaned by this fix. Truncation gets one too, since a
	// long id cut to fit collides with every other id sharing its first hundred
	// characters.
	const bool changed = (safe != id);
	if (safe.size() > 100) {
		safe.truncate(100);
		return m_dir + "/" + safe + "-" + digest(id) + ".blob";
	}
	if (changed)
		return m_dir + "/" + safe + "-" + digest(id) + ".blob";
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

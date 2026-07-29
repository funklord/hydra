// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QByteArray>

// Per-node suspended-state blobs, keyed by node id (architecture doc §4.2/§5.4).
// A blob is the serialized navigation history of a suspended tab; it lives in a
// sidecar directory next to the tree file, never inside the canonical outline.
class state_store {
public:
	explicit state_store(const QString &dir);

	bool       has_state(const QString &id) const;
	QByteArray load(const QString &id) const;
	bool       save(const QString &id, const QByteArray &blob);
	bool       remove(const QString &id);

private:
	QString path_for(const QString &id) const;
	QString m_dir;
};

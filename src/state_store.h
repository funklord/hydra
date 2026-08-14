// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QByteArray>

// Per-node suspended-state blobs, keyed by node id (architecture doc sec 4.2/sec 5.4).
// A blob is the serialized navigation history of a suspended tab; it lives in a
// sidecar directory next to the tree file, never inside the canonical outline.
class state_store {
public:
	explicit state_store(const QString &dir);

	bool       has_state(const QString &id) const;
	QByteArray load(const QString &id) const;
	bool       save(const QString &id, const QByteArray &blob);
	// Forget a node's saved state. Called when the node itself is deleted:
	// otherwise `state/<id>.blob` outlives the tab for ever, and an id that is
	// later reused -- `unused_id` only avoids collisions with what is *in the
	// tree* -- would inherit somebody else's scroll position and form contents.
	bool       remove(const QString &id);

private:
	QString path_for(const QString &id) const;
	QString m_dir;
};

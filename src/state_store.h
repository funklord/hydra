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
	// The imported back/forward record, beside the blob and never inside it.
	// Two files rather than one because they have different lifetimes and
	// different owners: the blob is the engine's, is unreadable to anything
	// else, and is rightly discarded when the engine version moves on -- while
	// this is ours, is a permanent record, and must survive exactly that.
	bool       has_history(const QString &id) const;
	QByteArray load_history(const QString &id) const;
	bool       save_history(const QString &id, const QByteArray &bytes);

	// Forget a node's saved state. Called when the node itself is deleted:
	// otherwise `state/<id>.blob` outlives the tab for ever, and an id that is
	// later reused -- `unused_id` only avoids collisions with what is *in the
	// tree* -- would inherit somebody else's scroll position and form contents.
	//
	// Removes **both** files. A history left behind by a delete is the same
	// bug this comment already describes, one file over: the next node to be
	// given that id would open wearing a stranger's past.
	bool       remove(const QString &id);

private:
	// `ext` includes its dot. One function rather than two so that the
	// collision fix below is shared: a second path builder is a second place
	// for it to be got wrong, and the bug it fixes is silent.
	QString path_for(const QString &id, const char *ext = ".blob") const;
	QString m_dir;
};

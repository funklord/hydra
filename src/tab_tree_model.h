// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tree_diff.h"   // for tree_change in apply_reorganization()

#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QString>

struct node;

// The single source of truth for the tree (architecture doc §5.1). Exposes
// node attributes through custom roles so the sort/filter proxy can order and
// filter without the model caring how.
// **Needs a QApplication, not merely a QCoreApplication.** It answers
// `DecorationRole` with a style icon and `FontRole` with a font, so it asks
// `QApplication::style()` — which is null when there is no GUI application, and
// dereferencing it crashes inside `data()`. That is the right failure for a
// misuse rather than something to guard: a model handing back blank icons is
// harder to trace than a crash where the mistake was made. Written down here
// because the crash's stack frame says `data()` and not "you used the wrong
// application class", and a test suite found that out the slow way.
class tab_tree_model : public QAbstractItemModel {
	Q_OBJECT
public:
	enum roles {
		title_role = Qt::UserRole + 1,
		url_role,
		created_role,
		last_seen_role,
		tree_order_role,
		node_type_role,
	};

	explicit tab_tree_model(QObject *parent = nullptr);
	~tab_tree_model() override;

	// Load/save the canonical outline file. Replaces the current tree.
	bool load(const QString &path);
	bool save(const QString &path) const;

	node *node_for_index(const QModelIndex &index) const;
	node *node_by_id(const QString &id) const;    // O(1) lookup for lifecycle/AI
	node *root() const { return m_root; }

	QModelIndex index_for_node(node *n) const;
	void        refresh_node(node *n);            // emit dataChanged for one node

	// Apply an accepted AI reorganization (architecture doc §9.5). Structural,
	// so it goes through a model reset rather than per-row moves; returns the
	// number of changes applied. Payloads follow ids, so nothing else moves.
	int apply_reorganization(const QList<tree_change> &changes);

	// The §9.4 undo snapshot: take one before applying, restore it to revert.
	tree_snapshot take_snapshot() const;
	int restore_snapshot(const tree_snapshot &snap);

	// QAbstractItemModel
	// --- Moving nodes about, the way a file manager does ------------------
	//
	// The tree advertises itself as a side-tree of tabs and could not, until
	// now, have a tab dragged into a folder: this model implemented the
	// read-only half of QAbstractItemModel and nothing else, so the view
	// refused every drag. Reorganising happened only through
	// `apply_reorganization` (the AI diff) and `restore_snapshot` (undo).
	//
	// **Reparenting is always offered; reordering only in tree order.** A drop
	// *between* two rows means "put it here", and that has no stable meaning
	// when the view is sorted by title or by date -- the row would jump
	// elsewhere the moment it re-sorted, which reads as the app ignoring you.
	// A drop *onto* a folder is unambiguous in every mode. Firefox and Chrome's
	// own bookmark managers make the same split.
	//
	// The happy consequence is that the proxy's index mapping stops being a
	// problem: reordering is only live in the one mode where the proxy's order
	// and the model's are the same.
	void set_reorder_allowed(bool on) { m_reorder_allowed = on; }

	// --- Operations the context menu offers -------------------------------
	// Each mutates the tree and emits `structure_changed`, so the shell saves
	// once, in one place, however the change was made.
	node *add_folder(node *parent, const QString &title);
	bool  remove_node(node *n);
	// Edit what a node *is*, as opposed to where it sits. The id is deliberately
	// not editable: it keys the state blob and the outline file, and letting a
	// person retype it would orphan a tab's history with no warning.
	void  update_node(node *n, const QString &title, const QString &url,
	                   const QStringList &tags);
	// A duplicate under the same parent, with an id of its own.
	node *duplicate_node(node *n);

	Qt::ItemFlags   flags(const QModelIndex &index) const override;
	Qt::DropActions supportedDropActions() const override;
	QStringList     mimeTypes() const override;
	QMimeData      *mimeData(const QModelIndexList &indexes) const override;
	bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row,
	                   int column, const QModelIndex &parent) override;

	QModelIndex index(int row, int column, const QModelIndex &parent) const override;
	QModelIndex parent(const QModelIndex &index) const override;
	int         rowCount(const QModelIndex &parent) const override;
	int         columnCount(const QModelIndex &parent) const override;
	QVariant    data(const QModelIndex &index, int role) const override;

signals:
	// The tree changed shape because a person moved something. The shell saves
	// on this: the tree file is the canonical record, and a drag that survived
	// only until the next launch would be worse than one that was refused.
	void structure_changed();

private:
	void reindex();  // rebuild m_id_index from the current tree
	// An id nothing in the tree is using. Copies need one: two nodes sharing an
	// id would share a `state/<id>.blob`, so one tab's scroll position and form
	// contents would be restored into the other.
	QString unused_id(const QString &like) const;
	bool    is_ancestor_of(const node *maybe_ancestor, const node *n) const;

	QString               m_path;
	node                 *m_root = nullptr;
	QHash<QString, node *> m_id_index;
	bool                   m_reorder_allowed = true;
};

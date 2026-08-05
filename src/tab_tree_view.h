// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStringList>
#include <QTreeView>

class QLabel;

class tab_tree_model;
class tree_sort_proxy;
struct node;

// The tree, with the drag-and-drop policy that only it can enforce.
//
// **Why a subclass rather than configuration on a plain QTreeView.** Dropping
// *between* two rows is a position, and a position only means anything in tree
// order -- sorted by title the row jumps back on the next re-sort, which reads
// as the app ignoring you. Something has to refuse that gesture, and the view
// is the only object that can: the model cannot see which sort is active, and
// the proxy cannot see where the pointer is.
//
// The first arrangement had the model hold a `reorder_allowed` flag that the
// shell set from the sort combo -- two calls that had to be kept in step by
// hand, so any other route to changing the sort (a settings restore, a
// shortcut, a test) would leave the model believing a stale answer. That is the
// same shape as every "wired but never exercised" defect in this project's
// history. The flag is gone: the answer is derived from the proxy at the moment
// of the gesture, so there is nothing to keep in sync.
class tab_tree_view : public QTreeView {
	Q_OBJECT
public:
	explicit tab_tree_view(QWidget *parent = nullptr);

	// Rename, in the sense a file manager means it: the properties of one node.
	// Public because it has more than one way in -- the context menu, and F2,
	// which is what a hand reaches for. A shell wanting it on a toolbar would
	// use the same entry.
	void edit_properties(node *n);

	// **Keeps folders open across a model reset.** Several operations rebuild
	// the model wholesale -- a drop, a load, a mirror swap -- and a reset tells
	// the view that everything it knew is void, so it collapses the lot. After
	// moving one tab between folders the whole tree folded up, which made drag
	// and drop unusable for anything past the first gesture.
	//
	// Done here rather than by teaching six call sites to emit fine-grained row
	// signals: which folders are open is the *view's* state, not the model's,
	// and a fix in the view covers resets that have not been written yet.
	// Nodes are remembered by id, so it works whether a reset moved the
	// existing nodes or replaced them all.
	void setModel(QAbstractItemModel *m) override;

protected:
	bool eventFilter(QObject *o, QEvent *e) override;

public:

signals:
	// The two things the menu offers that this view cannot do itself: opening a
	// tab needs an engine and a stacked widget, suspending it needs the state
	// store. Everything else on the menu -- duplicate, new folder, delete,
	// properties -- is the model's own business and is done here.
	void open_requested(node *n);
	// Hand this address to another application. The view cannot do it: on
	// Android it is an intent through the activity, on desktop the system's
	// default handler, and neither is a tree's business.
	void open_externally_requested(node *n);
	void suspend_requested(node *n);

protected:
	// Where the file-manager gestures are actually decided.
	void dragMoveEvent(QDragMoveEvent *event) override;

private:
	// Says why the tree is empty, which is not always the same reason.
	void update_empty_state();

	void remember_open_folders();
	void reopen_folders();
	QModelIndex view_index(node *n) const;
	node *node_at_index(const QModelIndex &idx) const;

	QLabel     *m_empty = nullptr;
	QStringList m_open_ids;
	QString     m_current_id;

	// Whether a drop *between* rows would mean anything right now.
	bool reordering_is_meaningful() const;

	tab_tree_model *source_model() const;
	node *node_at(const QPoint &pos) const;
	void show_menu(const QPoint &pos);

protected:
	void keyPressEvent(QKeyEvent *event) override;
};

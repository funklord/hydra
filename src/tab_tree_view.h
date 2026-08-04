// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QTreeView>

class tree_sort_proxy;

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

protected:
	// Where the file-manager gestures are actually decided.
	void dragMoveEvent(QDragMoveEvent *event) override;

private:
	// Whether a drop *between* rows would mean anything right now.
	bool reordering_is_meaningful() const;
};

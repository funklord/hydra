// SPDX-License-Identifier: GPL-3.0-or-later
#include "tab_tree_view.h"
#include "tree_sort_proxy.h"

#include <QDragMoveEvent>

tab_tree_view::tab_tree_view(QWidget *parent) : QTreeView(parent) {
	// The gesture set, kept here rather than at the call site so a second tree
	// -- or a test -- cannot get a differently configured one.
	setDragEnabled(true);
	setAcceptDrops(true);
	setDropIndicatorShown(true);
	// DragDrop rather than InternalMove: the model also publishes urls, so a
	// tab can be dragged out to another application, and InternalMove would
	// refuse to hand anything over.
	setDragDropMode(QAbstractItemView::DragDrop);
	setDefaultDropAction(Qt::MoveAction);
	setSelectionMode(QAbstractItemView::ExtendedSelection);
}

bool tab_tree_view::reordering_is_meaningful() const {
	// Asked of the proxy, now, rather than remembered from when the sort last
	// changed. A view with no sort proxy under it is in tree order by
	// definition, so reordering means what it says.
	if (auto *proxy = qobject_cast<tree_sort_proxy *>(model()))
		return proxy->in_tree_order();
	return true;
}

void tab_tree_view::dragMoveEvent(QDragMoveEvent *event) {
	QTreeView::dragMoveEvent(event);
	if (reordering_is_meaningful())
		return;
	// Sorted by something other than the tree's own order: a drop *onto* a
	// folder still means "put it in here" and is left alone, while a drop
	// between two rows is refused -- no indicator, no drop. Refusing the
	// gesture is kinder than accepting it and putting the row somewhere the
	// next re-sort will move again.
	const DropIndicatorPosition where = dropIndicatorPosition();
	if (where == QAbstractItemView::AboveItem || where == QAbstractItemView::BelowItem)
		event->ignore();
}

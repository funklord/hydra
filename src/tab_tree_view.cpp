// SPDX-License-Identifier: GPL-3.0-or-later
#include "tab_tree_view.h"
#include "tree_sort_proxy.h"

#include "node.h"
#include "tab_tree_model.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragMoveEvent>
#include <QKeyEvent>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>

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

	// **Hovering a closed folder opens it.** Without this a collapsed folder
	// cannot be dropped into at all: the drag has nowhere to land, so somebody
	// has to abandon it, expand the folder by hand, and start again. In a tree
	// whose whole point is folders that is the difference between drag-and-drop
	// working and merely existing.
	//
	// 600 ms is the interval this gesture has had in file managers for twenty
	// years -- long enough that passing over a folder on the way somewhere else
	// does not disturb it, short enough not to feel stuck.
	setAutoExpandDelay(600);

	// Dragging towards an edge scrolls rather than stopping at it, which is
	// what makes a tree taller than the window reachable at all. Qt defaults
	// this on; set it explicitly so it survives somebody tuning the view.
	setAutoScroll(true);
	setAutoScrollMargin(24);

	setContextMenuPolicy(Qt::CustomContextMenu);
	connect(this, &QWidget::customContextMenuRequested,
	        this, &tab_tree_view::show_menu);
}

tab_tree_model *tab_tree_view::source_model() const {
	if (auto *proxy = qobject_cast<tree_sort_proxy *>(model()))
		return qobject_cast<tab_tree_model *>(proxy->sourceModel());
	return qobject_cast<tab_tree_model *>(model());
}

node *tab_tree_view::node_at(const QPoint &pos) const {
	tab_tree_model *m = source_model();
	if (!m)
		return nullptr;
	const QModelIndex at = indexAt(pos);
	if (!at.isValid())
		return nullptr;
	// Through the proxy if there is one: the row under the pointer is a proxy
	// row, and mapping it here is the only place that has to know that.
	if (auto *proxy = qobject_cast<tree_sort_proxy *>(model()))
		return m->node_for_index(proxy->mapToSource(at));
	return m->node_for_index(at);
}

void tab_tree_view::show_menu(const QPoint &pos) {
	tab_tree_model *m = source_model();
	if (!m)
		return;
	// A right-click on empty space is still a place to make a folder. The first
	// version of this returned here, and returned for folders too, so the only
	// rows with a menu at all were tabs -- and the containers everything lives
	// in could not be renamed, emptied or added to.
	node *n = node_at(pos);

	QMenu menu(this);
	QAction *open_a = nullptr, *sus_a = nullptr;
	if (n && !n->is_folder()) {
		open_a = menu.addAction("&Open");
		sus_a  = menu.addAction("&Suspend");
		// **Asked of the node, not of the shell.** This used to test the
		// shell's map of live views, which is a second record of a fact the
		// node already carries -- `open_node` sets the type and `suspend_node`
		// clears it. Two records of one fact is one of them being wrong later.
		sus_a->setEnabled(n->type == node_type::open_tab);
		menu.addSeparator();
	}

	// **Ordered the way a file manager's context menu has been since Explorer:**
	// open at the top, then the things you do *with* the item, then the things
	// you make, then the destructive one, and Properties last on its own.
	//
	// Delete used to sit *below* Properties, which puts the irreversible action
	// where three decades of muscle memory expects the harmless one.
	QAction *copy_url_a = nullptr, *dup_a = nullptr, *external_a = nullptr;
	if (n && !n->url.isEmpty()) {
		copy_url_a = menu.addAction("&Copy Address");
		external_a = menu.addAction("Open in &Another App…");
		menu.addSeparator();
	}

	QAction *tab_a    = menu.addAction("New &Tab Here");
	QAction *folder_a = menu.addAction("New &Folder Here");
	if (n)
		dup_a = menu.addAction("Dup&licate");

	QAction *del_a = nullptr, *props_a = nullptr;
	if (n) {
		menu.addSeparator();
		del_a = menu.addAction("&Delete");
		menu.addSeparator();
		props_a = menu.addAction("P&roperties…");
	}

	QAction *chosen = menu.exec(viewport()->mapToGlobal(pos));
	if (!chosen)
		return;
	if (chosen == open_a)           emit open_requested(n);
	else if (chosen == sus_a)       emit suspend_requested(n);
	else if (chosen == copy_url_a)  QGuiApplication::clipboard()->setText(n->url);
	else if (chosen == external_a)  emit open_externally_requested(n);
	else if (chosen == dup_a)       m->duplicate_node(n);
	else if (chosen == folder_a) {
		// Into the folder that was clicked, or beside a tab -- which is what a
		// file manager does, and saves a drag immediately afterwards.
		node *parent = !n ? nullptr : (n->is_folder() ? n : n->parent);
		if (node *f = m->add_folder(parent, QString())) {
			expandAll();
			edit_properties(f);
		}
	} else if (chosen == tab_a) {
		node *parent = !n ? nullptr : (n->is_folder() ? n : n->parent);
		if (node *t = m->add_tab(parent, QString(), QString())) {
			expandAll();
			// Opened straight away rather than left as a row to find: a new tab
			// you then have to go and click is not what the gesture meant.
			emit open_requested(t);
		}
	} else if (chosen == props_a)   edit_properties(n);
	else if (chosen == del_a) {
		// Deleting a folder takes what is in it, so the count goes in the
		// question rather than being discovered afterwards.
		const int kids = n->children.size();
		const QString what = kids > 0
		  ? QString("Delete \"%1\" and the %2 item%3 inside it?")
		        .arg(n->title).arg(kids).arg(kids == 1 ? "" : "s")
		  : QString("Delete \"%1\"?").arg(n->title);
		if (QMessageBox::question(this, "Delete", what) == QMessageBox::Yes)
			m->remove_node(n);
	}
}

void tab_tree_view::keyPressEvent(QKeyEvent *event) {
	// F2 is what a hand reaches for to rename something in a tree, and it cost
	// nothing to answer. Delete is deliberately *not* bound here: a stray key
	// should not remove a folder and everything in it, and the menu entry asks
	// first.
	if (event->key() == Qt::Key_F2) {
		if (tab_tree_model *m = source_model()) {
			QModelIndex at = currentIndex();
			if (auto *proxy = qobject_cast<tree_sort_proxy *>(model()))
				at = proxy->mapToSource(at);
			if (node *n = m->node_for_index(at)) {
				edit_properties(n);
				return;
			}
		}
	}
	QTreeView::keyPressEvent(event);
}

// What a node *is*, as opposed to where it sits. The id is shown and not
// editable: it keys `state/<id>.blob` and the outline file, so retyping it
// would orphan a tab's history with no warning.
void tab_tree_view::edit_properties(node *n) {
	tab_tree_model *m = source_model();
	if (!n || !m)
		return;
	QDialog dlg(this);
	dlg.setWindowTitle(n->is_folder() ? "Folder properties" : "Tab properties");
	dlg.setObjectName("properties_dialog");
	// **Wide enough for the field that matters.** With no minimum this sizes to
	// whatever the current values happen to be, and a short title produced a
	// 200-pixel dialog whose Address box was about 120 wide -- for the one
	// field somebody is most likely to be editing, and the one that grows. The
	// values a form starts with are a poor guide to the values it is about to
	// hold.
	dlg.setMinimumWidth(460);
	auto *form = new QFormLayout(&dlg);

	auto *title = new QLineEdit(n->title, &dlg);
	auto *url   = new QLineEdit(n->url, &dlg);
	auto *tags  = new QLineEdit(n->tags.join(", "), &dlg);
	url->setPlaceholderText("about:blank");
	tags->setPlaceholderText("comma separated");

	auto *id = new QLabel(n->id, &dlg);
	id->setTextInteractionFlags(Qt::TextSelectableByMouse);
	id->setToolTip("Not editable: this is what the tab's saved state and the "
	                "tree file are keyed by.");

	form->addRow("Title", title);
	if (!n->is_folder())
		form->addRow("Address", url);
	form->addRow("Tags", tags);
	form->addRow("Id", id);
	form->addRow("Added", new QLabel(n->created.toString(Qt::ISODate), &dlg));
	form->addRow("Last seen", new QLabel(n->last_seen.toString(Qt::ISODate), &dlg));

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
	                                      QDialogButtonBox::Cancel, &dlg);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	form->addRow(buttons);

	if (dlg.exec() != QDialog::Accepted)
		return;
	QStringList tag_list;
	for (const QString &t : tags->text().split(',', Qt::SkipEmptyParts))
		if (!t.trimmed().isEmpty())
			tag_list << t.trimmed();
	m->update_node(n, title->text(), n->is_folder() ? n->url : url->text(),
	                tag_list);
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

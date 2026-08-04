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

	QAction *copy_url_a = nullptr, *dup_a = nullptr;
	if (n && !n->url.isEmpty())
		copy_url_a = menu.addAction("&Copy Address");
	if (n)
		dup_a = menu.addAction("&Duplicate");

	QAction *tab_a    = menu.addAction("New &Tab Here");
	QAction *folder_a = menu.addAction("New &Folder Here");
	menu.addSeparator();
	QAction *props_a = n ? menu.addAction("&Properties…") : nullptr;
	QAction *del_a   = n ? menu.addAction("&Delete") : nullptr;

	QAction *chosen = menu.exec(viewport()->mapToGlobal(pos));
	if (!chosen)
		return;
	if (chosen == open_a)           emit open_requested(n);
	else if (chosen == sus_a)       emit suspend_requested(n);
	else if (chosen == copy_url_a)  QGuiApplication::clipboard()->setText(n->url);
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

// What a node *is*, as opposed to where it sits. The id is shown and not
// editable: it keys `state/<id>.blob` and the outline file, so retyping it
// would orphan a tab's history with no warning.
void tab_tree_view::edit_properties(node *n) {
	tab_tree_model *m = source_model();
	if (!n || !m)
		return;
	QDialog dlg(this);
	dlg.setWindowTitle(n->is_folder() ? "Folder properties" : "Tab properties");
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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "tab_tree_view.h"
#include <QEvent>
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
	// 400 ms, chosen by dragging rather than by precedent. The first value here
	// was 600, picked because that is roughly what file managers have used for
	// twenty years -- a defensible argument and the wrong kind of evidence for
	// something whose only real measure is whether it feels stuck. Tried by
	// hand, 600 does.
	//
	// The tension is real in both directions: too eager and a folder opens
	// while a drag is merely passing over it on the way somewhere else, too
	// slow and the gesture stalls. 400 is on the near side of that and was
	// picked with a hand on the mouse.
	setAutoExpandDelay(400);

	// Dragging towards an edge scrolls rather than stopping at it, which is
	// what makes a tree taller than the window reachable at all. Qt defaults
	// this on; set it explicitly so it survives somebody tuning the view.
	setAutoScroll(true);
	setAutoScrollMargin(24);

	// **Why the tree is empty, which is two different questions.** Typing a
	// search that matches nothing emptied the pane with no explanation -- and
	// that reads identically to a tree with no tabs in it, or to the filter
	// being broken. The one thing it does not read as is "your search matched
	// nothing", which is what happened.
	m_empty = new QLabel(viewport());
	m_empty->setObjectName("tree_empty");
	m_empty->setAlignment(Qt::AlignCenter);
	m_empty->setWordWrap(true);
	m_empty->setAttribute(Qt::WA_TransparentForMouseEvents);
	m_empty->setEnabled(false);   // the style's dimmed text rather than a picked grey
	m_empty->hide();
	viewport()->installEventFilter(this);

	setContextMenuPolicy(Qt::CustomContextMenu);
	connect(this, &QWidget::customContextMenuRequested,
	        this, &tab_tree_view::show_menu);
}

void tab_tree_view::setModel(QAbstractItemModel *m) {
	if (tab_tree_model *old = source_model())
		old->disconnect(this);
	if (QAbstractItemModel *prev = model())
		prev->disconnect(this);
	QTreeView::setModel(m);
	if (m) {
		// Every way the visible row count can change. `layoutChanged` is the
		// one a filter emits, and the one that would have been missed by
		// watching insertions and removals alone.
		connect(m, &QAbstractItemModel::layoutChanged,
		        this, &tab_tree_view::update_empty_state);
		connect(m, &QAbstractItemModel::modelReset,
		        this, &tab_tree_view::update_empty_state);
		connect(m, &QAbstractItemModel::rowsInserted,
		        this, &tab_tree_view::update_empty_state);
		connect(m, &QAbstractItemModel::rowsRemoved,
		        this, &tab_tree_view::update_empty_state);
	}
	update_empty_state();
	if (tab_tree_model *src = source_model()) {
		connect(src, &QAbstractItemModel::modelAboutToBeReset,
		        this, &tab_tree_view::remember_open_folders);
		connect(src, &QAbstractItemModel::modelReset,
		        this, &tab_tree_view::reopen_folders);
	}
}

bool tab_tree_view::eventFilter(QObject *o, QEvent *e) {
	// The viewport knows when it is the size it will be drawn at; the widget's
	// own resizeEvent fires before that, which is how the same message came out
	// clipped into a corner in the downloads dialog.
	if (o == viewport() && e->type() == QEvent::Resize && m_empty &&
	    m_empty->isVisible())
		m_empty->setGeometry(viewport()->rect());
	return QTreeView::eventFilter(o, e);
}

void tab_tree_view::update_empty_state() {
	if (!m_empty)
		return;
	const bool nothing_shown = !model() || model()->rowCount() == 0;
	if (!nothing_shown) {
		m_empty->hide();
		return;
	}
	// The distinction worth drawing: a tree with nothing in it, and a tree
	// whose contents are all filtered away, look identical and mean opposite
	// things. Only the second is somebody's own doing, and only the second has
	// an obvious way out.
	tab_tree_model *src = source_model();
	const bool has_tabs = src && src->root() && !src->root()->children.isEmpty();
	m_empty->setText(has_tabs
	                     ? "Nothing matches that search.\n\nClear the box above "
	                        "to see the whole tree again."
	                     : "No tabs yet.\n\nFile > New Tab, or drag one in from "
	                        "another browser.");
	m_empty->setGeometry(viewport()->rect());
	m_empty->show();
	m_empty->raise();
}

// Walks the source tree and records the id of every folder the view has open,
// plus whatever was current, so the selection does not jump to the top either.
void tab_tree_view::remember_open_folders() {
	m_open_ids.clear();
	m_current_id.clear();
	tab_tree_model *src = source_model();
	if (!src)
		return;

	if (node *cur = node_at_index(currentIndex()))
		m_current_id = cur->id;

	QList<node *> stack;
	for (node *c : src->root()->children)
		stack << c;
	while (!stack.isEmpty()) {
		node *n = stack.takeLast();
		if (!n)
			continue;
		if (n->is_folder() && isExpanded(view_index(n)))
			m_open_ids << n->id;
		for (node *c : n->children)
			stack << c;
	}
}

void tab_tree_view::reopen_folders() {
	tab_tree_model *src = source_model();
	if (!src)
		return;
	// Parents before children, which the id list does not guarantee -- so this
	// runs until nothing more can be opened. Expanding a child of a collapsed
	// parent silently does nothing, and one pass would leave the deeper folders
	// shut.
	bool progress = true;
	QSet<QString> done;
	while (progress) {
		progress = false;
		for (const QString &id : std::as_const(m_open_ids)) {
			if (done.contains(id))
				continue;
			node *n = src->node_by_id(id);
			if (!n)
				continue;
			const QModelIndex idx = view_index(n);
			if (!idx.isValid())
				continue;
			setExpanded(idx, true);
			if (isExpanded(idx)) {
				done << id;
				progress = true;
			}
		}
	}
	if (!m_current_id.isEmpty()) {
		if (node *cur = src->node_by_id(m_current_id)) {
			const QModelIndex idx = view_index(cur);
			if (idx.isValid()) {
				setCurrentIndex(idx);
				scrollTo(idx, QAbstractItemView::EnsureVisible);
			}
		}
	}
}

// The view's index for a node, through the proxy when there is one.
QModelIndex tab_tree_view::view_index(node *n) const {
	tab_tree_model *src = source_model();
	if (!src || !n)
		return QModelIndex();
	const QModelIndex s = src->index_for_node(n);
	if (auto *proxy = qobject_cast<tree_sort_proxy *>(model()))
		return proxy->mapFromSource(s);
	return s;
}

node *tab_tree_view::node_at_index(const QModelIndex &idx) const {
	tab_tree_model *src = source_model();
	if (!src || !idx.isValid())
		return nullptr;
	if (auto *proxy = qobject_cast<tree_sort_proxy *>(model()))
		return src->node_for_index(proxy->mapToSource(idx));
	return src->node_for_index(idx);
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

	// **`k`, not `l`.** "Loc&k" rather than "&Lock", because Dup&licate already
	// holds l and a mnemonic that matches two items picks neither -- which is
	// what the Alt-key audit exists to catch.
	QAction *lock_a = nullptr;
	if (n) {
		lock_a = menu.addAction(n->locked ? "Un&lock" : "Loc&k");
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
	else if (chosen == lock_a)      emit lock_requested(n);
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

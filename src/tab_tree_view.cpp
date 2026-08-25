// SPDX-License-Identifier: GPL-3.0-or-later
#include "tab_tree_view.h"
#include <QEvent>
#include "tree_sort_proxy.h"

#include "node.h"
#include "tab_tree_model.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDragMoveEvent>
#include <QKeyEvent>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
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
	// **Rows, not cells.** The default is `SelectItems`, which is invisible
	// while a model has one column and breaks everything the moment it has
	// two: clicking the history column would select that cell alone, and
	// `selected_node` -- which takes column 0 and skips the rest -- would find
	// nothing to open, rename or delete. The row is the thing a person is
	// pointing at; the columns are how it is drawn.
	setSelectionBehavior(QAbstractItemView::SelectRows);
	// So the focus rectangle follows the whole row rather than boxing the
	// title and leaving the count outside it.
	setAllColumnsShowFocus(true);

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

// Walks the source tree and records the id of every row the view has open, plus
// whatever was current, so the selection does not jump to the top either.
//
// **Every row, not every folder**, and the difference is a reported bug. This
// filtered on `is_folder()`, written when only a folder could hold children --
// sub-tabs (arch sec 5.5) made a tab a parent too, and this was never revisited.
// So a tab opened to show its sub-tabs was never recorded, and any rebuild of
// the model shut it: deleting one sub-tab folded the parent it was under, which
// is where this was found. `isExpanded` is already false for a row with no
// children, so asking about folders bought nothing even before that.
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
		if (isExpanded(view_index(n)))
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

void tab_tree_view::show_node(node *n) {
	const QModelIndex idx = view_index(n);
	if (!idx.isValid())
		return;
	// Ancestors first, outermost in: `setExpanded` on a row inside a folder
	// that is still shut does nothing, so opening from the inside out leaves
	// the deeper ones closed -- the same ordering `reopen_folders` learned.
	QList<QModelIndex> chain;
	for (QModelIndex p = idx.parent(); p.isValid(); p = p.parent())
		chain.prepend(p);
	for (const QModelIndex &p : chain)
		setExpanded(p, true);
	setCurrentIndex(idx);
	scrollTo(idx, QAbstractItemView::EnsureVisible);
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

// **Never the root, whatever the index turns out to be.** `node_for_index`
// answers the root for an invalid index, because `rowCount(QModelIndex())` has
// to mean "how many top-level rows" -- correct there, and a trap here. A view
// index is a *row*, the root is not one, and a caller writing the obvious
// `if (node *n = ...)` gets a non-null answer it cannot tell apart from a real
// one. Two indices arrive invalid: no current row at all, and a source index
// the proxy could not map. F2 with nothing selected took the first and opened
// the properties editor on the invisible root, where OK edited a node nobody
// can see and the typing went nowhere.
static node *row_node(tab_tree_model *src, const QModelIndex &source_index) {
	if (!src || !source_index.isValid())
		return nullptr;
	node *n = src->node_for_index(source_index);
	return n == src->root() ? nullptr : n;
}

node *tab_tree_view::node_at_index(const QModelIndex &idx) const {
	tab_tree_model *src = source_model();
	if (!src || !idx.isValid())
		return nullptr;
	if (auto *proxy = qobject_cast<tree_sort_proxy *>(model()))
		return row_node(src, proxy->mapToSource(idx));
	return row_node(src, idx);
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
		return row_node(m, proxy->mapToSource(at));
	return row_node(m, at);
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
		// `node_at_index` rather than the mapping written out here: it is the
		// one that refuses the root, and this is where that mattered. With no
		// row selected the index is invalid, which used to resolve to the root
		// and open a properties dialog for a node that is not on screen.
		if (node *n = node_at_index(currentIndex())) {
			edit_properties(n);
			return;
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

	// **The lock belongs here too.** It is a property of the node, like the
	// title and the tags, and a dialog called "properties" that does not show
	// one of them makes the tree's context menu the only place to learn it
	// exists -- or to discover that a row will not move because of it.
	//
	// Ticking it here pins to the address in the box above, which is the
	// address OK is about to write. The context menu pins to the page actually
	// showing, because there it has no box to read. One rule underneath both:
	// locking pins to the url the node ends up with.
	auto *locked = new QCheckBox(
	  n->is_folder() ? "Keep this folder where it is"
	                  : "Keep this tab on this page, and in this place", &dlg);
	locked->setObjectName("properties_locked");
	locked->setChecked(n->locked);
	locked->setToolTip(n->is_folder()
	                       ? "A locked folder cannot be dragged, reordered, or "
	                         "moved by the reorganizer."
	                       : "A locked tab keeps its page -- browsing opens a "
	                         "sub-tab below it -- and cannot be moved.");

	form->addRow("Title", title);
	if (!n->is_folder())
		form->addRow("Address", url);
	form->addRow("Tags", tags);
	form->addRow("Locked", locked);
	form->addRow("Id", id);
	form->addRow("Added", new QLabel(n->created.toString(Qt::ISODate), &dlg));
	form->addRow("Last seen", new QLabel(n->last_seen.toString(Qt::ISODate), &dlg));

	// **Where this tab had been, when it was imported from another browser.**
	//
	// Shown here rather than in the tree because it is the answer to a
	// question nobody asks often: the row already says how many pages are
	// behind this one, and this is where somebody goes when they want to know
	// *which*. A read-only list, because it is a record -- there is nothing to
	// edit and nothing that would be true if it were edited.
	//
	// Absent entirely when there is none, which is the usual case: an empty
	// box captioned "History" tells a person their tab has lost something,
	// when in fact it never had one.
	// Set by the list below, acted on after `exec` returns. **Not emitted from
	// inside the dialog**: that runs while a modal event loop is still on the
	// stack, so the shell would build a view and start a load underneath one --
	// the same re-entrancy the locked-tab sub-tab path avoids by queueing.
	QUrl chosen_from_history;
	QListWidget *history = nullptr;
	if (!n->history.is_empty()) {
		history = new QListWidget(&dlg);
		history->setObjectName("properties_history");
		history->setAlternatingRowColors(true);
		// Four rows of it. The list can hold hundreds -- one imported tab here
		// had ninety -- and a dialog that grows to fit its longest field is
		// one that goes off the bottom of the screen for the tab that most
		// needs reading.
		history->setMaximumHeight(4 * history->fontMetrics().height() + 24);
		for (int i = 0; i < n->history.entries.size(); ++i) {
			const history_entry &e = n->history.entries.at(i);
			auto *row = new QListWidgetItem(e.title, history);
			row->setToolTip(e.url);
			// The url on the item, so a reordered or filtered list cannot hand
			// back the wrong one by index.
			row->setData(Qt::UserRole, e.url);
			if (i == n->history.index) {
				// Where the tab stands in its own past, marked rather than
				// merely selected: selection is the user's and is about to
				// move the moment they click anything.
				QFont f = row->font();
				f.setBold(true);
				row->setFont(f);
				row->setText(e.title + "  (this page)");
				history->setCurrentItem(row);
			}
		}
		connect(history, &QListWidget::itemActivated, &dlg,
		         [&chosen_from_history, &dlg](QListWidgetItem *item) {
			const QUrl u(item->data(Qt::UserRole).toString());
			if (!u.isValid() || u.isEmpty())
				return;
			// Rejected rather than accepted: opening a page out of the record
			// is not a reason to write the form back, and leaving the dialog
			// up over the tab that just appeared hides the thing the click
			// asked for.
			chosen_from_history = u;
			dlg.reject();
		});
		auto *caption = new QLabel(
		  QString("%1 %2, %3 back and %4 ahead. Double-click to open one below "
		           "this tab.")
		      .arg(n->history.entries.size())
		      .arg(n->history.entries.size() == 1 ? "page" : "pages")
		      .arg(n->history.back_count())
		      .arg(n->history.forward_count()), &dlg);
		caption->setWordWrap(true);
		form->addRow("History", history);
		form->addRow(QString(), caption);
	}

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
	                                      QDialogButtonBox::Cancel, &dlg);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	form->addRow(buttons);

	if (dlg.exec() != QDialog::Accepted) {
		if (!chosen_from_history.isEmpty())
			emit history_open_requested(n, chosen_from_history);
		return;
	}
	QStringList tag_list;
	for (const QString &t : tags->text().split(',', Qt::SkipEmptyParts))
		if (!t.trimmed().isEmpty())
			tag_list << t.trimmed();
	m->update_node(n, title->text(), n->is_folder() ? n->url : url->text(),
	                tag_list);
	// After update_node, so the pin is the address just written rather than the
	// one the dialog opened on.
	m->set_locked(n, locked->isChecked());
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

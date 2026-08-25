// SPDX-License-Identifier: GPL-3.0-or-later
#include "tab_tree_model.h"
#include "node.h"
#include "tree_outline.h"
#include "tree_diff.h"

#include <QMimeData>
#include <functional>
#include <QApplication>
#include <QStyle>
#include <QFont>
#include <QBrush>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

tab_tree_model::tab_tree_model(QObject *parent)
  : QAbstractItemModel(parent) {
	m_root = new node;
	m_root->id   = "root";
	m_root->type = node_type::folder;
}

tab_tree_model::~tab_tree_model() {
	delete m_root;  // deletes the whole tree recursively (node dtor)
}

// A padlock in the corner of whatever icon the row already had.
//
// **The desktop's own lock, where there is one.** The first version drew the
// shape by hand, on the reasoning that QStyle has no lock among its standard
// icons and a bundled image would need one per scale factor. Photographing it
// settled that: at the 16px a tree row actually gets, the shackle had about two
// pixels to exist in and vanished, the white outline disappeared against a
// light background, and what reached the screen was a dark blob. An emblem
// theme designers drew *for* 16px reads at 16px, which is the whole difference.
//
// The hand-drawn shape stays as the fallback for a machine with no icon theme:
// a light fill with a dark stroke, which is an outlined shape and therefore
// reads on either background. The first version was a white outline with no
// fill and vanished against a light tree -- and the reasoning that replaced it,
// "the tree's background is light", was itself wrong: this desktop's portal
// answers *prefer dark*, and every screenshot that had been used to judge the
// icon was taken in a light palette no user here sees, because the drivers
// applied no scheme at all. An outline needs neither assumption.
//
// The base icon is kept underneath instead of replaced, so a locked folder
// still reads as a folder and a locked tab as a tab. Locking is a property of
// a row, not a kind of row.
// What a desktop icon theme is likely to call a lock, most specific first.
// `emblem-*` is the freedesktop namespace for exactly this -- a mark placed on
// another icon -- and the rest are the fallbacks a theme without one tends to
// have. A theme with none of them gets the drawn shape below.
static const char *const k_lock_icon_names[] = {
	"emblem-locked", "object-locked", "system-lock-screen", "lock",
	"changes-prevent",
};

static QIcon with_padlock(const QIcon &base) {
	const int side = 16;
	QPixmap pm = base.pixmap(side, side);
	if (pm.isNull())
		return base;

	// Bottom-right, at 60% of the tile: big enough to be a shape rather than a
	// speck, small enough that the icon underneath is still identifiable.
	const int mark = qMax(9, pm.height() * 3 / 5);
	const QRect corner(pm.width() - mark, pm.height() - mark, mark, mark);

	QIcon lock;
	for (const char *name : k_lock_icon_names) {
		if (QIcon::hasThemeIcon(name)) {
			lock = QIcon::fromTheme(name);
			break;
		}
	}

	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);

	if (!lock.isNull()) {
		p.drawPixmap(corner, lock.pixmap(mark, mark));
	} else {
		const qreal w = mark * 0.72;
		const QRectF body(pm.width() - w - 0.5, pm.height() - mark * 0.55 - 0.5,
		                   w, mark * 0.55);
		const qreal shackle = body.width() * 0.32;
		p.setPen(QPen(QColor(20, 20, 20), 1.0));
		p.setBrush(Qt::NoBrush);
		p.drawArc(QRectF(body.center().x() - shackle, body.top() - shackle,
		                  shackle * 2, shackle * 2), 0, 180 * 16);
		p.setBrush(QColor(250, 250, 250));
		p.drawRoundedRect(body, 1.0, 1.0);
	}
	p.end();

	return QIcon(pm);
}

static void index_subtree(node *n, QHash<QString, node *> &map) {
	for (node *c : n->children) {
		map.insert(c->id, c);
		index_subtree(c, map);
	}
}

// A node's `order` is its position in its parent's child list, written down.
// Keep the two in step at the point the list changes.
//
// It used to be set once, at creation, from `parent->children.size()` -- one
// past the highest, which is true only while nothing has *left* the list. A
// delete and a drag-out each leave a gap in the numbering without leaving one
// in the count, so the next node added collides with a sibling that is still
// there; three siblings were measured holding order 2. Two rows with the same
// order are not in the wrong order, they have *no* order: `lessThan` reports
// them equal, and the reorganizer's diff reads the tie as a move nobody made.
static void renumber(node *parent) {
	if (!parent)
		return;
	for (int i = 0; i < parent->children.size(); ++i)
		parent->children[i]->order = i;
}

void tab_tree_model::reindex() {
	m_id_index.clear();
	index_subtree(m_root, m_id_index);
}

bool tab_tree_model::load(const QString &path) {
	node *fresh = tree_outline::load(path, &m_last_flattened);
	if (!fresh)
		return false;
	beginResetModel();
	delete m_root;
	m_root = fresh;
	m_path = path;
	reindex();
	endResetModel();
	return true;
}

bool tab_tree_model::save(const QString &path) const {
	return tree_outline::save(path.isEmpty() ? m_path : path, m_root);
}

node *tab_tree_model::node_for_index(const QModelIndex &index) const {
	if (!index.isValid())
		return m_root;
	return static_cast<node *>(index.internalPointer());
}

node *tab_tree_model::node_by_id(const QString &id) const {
	return m_id_index.value(id, nullptr);
}

QModelIndex tab_tree_model::index_for_node(node *n) const {
	if (!n || n == m_root)
		return {};
	return createIndex(n->row(), 0, n);
}

void tab_tree_model::refresh_node(node *n) {
	const QModelIndex idx = index_for_node(n);
	if (idx.isValid())
		emit dataChanged(idx, idx);
}

QModelIndex tab_tree_model::index(int row, int column, const QModelIndex &parent) const {
	if (!hasIndex(row, column, parent))
		return {};
	node *parent_node = node_for_index(parent);
	if (row < 0 || row >= parent_node->children.size())
		return {};
	return createIndex(row, column, parent_node->children.at(row));
}

QModelIndex tab_tree_model::parent(const QModelIndex &index) const {
	if (!index.isValid())
		return {};
	node *n = static_cast<node *>(index.internalPointer());
	node *parent_node = n ? n->parent : nullptr;
	if (!parent_node || parent_node == m_root)
		return {};
	return createIndex(parent_node->row(), 0, parent_node);
}

int tab_tree_model::rowCount(const QModelIndex &parent) const {
	if (parent.column() > 0)
		return 0;
	return node_for_index(parent)->children.size();
}

int tab_tree_model::columnCount(const QModelIndex &) const {
	return 1;
}

QVariant tab_tree_model::data(const QModelIndex &index, int role) const {
	if (!index.isValid())
		return {};
	node *n = static_cast<node *>(index.internalPointer());
	if (!n)
		return {};

	switch (role) {
		case Qt::DisplayRole: {
			const QString label = n->title.isEmpty() ? n->url : n->title;
			// **What a tab brought with it, in the fewest characters that say
			// it.** An imported tab carries the pages it had been on, and
			// without a mark on the row there is nothing to suggest opening
			// its properties to find them -- a record nobody can see is one
			// nobody reads.
			//
			// The count is of pages *behind* it, because that is the one a
			// person is looking for; the rest of the list is in the dialog.
			//
			// **Unless there is nothing behind it**, which was a silent hole:
			// a tab sitting at the start of its own history has pages only
			// ahead, so counting backwards gave zero and the row said nothing
			// at all -- a record with no way to find out it exists, which is
			// the exact failure this suffix is here to prevent. Measured on
			// the tabs recovered from the crashed Chromium session: 30 of
			// 1144 records were this shape.
			//
			// Only one of the two is ever shown, and back wins where both
			// apply. The suffix is a hint that there is something to open,
			// not a summary of it; two numbers on a row would cost more
			// width than they buy, and the dialog states both anyway.
			//
			// Appended to the drawn text only: search matches the node's own
			// title and url, and sorting reads `title_role`, so neither can
			// see this and a row cannot be found by typing "back".
			const int behind = n->history.back_count();
			if (behind > 0)
				return QString("%1  %2 %3 back")
					         .arg(label).arg(QChar(0x00b7)).arg(behind);
			const int ahead = n->history.forward_count();
			if (ahead > 0)
				return QString("%1  %2 %3 ahead")
					         .arg(label).arg(QChar(0x00b7)).arg(ahead);
			return label;
		}
		case Qt::ToolTipRole:
			return n->url;
		case Qt::DecorationRole: {
			QStyle *s = QApplication::style();
			const QIcon base = s->standardIcon(n->is_folder() ? QStyle::SP_DirIcon
				                                                 : QStyle::SP_FileIcon);
			return n->locked ? with_padlock(base) : base;
		}
		case Qt::FontRole: {
			// Open (live) tabs are bold; suspended are italic.
			QFont f;
			if (n->type == node_type::open_tab)      f.setBold(true);
			if (n->type == node_type::suspended_tab) f.setItalic(true);
			return f;
		}
		case Qt::ForegroundRole:
			// Unopened links are muted; everything else default.
			if (n->type == node_type::unopened_tab)
				return QBrush(QColor(140, 140, 140));
			return {};
		case title_role:      return n->title;
		case url_role:        return n->url;
		case created_role:    return n->created;
		case last_seen_role:  return n->last_seen;
		case tree_order_role: return n->order;
		case node_type_role:  return static_cast<int>(n->type);
		case locked_role:     return n->locked;
		default:              return {};
	}
}

tree_snapshot tab_tree_model::take_snapshot() const {
	return tree_diff::snapshot(m_root);
}

int tab_tree_model::restore_snapshot(const tree_snapshot &snap) {
	beginResetModel();
	const int n = tree_diff::restore(m_root, snap);
	reindex();
	endResetModel();
	return n;
}

int tab_tree_model::apply_reorganization(const QList<tree_change> &changes) {
	// Moves and new folders restructure whole subtrees at once, so a reset is
	// both simpler and safer here than a sequence of begin/endMoveRows calls --
	// and this runs once, on an explicit user action, not on a hot path.
	beginResetModel();
	const int applied = tree_diff::apply(m_root, changes);
	reindex();
	endResetModel();
	return applied;
}

// --- Moving nodes about ---------------------------------------------------

Qt::ItemFlags tab_tree_model::flags(const QModelIndex &index) const {
	Qt::ItemFlags f = QAbstractItemModel::flags(index);
	if (!index.isValid())
		return f | Qt::ItemIsDropEnabled;   // the root accepts drops
	node *n = node_for_index(index);
	f |= Qt::ItemIsDragEnabled;

	// **A tab can contain something now**, which reverses the rule this used to
	// state. The old one said only a folder may be a drop target, on the
	// grounds that dropping onto a tab would have to mean "beside it" and a
	// gesture meaning one thing on one row and another on the next is one
	// people stop trusting. That reasoning was right and its premise is gone:
	// sub-tabs (sec 5.5) make "inside a tab" a real place, so the gesture is the
	// same everywhere -- onto a row puts it under that row.
	f |= Qt::ItemIsDropEnabled;

	// A locked node does not move (sec 5.5). Refusing the drag here is what makes
	// that visible rather than surprising: the row simply does not lift, so
	// nobody carries it across the tree and discovers at the drop that it was
	// pinned. `dropMimeData` refuses it as well, since a drag is not the only
	// way a move can be asked for.
	if (n && n->locked)
		f &= ~Qt::ItemIsDragEnabled;
	return f;
}

Qt::DropActions tab_tree_model::supportedDropActions() const {
	return Qt::MoveAction | Qt::CopyAction;
}

QStringList tab_tree_model::mimeTypes() const {
	// Our own type, carrying ids rather than urls. An id is what the state
	// blob and the outline file are keyed by, so moving by id keeps a tab's
	// history and suspended state attached to it; moving by url would quietly
	// produce a new tab that had forgotten where it had been.
	return { QStringLiteral("application/x-hydra-node-ids"),
		       QStringLiteral("text/uri-list") };
}

QMimeData *tab_tree_model::mimeData(const QModelIndexList &indexes) const {
	QStringList ids, urls;
	for (const QModelIndex &i : indexes) {
		if (i.column() != 0)
			continue;   // one entry per row, not per column
		if (node *n = node_for_index(i)) {
			ids << n->id;
			if (!n->url.isEmpty())
				urls << n->url;
		}
	}
	if (ids.isEmpty())
		return nullptr;
	auto *m = new QMimeData;
	m->setData("application/x-hydra-node-ids", ids.join('\n').toUtf8());
	// And as plain urls, so a tab can be dragged to any other application that
	// takes one. Costs two lines and is the difference between a tree that
	// talks to the desktop and one that only talks to itself.
	if (!urls.isEmpty())
		m->setText(urls.join('\n'));
	return m;
}

bool tab_tree_model::is_ancestor_of(const node *maybe_ancestor, const node *n) const {
	for (const node *p = n; p; p = p->parent)
		if (p == maybe_ancestor)
			return true;
	return false;
}

QString tab_tree_model::unused_id(const QString &like) const {
	// Keep the shape of the id it came from -- they are short and opaque, and a
	// copy of `a1` reading `a1-2` stays readable in the outline file a person
	// may well open in an editor.
	for (int n = 2; ; ++n) {
		const QString candidate = QString("%1-%2").arg(like).arg(n);
		if (!m_id_index.contains(candidate))
			return candidate;
	}
}

static node *deep_copy(const node *src, tab_tree_model *model,
                        const std::function<QString(const QString &)> &fresh) {
	node *c = new node;
	c->id       = fresh(src->id);
	c->type     = src->type;
	c->title    = src->title;
	c->url      = src->url;
	c->created  = src->created;
	c->last_seen = src->last_seen;
	c->tags     = src->tags;
	// Copied, for the same reason `tags` is and the state blob is not: it
	// describes the *address*, not a live view of it. This is also the one
	// path that matters most -- dragging a tab out of the Firefox or Chromium
	// mirror is a `deep_copy`, and it is the only moment the imported history
	// has to cross into the tree or it is lost with the mirror.
	c->history  = src->history;
	// **Not** a copy of the open/suspended state. A copied tab is a second
	// bookmark of the same address, not a second live view of it: the state
	// blob belongs to the id it was written under, and duplicating the id is
	// exactly what `unused_id` exists to prevent.
	if (c->type == node_type::open_tab || c->type == node_type::suspended_tab)
		c->type = node_type::unopened_tab;
	// Nor a copy of `locked` or `renamed`, for a related reason: both say
	// something a person decided about *that* row. A copy nobody has pinned yet
	// starts unpinned, or the gesture hands back something that has to be
	// unlocked before it will move (sec 5.5).
	for (const node *k : src->children) {
		node *kid = deep_copy(k, model, fresh);
		kid->parent = c;
		c->children << kid;
	}
	return c;
}

bool tab_tree_model::dropMimeData(const QMimeData *data, Qt::DropAction action,
                                   int row, int column, const QModelIndex &parent) {
	Q_UNUSED(column);
	if (!data || action == Qt::IgnoreAction)
		return false;
	if (!data->hasFormat("application/x-hydra-node-ids"))
		return false;

	node *target = parent.isValid() ? node_for_index(parent) : m_root;
	if (!target)
		return false;

	const QStringList ids =
	  QString::fromUtf8(data->data("application/x-hydra-node-ids")).split('\n');

	QList<node *> moving;
	for (const QString &id : ids) {
		node *n = m_id_index.value(id);
		if (!n)
			continue;
		// A folder cannot be dropped inside itself: the tree would become a
		// ring, the outline writer would recurse forever, and every node below
		// the drag would vanish from the file. The reorganizer refuses the same
		// move for the same reason (sec 9.4) and this is that rule again, one
		// gesture closer to the user.
		if (is_ancestor_of(n, target))
			return false;
		if (n == target)
			return false;
		// Pinned nodes do not move, and a *copy* of one is a new row rather
		// than a move, so only the move branch is refused. Refusing the whole
		// drop would mean one locked node in a multi-node selection cancelled
		// everything, which punishes the wrong rows.
		if (n->locked && action != Qt::CopyAction)
			continue;
		moving << n;
	}
	if (moving.isEmpty())
		return false;

	// Reset rather than fine-grained move signals, matching what `load` and
	// `apply_reorganization` already do here. It costs the view's expansion
	// state on a drop, which is worth revisiting; it is not worth risking a
	// begin/endMoveRows index mistake for, since those corrupt the view in ways
	// that show up much later than they happen.
	beginResetModel();
	// Parents a move takes something *out* of. Only the target was renumbered
	// before, so a tab dragged from one folder to another left the folder it
	// came from numbered around the gap -- and the next tab added there took a
	// number one of its siblings already had.
	QList<node *> emptied;
	for (node *n : moving) {
		if (action == Qt::CopyAction) {
			node *c = deep_copy(n, this, [this](const QString &like) {
				return unused_id(like);
			});
			c->parent = target;
			if (row >= 0 && row <= target->children.size())
				target->children.insert(row++, c);
			else
				target->children << c;
		} else {
			// **Where it sits now matters when it is already in here.** A view
			// hands `dropMimeData` a row counted against the list as it stands
			// *before* anything moves. Taking the node out of that same list
			// shifts every later sibling down by one, so inserting at the
			// number we were given lands one place too far.
			//
			// Only when moving downwards -- removing from *below* the insertion
			// point changes nothing above it -- and dropping past the last row
			// appends either way. So the fault was invisible in three of the
			// four directions somebody might drag, which is what made it read
			// as the tree being unreliable rather than as an off-by-one.
			//
			// Worth stating what it cost, because the no-op case is the one
			// that reads as corruption: dropping a row back into its own gap
			// moved it one place down. The gesture that means "leave this
			// where it is" was the gesture that disturbed it.
			const int was = n->parent == target
			                  ? target->children.indexOf(n) : -1;
			if (n->parent) {
				if (n->parent != target && !emptied.contains(n->parent))
					emptied << n->parent;
				n->parent->children.removeOne(n);
			}
			if (was >= 0 && row > was)
				--row;
			n->parent = target;
			if (row >= 0 && row <= target->children.size())
				target->children.insert(row++, n);
			else
				target->children << n;
		}
	}
	// **Out of a mirror is out of it.** A copy never carried the mark --
	// `deep_copy` does not take it -- but a move did, so a plainly dragged
	// row landed in the user's folder looking filed and was then skipped by
	// the writer for belonging to somebody else. It was gone at the next
	// launch, with nothing having reported anything: the tree on screen and
	// the tree on disk disagreed, and only one of them survived.
	//
	// Asked of the destination rather than the source, because that is what
	// decides it. Dropping *into* a mirror is the same rule read the other
	// way and does not arise -- a refresh replaces the whole folder.
	if (target->mirror.isEmpty())
		for (node *n : moving)
			clear_mirror(n);

	// Sibling order is what the outline file records, so it has to agree with
	// the list we just rearranged or the next save undoes the drag.
	renumber(target);
	for (node *p : emptied)
		renumber(p);
	reindex();
	endResetModel();
	emit structure_changed();
	return true;
}

// --- Operations the context menu offers -----------------------------------

node *tab_tree_model::add_folder(node *parent, const QString &title) {
	if (!parent)
		parent = m_root;
	// **An insertion, not a reset.** A reset invalidates every index the view
	// holds and it rebuilds from nothing, so adding one row shut every open row
	// in the tree; what put them back afterwards only knew about folders. Both
	// halves are fixed, and this is the half that means there is nothing to put
	// back.
	const int at = int(parent->children.size());
	beginInsertRows(index_for_node(parent), at, at);
	node *f = new node;
	f->id      = unused_id("f");
	f->type    = node_type::folder;
	f->title   = title.isEmpty() ? QStringLiteral("New folder") : title;
	f->created = QDateTime::currentDateTime();
	f->last_seen = f->created;
	f->parent  = parent;
	parent->children << f;
	renumber(parent);
	reindex();
	endInsertRows();
	emit structure_changed();
	return f;
}

node *tab_tree_model::add_tab(node *parent, const QString &title,
                               const QString &url) {
	if (!parent)
		parent = m_root;
	// **A tab is a legal parent now.** This used to redirect to the nearest
	// folder, because a tab held no children and "in here" had no meaning for
	// one. Sub-tabs (sec 5.5) give it a meaning, and the redirect would now
	// silently put a sub-tab somewhere other than under the tab that spawned
	// it -- which is the whole relationship the feature exists to record.
	const int at = int(parent->children.size());
	beginInsertRows(index_for_node(parent), at, at);
	node *t = new node;
	t->id        = unused_id("t");
	t->type      = node_type::unopened_tab;
	t->title     = title.isEmpty() ? QStringLiteral("New tab") : title;
	t->url       = url;
	t->created   = QDateTime::currentDateTime();
	t->last_seen = t->created;
	t->parent    = parent;
	parent->children << t;
	renumber(parent);
	reindex();
	endInsertRows();
	emit structure_changed();
	return t;
}

bool tab_tree_model::remove_node(node *n) {
	// The root is the tree; removing it would leave the model pointing at
	// nothing and the outline writer with no document to write.
	if (!n || n == m_root || !n->parent)
		return false;
	// Announced first, while `n` and its children are still there to be found.
	// Deleting a node with a live view used to leave that view in the shell's
	// map under an id nothing could resolve any more -- which leaked the view
	// and, worse, stopped the live-view cap being enforced at all, because the
	// cap gives up when it cannot resolve its chosen victim.
	node *parent = n->parent;
	const int row = parent->children.indexOf(n);
	if (row < 0)
		return false;

	emit about_to_remove(n);
	// **A removal, not a reset.** `beginResetModel` invalidates every index the
	// view holds, and a QTreeView rebuilding from nothing has no expansion
	// state left to restore -- so deleting one sub-tab folded its parent, and
	// in fact folded the whole tree. Nothing here saves and restores that,
	// because with a targeted removal there is nothing to save: every row the
	// view keeps is still the row it was.
	beginRemoveRows(index_for_node(parent), row, row);
	parent->children.removeOne(n);
	// Deletes the whole subtree through node's destructor, which is what the
	// gesture means -- deleting a folder in a file manager takes what is in it.
	// The caller is responsible for having asked first.
	delete n;
	renumber(parent);
	reindex();
	endRemoveRows();
	emit structure_changed();
	return true;
}

void tab_tree_model::update_node(node *n, const QString &title,
                                  const QString &url, const QStringList &tags) {
	if (!n)
		return;
	// Coming through here means a person did it -- this is what the properties
	// editor and the rename prompt call, and nothing else does. Marked only on
	// an actual change, so opening the editor and pressing OK does not pin a
	// title nobody touched.
	//
	// **Clearing the name gives the tab back to the page.** Emptying the field
	// is the natural way to say "stop calling it that", and the alternative --
	// a checkbox marked "follow the page title" beside the name -- explains a
	// mechanism where the gesture already says it. The label falls back to the
	// address meanwhile, which is what an unvisited tab wears anyway, and the
	// next title the page reports replaces it.
	if (title.trimmed().isEmpty()) {
		n->renamed = false;
		n->title   = url.isEmpty() ? QStringLiteral("New tab") : url;
	} else {
		if (title != n->title)
			n->renamed = true;
		n->title = title;
	}
	n->url   = url;
	n->tags  = tags;
	refresh_node(n);
	// Saved like a move is: what a tab is called is as much a part of the
	// canonical file as where it sits.
	emit structure_changed();
}

bool tab_tree_model::set_locked(node *n, bool locked, const QString &pin_url) {
	// The root is the tree rather than a row in it: there is nothing to pin it
	// beside and no view to pin to a url.
	if (!n || n == m_root || n->locked == locked)
		return false;
	n->locked = locked;
	// Recorded on the node, so the pin survives a suspend, a restart, and the
	// tab being reopened from the outline file -- all of which throw away the
	// live view that knew which page was showing.
	if (locked && !pin_url.isEmpty())
		n->url = pin_url;
	// `refresh_node` is a dataChanged, which repaints the row and is what puts
	// the padlock there. It is not enough on its own: `flags()` now answers
	// differently for this row, and a view that has already decided the row is
	// draggable keeps that answer until something tells it otherwise. A
	// dataChanged does tell it -- the view re-reads flags for a changed row --
	// which is why this needs no separate reset.
	refresh_node(n);
	emit structure_changed();
	return true;
}

bool tab_tree_model::set_page_title(node *n, const QString &title) {
	if (!n || title.isEmpty() || n->renamed || title == n->title)
		return false;
	// **`about:blank` is not a name.** An empty tab now genuinely loads the
	// blank page -- that is what makes it the current tab, and what stopped a
	// typed address going to the tab before it -- and Chromium duly reports the
	// address as the title, which is correct of Chromium and useless on a row.
	// The label a person is looking at should say what `add_tab` called it
	// until a real page says otherwise.
	if (title == QLatin1String("about:blank"))
		return false;
	n->title = title;
	refresh_node(n);
	return true;
}

node *tab_tree_model::duplicate_node(node *n) {
	if (!n || !n->parent)
		return nullptr;
	// Directly below the row it copies, which is where the gesture puts it --
	// and an insertion rather than a reset, so the rest of the tree keeps the
	// rows it had open.
	node *parent = n->parent;
	const int at = parent->children.indexOf(n) + 1;
	beginInsertRows(index_for_node(parent), at, at);
	node *c = deep_copy(n, this, [this](const QString &like) {
		return unused_id(like);
	});
	c->parent = parent;
	parent->children.insert(at, c);
	for (int i = 0; i < parent->children.size(); ++i)
		parent->children[i]->order = i;
	reindex();
	endInsertRows();
	emit structure_changed();
	return c;
}

// --- Mirrors of other browsers --------------------------------------------

void tab_tree_model::mark_mirror(node *n, const QString &source) {
	if (!n)
		return;
	n->mirror = source;
	for (node *c : n->children)
		mark_mirror(c, source);
}

void tab_tree_model::clear_mirror(node *n) {
	// **Nothing to do, and it must be nothing.** A node with no mark has no
	// marked descendant either -- a mirror is a whole subtree -- so returning
	// here is not an optimisation, it is what keeps an ordinary drag from
	// re-minting ids. Falling through renamed every moved node in the tree,
	// which three tests caught by asking whether a dragged row was still
	// itself afterwards.
	if (!n || n->mirror.isEmpty())
		return;
	n->mirror.clear();
	// **And the id, which belonged to the mirror as much as the mark did.**
	// Mirror ids are scoped to their source -- `firefox-0`, `firefox-1` -- and
	// `replace_mirror` mints those same names again on the next refresh. A row
	// that kept one after being dragged into the tree would collide with a
	// mirrored tab in `m_id_index`, and `node_by_id` -- which the lifecycle and
	// the AI payload both use -- would answer with whichever won. They would
	// also share `state/<id>.blob` and `state/<id>.history`.
	//
	// Announced rather than done quietly: the shell keys live views and the
	// recently-used list by id, and a mirrored tab can be open at the moment it
	// is dragged.
	if (!n->id.isEmpty()) {
		const QString was = n->id;
		n->id = unused_id(n->is_folder() ? "f" : "t");
		m_id_index.insert(n->id, n);
		emit id_changed(was, n->id);
	}
	// The whole subtree, because a mirrored folder's children are mirrored
	// too and half a mirror is the shape `tree_invariants` refuses.
	for (node *c : n->children)
		clear_mirror(c);
}

node *tab_tree_model::replace_mirror(const QString &source, const QString &title,
                                      const QList<node *> &tabs) {
	if (source.isEmpty())
		return nullptr;

	// Row signals rather than a full reset, and that matters once this is on a
	// timer: a reset collapses every folder in the tree, so a background
	// refresh would fold up the user's own work every time the other browser
	// happened to write its file. Only the root's own children change here, so
	// the fine-grained calls are tractable -- one row out, one row in.
	for (int i = m_root->children.size() - 1; i >= 0; --i) {
		node *c = m_root->children.at(i);
		if (c->mirror == source) {
			// A mirrored tab can be opened like any other, so a refresh that
			// replaces the folder has to say so first for the same reason.
			emit about_to_remove(c);
			beginRemoveRows(QModelIndex(), i, i);
			m_root->children.removeAt(i);
			delete c;
			endRemoveRows();
		}
	}

	node *folder = new node;
	folder->id        = unused_id("m");
	folder->type      = node_type::folder;
	folder->title     = title;
	folder->created   = QDateTime::currentDateTime();
	folder->last_seen = folder->created;
	folder->parent    = m_root;
	for (node *t : tabs) {
		t->parent = folder;
		t->order  = folder->children.size();
		folder->children << t;
	}
	mark_mirror(folder, source);

	// Mirrors sit at the top, where a thing that is not yours is easiest to
	// tell apart from the tree you built.
	beginInsertRows(QModelIndex(), 0, 0);
	m_root->children.prepend(folder);
	for (int i = 0; i < m_root->children.size(); ++i)
		m_root->children[i]->order = i;
	reindex();
	endInsertRows();

	// **Not** `structure_changed`: that signal means "save the tree", and a
	// mirror is the one thing that must not be saved. The shell shows it and
	// forgets it, which is what makes it safe to replace on every refresh.
	return folder;
}

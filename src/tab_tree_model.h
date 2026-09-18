#pragma once

#include "tree_diff.h"   // for tree_change in apply_reorganization()

#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QString>

struct node;

// The single source of truth for the tree (architecture doc sec 5.1). Exposes
// node attributes through custom roles so the sort/filter proxy can order and
// filter without the model caring how.
// **Needs a QApplication, not merely a QCoreApplication.** It answers
// `DecorationRole` with a style icon and `FontRole` with a font, so it asks
// `QApplication::style()` -- which is null when there is no GUI application, and
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
		locked_role,
	};

	// The columns. Named rather than written as 0 and 1 at each call site,
	// because a bare `1` in a view's resize call says nothing about what it
	// is sizing.
	enum columns {
		title_column   = 0,
		// How much history the row brought with it, right-aligned. Its own
		// column so that it is sized to its contents and cannot be elided
		// away, which is what happened when it was a suffix on the title.
		history_column = 1,
		column_count   = 2,
	};

	explicit tab_tree_model(QObject *parent = nullptr);
	~tab_tree_model() override;

	// Load/save the canonical outline file. Replaces the current tree.
	bool load(const QString &path);

	// How many nodes the last `load` had to move up because the file nested
	// them deeper than `tree_limits::max_depth`. Zero for any tree anyone
	// filed by hand; non-zero means the tree on screen is not the shape the
	// file described, which is a thing the shell has to say out loud.
	int last_flattened() const { return m_last_flattened; }
	// Non-blank lines the last load could not turn into a node. **Whatever
	// reads this has to act on it**, because what gets written back is what
	// parsed: a line nobody was told about is a line that stops existing on
	// the next save. `last_flattened` had the same job and nothing called it
	// for as long as it existed, which is why this one is wired at the same
	// time rather than added beside it.
	int last_unparsed() const { return m_last_unparsed; }
	bool save(const QString &path) const;

	node *node_for_index(const QModelIndex &index) const;
	node *node_by_id(const QString &id) const;    // O(1) lookup for lifecycle/AI
	node *root() const { return m_root; }

	QModelIndex index_for_node(node *n) const;
	void        refresh_node(node *n);            // emit dataChanged for one node

	// Apply an accepted AI reorganization (architecture doc sec 9.5). Structural,
	// so it goes through a model reset rather than per-row moves; returns the
	// number of changes applied. Payloads follow ids, so nothing else moves.
	int apply_reorganization(const QList<tree_change> &changes);

	// The sec 9.4 undo snapshot: take one before applying, restore it to revert.
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
	//
	// **Enforced by the view, not by a flag here.** This class held a
	// `reorder_allowed` bool that the shell set from the sort combo -- two calls
	// that had to be kept in step, so any other route to changing the sort left
	// a stale answer behind. `tab_tree_view` asks the proxy at the moment of the
	// gesture instead, and the model simply honours whatever row it is handed.

	// --- Operations the context menu offers -------------------------------
	// Each mutates the tree and emits `structure_changed`, so the shell saves
	// once, in one place, however the change was made.
	node *add_folder(node *parent, const QString &title);
	// A new, empty tab. Until this existed a tab could only arrive from the
	// tree file, a duplicate, a browser mirror or the AI reorganizer -- so the
	// one thing every browser does, opening a new tab, was the one thing this
	// one could not do.
	node *add_tab(node *parent, const QString &title, const QString &url);

	// Create or replace the mirror folder for `source`, holding `tabs`.
	//
	// Replace rather than merge, and that is the design: re-reading a session
	// is not a diff, it is a fresh answer to "what does that browser have open
	// now". Merging would leave tabs the user closed elsewhere sitting in this
	// tree forever, which is the failure mode of every stale mirror.
	//
	// Anything the user dragged *out* of the mirror is untouched by this, since
	// a drag out makes a copy with no `mirror` set -- that copy is theirs.
	// Stop a subtree being another browser's -- what dragging a row out of a
	// mirror into the tree means.
	void clear_mirror(node *n);
	node *replace_mirror(const QString &source, const QString &title,
	                      const QList<node *> &tabs);
	// Every node under `n` is marked as belonging to the same source.
	static void mark_mirror(node *n, const QString &source);
	bool  remove_node(node *n, bool remember = false);
	// Remove several subtrees at once (a multi-selection delete). Descendants
	// of another node in the set are dropped first -- removing the ancestor
	// takes the whole subtree, so removing the descendant afterwards would
	// touch freed memory. Returns how many top-level nodes were removed.
	int   remove_nodes(const QList<node *> &nodes, bool remember = false);
	// The subset of `nodes` with no ancestor also in `nodes`. Pure and static
	// so the safety filter above can be tested without touching the tree.
	static QList<node *> top_level_only(const QList<node *> &nodes);
	// Move a node one place up (delta -1) or down (+1) among its siblings, in
	// canonical order. Returns false at the ends. Reordering is only visible in
	// tree order; the view gates on that, as it does for a drag.
	bool  move_sibling(node *n, int delta);
	// Reopen the most recently deleted subtree (Ctrl+Shift+T). Deletion is
	// otherwise permanent -- `remove_node` does `delete n` and takes the whole
	// subtree -- so a *user* delete (remember=true) stashes a faithful copy,
	// locked pins and all, and this grafts it back where it was. Internal
	// removals (a mirror refresh, a reorganize) pass remember=false and are not
	// reopenable, so Reopen never resurrects churn the user did not do.
	bool  has_closed() const { return !m_closed.isEmpty(); }
	// How many closed tabs are remembered, and the title of one, counted
	// newest-first (0 is the most recently closed). For a Recently Closed list.
	int   closed_count() const { return int(m_closed.size()); }
	QString closed_title(int index) const;
	node *reopen_closed();
	// Reopen a specific remembered tab, newest-first index. reopen_closed() is
	// this at 0.
	node *reopen_closed_at(int index);
	// Edit what a node *is*, as opposed to where it sits. The id is deliberately
	// not editable: it keys the state blob and the outline file, and letting a
	// person retype it would orphan a tab's history with no warning.
	void  update_node(node *n, const QString &title, const QString &url,
	                   const QStringList &tags);

	// A title that came from the page rather than from a person.
	//
	// Refused on a node somebody has named: that is the whole distinction. A
	// tab called "Bank -- statements" should stay called that when it is used,
	// while a tab that has only ever worn the page's own title should follow
	// the page. Returns whether anything changed, so a caller can avoid saving
	// the tree for a title that is already right -- `titleChanged` fires
	// several times during an ordinary page load.
	bool  set_page_title(node *n, const QString &title);
	// A duplicate under the same parent, with an id of its own.
	node *duplicate_node(node *n);
	// Duplicate several nodes at once (a multi-selection). Descendants of
	// another selected node are dropped -- duplicating a folder already copies
	// what is inside it. Returns how many top-level copies were made.
	int   duplicate_nodes(const QList<node *> &nodes);

	// Wrap the selected rows in a new folder, in place. Takes the roots of the
	// selection (a child whose parent is also selected travels inside it) and
	// leaves a pinned row where it is, like a drag. The folder takes the first
	// grouped row's place under its parent. Returns the folder, or null when
	// nothing movable was selected.
	node *group_into_folder(const QList<node *> &nodes, const QString &title);

	// The inverse of grouping: move a folder's children up into its parent,
	// where the folder was, and delete the now-empty folder. Refuses a locked
	// folder or one with a locked child, since a lock means the row does not
	// change parent -- so the folder could not be emptied. Returns success.
	bool dissolve_folder(node *folder);

	// Promote a node to its folder's own level, placed just after that folder.
	// The touch-friendly un-nest, since a phone has no reorder keys and a drag
	// across the tree is awkward. Refuses a locked node (a lock does not
	// reparent) and a node already at the top level. Returns success.
	bool move_out(node *n);

	// Pin or unpin a node (architecture doc sec 5.5). Returns whether anything
	// changed, for the same reason `set_page_title` does: the caller saves the
	// tree on a change and should not write the file for a no-op.
	//
	// The lock is not copied by `duplicate_node` or by a Ctrl-drag. A copy is a
	// new row the user just made, and starting it pinned would mean the gesture
	// produced something they then have to unpin before it will move.
	//
	// `pin_url`, when locking and non-empty, is written to the node as the
	// address it is pinned to. It has to be passed in because **a node's url
	// does not follow the page** -- only its title does -- so a tab opened at
	// one address and browsed to another still records the first, and pinning
	// to that would pin to a page nobody is looking at.
	bool set_locked(node *n, bool locked, const QString &pin_url = QString());

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

	// About to delete this node and everything under it, while the pointers are
	// still valid. The shell needs this: it may be holding a live view for one
	// of them, and the model has no idea that views exist. Emitted *before* the
	// delete, because afterwards there is nothing left to identify.
	void about_to_remove(node *n);

	// A node's id changed, which normally never happens -- an id is stable for
	// the node's lifetime. The exception is a row leaving a mirror: its id was
	// minted in the mirror's own namespace (`firefox-0`) and the next refresh
	// mints that name again, so it cannot be carried into the tree. The shell
	// keys live views, the recently-used list and the state sidecar by id, and
	// none of them can see this happen.
	void id_changed(const QString &was, const QString &now);

private:
	void reindex();  // rebuild m_id_index from the current tree
	// An id nothing in the tree is using. Copies need one: two nodes sharing an
	// id would share a `state/<id>.blob`, so one tab's scroll position and form
	// contents would be restored into the other.
	QString unused_id(const QString &like) const;
	// Re-mint any id in this subtree that a live node already holds, so a
	// reopened tab never collides with one created since it was deleted.
	void remint_if_taken(node *n);

	// Deleted subtrees, newest last, owned here until grafted back by
	// `reopen_closed` or evicted past the cap; the destructor frees the rest.
	struct closed_entry { node *tree = nullptr; QString parent_id; int index = 0; };
	QList<closed_entry> m_closed;
	bool    is_ancestor_of(const node *maybe_ancestor, const node *n) const;

	QString               m_path;
	node                 *m_root = nullptr;
	QHash<QString, node *> m_id_index;
	int m_last_flattened = 0;
	int m_last_unparsed  = 0;
};

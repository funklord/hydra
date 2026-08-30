#pragma once

#include <QUrl>

#include <QStringList>
#include <QPoint>

class QTimer;
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

	// Make `n` the current row: expanded to, highlighted, scrolled into view.
	//
	// Opening a tab and highlighting it are one act from where the user sits,
	// and they were two here -- nothing set the current row when a tab opened,
	// so the highlight stayed wherever it was last clicked. More than cosmetic:
	// the shell reads the highlight to decide where a *new* tab goes, so a tab
	// opened by any route but a click filed the next one under the wrong parent.
	void show_node(node *n);

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

#ifdef Q_OS_ANDROID
	// **The tab menu, on a device with no right button.** Everything done to a
	// tab -- rename, new folder, lock, forget -- arrives through
	// `customContextMenuRequested`, which a finger never raises, so the tree
	// was read-only on a phone.
	//
	// Qt's plugin has `QT_ANDROID_ENABLE_RIGHT_MOUSE_FROM_LONG_PRESS` and it
	// was tried first, on the reasoning that reusing the desktop's path beats
	// a second one. Reported back: the menu came on a *double tap* rather than
	// a hold, and appeared where it was not wanted. So it is done here
	// instead, where it applies to this widget and cannot surprise the page or
	// the address bar.
	//
	// A hold that *moves* is a drag and not a menu, which is why the timer is
	// cancelled on movement rather than only on release -- the tree's
	// drag-and-drop has to keep working, and it is the same gesture up to the
	// point where a finger travels.
	QTimer *m_hold = nullptr;
	QPoint  m_hold_at;
#endif

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
	// Pin or unpin, and it goes to the shell for one reason: a node's url does
	// not follow the page -- only its title does -- so a tab opened at one
	// address and browsed to another still records the first. Locking means
	// "keep *this* page", and only the shell can see which page that is.
	void lock_requested(node *n);
	// One entry from a tab's imported history, opened as a **sub-tab** of the
	// row it belongs to (sec 5.5). The shell, for the same reason as the rest
	// of these: making a tab needs an engine and a stacked widget. A sub-tab
	// rather than a navigation because the record is the point -- sending the
	// row itself back into its own past would rewrite the address the record
	// exists to preserve.
	void history_open_requested(node *parent, const QUrl &url);

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

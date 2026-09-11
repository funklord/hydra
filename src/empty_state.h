//
// The message a list shows when it has nothing in it, drawn *in* the list.
//
// **Where it goes is the whole point.** A label placed under a table reads as a
// footnote about the window; the same words centred in the empty table read as
// an answer to "why is this blank", which is the question actually being asked.
// The downloads and media dialogs each learned this separately and each grew
// its own overlay label, its own resize filter and its own placement function
// -- and the consent dialog, written from the same idea, put its sentence in a
// status label under the table, where it looked like a footnote nobody reads.
//
// So this is the third writing of a thing that had two copies, collapsed rather
// than continued. Attach one to a view and give it words; it handles the rest:
//
//     m_empty = new empty_state(m_list, this);
//     m_empty->set_text("Nothing recorded yet.");
//
// **It follows the model rather than being told.** Showing and hiding by hand
// is what a caller forgets on the one path that empties the list, and then the
// message sits over a table with rows in it. Row count is asked of the model,
// so a QTreeWidget and a QTreeView are the same case.
//
// **The view must already have its model.** A QTreeWidget always does; a plain
// QTreeView gets one from `setModel()`, and one attached before that call
// follows nothing and silently never updates. Stated rather than worked around,
// because the machinery to re-find a model later would be untested code
// guarding against a case no caller here has.
#pragma once

#include <QObject>
#include <QPointer>

class QAbstractItemView;
class QLabel;
class QWidget;

class empty_state : public QObject {
	Q_OBJECT
public:
	// The view is the parent of the overlay, so the label dies with it.
	explicit empty_state(QAbstractItemView *view, QObject *parent = nullptr);
	~empty_state() override;

	// Empty text means "say nothing", which is not the same as an empty list.
	void set_text(const QString &text);
	QString text() const;

	// Re-read the model and reposition. Connected to the model's own signals,
	// so callers rarely need it -- it is public for the case where rows were
	// replaced wholesale and the view was given a different model.
	void refresh();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void follow_model();

	// **`QPointer`, and it is load-bearing.** Both of these outlive nothing:
	// they are deleted by the view's own destructor, and the view emits
	// `modelReset` from inside `~QTreeWidget` *after* `deleteChildren()` has
	// already taken the viewport and the label with it. A raw pointer here
	// segfaulted every time the consent dialog closed -- `refresh()` running on
	// a label that had been freed one frame earlier. `QPointer` nulls itself, so
	// teardown makes `refresh()` a no-op instead of a crash.
	// **And `QPointer<QAbstractItemView>` was not enough, which is the second
	// half of the same lesson.** A `QPointer` clears itself in `~QObject`, and
	// a view dies derived-first: by the time `~QAbstractScrollArea` has run,
	// the QObject part is still alive and the pointer still reads non-null.
	// Every member call in that window -- `viewport()`, `model()` -- is a call
	// on a sub-object that no longer exists, and `QPointer<QAbstractItemView>`
	// makes it worse by static_cast-ing on every access, so merely reading it
	// is undefined.
	//
	// Caught by UBSan: "downcast of address ... which does not point to an
	// object of type QAbstractItemView", from the resize filter during a media
	// dialog teardown. ASan saw nothing, because the object is mid-destruction
	// rather than freed -- the allocation is still perfectly valid.
	//
	// So the handle is a plain QObject, which needs no cast to read, and every
	// use goes through `view()`. `qobject_cast` consults the metaobject, and
	// that IS updated as each destructor runs -- so it returns null exactly
	// once the object has stopped being a view.
	QPointer<QObject>  m_view;
	// Compared against in the filter so the viewport never has to be asked of
	// a view that may be half gone.
	QPointer<QWidget>  m_viewport;
	QPointer<QLabel>   m_label;

	// Null once the object is no longer a view, including mid-destruction.
	QAbstractItemView *view() const;
};

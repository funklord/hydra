// SPDX-License-Identifier: GPL-3.0-or-later
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

class empty_state : public QObject {
	Q_OBJECT
public:
	// The view is the parent of the overlay, so the label dies with it.
	explicit empty_state(QAbstractItemView *view, QObject *parent = nullptr);

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
	QPointer<QAbstractItemView> m_view;
	QPointer<QLabel>            m_label;
};

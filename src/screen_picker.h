#pragma once

#include <QDialog>
#include <QModelIndex>

class QAbstractListModel;
class QListView;
class QPushButton;

// Which screen or window is about to be sent to other people.
//
// **Granting the permission is not the answer to this**, and that is the whole
// reason this is a separate dialog rather than a line in the prompt. "May this
// site share a screen" is a decision about a site and can sensibly be
// remembered; "share *this* window, now" is a decision about this moment and
// must not be. A meeting that was allowed to present last week has not been
// allowed to present whatever happens to be open today.
//
// So the order is: the shield asks whether the site may share at all, and if the
// answer is yes, this asks what -- every time, with nothing remembered.
//
// It is deliberately engine-neutral. Qt hands over two `QAbstractListModel`s and
// takes back an index into one of them, and those are Core types, so this
// dialog can be built and measured against fakes -- which is how it is checked
// at phone geometry, and how it is checked at all without a compositor that
// will hand out a screen capture.
class screen_picker : public QDialog {
	Q_OBJECT
public:
	screen_picker(const QString &host, QAbstractListModel *screens,
	               QAbstractListModel *windows, QWidget *parent = nullptr);

	// True when a screen was chosen rather than a window. Meaningless unless
	// `chosen_row()` is non-negative.
	bool is_screen() const { return m_is_screen; }

	// The row in whichever model `is_screen()` names, or -1 for "nothing" --
	// which is what closing the dialog gives, and is the answer the page gets
	// when somebody changes their mind.
	int  chosen_row() const { return m_row; }

private:
	void refresh_button();

	QListView   *m_screens = nullptr;
	QListView   *m_windows = nullptr;
	QPushButton *m_share   = nullptr;
	bool         m_is_screen = true;
	int          m_row = -1;
};

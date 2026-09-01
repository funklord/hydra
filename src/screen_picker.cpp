#include "screen_picker.h"

#include <QAbstractListModel>
#include <QDialogButtonBox>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QVBoxLayout>

screen_picker::screen_picker(const QString &host, QAbstractListModel *screens,
                              QAbstractListModel *windows, QWidget *parent)
    : QDialog(parent) {
	setWindowTitle("Share a screen");
	setObjectName("screen_picker");
	auto *column = new QVBoxLayout(this);

	auto *asking = new QLabel(this);
	asking->setObjectName("screen_picker_question");
	asking->setWordWrap(true);
	asking->setText(QStringLiteral("Choose what to send to <b>%1</b>.")
	                  .arg(host.isEmpty() ? QStringLiteral("this page") : host));
	column->addWidget(asking);

	// **What is actually at stake, said once.** A shared screen is not the
	// window somebody has in mind; it is everything on that screen for as long
	// as it is shared, including whatever arrives later. People know this in the
	// abstract and forget it while joining a meeting, which is when they are
	// being asked.
	auto *warning = new QLabel(this);
	warning->setObjectName("screen_picker_note");
	warning->setWordWrap(true);
	warning->setText("Everything on what you pick is sent, including anything "
	                  "that appears on it later.");
	column->addWidget(warning);

	auto *screens_head = new QLabel("Screens", this);
	screens_head->setObjectName("screen_picker_screens_head");
	column->addWidget(screens_head);
	m_screens = new QListView(this);
	m_screens->setObjectName("screen_picker_screens");
	m_screens->setModel(screens);
	m_screens->setSelectionMode(QAbstractItemView::SingleSelection);
	// **Elided, not scrolled.** A window title is arbitrarily long -- the test
	// fake carries a realistic one -- and on a 360-pixel screen the default put
	// a horizontal scrollbar under the list, so reading a title meant scrolling
	// sideways to it. Seen in the phone-geometry capture rather than reasoned
	// about. The beginning of a title is the part that identifies a window.
	m_screens->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_screens->setTextElideMode(Qt::ElideRight);
	column->addWidget(m_screens);

	auto *windows_head = new QLabel("Windows", this);
	windows_head->setObjectName("screen_picker_windows_head");
	column->addWidget(windows_head);
	m_windows = new QListView(this);
	m_windows->setObjectName("screen_picker_windows");
	m_windows->setModel(windows);
	m_windows->setSelectionMode(QAbstractItemView::SingleSelection);
	m_windows->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_windows->setTextElideMode(Qt::ElideRight);
	column->addWidget(m_windows);

	auto *buttons = new QDialogButtonBox(this);
	// Cancel is the default, for the reason the permission prompt gives: the
	// button a stray keypress lands on should be the one that costs nothing.
	auto *cancel = buttons->addButton("&Cancel", QDialogButtonBox::RejectRole);
	m_share = buttons->addButton("&Share", QDialogButtonBox::AcceptRole);
	cancel->setObjectName("screen_picker_cancel");
	m_share->setObjectName("screen_picker_share");
	cancel->setDefault(true);
	// **Disabled until something is picked**, rather than sharing a default. A
	// picker whose Share button works before a choice has been made will
	// eventually send the wrong surface to a meeting, and nothing about the
	// dialog would have warned anybody.
	m_share->setEnabled(false);
	column->addWidget(buttons);

	// **The two lists are one choice**, so selecting in either clears the other.
	// Without this both can hold a selection and the dialog has to guess which
	// the person meant -- and a guess here picks a surface to broadcast.
	connect(m_screens->selectionModel(), &QItemSelectionModel::selectionChanged,
	         this, [this] {
		if (!m_screens->selectionModel()->hasSelection())
			return;
		m_windows->clearSelection();
		m_is_screen = true;
		m_row = m_screens->currentIndex().row();
		refresh_button();
	});
	connect(m_windows->selectionModel(), &QItemSelectionModel::selectionChanged,
	         this, [this] {
		if (!m_windows->selectionModel()->hasSelection())
			return;
		m_screens->clearSelection();
		m_is_screen = false;
		m_row = m_windows->currentIndex().row();
		refresh_button();
	});

	connect(m_share, &QPushButton::clicked, this, &QDialog::accept);
	connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
	// **Cleared on every refusal, not on the Cancel button.** Escape and the
	// window's close button reach `reject()` without passing through any button,
	// so hanging this off the button would leave `chosen_row()` holding whatever
	// was highlighted when somebody pressed Escape -- and the caller would have
	// to remember to check the result code instead. Making the row itself the
	// answer means there is only one thing to read and no way to read it wrong.
	connect(this, &QDialog::rejected, this, [this] {
		m_row = -1;
		// The highlight goes with it. Nothing depends on this while a picker is
		// built fresh for each request, which is what the shell does -- but a
		// dialog left showing a selected row beside a live Share button, whose
		// `chosen_row()` is -1, is an object that contradicts itself, and the
		// next person to reuse one would be right to believe the screen.
		m_screens->clearSelection();
		m_windows->clearSelection();
		refresh_button();
	});
}

void screen_picker::refresh_button() {
	m_share->setEnabled(m_row >= 0);
}

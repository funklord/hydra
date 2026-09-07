#include "find_bar.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QToolButton>

// **The one place this string is written.** The label's minimum width is
// derived from it, so a reworded message that left a second copy behind
// would size the floor to a sentence nothing shows.
static const QString k_no_matches = QStringLiteral("No matches");

find_bar::find_bar(QWidget *parent) : QWidget(parent) {
	setObjectName("find_bar");
	auto *row = new QHBoxLayout(this);
	row->setContentsMargins(6, 3, 6, 3);
	row->setSpacing(6);

	row->addWidget(new QLabel("Find:", this));

	m_input = new QLineEdit(this);
	m_input->setObjectName("find_input");
	m_input->setPlaceholderText("Text on this page");
	m_input->setClearButtonEnabled(true);
	row->addWidget(m_input, 1);

	// **Icons from the style, with a word as the text.** The toolbar started
	// with characters and a phone font had no glyph for one of them, so the
	// button drew an empty box; asking the style is both prettier and not a bet
	// on what fonts a platform ships.
	auto *prev = new QToolButton(this);
	prev->setObjectName("find_prev");
	prev->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
	prev->setText("Previous");
	prev->setToolTip("Previous match (Shift+Enter)");
	auto *next = new QToolButton(this);
	next->setObjectName("find_next");
	next->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
	next->setText("Next");
	next->setToolTip("Next match (Enter)");
	row->addWidget(prev);
	row->addWidget(next);

	m_count = new QLabel(this);
	m_count->setObjectName("find_count");
	// **A floor so the buttons beside it do not jump, measured rather than
	// rounded.**
	//
	// The label's width has to be stable, because it changes on every
	// keystroke and everything to its right moves with it. It was 90 with no
	// reason recorded, and 90 is wrong in both directions: the widest thing
	// this label ever shows is "No matches" at 67 px in this font, so it took
	// 23 px from the field somebody types into -- which on a 320-wide phone
	// is a search box of 90 px -- and on a device with a larger font 90 would
	// be too NARROW and elide the same string.
	//
	// Derived from the string, so it is right in whichever direction the font
	// moves. "No matches" is the longest FIXED text; "999 of 9999" is 66 px,
	// just inside it, and a count longer than that grows the label through
	// its own size hint rather than through this floor.
	m_count->setMinimumWidth(
	  m_count->fontMetrics().horizontalAdvance(k_no_matches) + 4);
	row->addWidget(m_count);

	auto *close = new QToolButton(this);
	close->setObjectName("find_close");
	close->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
	close->setText("Close");
	close->setToolTip("Close (Esc)");
	row->addWidget(close);

	// Searching as it is typed, which is what makes it feel like a search
	// rather than a form. Every keystroke is a *fresh* search: the term
	// changed, so the engine starts over rather than advancing.
	connect(m_input, &QLineEdit::textChanged, this, [this](const QString &t) {
		if (t.isEmpty())
			m_count->clear();
		emit search(t, true, true);
	});
	connect(m_input, &QLineEdit::returnPressed, this, [this] {
		// Shift is read here rather than in keyPressEvent because the line
		// edit consumes Return before the bar ever sees it.
		step(!(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier));
	});
	connect(next, &QToolButton::clicked, this, [this] { step(true); });
	connect(prev, &QToolButton::clicked, this, [this] { step(false); });
	connect(close, &QToolButton::clicked, this, [this] { emit dismissed(); });

	hide();
}

void find_bar::begin() {
	show();
	m_input->setFocus();
	m_input->selectAll();
	if (!m_input->text().isEmpty())
		emit search(m_input->text(), true, true);
}

QString find_bar::text() const { return m_input->text(); }

void find_bar::step(bool forward) {
	if (!m_input->text().isEmpty())
		emit search(m_input->text(), forward, false);
}

void find_bar::set_result(int matches, int active) {
	if (m_input->text().isEmpty()) {
		m_count->clear();
		return;
	}
	// **"No matches" is worth saying and "" is not.** A blank label beside a
	// term somebody just typed reads as the search not having run.
	m_count->setText(matches == 0
	                     ? k_no_matches
	                     : QString("%1 of %2").arg(active).arg(matches));
}

void find_bar::clear_result() { m_count->clear(); }

void find_bar::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Escape) {
		emit dismissed();
		return;
	}
	QWidget::keyPressEvent(event);
}

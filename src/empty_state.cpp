// SPDX-License-Identifier: GPL-3.0-or-later
#include "empty_state.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QEvent>
#include <QLabel>

empty_state::empty_state(QAbstractItemView *view, QObject *parent)
    : QObject(parent), m_view(view) {
	if (!m_view)
		return;

	// In the viewport, not the view: the viewport is the area inside the header
	// and the scrollbars, which is the space the message is about. A label
	// parented to the view itself sits over the header.
	m_label = new QLabel(m_view->viewport());
	m_label->setObjectName("empty_state");
	m_label->setAlignment(Qt::AlignCenter);
	m_label->setWordWrap(true);
	// Clicks belong to the view underneath -- an overlay that eats them makes
	// an empty list feel broken rather than empty.
	m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
	// The style's own dimmed text rather than a hand-picked grey, so it stays
	// legible in both colour schemes. A literal colour here was how an earlier
	// version of this ended up as dark grey on dark.
	m_label->setEnabled(false);
	m_label->hide();

	m_view->viewport()->installEventFilter(this);
	follow_model();
	refresh();
}

void empty_state::set_text(const QString &text) {
	if (!m_label)
		return;
	m_label->setText(text);
	refresh();
}

QString empty_state::text() const {
	return m_label ? m_label->text() : QString();
}

void empty_state::follow_model() {
	QAbstractItemModel *model = m_view ? m_view->model() : nullptr;
	if (!model)
		return;
	// Everything that can change how many top-level rows there are. Missing one
	// leaves the message showing over rows, which is worse than not having it.
	connect(model, &QAbstractItemModel::rowsInserted, this, &empty_state::refresh);
	connect(model, &QAbstractItemModel::rowsRemoved,  this, &empty_state::refresh);
	connect(model, &QAbstractItemModel::modelReset,   this, &empty_state::refresh);
	connect(model, &QAbstractItemModel::layoutChanged, this, &empty_state::refresh);
}

void empty_state::refresh() {
	if (!m_label || !m_view)
		return;
	const QAbstractItemModel *model = m_view->model();
	const bool empty = !model || model->rowCount(QModelIndex()) == 0;
	m_label->setVisible(empty && !m_label->text().isEmpty());
	m_label->setGeometry(m_view->viewport()->rect());
	m_label->raise();
}

bool empty_state::eventFilter(QObject *watched, QEvent *event) {
	if (m_view && watched == m_view->viewport() && event->type() == QEvent::Resize)
		refresh();
	return QObject::eventFilter(watched, event);
}

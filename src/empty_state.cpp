#include "empty_state.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QEvent>
#include <QLabel>
#include <QWidget>

empty_state::empty_state(QAbstractItemView *view, QObject *parent)
    : QObject(parent), m_view(view) {
	if (!m_view)
		return;

	// In the viewport, not the view: the viewport is the area inside the header
	// and the scrollbars, which is the space the message is about. A label
	// parented to the view itself sits over the header.
	// The parameter, not `m_view`: this runs with a fully constructed view in
	// hand, and the handle is now a plain QObject precisely so that nothing
	// elsewhere can ask a half-destroyed one for its viewport.
	m_label = new QLabel(view->viewport());
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

	m_viewport = view->viewport();
	m_viewport->installEventFilter(this);
	follow_model();
	refresh();
}

// Removed explicitly rather than left to Qt. Qt does clear filters when either
// object dies, so this is not load-bearing -- but an installer that never
// uninstalls is the shape that made this class unsafe in the first place, and
// the destructor is where a reader looks to check.
empty_state::~empty_state() {
	if (m_viewport)
		m_viewport->removeEventFilter(this);
}

QAbstractItemView *empty_state::view() const {
	// `m_view.data()` is a QObject* and needs no cast; the cast that follows
	// asks the metaobject, which is maintained through destruction. See the
	// header for why the obvious `QPointer<QAbstractItemView>` is not safe.
	return qobject_cast<QAbstractItemView *>(m_view.data());
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
	QAbstractItemView *v = view();
	QAbstractItemModel *model = v ? v->model() : nullptr;
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
	// `view()` rather than the pointer: this runs from `modelReset`, which a
	// QTreeWidget emits from inside its own destructor, and at that point the
	// object has stopped being a view even though it still exists.
	QAbstractItemView *v = view();
	if (!m_label || !v || !m_viewport)
		return;
	const QAbstractItemModel *model = v->model();
	const bool empty = !model || model->rowCount(QModelIndex()) == 0;
	m_label->setVisible(empty && !m_label->text().isEmpty());
	m_label->setGeometry(m_viewport->rect());
	m_label->raise();
}

bool empty_state::eventFilter(QObject *watched, QEvent *event) {
	// Compared against the stored viewport. Asking `m_view->viewport()` here
	// is what UBSan caught: during a dialog teardown the view is part-way
	// through its destructors, and that call lands on a QAbstractScrollArea
	// sub-object that has already been destroyed.
	if (watched == m_viewport && event->type() == QEvent::Resize)
		refresh();
	return QObject::eventFilter(watched, event);
}

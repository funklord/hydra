#include "flow_layout.h"

#include <QStyle>
#include <QWidget>

flow_layout::flow_layout(QWidget *parent, int margin, int h_spacing,
                          int v_spacing)
    : QLayout(parent), m_h_spacing(h_spacing), m_v_spacing(v_spacing) {
	setContentsMargins(margin, margin, margin, margin);
}

flow_layout::~flow_layout() {
	// A layout owns its items; Qt does not delete them for us.
	while (QLayoutItem *item = takeAt(0))
		delete item;
}

int flow_layout::horizontal_spacing() const {
	if (m_h_spacing >= 0)
		return m_h_spacing;
	return spacing_from_style(QSizePolicy::PushButton, true);
}

int flow_layout::vertical_spacing() const {
	if (m_v_spacing >= 0)
		return m_v_spacing;
	return spacing_from_style(QSizePolicy::PushButton, false);
}

// Asks the style rather than picking a number, so the gaps match every other
// row of buttons in the application on whatever desktop it is running on.
int flow_layout::spacing_from_style(QSizePolicy::ControlType type,
                                     bool horizontal) const {
	QObject *p = parent();
	if (!p)
		return -1;
	if (auto *w = qobject_cast<QWidget *>(p))
		return w->style()->layoutSpacing(type, type,
		                                  horizontal ? Qt::Horizontal
		                                             : Qt::Vertical);
	if (auto *l = qobject_cast<QLayout *>(p))
		return l->spacing();
	return -1;
}

void flow_layout::addItem(QLayoutItem *item) { m_items.append(item); }

int flow_layout::count() const { return int(m_items.size()); }

QLayoutItem *flow_layout::itemAt(int index) const {
	return m_items.value(index);
}

QLayoutItem *flow_layout::takeAt(int index) {
	if (index < 0 || index >= m_items.size())
		return nullptr;
	return m_items.takeAt(index);
}

// Nothing: it takes the width it is given and answers with a height. Saying
// otherwise makes a dialog grow to fill space it does not need.
Qt::Orientations flow_layout::expandingDirections() const { return {}; }

bool flow_layout::hasHeightForWidth() const { return true; }

int flow_layout::heightForWidth(int width) const {
	return lay_out(QRect(0, 0, width, 0), false);
}

void flow_layout::setGeometry(const QRect &rect) {
	QLayout::setGeometry(rect);
	lay_out(rect, true);
}

QSize flow_layout::sizeHint() const {
	// The unwrapped width, so a dialog that has room opens as one row -- the
	// shape every one of these had before this layout existed.
	QSize total(0, 0);
	for (const QLayoutItem *item : m_items) {
		const QSize s = item->sizeHint();
		total.setWidth(total.width() + s.width() +
		                (total.width() ? horizontal_spacing() : 0));
		total.setHeight(qMax(total.height(), s.height()));
	}
	const QMargins m = contentsMargins();
	return total + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize flow_layout::minimumSize() const {
	QSize widest(0, 0);
	for (const QLayoutItem *item : m_items)
		widest = widest.expandedTo(item->minimumSize());
	const QMargins m = contentsMargins();
	return widest + QSize(m.left() + m.right(), m.top() + m.bottom());
}

int flow_layout::lay_out(const QRect &rect, bool apply) const {
	const QMargins m = contentsMargins();
	const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
	int x = area.x();
	int y = area.y();
	int line_height = 0;

	for (QLayoutItem *item : m_items) {
		const QSize hint = item->sizeHint();
		int gap_x = horizontal_spacing();
		int gap_y = vertical_spacing();
		// A style that declines to answer (-1) leaves the layout's own spacing,
		// which is always a real number.
		if (gap_x < 0) gap_x = spacing();
		if (gap_y < 0) gap_y = spacing();

		const int next = x + hint.width();
		// Wrap, unless this is the first item on the line -- an item wider than
		// the whole area would otherwise loop forever putting itself on a fresh
		// line and finding it still does not fit.
		if (next - area.x() > area.width() && line_height > 0) {
			x = area.x();
			y = y + line_height + gap_y;
			line_height = 0;
		}
		if (apply)
			item->setGeometry(QRect(QPoint(x, y), hint));
		x = x + hint.width() + gap_x;
		line_height = qMax(line_height, hint.height());
	}
	return y + line_height - rect.y() + m.bottom();
}

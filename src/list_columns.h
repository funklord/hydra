#pragma once

#include <QEvent>
#include <QHeaderView>
#include <QObject>
#include <QTreeView>

// **Every column of a list can be resized by hand**, and one of them takes
// the width the others leave. Asked for by the holder 2026-10-22: lists
// whose columns hold names, titles and rules -- anything of varying length
// -- had that column set to `QHeaderView::Stretch`, which fills the width
// and cannot be dragged, so a long title stayed cut off whatever anybody
// did.
//
// Qt has no mode that is both: a stretched section is not interactive, and
// an interactive one does not fill. So every section is interactive, and
// `fill` is sized to what the others leave whenever the view or a
// neighbour changes width -- until somebody drags `fill` itself, after
// which the width they chose is theirs.
//
// Header-only, because it is a few lines of behaviour with no state of its
// own beyond the view's.
namespace list_columns {

class filler : public QObject {
public:
	filler(QTreeView *view, int fill) : QObject(view), m_view(view), m_fill(fill) {
		QHeaderView *h = view->header();
		h->setStretchLastSection(false);
		h->setSectionResizeMode(QHeaderView::Interactive);
		view->viewport()->installEventFilter(this);
		connect(h, &QHeaderView::sectionResized, this,
		        [this](int section, int, int) {
			if (m_fitting)
				return;
			if (section == m_fill)
				m_chosen = true;   // dragged by hand: left alone from now on
			else
				fit();
		});
	}

	void fit() {
		if (m_chosen)
			return;
		QHeaderView *h = m_view->header();
		int others = 0;
		for (int c = 0; c < h->count(); ++c)
			if (c != m_fill && !h->isSectionHidden(c))
				others += h->sectionSize(c);
		const int want = std::max(h->minimumSectionSize() * 3,
		                          m_view->viewport()->width() - others);
		m_fitting = true;
		h->resizeSection(m_fill, want);
		m_fitting = false;
	}

protected:
	bool eventFilter(QObject *watched, QEvent *e) override {
		if (e->type() == QEvent::Resize)
			fit();
		return QObject::eventFilter(watched, e);
	}

private:
	QTreeView *m_view;
	int        m_fill;
	bool       m_fitting = false;
	bool       m_chosen  = false;
};

// Make `view`'s columns resizable, with `fill` taking what is left.
inline void resizable(QTreeView *view, int fill) {
	new filler(view, fill);
}

}  // namespace list_columns

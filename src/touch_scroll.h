#pragma once

#include <QAbstractScrollArea>
#ifdef Q_OS_ANDROID
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QMouseEvent>
#include <QScroller>
#include <QScrollerProperties>
#endif

// **A finger drags a page of settings the way it drags any page on a
// phone.** Qt Widgets turns a touch into a mouse drag, and a mouse drag over
// a scroll area's contents scrolls nothing: on the emulator a swipe up the
// Settings page left it exactly where it was, so the only way down was the
// scrollbar, a few pixels wide at the edge (2026-11-03).
//
// Mostly for a scroll area holding controls and text to read. An editor is
// left alone, since a drag there selects text. The tab tree uses it too, and
// says at its call why: a drag there moved a tab, which a swipe meant to
// scroll was doing by accident. A tap that does not move still reaches the
// control under it, and a press held still is handed back to the widget
// after the scroller's press delay.
//
// Nothing on a desktop, where a wheel and a scrollbar are the way.
inline void enable_touch_scroll(QAbstractScrollArea *area) {
#ifdef Q_OS_ANDROID
	if (!area)
		return;
	QScroller::grabGesture(area->viewport(), QScroller::LeftMouseButtonGesture);
	// No rubber band past the ends: a Widgets page drawn bouncing reads as a
	// glitch rather than as the platform's overscroll.
	QScrollerProperties p = QScroller::scroller(area->viewport())->scrollerProperties();
	p.setScrollMetric(QScrollerProperties::OvershootDragResistanceFactor, 0.0);
	p.setScrollMetric(QScrollerProperties::OvershootScrollDistanceFactor, 0.0);
	p.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy,
	                  QVariant::fromValue(QScrollerProperties::OvershootAlwaysOff));
	p.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
	                  QVariant::fromValue(QScrollerProperties::OvershootAlwaysOff));
	QScroller::scroller(area->viewport())->setScrollerProperties(p);
#else
	(void)area;
#endif
}

// **Every list and dropdown a finger meets, not only the ones somebody
// remembered.** On a phone a drag in a dialog's list -- downloads, media,
// the AI reviews -- extended a selection rather than scrolling, the same
// fault the Settings pages had, and a tap on a dropdown opened nothing.
// Installed once on the application, it gives each item view the scroller
// when the view is first polished, unless it has one already (the tab tree
// sets its own and says why), and opens each dropdown on release. Editors
// are not item views, so text selection is untouched. Nothing on a desktop.
inline void install_touch_scroll_for_lists() {
#ifdef Q_OS_ANDROID
	struct lists : QObject {
		using QObject::QObject;
		bool eventFilter(QObject *o, QEvent *e) override {
			// **A dropdown opens when the finger lifts, not when it lands.**
			// On the emulator a tap on any combo outside a scroll area --
			// the Settings section picker, the tree's Sort -- opened
			// nothing, quick or held, while combos inside a scroll area
			// (whose scroller re-sends the press itself) opened. Opening
			// on release puts the popup up after the touch has ended
			// (2026-11-03). A press elsewhere in an editable combo goes to
			// its line edit, which is a different object, so typing is
			// untouched.
			if (auto *c = qobject_cast<QComboBox *>(o)) {
				if (e->type() == QEvent::MouseButtonPress &&
				    static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton)
					return true;
				if (e->type() == QEvent::MouseButtonRelease &&
				    static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
					if (c->isEnabled() &&
					    c->rect().contains(static_cast<QMouseEvent *>(e)->position().toPoint()))
						c->showPopup();
					return true;
				}
			}
			if (e->type() == QEvent::Polish) {
				auto *v = qobject_cast<QAbstractItemView *>(o);
				// Not a combo's popup list: it opens on a press and closes
				// on the release, and with a scroller in between the
				// category combo on the Settings page opened to nothing on
				// the emulator (2026-11-03).
				const bool popup = v && v->parentWidget() &&
				                   v->parentWidget()->inherits("QComboBoxPrivateContainer");
				if (v && !popup)
					if (!QScroller::hasScroller(v->viewport())) {
						v->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
						enable_touch_scroll(v);
					}
			}
			return false;
		}
	};
	qApp->installEventFilter(new lists(qApp));
#endif
}

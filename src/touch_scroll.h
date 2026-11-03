#pragma once

#include <QAbstractScrollArea>
#ifdef Q_OS_ANDROID
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

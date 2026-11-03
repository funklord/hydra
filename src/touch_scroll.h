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
// For a scroll area holding controls and text to read, not for item views
// or editors: there a drag already means something -- moving a tab in the
// tree, selecting text -- and a scroller would take it. A tap that does not
// move still reaches the control under it, since the scroller only claims a
// press once it has moved.
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

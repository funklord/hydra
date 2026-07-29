// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// QPointer needs its target types complete wherever this header is included,
// so these are real includes rather than forward declarations.
#include "web_view_backend.h"

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QUrl>
#include <QWidget>
#include <Qt>

class QTimer;
class QGraphicsScene;
class QGraphicsView;
class QGraphicsProxyWidget;

// How pixels get from the page to the screen (architecture doc §8.1).
enum class scale_mode {
	reflow,      // setZoomFactor: the page re-lays out. Robust, and the default.
	none,        // no scaling; oversize geometry + parent clipping crops.
	geometric,   // QGraphicsProxyWidget transform: exact layout, no reflow.
};

// Composes scale and crop the way CSS object-fit does (architecture doc §8.2).
enum class fit_mode { contain, cover, stretch, actual };

struct kiosk_config {
	QUrl  home;                       // idle-reset and watchdog target
	QSize design_size;                // invalid = "whatever the screen is"
	scale_mode scale = scale_mode::reflow;
	fit_mode   fit   = fit_mode::contain;
	Qt::Alignment alignment = Qt::AlignCenter;   // which edges crop under cover
	bool hide_cursor = true;
	int  idle_reset_seconds = 0;      // 0 = off
	bool watchdog = true;             // reload if the render process dies
	bool allow_escape = true;         // false = locked down; Esc will not exit
};

// Kiosk mode: a presentation mode plus a policy preset, not a new engine
// (architecture doc §8).
//
// The view's widget is reparented into a frameless fullscreen stage of our own
// for the duration, which is what makes "strip all chrome" free — the stage has
// none to strip — and gives crop-via-clip somewhere to clip against. On exit the
// widget goes back where it came from.
//
// How scale and fit combine, since not every pair is meaningful:
//
//   reflow    + contain/cover/actual : one zoom factor, viewport fills the
//                                      stage, page reflows. Nothing overflows,
//                                      so alignment does not apply.
//   reflow    + stretch              : not representable — a single zoom factor
//                                      cannot scale axes independently. Falls
//                                      back to cover and says so.
//   none      + any                  : viewport is the design size, positioned
//                                      by alignment, overflow clipped by the
//                                      stage. This is the robust crop path.
//   geometric + any                  : exact per-axis transform, including
//                                      stretch. Historically fragile — see
//                                      §8.3; may render black on some GPUs, so
//                                      it stays opt-in and wants testing on the
//                                      target hardware.
class kiosk_controller : public QObject {
	Q_OBJECT
public:
	explicit kiosk_controller(QObject *parent = nullptr);
	~kiosk_controller() override;

	const kiosk_config &config() const { return m_config; }
	void set_config(const kiosk_config &c);

	bool active() const { return m_stage != nullptr; }
	web_view_backend *view() const { return m_view; }

	// Takes the view's widget for the duration. `restore_to` is where the
	// widget is handed back on exit; nullptr means "just reparent to nothing".
	bool enter(web_view_backend *view, QWidget *restore_to);
	void exit();

signals:
	void entered();
	void left();

protected:
	bool eventFilter(QObject *obj, QEvent *e) override;

private:
	void relayout();
	void reset_idle_timer();
	void set_cursor_hidden(bool hidden);
	void teardown_geometric();

	kiosk_config m_config;
	// QPointer throughout: teardown can be driven by the shell being destroyed
	// while a session is live, in which case the widgets we borrowed may
	// already be gone by the time exit() runs.
	QPointer<web_view_backend> m_view;
	QPointer<QWidget>          m_stage;
	QPointer<QWidget>          m_restore_to;
	QTimer                    *m_idle_timer = nullptr;

	// Geometric-scale path only.
	QGraphicsScene       *m_scene = nullptr;
	QGraphicsView        *m_gview = nullptr;
	QGraphicsProxyWidget *m_proxy = nullptr;
};

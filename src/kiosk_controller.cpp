#include "kiosk_controller.h"
#include "web_view_backend.h"
#include "web_view_factory.h"

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QScreen>
#include <QTimer>
#include <QTransform>
#include <QWidget>

#include <algorithm>

namespace {

// Place `inner` inside `outer` per alignment. Sizes larger than `outer` get
// negative offsets, which is exactly what produces the crop: child widgets are
// clipped to their parent's bounds, so the overflow simply is not drawn, and
// input still maps correctly (architecture doc sec 8.1).
QRect aligned_rect(const QSize &inner, const QSize &outer, Qt::Alignment a) {
	int x = (outer.width() - inner.width()) / 2;
	int y = (outer.height() - inner.height()) / 2;
	if (a & Qt::AlignLeft)    x = 0;
	if (a & Qt::AlignRight)   x = outer.width() - inner.width();
	if (a & Qt::AlignTop)     y = 0;
	if (a & Qt::AlignBottom)  y = outer.height() - inner.height();
	return QRect(QPoint(x, y), inner);
}

}  // namespace

kiosk_controller::kiosk_controller(QObject *parent) : QObject(parent) {
	m_idle_timer = new QTimer(this);
	m_idle_timer->setSingleShot(true);
	connect(m_idle_timer, &QTimer::timeout, this, [this] {
		// The single most-used real kiosk feature: walk back to the home URL
		// after inactivity (sec 8.3).
		//
		// **Walking back used to be all it did.** Pages load into the
		// factory's persistent profile, so the last person's cookies and
		// logins stayed on disk for whoever touched the screen next; the
		// navigation reset and the session did not. Idle is precisely the
		// moment a session ends -- it is the only signal a kiosk gets that
		// somebody has walked away -- so it is where the forgetting belongs.
		//
		// The navigation is issued first and the clear runs beside it. An
		// unattended screen must not sit on a stranger's page while a cookie
		// store empties, and what is being deleted is what was there before
		// this navigation started.
		if (m_view && m_config.home.isValid())
			m_view->load(m_config.home);
		forget_session("idle");
	});
}

kiosk_controller::~kiosk_controller() {
	if (active())
		exit();
}

void kiosk_controller::set_config(const kiosk_config &c) {
	m_config = c;
	if (active()) {
		reset_idle_timer();
		set_cursor_hidden(m_config.hide_cursor);
		relayout();
	}
}

bool kiosk_controller::enter(web_view_backend *view, QWidget *restore_to) {
	if (active() || !view)
		return false;

	m_view       = view;
	m_restore_to = restore_to;

	// A frameless fullscreen top-level of our own. Nothing to strip, and it
	// gives the crop path a container to clip against.
	m_stage = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
	m_stage->setWindowTitle("Hydra — kiosk");
	m_stage->setContextMenuPolicy(Qt::NoContextMenu);
	m_stage->setAutoFillBackground(true);
	m_stage->installEventFilter(this);

	QWidget *w = view->widget();
	w->setParent(m_stage);
	w->setContextMenuPolicy(Qt::NoContextMenu);
	w->show();

	// Scrollbars off; the rest of the page policy is untouched.
	view_settings s;
	s.scrollbars = false;
	view->apply_settings(s);

	if (m_config.watchdog) {
		connect(view, &web_view_backend::render_process_gone, this, [this] {
			// Self-heal: an unattended screen must not stay dead (sec 8.3).
			if (m_view)
				m_view->load(m_config.home.isValid() ? m_config.home : m_view->url());
		});
	}

	if (m_config.home.isEmpty())
		m_config.home = view->url();

	// **Before the screen goes public, not after.** Whoever set the kiosk up
	// was signed in to their own accounts a moment ago, in the same profile
	// the kiosk is about to present, and handing that to the first passer-by
	// is the same leak as leaving the last visitor's session for the next one.
	forget_session("entering");

	m_stage->showFullScreen();
	set_cursor_hidden(m_config.hide_cursor);
	reset_idle_timer();
	relayout();

	emit entered();
	return true;
}

// Empty the shared profile's browsing stores, if this kiosk was configured to.
//
// **The shared profile, cleared, rather than a profile of kiosk's own** -- the
// choice is recorded here because the other one is the obvious idea. A separate
// off-the-record profile would forget by construction and cost nothing to the
// rest of the browser, and it is wrong for what this class is: kiosk borrows a
// view the shell already has, with that tab's history and the tree entry behind
// it (`enter()` takes a `web_view_backend *` and hands it back), and a home of
// "blank = whatever tab you were on" only means anything if it is that tab.
// Giving kiosk its own profile means giving it its own view, which means the
// presented page is not the tab any more -- a different feature with the same
// name, and one that would have to answer for a second profile's lifetime
// against `main()`'s declaration order.
//
// What it costs instead is honesty about the deletion being real and shared,
// which is why `clear_between_sessions` defaults to off and says so on the
// settings page.
void kiosk_controller::forget_session(const char *why) {
	if (!m_config.clear_between_sessions)
		return;
	if (!m_factory) {
		// Configured to forget with nothing able to do it. Loud, because the
		// whole point of the setting is that somebody is relying on it.
		qWarning("kiosk: asked to clear browsing data on %s, but no view "
		          "factory was supplied -- nothing was cleared", why);
		return;
	}

	// Visited links go too: on a public screen the purple-link trail is a
	// readable list of where the last person went, and it is the cheapest of
	// the three to drop.
	web_view_factory::browsing_data what;
	what.cookies       = true;
	what.cache         = true;
	what.visited_links = true;

	const QString when = QString::fromUtf8(why);
	m_factory->clear_browsing_data(what, [when](
	    const web_view_factory::clear_report &r) {
		// Nobody is watching a kiosk, so the report goes to the log. It is
		// still worth making: an operator who suspects the screen is not
		// forgetting has something to read, and a refusal or an unconfirmed
		// store is exactly what they would be looking for.
		qInfo("kiosk: cleared on %s -- cookies removed %d",
		       qUtf8Printable(when), r.cookies_removed);
		for (const QString &note : r.notes)
			qInfo("kiosk: %s", qUtf8Printable(note));
	});
}

void kiosk_controller::exit() {
	if (!active())
		return;

	m_idle_timer->stop();
	set_cursor_hidden(false);

	if (m_view) {
		disconnect(m_view, &web_view_backend::render_process_gone, this, nullptr);
		m_view->set_zoom_factor(1.0);

		view_settings s;   // defaults restore scrollbars
		m_view->apply_settings(s);

		QWidget *w = m_view->widget();
		teardown_geometric();
		if (w) {
			w->setParent(m_restore_to);   // null parent is fine: it just detaches
			w->setContextMenuPolicy(Qt::DefaultContextMenu);
		}
	}

	if (m_stage) {
		m_stage->removeEventFilter(this);
		m_stage->deleteLater();
	}
	m_stage      = nullptr;
	m_view       = nullptr;
	m_restore_to = nullptr;

	// The other end of the same fence. Entering protects the public from the
	// operator's session; leaving protects the operator from the public's, and
	// stops a stranger's cookies sitting on the disk of a machine that has gone
	// back to being somebody's browser.
	forget_session("leaving");

	emit left();
}

void kiosk_controller::teardown_geometric() {
	if (!m_proxy)
		return;
	// Take the widget back off the scene before anything owning it is deleted.
	m_proxy->setWidget(nullptr);
	delete m_gview;   // owns nothing else we need
	delete m_scene;
	m_gview = nullptr;
	m_scene = nullptr;
	m_proxy = nullptr;
}

void kiosk_controller::relayout() {
	if (!active() || !m_view)
		return;

	QWidget *w = m_view->widget();
	const QSize stage = m_stage->size();
	const QSize design = m_config.design_size.isValid() ? m_config.design_size : stage;
	if (design.isEmpty() || stage.isEmpty())
		return;

	const double sx = double(stage.width())  / design.width();
	const double sy = double(stage.height()) / design.height();

	switch (m_config.scale) {
	case scale_mode::reflow: {
		// One factor, viewport fills the stage, the page reflows into it.
		double z = 1.0;
		switch (m_config.fit) {
			case fit_mode::contain: z = std::min(sx, sy); break;
			case fit_mode::cover:   z = std::max(sx, sy); break;
			case fit_mode::actual:  z = 1.0;              break;
			case fit_mode::stretch:
				// A single zoom factor cannot scale axes independently; cover
				// is the closest honest approximation. Use geometric scale if
				// genuine stretch matters.
				z = std::max(sx, sy);
				break;
		}
		m_view->set_zoom_factor(z);
		w->setGeometry(QRect(QPoint(0, 0), stage));
		break;
	}
	case scale_mode::none: {
		// Crop via clip: native size, positioned, overflow clipped by the
		// stage. No transform anywhere, which is why this path is the robust
		// one (sec 8.1).
		m_view->set_zoom_factor(1.0);
		w->setGeometry(aligned_rect(design, stage, m_config.alignment));
		break;
	}
	case scale_mode::geometric: {
		// Render at the design size and transform the pixels. Exact layout, no
		// reflow -- and the historically fragile path (sec 8.3).
		m_view->set_zoom_factor(1.0);
		if (!m_scene) {
			m_scene = new QGraphicsScene;
			m_gview = new QGraphicsView(m_scene, m_stage);
			m_gview->setFrameShape(QFrame::NoFrame);
			m_gview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			m_gview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			m_gview->setContextMenuPolicy(Qt::NoContextMenu);
			// QGraphicsProxyWidget only embeds a top-level widget: with a
			// parent still set, setWidget() refuses and warns, and the scene
			// silently stays empty. Detach first.
			w->setParent(nullptr);
			m_proxy = m_scene->addWidget(w);
			if (!m_proxy) {
				// Embedding failed; fall back rather than present a blank
				// screen (sec 8.3 -- this path is the fragile one).
				w->setParent(m_stage);
				w->show();
				m_config.scale = scale_mode::reflow;
				delete m_gview; delete m_scene;
				m_gview = nullptr; m_scene = nullptr;
				relayout();
				return;
			}
			m_gview->show();
		}
		w->resize(design);
		m_gview->setGeometry(QRect(QPoint(0, 0), stage));

		double tx = sx, ty = sy;
		switch (m_config.fit) {
			case fit_mode::contain: tx = ty = std::min(sx, sy); break;
			case fit_mode::cover:   tx = ty = std::max(sx, sy); break;
			case fit_mode::actual:  tx = ty = 1.0;              break;
			case fit_mode::stretch: break;   // per-axis, as asked
		}
		m_proxy->setTransform(QTransform::fromScale(tx, ty));
		m_scene->setSceneRect(m_proxy->sceneBoundingRect());
		m_gview->setAlignment(m_config.alignment);
		break;
	}
	}
}

void kiosk_controller::reset_idle_timer() {
	if (m_config.idle_reset_seconds > 0)
		m_idle_timer->start(m_config.idle_reset_seconds * 1000);
	else
		m_idle_timer->stop();
}

void kiosk_controller::set_cursor_hidden(bool hidden) {
	if (!m_stage)
		return;
	if (hidden)
		m_stage->setCursor(Qt::BlankCursor);
	else
		m_stage->unsetCursor();
}

bool kiosk_controller::eventFilter(QObject *obj, QEvent *e) {
	if (obj != m_stage)
		return QObject::eventFilter(obj, e);

	switch (e->type()) {
		case QEvent::Resize:
			relayout();
			break;
		case QEvent::KeyPress: {
			// **F11 as well as Esc, because the stage is a window of its own.**
			// F11 is the shell's action and the shell is hidden while this is
			// up, so the key it is bound to reaches nothing at all -- the way
			// in stopped being the way out. Handled here, where the key
			// actually arrives.
			const int k = static_cast<QKeyEvent *>(e)->key();
			const bool leaving = k == Qt::Key_Escape || k == Qt::Key_F11;
			if (m_config.allow_escape && leaving) {
				exit();
				return true;
			}
			reset_idle_timer();
			break;
		}
		case QEvent::MouseMove:
		case QEvent::MouseButtonPress:
			reset_idle_timer();
			break;
		default:
			break;
	}
	return QObject::eventFilter(obj, e);
}

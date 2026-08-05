// SPDX-License-Identifier: GPL-3.0-or-later
#include "qtwebengine_view.h"

#include <QDataStream>
#include <QIODevice>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineHistory>
#include <QWebEngineSettings>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebChannel>
#include <QFile>

namespace {

// One QWebChannel per page, shared by every content script.
//
// Each script used to construct its own over the same `qt.webChannelTransport`.
// A QWebChannel takes ownership of that transport's `onmessage` when it is
// built, so the last one constructed received every reply and the others sat
// waiting for a handshake that had already been answered to someone else. It
// presented as `channel.execCallbacks[message.id] is not a function` in the
// console -- visible for as long as there have been two of these -- and as a
// script whose bridge simply never arrived. Adding a fourth script is what made
// it deterministic rather than a race that usually went the right way.
const char *k_channel_bootstrap = R"JS(
(function () {
  if (window.hydraChannel) return;
  var objects = null, waiting = [];
  window.hydraChannel = function (cb) {
    if (objects) return cb(objects);
    waiting.push(cb);
  };
  var start = function () {
    if (typeof QWebChannel === 'undefined' || !window.qt || !qt.webChannelTransport)
      return setTimeout(start, 50);
    new QWebChannel(qt.webChannelTransport, function (ch) {
      objects = ch.objects;
      var q = waiting; waiting = [];
      q.forEach(function (f) { try { f(objects); } catch (e) {} });
    });
  };
  start();
})();
)JS";

}  // namespace

qtwebengine_view::qtwebengine_view(QWebEngineProfile *profile, QWidget *parent)
  : web_view_backend(nullptr) {
	m_view = new QWebEngineView(parent);
	m_page = new QWebEnginePage(profile, m_view);
	m_view->setPage(m_page);

	// The backend is owned by the widget, not the other way round: the shell
	// removes the widget from its stack and deletes it, and this goes with it.
	setParent(m_view);

	connect(m_view, &QWebEngineView::urlChanged, this, &qtwebengine_view::url_changed);
	// Qt WebEngine has no history-changed signal of its own -- QWebEngineHistory
	// emits nothing -- so it is derived from the two moments history can move:
	// a navigation committing, and a load ending. Answering from the history
	// object each time means no state is mirrored here to fall out of date.
	connect(m_view, &QWebEngineView::urlChanged, this,
	         [this](const QUrl &) { emit history_changed(); });
	connect(m_view, &QWebEngineView::loadFinished, this,
	         [this](bool) { emit history_changed(); });
	// Qt gives the page's own title, and falls back to the url when a document
	// has none -- which is the right label either way, so it is passed on as it
	// comes rather than second-guessed here.
	connect(m_view, &QWebEngineView::titleChanged, this,
	        &qtwebengine_view::title_changed);
	connect(m_view, &QWebEngineView::renderProcessTerminated, this,
	         [this](QWebEnginePage::RenderProcessTerminationStatus, int) {
		emit render_process_gone();
	});

	// Feature permissions (geo/cam/mic/notifications) answered from the decider
	// the shell installs.
	//
	// Two paths, because the API changed in 6.8 and **the old one stopped
	// working**. `featurePermissionRequested` is documented as deprecated but
	// functional; on 6.11 it is not, for geolocation: the signal never arrives,
	// so nothing answers, and the page's `getCurrentPosition` callback simply
	// never fires. That is worse than a refusal — a refused page moves on, a
	// page waiting on a callback that will never come just stops. `try_permissions`
	// passes 11 of 11 against 6.8.2 and fails against 6.11 on exactly the checks
	// that need geolocation, which is how this was found rather than guessed.
	//
	// So: `QWebEnginePermission` where it exists, and the old signal below it for
	// the 6.4 floor the build still claims. Both answer the same decider, so the
	// shell above this file sees no difference.
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	connect(m_page, &QWebEnginePage::permissionRequested, this,
	         [this](QWebEnginePermission perm) {
		using PT = QWebEnginePermission::PermissionType;
		const QUrl origin = perm.origin();
		policy::feature pf;
		switch (perm.permissionType()) {
			case PT::Geolocation:            pf = policy::feature::geolocation;   break;
			case PT::MediaAudioCapture:      pf = policy::feature::microphone;    break;
			case PT::MediaVideoCapture:
			case PT::MediaAudioVideoCapture: pf = policy::feature::camera;        break;
			case PT::Notifications:          pf = policy::feature::notifications; break;
			default:
				// Nothing the policy model covers — clipboard, local fonts,
				// desktop capture, pointer lock. Refused rather than prompted,
				// which is the same answer the old path gave.
				perm.deny();
				return;
		}
		const bool grant = m_decider ? m_decider(origin, pf) : false;
		// What was asked and what was answered, on request. Without this the
		// only observable is what the page ends up seeing, and "we refused" and
		// "we granted and the engine could not deliver" look identical from
		// there — which cost a diagnosis once already.
		//
		// Our feature name, not Qt's enum number. The number was readable until
		// 6.8 renumbered the enum underneath it, at which point a driver matching
		// on it was quietly asking about a different permission than it thought.
		if (qEnvironmentVariableIsSet("HYDRA_PERM_DEBUG"))
			qWarning("permission: %s asked for %s -> %s",
			          qPrintable(origin.toString()),
			          policy::feature_name(pf),
			          grant ? "GRANTED" : "denied");
		if (grant)
			perm.grant();
		else
			perm.deny();
	});
#else
	connect(m_page, &QWebEnginePage::featurePermissionRequested, this,
	         [this](const QUrl &origin, QWebEnginePage::Feature f) {
		policy::feature pf;
		switch (f) {
			case QWebEnginePage::Geolocation:
				pf = policy::feature::geolocation; break;
			case QWebEnginePage::MediaAudioCapture:
				pf = policy::feature::microphone; break;
			case QWebEnginePage::MediaVideoCapture:
			case QWebEnginePage::MediaAudioVideoCapture:
				pf = policy::feature::camera; break;
			case QWebEnginePage::Notifications:
				pf = policy::feature::notifications; break;
			default:
				// Nothing the policy model covers: refuse rather than prompt.
				m_page->setFeaturePermission(origin, f,
				  QWebEnginePage::PermissionDeniedByUser);
				return;
		}
		const bool grant = m_decider ? m_decider(origin, pf) : false;
		// What was asked and what was answered, on request. Without this the
		// only observable is what the page ends up seeing, and "we refused" and
		// "we granted and the engine could not deliver" look identical from
		// there — which cost a diagnosis once already.
		//
		// Our feature name, not Qt's enum number. The number was readable until
		// 6.8 renumbered the enum underneath it, at which point a driver matching
		// on it was quietly asking about a different permission than it thought.
		if (qEnvironmentVariableIsSet("HYDRA_PERM_DEBUG"))
			qWarning("permission: %s asked for %s -> %s",
			          qPrintable(origin.toString()),
			          policy::feature_name(pf),
			          grant ? "GRANTED" : "denied");
		m_page->setFeaturePermission(origin, f,
		  grant ? QWebEnginePage::PermissionGrantedByUser
		        : QWebEnginePage::PermissionDeniedByUser);
	});
#endif
}

QWidget *qtwebengine_view::widget() {
	return m_view;
}

QUrl qtwebengine_view::url() const {
	return m_view->url();
}

void qtwebengine_view::load(const QUrl &url) {
	m_view->setUrl(url);
}

void qtwebengine_view::back() {
	m_view->triggerPageAction(QWebEnginePage::Back);
}

void qtwebengine_view::forward() {
	m_view->triggerPageAction(QWebEnginePage::Forward);
}

void qtwebengine_view::reload() {
	m_view->triggerPageAction(QWebEnginePage::Reload);
}

void qtwebengine_view::apply_settings(const view_settings &s) {
	QWebEngineSettings *set = m_page->settings();
	set->setAttribute(QWebEngineSettings::JavascriptEnabled, s.javascript);
	set->setAttribute(QWebEngineSettings::AutoLoadImages, s.images);
	set->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, !s.autoplay);
	set->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, s.popups);
	set->setAttribute(QWebEngineSettings::ShowScrollBars, s.scrollbars);
}

void qtwebengine_view::set_permission_decider(permission_decider fn) {
	m_decider = std::move(fn);
}


void qtwebengine_view::set_zoom_factor(double factor) {
	m_view->setZoomFactor(factor);
}

QByteArray qtwebengine_view::save_state() const {
	QByteArray blob;
	QDataStream ds(&blob, QIODevice::WriteOnly);
	ds << *m_view->history();
	return blob;
}

bool qtwebengine_view::restore_state(const QByteArray &blob) {
	QByteArray copy = blob;
	QDataStream ds(&copy, QIODevice::ReadOnly);
	ds >> *m_view->history();
	return ds.status() == QDataStream::Ok;
}

void qtwebengine_view::inject_script(const QString &name, const QString &source,
                                      bool subframes) {
	QWebEngineScript s;
	s.setName(name);
	s.setSourceCode(source);
	// Document creation, so the script is in place before the page's own code
	// runs; and an isolated world so the page cannot read or rewrite it (§13.2).
	s.setInjectionPoint(QWebEngineScript::DocumentCreation);
	s.setWorldId(QWebEngineScript::ApplicationWorld);
	// Off by default: credentials and picked elements must not be reachable from
	// a third-party iframe. Where it is on, the script has to survive without a
	// bridge — Qt installs `qt.webChannelTransport` in the main frame alone, so
	// a content script in a subframe has nothing to connect a QWebChannel to and
	// must talk to the top frame instead. Measured; see project.md.
	s.setRunsOnSubFrames(subframes);
	m_page->scripts().insert(s);
}

void qtwebengine_view::inject_main_world_script(const QString &name,
                                                 const QString &source) {
	QWebEngineScript s;
	s.setName(name);
	s.setSourceCode(source);
	s.setInjectionPoint(QWebEngineScript::DocumentCreation);
	// MainWorld and every frame — see the comment on the seam. Both are
	// required rather than convenient: an isolated world cannot wrap the page's
	// MediaSource, and on real sites the player lives in an iframe.
	s.setWorldId(QWebEngineScript::MainWorld);
	s.setRunsOnSubFrames(true);
	m_page->scripts().insert(s);
}

QString qtwebengine_view::page_title() const {
	return m_view ? m_view->title() : QString();
}

void qtwebengine_view::set_script_bridge(QObject *object, const QString &name) {
	if (!object) {
		m_page->setWebChannel(nullptr);
		return;
	}
	if (!m_channel)
		m_channel = new QWebChannel(this);
	m_channel->registerObject(name, object);
	// Same world the content script runs in, or the two cannot see each other.
	m_page->setWebChannel(m_channel, QWebEngineScript::ApplicationWorld);

	// qwebchannel.js ships with Qt as a resource; the page-side script needs it
	// before it can construct a QWebChannel. Inject it exactly once — more than
	// one bridge object registers here, and a second copy of the transport
	// would run the whole setup twice in the same page.
	if (!m_channel_api_injected) {
		QFile api(":/qtwebchannel/qwebchannel.js");
		if (api.open(QIODevice::ReadOnly)) {
			inject_script("qwebchannel", QString::fromUtf8(api.readAll()));
			inject_script("hydra-channel", QString::fromUtf8(k_channel_bootstrap));
			m_channel_api_injected = true;
		}
	}
}

bool qtwebengine_view::can_go_back() const {
	return m_view && m_view->history()->canGoBack();
}

bool qtwebengine_view::can_go_forward() const {
	return m_view && m_view->history()->canGoForward();
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "qtwebengine_view.h"

#include <QDataStream>
#include <QIODevice>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineFindTextResult>
#include <QWebEngineFullScreenRequest>
#include <QAuthenticator>
#include <QWebEngineCertificateError>
#include <QWebEngineClientCertificateSelection>
#include <QSslCertificate>
#include <QWebEngineHistory>
#include <QWebEngineNewWindowRequest>
#include <QWebEngineSettings>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebChannel>
#include <QFile>
#include <QPrintDialog>
#include <QPrinter>

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

// The one place a navigation can be refused before it happens.
//
// `acceptNavigationRequest` is a virtual on the page rather than a signal, so
// asking the question at all costs a subclass. It is worth one: the alternative
// is watching `urlChanged` and sending the view back afterwards, which loads
// the page you did not want, shows it, and then bounces -- visible, slower, and
// it puts an entry in the history for somewhere the tab was never meant to go.
class navigating_page : public QWebEnginePage {
public:
	navigating_page(QWebEngineProfile *profile, QObject *parent)
	  : QWebEnginePage(profile, parent) {}

	web_view_backend::navigation_decider decider;

protected:
	bool acceptNavigationRequest(const QUrl &url, NavigationType type,
	                              bool is_main_frame) override {
		if (decider) {
			// What counts as the user going somewhere. A typed address, a
			// clicked link and a submitted form are a person; a redirect and
			// whatever Qt calls Other are the page. Back and forward are a
			// person too, but of a navigation that already happened -- the
			// shell refuses those for a locked tab rather than spawning, and
			// greys the buttons so it does not come up.
			const bool user = type == NavigationTypeLinkClicked
			               || type == NavigationTypeTyped
			               || type == NavigationTypeFormSubmitted;
			if (!decider(url, is_main_frame, user))
				return false;
		}
		return QWebEnginePage::acceptNavigationRequest(url, type, is_main_frame);
	}
};

qtwebengine_view::qtwebengine_view(QWebEngineProfile *profile, QWidget *parent)
  : web_view_backend(nullptr) {
	m_view = new QWebEngineView(parent);
	navigating_page *page = new navigating_page(profile, m_view);
	m_nav_page = page;
	m_page = page;
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
	// Left empty when nobody answers, which is what Qt treats as a refusal --
	// the same outcome as before this existed, reached deliberately.
	connect(m_page, &QWebEnginePage::authenticationRequired, this,
	         [this](const QUrl &url, QAuthenticator *auth) {
		if (!m_authenticator || !auth)
			return;
		QString user, password;
		if (m_authenticator(url, auth->realm(), &user, &password)) {
			auth->setUser(user);
			auth->setPassword(password);
		}
	});

	// The proxy asking, which is a different question and gets a different
	// callback -- see the seam's note on why the two must not share one.
	connect(m_page, &QWebEnginePage::proxyAuthenticationRequired, this,
	         [this](const QUrl &, QAuthenticator *auth, const QString &host) {
		if (!m_proxy_authenticator || !auth)
			return;
		QString user, password;
		if (m_proxy_authenticator(host, auth->realm(), &user, &password)) {
			auth->setUser(user);
			auth->setPassword(password);
		}
	});

	// A site asking who you are, by certificate.
	//
	// **Answered while the signal runs.** Qt hands over a selection object that
	// the engine is waiting on; storing it and answering later answers a
	// request that has already been abandoned. Unconnected, Qt selects none --
	// so the outcome without a chooser was already "send nothing", and this
	// makes that a decision instead of the only thing available.
	//
	// The certificates are flattened to plain strings before they cross the
	// seam, because the shell must not learn what kind of engine produced
	// them (sec 19.2).
	connect(m_page, &QWebEnginePage::selectClientCertificate, this,
	         [this](QWebEngineClientCertificateSelection selection) {
		const QList<QSslCertificate> certs = selection.certificates();
		if (!m_certificate_chooser || certs.isEmpty()) {
			selection.selectNone();
			return;
		}
		QList<certificate_offer> offered;
		offered.reserve(certs.size());
		for (const QSslCertificate &c : certs) {
			certificate_offer o;
			o.subject     = c.subjectDisplayName();
			o.issuer      = c.issuerDisplayName();
			o.valid_until = c.expiryDate().toString(Qt::ISODate);
			o.serial      = QString::fromLatin1(c.serialNumber());
			offered << o;
		}
		const int pick = m_certificate_chooser(m_view->url(), offered);
		if (pick >= 0 && pick < certs.size())
			selection.select(certs.at(pick));
		else
			selection.selectNone();
	});

	// **Rejected, and said out loud.** Qt's default for an unhandled signal is
	// to reject, which is the right answer -- so this changes nothing about
	// what loads, only about what the person is told. Deliberately no
	// click-through: letting somebody past a certificate error is a security
	// decision this browser has not made, and adding one in passing would be
	// making it.
	connect(m_page, &QWebEnginePage::certificateError, this,
	         [this](QWebEngineCertificateError error) {
		error.rejectCertificate();
		emit certificate_rejected(error.url(), error.description());
	});

	// The request is answered by the shell rather than here: it becomes a tab
	// in the tree, which is this browser's whole answer to "another window".
	//
	// **This used to say `openIn` was deliberately not called**, on the
	// grounds that taking the url and opening it ourselves drops the opener
	// relationship "which a page can otherwise use to reach back into the
	// window that spawned it". That reasoning is sound and the protection is
	// real -- it is the tabnabbing vector -- but the cost was not stated and
	// turned out to be the whole of one class of site: a popup with no opener
	// cannot answer the page that opened it, so every OAuth sign-in flow
	// renders a blank window and nothing completes. Measured on a real one.
	//
	// So it is called now, and **only where the person asked for the window**.
	// A script-initiated popup that policy lets through still gets the old
	// treatment -- the url copied into a fresh page, no opener, nothing to
	// reach back through -- because that is where a window nobody asked for
	// comes from. The shell draws that line: it fills the adopting view in
	// only on the user-initiated branch, and leaves it null otherwise.
	// **Accepted here, presented by the shell.** The request must be answered
	// synchronously -- Chromium is waiting on it and a reply that arrives later
	// answers a request already abandoned, the same shape as the permission and
	// authentication deciders above. So accept it, then tell the shell which
	// way it went and let it decide what fullscreen looks like.
	//
	// Accepting is not the same as presenting: if the shell does nothing, the
	// page believes it is fullscreen and lays itself out for a viewport that
	// never changed. That is the shell's half of the contract.
	connect(m_page, &QWebEnginePage::fullScreenRequested, this,
	         [this](QWebEngineFullScreenRequest request) {
		const bool on = request.toggleOn();
		request.accept();
		emit fullscreen_requested(on);
	});

	connect(m_page, &QWebEnginePage::newWindowRequested, this,
	         [this](QWebEngineNewWindowRequest &request) {
		// **`openIn`, not "load the url somewhere else".** Handing the request
		// to a page is what makes Chromium wire the opener, and the opener is
		// what a popup exists to talk to. Reading `requestedUrl()` and loading
		// it into a fresh page looks identical in a screenshot and is a
		// different thing: `window.opener` is null, so a sign-in popup has
		// nowhere to post its answer and sits there blank.
		//
		// The receiver fills this in during the emit, because `request` is
		// only valid until this lambda returns.
		web_view_backend *adopt = nullptr;
		emit new_window_requested(request.requestedUrl(),
		                           request.isUserInitiated(), &adopt);
		if (auto *view = qobject_cast<qtwebengine_view *>(adopt))
			request.openIn(view->m_page);
	});

	connect(m_page, &QWebEnginePage::windowCloseRequested, this,
	         [this] { emit close_requested(); });

	// One line, because the page already knows: Qt reports the link under the
	// pointer and an empty string when the pointer leaves it.
	connect(m_page, &QWebEnginePage::linkHovered, this,
	         [this](const QString &url) { emit link_hovered(QUrl(url)); });
	connect(m_view, &QWebEngineView::loadStarted, this,
	         [this] { emit load_progress(0); });
	connect(m_view, &QWebEngineView::loadProgress, this,
	         [this](int p) { emit load_progress(p); });
	connect(m_view, &QWebEngineView::loadFinished, this,
	         [this](bool ok) { emit load_finished(ok); });
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
	// never fires. That is worse than a refusal -- a refused page moves on, a
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
				// Nothing the policy model covers -- clipboard, local fonts,
				// desktop capture, pointer lock. Refused rather than prompted,
				// which is the same answer the old path gave.
				perm.deny();
				return;
		}
		const bool grant = m_decider ? m_decider(origin, pf) : false;
		// What was asked and what was answered, on request. Without this the
		// only observable is what the page ends up seeing, and "we refused" and
		// "we granted and the engine could not deliver" look identical from
		// there -- which cost a diagnosis once already.
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
		// there -- which cost a diagnosis once already.
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
	// **Not a per-page policy, so it is set here rather than plumbed through
	// view_settings.** Qt defaults this off, and with it off the engine refuses
	// `requestFullscreen()` before the shell is ever asked -- which is why a
	// site's own fullscreen button did nothing at all, on every page, with no
	// error anywhere. Whether the shell then *grants* a request is the shell's
	// decision and is made on the signal below; this only makes the request
	// reach it.
	set->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
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
	// runs; and an isolated world so the page cannot read or rewrite it (sec 13.2).
	s.setInjectionPoint(QWebEngineScript::DocumentCreation);
	s.setWorldId(QWebEngineScript::ApplicationWorld);
	// Off by default: credentials and picked elements must not be reachable from
	// a third-party iframe. Where it is on, the script has to survive without a
	// bridge -- Qt installs `qt.webChannelTransport` in the main frame alone, so
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
	// MainWorld and every frame -- see the comment on the seam. Both are
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
	// before it can construct a QWebChannel. Inject it exactly once -- more than
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

// `fresh` is not passed to Qt, and does not need to be: `findText` restarts
// when the term differs from the last one and advances when it matches, which
// is the same distinction arrived at from the other side.
void qtwebengine_view::find_text(const QString &text, bool forward, bool fresh) {
	Q_UNUSED(fresh)
	if (!m_view)
		return;
	QWebEnginePage::FindFlags flags;
	if (!forward)
		flags |= QWebEnginePage::FindBackward;
	m_view->findText(text, flags, [this](const QWebEngineFindTextResult &r) {
		emit find_result(r.numberOfMatches(), r.activeMatch());
	});
}

double qtwebengine_view::zoom_factor() const {
	return m_view ? m_view->zoomFactor() : 1.0;
}

void qtwebengine_view::stop() {
	if (m_view)
		m_view->stop();
}

// Tell the engine the page is no longer fullscreen. `ExitFullScreen` is the
// action Chromium fires for its own Esc handling, so the page sees exactly what
// it would have seen had the user pressed Esc inside it.
void qtwebengine_view::exit_fullscreen() {
	if (m_page)
		m_page->triggerAction(QWebEnginePage::ExitFullScreen);
}

bool qtwebengine_view::can_print() const {
	return m_view != nullptr;
}

// **The dialog is modal and the printing is not**, which is the trap here.
// `QWebEngineView::print` hands the job to the render process and returns
// immediately, so the QPrinter has to outlive the call: a stack one is
// destroyed while a spool is still reading it. It is heap-allocated and freed
// when `printFinished` says the job is over.
//
// **Freed by hand, because a QPrinter is not a QObject.** It derives from
// QPagedPaintDevice, so there is no `deleteLater` and no parent to own it --
// which the first version of this assumed both of, and the compiler said so.
// Deleting inside the handler is safe rather than merely convenient: the
// signal is what marks the engine finished with it.
//
// `Qt::SingleShotConnection` disconnects after the first emission, so a second
// print cannot re-enter this lambda and free a printer the first job is still
// using. Doing it by hand wanted a heap-allocated QMetaObject::Connection to
// disconnect itself from inside its own callback, which is a lifetime puzzle
// in exchange for nothing.
//
// The dialog runs first and synchronously, because a cancelled dialog must not
// start a job at all -- and cancelling is reported as a finish that produced
// nothing, since from the shell's side "you did not print" is one outcome
// however it was reached.
void qtwebengine_view::print() {
	if (!m_view)
		return;

	auto *printer = new QPrinter(QPrinter::HighResolution);
	QPrintDialog dialog(printer, m_view);
	dialog.setWindowTitle("Print Page");
	if (dialog.exec() != QDialog::Accepted) {
		delete printer;
		emit print_finished(false);
		return;
	}

	connect(m_view, &QWebEngineView::printFinished, this,
	         [this, printer](bool ok) {
		delete printer;
		emit print_finished(ok);
	}, Qt::SingleShotConnection);
	m_view->print(printer);
}

void qtwebengine_view::set_authenticator(authenticator fn) {
	m_authenticator = std::move(fn);
}

void qtwebengine_view::set_navigation_decider(navigation_decider fn) {
	if (m_nav_page)
		m_nav_page->decider = std::move(fn);
}

void qtwebengine_view::set_proxy_authenticator(proxy_authenticator fn) {
	m_proxy_authenticator = std::move(fn);
}

void qtwebengine_view::set_certificate_chooser(certificate_chooser fn) {
	m_certificate_chooser = std::move(fn);
}

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
#include <QWebEngineFileSystemAccessRequest>
#include <QWebEngineRegisterProtocolHandlerRequest>
#include <QMessageBox>
#include <QMetaEnum>
#include <QPushButton>
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

// --- the engine's passkey vocabulary, translated to this project's ----------
//
// Three switches that look like busywork and are the seam (architecture doc
// sec 19.2): `webauth_dialog` compiles on Android, where Qt WebEngine does not
// exist, so it cannot name a single one of these types. Each arm is written out
// rather than cast from the underlying value, because the two enumerations are
// only accidentally in the same order and a `static_cast` between them would go
// on compiling after Qt inserted a member.
//
// Every one of them ends with a fallback rather than relying on the switch
// being exhaustive: Qt's failure list has grown between releases, and a value
// this build has never seen has to arrive as `unknown` -- which the dialog
// turns into a sentence -- rather than as an uninitialised enum.

webauth_dialog::pin_reason to_pin_reason(
        QWebEngineWebAuthUxRequest::PinEntryReason reason) {
	using PR = QWebEngineWebAuthUxRequest::PinEntryReason;
	switch (reason) {
		case PR::Set:       return webauth_dialog::pin_reason::set;
		case PR::Change:    return webauth_dialog::pin_reason::change;
		case PR::Challenge: break;
	}
	return webauth_dialog::pin_reason::challenge;
}

webauth_dialog::pin_error to_pin_error(
        QWebEngineWebAuthUxRequest::PinEntryError error) {
	using PE = QWebEngineWebAuthUxRequest::PinEntryError;
	switch (error) {
		case PE::InternalUvLocked:   return webauth_dialog::pin_error::uv_locked;
		case PE::WrongPin:           return webauth_dialog::pin_error::wrong_pin;
		case PE::TooShort:           return webauth_dialog::pin_error::too_short;
		case PE::InvalidCharacters:
			return webauth_dialog::pin_error::invalid_characters;
		case PE::SameAsCurrentPin:
			return webauth_dialog::pin_error::same_as_current;
		case PE::NoError:            break;
	}
	return webauth_dialog::pin_error::none;
}

webauth_dialog::failure to_failure(
        QWebEngineWebAuthUxRequest::RequestFailureReason reason) {
	using FR = QWebEngineWebAuthUxRequest::RequestFailureReason;
	switch (reason) {
		case FR::Timeout:
			return webauth_dialog::failure::timeout;
		case FR::KeyNotRegistered:
			return webauth_dialog::failure::key_not_registered;
		case FR::KeyAlreadyRegistered:
			return webauth_dialog::failure::key_already_registered;
		case FR::SoftPinBlock:
			return webauth_dialog::failure::soft_pin_block;
		case FR::HardPinBlock:
			return webauth_dialog::failure::hard_pin_block;
		case FR::AuthenticatorRemovedDuringPinEntry:
			return webauth_dialog::failure::removed_during_pin_entry;
		case FR::AuthenticatorMissingResidentKeys:
			return webauth_dialog::failure::no_resident_keys;
		case FR::AuthenticatorMissingUserVerification:
			return webauth_dialog::failure::no_user_verification;
		case FR::AuthenticatorMissingLargeBlob:
			return webauth_dialog::failure::no_large_blob;
		case FR::NoCommonAlgorithms:
			return webauth_dialog::failure::no_common_algorithms;
		case FR::StorageFull:
			return webauth_dialog::failure::storage_full;
		case FR::UserConsentDenied:
			return webauth_dialog::failure::consent_denied;
		case FR::WinUserCancelled:
			return webauth_dialog::failure::cancelled_by_system;
	}
	return webauth_dialog::failure::unknown;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
// Qt's own spelling of a permission, for the ones this project's policy model
// has no word for.
//
// **The enum's key, never its number.** 6.8 renumbered this enumeration, and a
// log line carrying the number was quietly naming a different permission than
// the reader thought -- which the granted/denied line below already records
// paying for once. `valueToKey` reads the name out of the meta-object, so it
// stays right through a renumbering and answers for a member added after this
// was written.
const char *permission_type_name(QWebEnginePermission::PermissionType type) {
	const QMetaEnum spelled =
	  QMetaEnum::fromType<QWebEnginePermission::PermissionType>();
	const char *key = spelled.valueToKey(static_cast<int>(type));
	return key ? key : "an unnamed permission";
}
#endif

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

	// **A page asking to print itself**, which is `window.print()` and every
	// Print button a site draws in its own chrome. Nothing was connected, and
	// an unconnected `printRequested` is not an error anywhere -- the click
	// simply did nothing, which reads as a broken page rather than a browser
	// that never listened. File > Print worked the whole time, so the gap was
	// invisible to anyone who tested printing the way the menu offers it.
	//
	// Straight to the same `print()` the menu calls, so a page-initiated print
	// gets the same dialog, the same printer and the same status-bar answer.
	// The page does not get to choose a printer or skip the dialog: what it
	// asked for is a print, and which paper it comes out on is the user's.
	connect(m_page, &QWebEnginePage::printRequested, this,
	         [this] { print(); });

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
			case PT::ClipboardReadWrite:     pf = policy::feature::clipboard_read; break;
			case PT::MouseLock:              pf = policy::feature::pointer_lock;   break;
			// **Nothing in `policy::feature` covers these, so they are refused
			// without the shield ever being asked** -- clipboard reading,
			// pointer lock, desktop capture, the local font list. That was
			// already true and is now written down: the arms are named
			// individually rather than swept into `default`, so a permission
			// the engine adds later shows up as a compiler warning about an
			// unhandled value instead of silently joining the refused pile.
			//
			// Clipboard reading and pointer lock have since been given the
			// features they were missing, and are answered above with the
			// rest. They earned them on the distinction `policy.h` draws
			// against `extractor_dom`: that one stays absent because the
			// *power* does not exist, so a permission for it would promise
			// something nobody delivers, while these two are real capabilities
			// the engine asks about and only had nowhere to record an answer.
			// Both default to block, so the behaviour is what it always was.
			//
			// What remains below genuinely has no home. Desktop capture and
			// the local font list are refused, and the refusal is at least
			// visible now: the debug line is the same one the answered
			// permissions get, and without it "we refused" and "we granted and
			// the engine could not deliver" looked identical from the page's
			// side, which cost a diagnosis once already.
			case PT::DesktopVideoCapture:
			case PT::DesktopAudioVideoCapture:
			case PT::LocalFontsAccess:
			case PT::Unsupported:
			default:
				if (qEnvironmentVariableIsSet("HYDRA_PERM_DEBUG"))
					qWarning("permission: %s asked for %s -> denied, "
					          "no policy feature covers it",
					          qPrintable(origin.toString()),
					          permission_type_name(perm.permissionType()));
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
				// Nothing the policy model covers -- pointer lock and desktop
				// capture, this enumeration having no clipboard member at all.
				// Refused rather than prompted, for the reasons written out
				// against the 6.8 branch above.
				//
				// **No debug line here, deliberately.** The branch cannot be
				// compiled on the Qt this tree builds against, so anything
				// added to it would ship unverified; the reasoning lives once,
				// where it can be built and read.
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

	// **A page offering to handle a scheme itself** -- `registerProtocolHandler`,
	// which is how a webmail asks to take every `mailto:` link on the machine.
	// Nothing was connected, and Qt's answer to an unconnected one is to reject
	// it and say nothing: the site's "make this your mail handler" button did
	// nothing whatsoever, which is the same silent-nothing this seam already
	// records for fullscreen, window and print requests.
	//
	// **Answered while the signal runs**, like the certificate selection above.
	// The request holds a shared pointer to its controller and would survive
	// being stored, but the engine is waiting on it and the page will have moved
	// on; a modal question inside the slot is what Qt's own simplebrowser does.
	//
	// **Refusing stays the default**, and is what escape and the window's close
	// button answer. A scheme handler is a standing grant covering a whole class
	// of link, given once and used afterwards without asking.
	connect(m_page, &QWebEnginePage::registerProtocolHandlerRequested, this,
	         [this](QWebEngineRegisterProtocolHandlerRequest request) {
		// A page can ask in a loop, and each question runs a nested event loop
		// that the page keeps running underneath. One at a time, or a site can
		// stack modal windows over a window nobody can reach.
		if (m_prompting || !m_view) {
			request.reject();
			return;
		}
		const QString host = request.origin().host();
		QMessageBox box(m_view);
		box.setWindowTitle("Send links to this site?");
		box.setObjectName("protocol_handler_box");
		box.setIcon(QMessageBox::Question);
		box.setTextFormat(Qt::RichText);
		box.setText(QString("<b>%1</b> wants to open every <b>%2:</b> link.")
		                .arg(host.isEmpty() ? QStringLiteral("This site")
		                                     : host.toHtmlEscaped(),
		                      request.scheme().toHtmlEscaped()));
		box.setInformativeText("Allowing this means links of that kind open at "
		                        "the site instead of in whatever opens them now.");
		QPushButton *allow = box.addButton("&Allow", QMessageBox::AcceptRole);
		QPushButton *refuse = box.addButton("&Not now", QMessageBox::RejectRole);
		box.setDefaultButton(refuse);
		box.setEscapeButton(refuse);
		m_prompting = true;
		box.exec();
		m_prompting = false;
		if (box.clickedButton() == allow)
			request.accept();
		else
			request.reject();
	});

	// **A page asking to read or write a real file or folder** -- the File
	// System Access API, which is what an in-browser editor uses to open a file
	// from the disk and save back over it. Unconnected, Qt refuses, silently:
	// `showOpenFilePicker()` rejects its promise and the site reports whatever
	// it reports, with nothing from the browser to say a decision was made.
	//
	// Answered while the signal runs, for the same reason as the request above.
	connect(m_page, &QWebEnginePage::fileSystemAccessRequested, this,
	         [this](QWebEngineFileSystemAccessRequest request) {
		if (m_prompting || !m_view) {
			request.reject();
			return;
		}
		using fsa = QWebEngineFileSystemAccessRequest;
		const fsa::AccessFlags flags = request.accessFlags();
		const bool may_read  = flags.testFlag(fsa::Read);
		const bool may_write = flags.testFlag(fsa::Write);
		// **No `Q_UNREACHABLE` on a combination this build does not know**,
		// which is what Qt's example does here: that turns an engine which
		// grows a third flag into a browser that aborts. A word that says less
		// is a better answer than a crash, and the path is still named.
		QString doing = QStringLiteral("use");
		if (may_read && may_write)
			doing = QStringLiteral("read and change");
		else if (may_write)
			doing = QStringLiteral("change");
		else if (may_read)
			doing = QStringLiteral("read");

		const bool folder = request.handleType() == fsa::Directory;
		const QUrl where = request.filePath();
		// A local path where there is one. `toLocalFile` is empty for anything
		// that is not a `file:` url, and an empty line under a question about
		// which file is the one thing this box must not show.
		QString shown = where.toLocalFile();
		if (shown.isEmpty())
			shown = where.toString();

		const QString host = request.origin().host();
		QMessageBox box(m_view);
		box.setWindowTitle("Give this site access to your files?");
		box.setObjectName("file_access_box");
		box.setIcon(QMessageBox::Question);
		box.setTextFormat(Qt::RichText);
		box.setText(QString("<b>%1</b> wants to %2 a %3 on this computer.")
		                .arg(host.isEmpty() ? QStringLiteral("This site")
		                                     : host.toHtmlEscaped(),
		                      doing, folder ? QStringLiteral("folder")
		                                     : QStringLiteral("file")));
		// **The path is escaped and the break is a tag**, because the box is in
		// rich text mode: a file called `<b>` would otherwise be shown as
		// something other than its own name, in the one place the name is the
		// whole decision.
		QString detail = shown.toHtmlEscaped();
		if (folder)
			detail += "<br><br>A folder is granted whole: everything in it now, "
			           "and everything put there afterwards.";
		box.setInformativeText(detail);
		QPushButton *allow = box.addButton("&Allow", QMessageBox::AcceptRole);
		QPushButton *refuse = box.addButton("&Don't allow",
		                                     QMessageBox::RejectRole);
		box.setDefaultButton(refuse);
		box.setEscapeButton(refuse);
		m_prompting = true;
		box.exec();
		m_prompting = false;
		if (box.clickedButton() == allow)
			request.accept();
		else
			request.reject();
	});

	// **Passkeys.** See `webauth_dialog.h` for what was missing and what it
	// cost; this is the half that speaks to the engine.
	//
	// **The one request here that is not answered while the signal runs**, and
	// the exception is the API's rather than a choice: Qt hands over a QObject
	// with a state machine on it, and the conversation takes as long as a person
	// takes to find a security key. So the object is kept, and every reply is
	// guarded by a QPointer -- the engine owns it and may destroy it the moment
	// the request ends, which can happen while the window is still up.
	connect(m_page, &QWebEnginePage::webAuthUxRequested, this,
	         [this](QWebEngineWebAuthUxRequest *request) {
		if (!request || !m_view)
			return;
		// A second request arriving while one is outstanding. The engine should
		// only do that once the first has finished -- but "should" is doing the
		// work in that sentence, and an outstanding request nobody withdraws
		// holds the authenticator until it times out. So the old one is
		// cancelled by name rather than merely forgotten.
		if (m_webauth_request && m_webauth_request != request)
			m_webauth_request->cancel();
		drop_webauth_dialog();
		m_webauth_request = request;

		// **Parented to the view, not to its window.** Closing a tab deletes
		// the view and everything under it, this backend included; a dialog
		// parented to the top level would outlive both and go on calling into a
		// deleted object. A QDialog is a window whoever its parent is, so
		// parenting it low costs nothing on screen.
		auto *dialog = new webauth_dialog(request->relyingPartyId(), m_view);
		m_webauth_dialog = dialog;
		connect(dialog, &webauth_dialog::account_chosen, this,
		         [this](const QString &name) {
			if (m_webauth_request)
				m_webauth_request->setSelectedAccount(name);
		});
		connect(dialog, &webauth_dialog::pin_entered, this,
		         [this](const QString &pin) {
			if (m_webauth_request)
				m_webauth_request->setPin(pin);
		});
		connect(dialog, &webauth_dialog::retry_requested, this, [this] {
			if (m_webauth_request)
				m_webauth_request->retry();
		});
		connect(dialog, &webauth_dialog::cancelled, this, [this] {
			if (m_webauth_request)
				m_webauth_request->cancel();
		});
		connect(request, &QWebEngineWebAuthUxRequest::stateChanged, this,
		         &qtwebengine_view::show_webauth_state);

		// Whatever state it is already in, rather than assuming it starts at
		// the beginning: the signal is emitted once the engine has something to
		// ask, so the first question is usually already chosen by the time this
		// runs and waiting for a `stateChanged` that has been and gone would
		// leave an empty window.
		show_webauth_state(request->state());
	});
}

// One state, one question. Shows the window as a side effect, because a state
// worth displaying is a state worth being able to see.
void qtwebengine_view::show_webauth_state(
        QWebEngineWebAuthUxRequest::WebAuthUxState state) {
	using ux = QWebEngineWebAuthUxRequest::WebAuthUxState;
	if (state == ux::Completed || state == ux::Cancelled) {
		// The conversation is over, however it ended. The window goes without
		// answering, and the request pointer goes with it -- the engine is free
		// to destroy the object now and a stale pointer here would be answered
		// into.
		drop_webauth_dialog();
		m_webauth_request = nullptr;
		return;
	}
	if (!m_webauth_dialog || !m_webauth_request)
		return;
	switch (state) {
		case ux::SelectAccount:
			m_webauth_dialog->ask_for_account(m_webauth_request->userNames());
			break;
		case ux::CollectPin: {
			const QWebEngineWebAuthPinRequest asked = m_webauth_request->pinRequest();
			webauth_dialog::pin_prompt prompt;
			prompt.reason     = to_pin_reason(asked.reason);
			prompt.error      = to_pin_error(asked.error);
			prompt.min_length = asked.minPinLength;
			// **Only after the key has refused a PIN.** That is where Qt's own
			// example reads this field, and it is the only place it is known to
			// mean anything: shown on a first prompt, a `remainingAttempts` of
			// zero would tell somebody their key is one mistake from locking
			// before they have made any, which is a sentence that stops people
			// typing.
			prompt.remaining_attempts =
				prompt.error == webauth_dialog::pin_error::none
				  ? -1
				  : asked.remainingAttempts;
			m_webauth_dialog->ask_for_pin(prompt);
			break;
		}
		case ux::FinishTokenCollection:
			m_webauth_dialog->ask_for_touch();
			break;
		case ux::RequestFailed:
			m_webauth_dialog->report_failure(
			  to_failure(m_webauth_request->requestFailureReason()));
			break;
		case ux::NotStarted:
			// Nothing to ask yet, so nothing on screen. An empty window with a
			// Cancel button in it is worse than no window: it invites somebody
			// to cancel a request that has not begun.
			return;
		case ux::Cancelled:
		case ux::Completed:
			// Answered above; named here so the switch stays exhaustive and a
			// state added later is a compiler warning rather than a silence.
			return;
	}
	m_webauth_dialog->show();
	m_webauth_dialog->raise();
	m_webauth_dialog->activateWindow();
}

void qtwebengine_view::drop_webauth_dialog() {
	if (!m_webauth_dialog)
		return;
	webauth_dialog *dialog = m_webauth_dialog;
	m_webauth_dialog = nullptr;
	// **Disconnected before it is taken down.** The dialog reports a closed
	// window as a cancellation, which is right when a person closed it and
	// wrong here: telling the engine to withdraw a request it has just
	// completed is a sign-in thrown away at the last step.
	disconnect(dialog, nullptr, this, nullptr);
	dialog->hide();
	// **`deleteLater`, not `delete`.** This is reached from `stateChanged`, and
	// the state can change inside `cancel()` -- which is called from one of the
	// dialog's own signal handlers. Deleting an object while a signal of its is
	// still being emitted is a use-after-free. Qt's own example deletes outright
	// and gets away with it only for as long as Chromium happens to answer
	// `cancel()` asynchronously, which is not a property this file should be
	// relying on.
	dialog->deleteLater();
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
	// A page that calls window.print() twice, or calls it while the dialog is
	// already up, must not get two print runs: Qt drops the second with a
	// warning and the first one's `printFinished` then answers for both.
	if (m_printing)
		return;

	auto *printer = new QPrinter(QPrinter::HighResolution);
	QPrintDialog dialog(printer, m_view);
	dialog.setWindowTitle("Print Page");
	if (dialog.exec() != QDialog::Accepted) {
		delete printer;
		emit print_finished(false);
		return;
	}

	m_printing = true;
	connect(m_view, &QWebEngineView::printFinished, this,
	         [this, printer](bool ok) {
		m_printing = false;
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

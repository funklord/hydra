#pragma once

#include "policy.h"

#include <QObject>
#include <QUrl>
#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>

class QAbstractListModel;

class QWidget;

// Per-page toggles the shell derives from the policy engine and hands down.
// Deliberately plain bools rather than engine settings: a backend applies what
// its engine supports and ignores the rest, which is the honest shape given
// Android's System WebView offers a reduced set (architecture doc sec 19.2).
struct view_settings {
	bool javascript = true;
	bool images     = true;
	bool autoplay   = true;
	bool popups     = false;
	bool scrollbars = true;   // kiosk mode turns these off (architecture doc sec 8)
};

// One rendered page.
//
// This is the seam described in architecture doc sec 19.2: the shell owns nodes,
// policy, and lifecycle, and a backend owns whatever engine actually draws a
// page. Qt WebEngine on desktop, the Android System WebView later -- the shell
// must never learn which, so nothing Qt-WebEngine-shaped may appear here.
class web_view_backend : public QObject {
	Q_OBJECT
public:
	// Answers a permission request, **eventually**. The shell supplies it.
	//
	// It returned `bool` and this comment used to say "synchronously... no UI
	// and no waiting", which was true and was the whole limitation: a bool
	// cannot say "ask the person and I will tell you". So a site set to `ask`
	// could not be honoured by any caller, and a refusal was silent -- the page
	// saw a rejected promise and nobody was told a decision had been made for
	// them.
	//
	// The answer callback may be invoked before this returns, which is what the
	// common case does: a policy that already says allow or block answers
	// immediately and nothing is posted anywhere. Callers must therefore cope
	// with being answered re-entrantly, which is why every call site below
	// captures what it needs by value rather than relying on a scope that may
	// already have gone.
	using permission_answer  = std::function<void(bool granted)>;
	using permission_decider =
	  std::function<void(const QUrl &origin, policy::feature f,
	                      permission_answer answer)>;


	explicit web_view_backend(QObject *parent = nullptr) : QObject(parent) {}

	// The widget to place in the shell's view stack. Owned by the backend.
	virtual QWidget *widget() = 0;

	virtual QUrl url() const = 0;
	virtual void load(const QUrl &url) = 0;
	virtual void back() = 0;
	virtual void forward() = 0;
	virtual void reload() = 0;

	// Abandon whatever is loading. **Not pure**: a backend that cannot stop a
	// load does nothing, and the shell's Stop button is then a button that
	// does nothing -- which is why the shell only offers one while a load is
	// actually running.
	virtual void stop() {}

	// Leave fullscreen, when the shell is what ended it.
	//
	// **Both directions are needed or the two disagree.** The page asks to go
	// fullscreen and the shell presents it; but the shell can also come back on
	// its own -- Esc, or the presentation closing -- and a page never told that
	// still believes it is fullscreen, so its own control does nothing and the
	// video stays at the size it chose for a screen it no longer has.
	virtual void exit_fullscreen() {}

	// Put the page on paper, running whatever flow the platform provides.
	//
	// **The backend owns the whole flow, dialog included**, which is the one
	// place the shell deliberately does not own the UI. It is not squeamishness
	// about QPrintDialog: Android prints through the system PrintManager, which
	// presents its own chooser and cannot be driven from a QPrinter the shell
	// filled in. A seam that took a configured printer would be Qt's spelling
	// of printing written into every backend that ever implements this, which
	// is exactly what sec 19.2 exists to prevent. So the shell asks for a print
	// and the platform asks the questions.
	//
	// **Not pure, and the shell must ask before offering it.** A backend with
	// no printing does nothing here, so an unconditional menu entry would be a
	// second Stop button that silently fails -- the failure this seam already
	// records once. `can_print()` is what the shell greys the action on.
	virtual void print() {}
	virtual bool can_print() const { return false; }

	virtual void apply_settings(const view_settings &s) = 0;
	virtual void set_permission_decider(permission_decider fn) = 0;

	// Which screen or window a page may capture, chosen by the person.
	//
	// **A second question, after the permission.** The shield answers whether a
	// site may share at all, and that answer can be remembered; this answers
	// what to share *now*, and must not be. A site allowed to present last week
	// has not been allowed to present whatever is open today.
	//
	// The models are the engine's, and both are Core types -- so the dialog the
	// shell puts up is engine-neutral and can be built against fakes, which is
	// how it is measured at phone geometry without a compositor willing to hand
	// out a screen capture.
	//
	// `row < 0` means nothing was chosen, which is what closing the dialog
	// gives. Asynchronous for the same reason the decider is: the answer waits
	// for a person.
	using capture_answer =
	  std::function<void(bool is_screen, int row)>;
	using capture_chooser =
	  std::function<void(const QUrl &origin, QAbstractListModel *screens,
	                      QAbstractListModel *windows, capture_answer answer)>;
	virtual void set_capture_chooser(capture_chooser fn) = 0;

	// A site asking for a username and password. **Answered while the callback
	// runs**, which is why this is a decider rather than a signal: Qt hands
	// over an authenticator to fill in, and a request answered later has
	// already been abandoned. Returning false declines, which is what happens
	// today by default -- the difference is that declining becomes a choice
	// somebody made rather than the only thing the browser could do.
	using authenticator = std::function<bool(const QUrl &url, const QString &realm,
	                                          QString *user, QString *password)>;
	virtual void set_authenticator(authenticator fn) { Q_UNUSED(fn) }

	// The *proxy* asking for a username and password, which is a different
	// question from the site asking and must not be asked in the same words.
	//
	// **Separate from `authenticator` on purpose.** The two prompts look
	// identical and the credentials are not interchangeable: one is the
	// password for a site, the other for the network you are reaching it
	// through. A prompt that does not say which is asking invites typing the
	// site's password into the proxy's box, which hands it to a party that was
	// never entitled to it. So the shell gets a different callback, and can
	// therefore say a different thing.
	using proxy_authenticator = std::function<bool(const QString &proxy_host,
	                                                const QString &realm,
	                                                QString *user,
	                                                QString *password)>;
	virtual void set_proxy_authenticator(proxy_authenticator fn) { Q_UNUSED(fn) }

	// One certificate a site is willing to accept, described in plain strings.
	//
	// Strings rather than the engine's certificate type because this header is
	// the line the shell must not see an engine through (sec 19.2): a chooser that
	// took a QSslCertificate would put Qt Networking's spelling of a
	// certificate into every backend that ever implements this.
	struct certificate_offer {
		QString subject;      // who it identifies
		QString issuer;       // who vouches for it
		QString valid_until;  // when that stops being true
		QString serial;
	};

	// Which certificate to send, or **-1 to send none** -- and none is the
	// answer that must be reachable, because sending one identifies you to the
	// site by name. Qt aborts the selection when nothing is connected, so
	// before this existed the answer was always none; the difference is that
	// none becomes a decision rather than the only outcome available.
	//
	// Answered while the callback runs, like the two above: the engine is
	// waiting on the selection object and a choice made later arrives after it
	// has been abandoned.
	using certificate_chooser =
	  std::function<int(const QUrl &url, const QList<certificate_offer> &offered)>;
	virtual void set_certificate_chooser(certificate_chooser fn) { Q_UNUSED(fn) }

	// Whether this view may go somewhere. **A decider and not a signal**, for
	// the reason the two above are: the engine asks while it is deciding, and
	// an answer that arrives afterwards answers a navigation that has already
	// been committed or dropped.
	//
	// Returning false leaves the view where it is. That is what a locked tab
	// needs (sec 5.5) -- the shell opens a sub-tab for the refused url and the
	// pinned page never moves -- but the seam does not know about locking, and
	// should not: it asks, the shell decides.
	//
	// `user_initiated` separates a click or a typed address from a redirect or
	// a script, because those deserve different answers, exactly as they do for
	// a window request. `in_main_frame` is false for an iframe navigating
	// itself, which is not the page going anywhere.
	//
	// A backend that cannot ask this simply never calls it and navigates as it
	// always did; the default here is no decider at all. Android's WebView has
	// the hook (`shouldOverrideUrlLoading`) and is not wired to it yet.
	using navigation_decider = std::function<bool(const QUrl &url,
	                                               bool in_main_frame,
	                                               bool user_initiated)>;
	virtual void set_navigation_decider(navigation_decider fn) { Q_UNUSED(fn) }

	// Reflow zoom -- the page re-lays out at the new scale. Kiosk mode's
	// reliable scaling path (architecture doc sec 8.1); every engine has this.
	virtual void set_zoom_factor(double factor) = 0;

	// What it is now. **Not pure, and it answers 1.0** for a backend that
	// cannot scale: a caller stepping up from "whatever it is" then starts
	// from the same place it would have anyway, and the shell can report a
	// level without keeping a second copy of it to fall out of step.
	virtual double zoom_factor() const { return 1.0; }

	// Run `source` in every page this view loads, in an isolated world so the
	// page cannot see or tamper with it (architecture doc sec 13.2). Android's
	// System WebView has its own injection mechanism, which is why this sits on
	// the seam instead of appearing as QWebEngineScript in the shell.
	// `subframes` defaults off and that default is the security one: a script
	// that fills credentials or reads a picked element must not run inside a
	// third-party iframe. The consent blocker is the exception, because the
	// thing it acts on is frequently *shipped* as one -- a CMP in an iframe is
	// the normal way vendors deliver them, and a top-frame-only script leaves
	// those banners standing.
	virtual void inject_script(const QString &name, const QString &source,
	                            bool subframes = false) = 0;

	// The same, but in the page's **own** world, and in every frame.
	//
	// Separate from inject_script() rather than a flag on it, because this is a
	// security escalation and should be greppable. An isolated world has its
	// own globals, so a script there cannot see or wrap the page's objects --
	// which is exactly why autofill and the picker live in one, and exactly why
	// a Media Source tap (sec 11.6) cannot. A script injected here is visible and
	// modifiable by the page, so it must hold nothing worth stealing and grant
	// nothing: no bridge, no tokens, no privileged calls. It reports by
	// dispatching DOM events that an isolated-world relay picks up.
	//
	// Frames matter too: on real sites the player is in a third-party iframe,
	// so a tap confined to the top frame would see nothing at all.
	virtual void inject_main_world_script(const QString &name,
	                                       const QString &source) = 0;

	// Expose `object` to injected scripts under `name`. Desktop wires this
	// through QWebChannel; Android would use addJavascriptInterface. Passing
	// nullptr withdraws it.
	virtual void set_script_bridge(QObject *object, const QString &name) = 0;

	// Session state -- navigation history and whatever else the engine can
	// serialize. Opaque to the shell, which only stores and returns the blob
	// (state_store keys it by node id, architecture doc sec 4.2).
	virtual QByteArray save_state() const = 0;
	virtual bool       restore_state(const QByteArray &blob) = 0;

	// What the page currently calls itself, for a caller that needs it now
	// rather than on the next change. Not pure: a backend with no notion of a
	// title answers with nothing and callers fall back, which is the same thing
	// they do before a page has loaded.
	virtual QString page_title() const { return QString(); }

	// Whether the page's history has anywhere to go. **Not pure, and the
	// default is yes**: a backend that does not track history should keep the
	// buttons it has always had rather than have them switched off on a guess.
	// A backend that does know says so, and the shell greys them accordingly.
	// Find text on the page. **Not pure**: a backend with no search does
	// nothing and reports no matches, which is exactly what the bar then
	// shows. `fresh` says the term changed, so the engine restarts rather than
	// advancing to the next match.
	virtual void find_text(const QString &text, bool forward, bool fresh) {
		Q_UNUSED(text) Q_UNUSED(forward) Q_UNUSED(fresh)
		emit find_result(0, 0);
	}

	// Something in the shell is covering this view; get out of the way.
	//
	// **A no-op wherever the page is drawn by Qt**, because Qt's own stacking
	// already handles it -- which is why it defaults to nothing rather than
	// being pure virtual. It exists for a backend whose surface is not Qt's.
	//
	// On Android the page is a real `android.webkit.WebView` added to the
	// Activity's view hierarchy, so it composites above everything Qt paints
	// and `raise()` on a Qt widget cannot reach over it. The tab drawer slid
	// out *underneath* the page and was invisible: the widget was where it
	// should be, the right size and `isVisible()`, and none of that mattered.
	// `android_view` already knew the shape of this problem for modal dialogs
	// and hid the native view while one was up; the drawer is the same
	// problem arriving from the shell rather than from a QDialog.
	virtual void set_obscured(bool) {}

	virtual bool can_go_back() const { return true; }
	virtual bool can_go_forward() const { return true; }

signals:
	void url_changed(const QUrl &url);

	// What the page calls itself. The seam had no title at all, so a tab was
	// labelled with whatever the tree file said or somebody typed, for ever --
	// browsing to another page left the old name in place. A backend that has
	// no notion of a title simply never emits this.
	void title_changed(const QString &title);

	// History moved: something was pushed onto it, or the position within it
	// changed. Separate from `url_changed` because the two do not coincide --
	// going back changes the url *and* what is now reachable, while a fragment
	// jump changes the url and nothing else -- and because a backend can know
	// one without the other.
	void history_changed();

	// How far a page has got, 0 to 100, and whether it arrived. **The seam had
	// no notion of loading at all**, so nothing in the window could say that a
	// slow site was working rather than broken, and a load that failed outright
	// said nothing whatsoever. A backend that cannot report progress simply
	// never emits these, and the shell shows no bar.
	// How many matches the page holds and which one is showing, 1-based. Zero
	// matches and zero active is "nothing found", which the bar says out loud.
	void find_result(int matches, int active);

	void load_progress(int percent);
	void load_finished(bool ok);

	// A TLS certificate was refused, and why. **The refusal is not the news** --
	// rejecting a bad certificate is correct and is what happened before this
	// existed. The news is that it happened at all: unreported, the page simply
	// failed, and "could not be loaded" is indistinguishable from a site being
	// down when the real answer is that its identity could not be established.
	void certificate_rejected(const QUrl &url, const QString &reason);

	// A page asked for a new window: a target="_blank" link, or window.open.
	// **Nothing handled this at all**, and an unhandled request in Qt is not a
	// refusal -- it is a click that silently does nothing, which is the worst
	// outcome of the three available. `user_initiated` separates a click from
	// a script, because those deserve different answers.
	// A page asked to open a window.
	//
	// **`adopt` is an out-parameter and the whole point of this signal.** A
	// receiver that wants the new window to be a real child of the opener sets
	// `*adopt` to the backend that should take it over, synchronously, before
	// the emit returns -- the engine's request object is only valid for the
	// duration of the call. Leaving it null means "I have handled the url
	// myself", which is what a receiver that merely files a bookmark does.
	//
	// The distinction is not cosmetic. Loading the requested url into a fresh
	// page is *not* the same as opening the window: `window.opener` is null in
	// the copy, so the popup has nothing to talk back to. Every OAuth popup
	// works that way -- Google Identity Services posts the credential to the
	// opener and never redirects -- which is why signing in to claude.ai with
	// Google produced a blank tab and no session.
	void new_window_requested(const QUrl &url, bool user_initiated,
	                           web_view_backend **adopt);

	// Where the link under the pointer would take you. An empty url means the
	// pointer left one. **This is the browser's oldest security affordance**:
	// the only way to see where a link goes before committing to it, and the
	// only check on link text that says one thing and points at another.
	void link_hovered(const QUrl &url);

	// The engine's render process died. Kiosk mode's watchdog reloads on this
	// so an unattended screen self-heals (architecture doc sec 8.3).
	// The page asked to close its own window -- `window.close()`.
	//
	// **The popup half of the opener work.** A sign-in popup finishes by
	// closing itself; with nothing connected here it stays open, blank or
	// reading "you may close this window", and the person cannot tell whether
	// it worked. Chromium only honours `window.close()` for a window script
	// opened, so obeying it cannot be used to shut a tab somebody opened
	// themselves.
	void close_requested();

	void render_process_gone();

	// The page asked to fill the screen, or to stop. **Nothing handled this at
	// all**, and an unhandled fullscreen request is not a refusal the page can
	// see: `requestFullscreen()` is rejected by the engine before the shell is
	// consulted, so a site's own fullscreen button did nothing whatsoever and
	// said nothing about why. That is the worst of the three outcomes
	// available, and it is the one this seam already records for window
	// requests.
	//
	// `on` is false when the page is asking to come back, which it does for
	// its own Esc handling and when a video ends.
	void fullscreen_requested(bool on);

	// A print run ended, and whether paper came out of it. Emitted for a
	// cancelled dialog as well as a failed spool, because from the shell's side
	// those are the same event: the page was not printed. It exists so the
	// status bar can say so -- printing is one of the few things a browser does
	// whose outcome is invisible from inside the window.
	void print_finished(bool ok);
};

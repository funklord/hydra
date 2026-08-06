// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "policy.h"

#include <QObject>
#include <QUrl>
#include <QByteArray>

#include <functional>

class QWidget;

// Per-page toggles the shell derives from the policy engine and hands down.
// Deliberately plain bools rather than engine settings: a backend applies what
// its engine supports and ignores the rest, which is the honest shape given
// Android's System WebView offers a reduced set (architecture doc §19.2).
struct view_settings {
	bool javascript = true;
	bool images     = true;
	bool autoplay   = true;
	bool popups     = false;
	bool scrollbars = true;   // kiosk mode turns these off (architecture doc §8)
};

// One rendered page.
//
// This is the seam described in architecture doc §19.2: the shell owns nodes,
// policy, and lifecycle, and a backend owns whatever engine actually draws a
// page. Qt WebEngine on desktop, the Android System WebView later — the shell
// must never learn which, so nothing Qt-WebEngine-shaped may appear here.
class web_view_backend : public QObject {
	Q_OBJECT
public:
	// Answers a permission request synchronously. The shell supplies this; it
	// is a pure policy lookup today, with no UI and no waiting.
	using permission_decider = std::function<bool(const QUrl &origin, policy::feature f)>;


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

	virtual void apply_settings(const view_settings &s) = 0;
	virtual void set_permission_decider(permission_decider fn) = 0;

	// A site asking for a username and password. **Answered while the callback
	// runs**, which is why this is a decider rather than a signal: Qt hands
	// over an authenticator to fill in, and a request answered later has
	// already been abandoned. Returning false declines, which is what happens
	// today by default -- the difference is that declining becomes a choice
	// somebody made rather than the only thing the browser could do.
	using authenticator = std::function<bool(const QUrl &url, const QString &realm,
	                                          QString *user, QString *password)>;
	virtual void set_authenticator(authenticator fn) { Q_UNUSED(fn) }

	// Whether this view may go somewhere. **A decider and not a signal**, for
	// the reason the two above are: the engine asks while it is deciding, and
	// an answer that arrives afterwards answers a navigation that has already
	// been committed or dropped.
	//
	// Returning false leaves the view where it is. That is what a locked tab
	// needs (§5.5) -- the shell opens a sub-tab for the refused url and the
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

	// Reflow zoom — the page re-lays out at the new scale. Kiosk mode's
	// reliable scaling path (architecture doc §8.1); every engine has this.
	virtual void set_zoom_factor(double factor) = 0;

	// What it is now. **Not pure, and it answers 1.0** for a backend that
	// cannot scale: a caller stepping up from "whatever it is" then starts
	// from the same place it would have anyway, and the shell can report a
	// level without keeping a second copy of it to fall out of step.
	virtual double zoom_factor() const { return 1.0; }

	// Run `source` in every page this view loads, in an isolated world so the
	// page cannot see or tamper with it (architecture doc §13.2). Android's
	// System WebView has its own injection mechanism, which is why this sits on
	// the seam instead of appearing as QWebEngineScript in the shell.
	// `subframes` defaults off and that default is the security one: a script
	// that fills credentials or reads a picked element must not run inside a
	// third-party iframe. The consent blocker is the exception, because the
	// thing it acts on is frequently *shipped* as one — a CMP in an iframe is
	// the normal way vendors deliver them, and a top-frame-only script leaves
	// those banners standing.
	virtual void inject_script(const QString &name, const QString &source,
	                            bool subframes = false) = 0;

	// The same, but in the page's **own** world, and in every frame.
	//
	// Separate from inject_script() rather than a flag on it, because this is a
	// security escalation and should be greppable. An isolated world has its
	// own globals, so a script there cannot see or wrap the page's objects —
	// which is exactly why autofill and the picker live in one, and exactly why
	// a Media Source tap (§11.6) cannot. A script injected here is visible and
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

	// Session state — navigation history and whatever else the engine can
	// serialize. Opaque to the shell, which only stores and returns the blob
	// (state_store keys it by node id, architecture doc §4.2).
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
	void new_window_requested(const QUrl &url, bool user_initiated);

	// Where the link under the pointer would take you. An empty url means the
	// pointer left one. **This is the browser's oldest security affordance**:
	// the only way to see where a link goes before committing to it, and the
	// only check on link text that says one thing and points at another.
	void link_hovered(const QUrl &url);

	// The engine's render process died. Kiosk mode's watchdog reloads on this
	// so an unattended screen self-heals (architecture doc §8.3).
	void render_process_gone();
};

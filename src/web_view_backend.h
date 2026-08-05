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

	virtual void apply_settings(const view_settings &s) = 0;
	virtual void set_permission_decider(permission_decider fn) = 0;

	// Reflow zoom — the page re-lays out at the new scale. Kiosk mode's
	// reliable scaling path (architecture doc §8.1); every engine has this.
	virtual void set_zoom_factor(double factor) = 0;

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

	// The engine's render process died. Kiosk mode's watchdog reloads on this
	// so an unattended screen self-heals (architecture doc §8.3).
	void render_process_gone();
};

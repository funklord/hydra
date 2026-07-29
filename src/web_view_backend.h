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

	// Session state — navigation history and whatever else the engine can
	// serialize. Opaque to the shell, which only stores and returns the blob
	// (state_store keys it by node id, architecture doc §4.2).
	virtual QByteArray save_state() const = 0;
	virtual bool       restore_state(const QByteArray &blob) = 0;

signals:
	void url_changed(const QUrl &url);

	// The engine's render process died. Kiosk mode's watchdog reloads on this
	// so an unattended screen self-heals (architecture doc §8.3).
	void render_process_gone();
};

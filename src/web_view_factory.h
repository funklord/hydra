// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QUrl>

#include <functional>

class QWidget;
class web_view_backend;

// Makes views, and owns whatever profile-wide machinery an engine needs behind
// them — on desktop that is the shared QWebEngineProfile with the request
// interceptor and cookie filter installed on it (architecture doc §6/§7.3).
//
// The shell holds only this interface, so the concrete backend is named in
// exactly one place: main(). That is what keeps §19.2's rule enforceable rather
// than merely intended.
class web_view_factory {
public:
	virtual ~web_view_factory() = default;

	virtual web_view_backend *create_view(QWidget *parent) = 0;

	// Called when the engine is handed a URL it will not render as a page —
	// a `magnet:` link being the motivating case (§11.4). The shell decides
	// what to do with it; the engine's only job is to hand it over and not
	// draw an error page.
	//
	// This sits on the factory rather than on a view because it is a
	// browser-wide policy, and because on desktop the mechanism is genuinely
	// profile-wide: Chromium treats unregistered schemes as external protocols
	// and drops them *before* any per-navigation callback runs, so the only
	// thing that sees a magnet link is a registered custom scheme handler on
	// the profile. (Measured, not assumed: `navigationRequested` is never
	// invoked for `magnet:`, while it fires normally for http.) Android's
	// `shouldOverrideUrlLoading` would satisfy the same interface per view.
	using external_url_handler = std::function<void(const QUrl &url)>;
	virtual void set_external_url_handler(external_url_handler fn) = 0;
};

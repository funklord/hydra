// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QUrl>

#include <functional>

class QWidget;
class web_view_backend;

// Makes views, and owns whatever profile-wide machinery an engine needs behind
// them -- on desktop that is the shared QWebEngineProfile with the request
// interceptor and cookie filter installed on it (architecture doc sec 6/sec 7.3).
//
// The shell holds only this interface, so the concrete backend is named in
// exactly one place: main(). That is what keeps sec 19.2's rule enforceable rather
// than merely intended.
class web_view_factory {
public:
	virtual ~web_view_factory() = default;

	virtual web_view_backend *create_view(QWidget *parent) = 0;

	// Called when the engine is handed a URL it will not render as a page --
	// a `magnet:` link being the motivating case (sec 11.4). The shell decides
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

	// A page asked to download something, and the engine is doing it.
	//
	// **Not routed through `download_manager`, deliberately.** That manager
	// fetches a url itself, which is right for a magnet link or a stream the
	// media dialog found -- things the page never had. A download a page
	// starts is the opposite case: it may be a `blob:` the page built in
	// memory, or a url that only means anything with the session's cookies
	// and headers attached. Refetching it from outside the engine gets a
	// login screen or nothing at all. So the engine keeps the transfer and
	// this only reports it.
	//
	// `path` is where the file is being written, chosen before the engine is
	// told to proceed. Called once when the transfer starts and again when it
	// ends, with `ok` saying which -- a download that fails silently is the
	// shape this whole feature existed to fix.
	using download_note =
	  std::function<void(const QUrl &url, const QString &path, bool finished,
	                      bool ok)>;
	virtual void set_download_handler(download_note fn) = 0;
};

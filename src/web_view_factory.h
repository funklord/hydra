#pragma once

#include <QString>
#include <QStringList>
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

	// --- Forgetting ---------------------------------------------------------
	//
	// **Nothing in this browser could delete a byte of what it stored**, from
	// the moment the profile stopped being off the record. Cookies,
	// localStorage, the visited-link database and the http cache all went to
	// disk and stayed there, and the only `forget_*` calls in the tree are
	// about other things entirely -- tabs, imported site rules, a KeePass
	// pairing. The policy on the privacy page governs what a site may *store*
	// from now on; it has never had anything to say about what is already
	// stored.
	//
	// This is on the factory rather than on a view for the reason
	// `set_external_url_handler` is: the stores are profile-wide, one page's
	// cookie jar is every page's cookie jar, and a view is the wrong thing to
	// ask.

	// Which stores to empty. One flag each rather than a single "everything"
	// switch, because they cost very different things: the cache costs a slow
	// reload, and the cookies cost every login the browser is holding. Nothing
	// here touches the tab tree or a tab's history, which this browser
	// deliberately persists and which are not browsing data in this sense.
	struct browsing_data {
		bool cookies       = false;
		bool cache         = false;
		bool visited_links = false;

		bool any() const { return cookies || cache || visited_links; }
	};

	// How far one store's clear actually got.
	//
	// **`unconfirmed` is the reason this is an enum and not a bool.** A
	// backend can be certain about some of these and not others -- one call
	// answers with a completion signal, another answers with nothing at all --
	// and reporting the second as success would be the blind claim this whole
	// call exists to stop making. `refused` is its opposite and just as
	// necessary: a backend that cannot do something has to be able to say so,
	// because a stub that quietly does nothing is indistinguishable from a
	// clear that worked.
	enum class clear_state { not_asked, done, unconfirmed, refused };

	struct clear_report {
		clear_state cookies       = clear_state::not_asked;
		clear_state cache         = clear_state::not_asked;
		clear_state visited_links = clear_state::not_asked;

		// How many cookies were observed to go. -1 where nothing counted them,
		// which is not the same answer as 0 -- "there were none" and "nobody
		// looked" have to be tellable apart.
		int cookies_removed = -1;

		// Everything the caller has to be told and the states above cannot
		// carry: what was refused and why, and where a store that says `done`
		// is narrower than its name suggests. Meant to be shown to a person
		// verbatim.
		QStringList notes;
	};

	// Reports when the work has actually finished, not when the call returns.
	// None of these stores empties synchronously, so a caller that treated the
	// return as the answer would be saying "cleared" while the deletion was
	// still in flight.
	using clear_note = std::function<void(const clear_report &report)>;
	virtual void clear_browsing_data(const browsing_data &what,
	                                  clear_note done) = 0;
};

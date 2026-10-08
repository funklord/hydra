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

	// **What this browser tells a server it is**, so that anything fetching a
	// stream outside the engine can say the same thing.
	//
	// The local proxy exists because a CDN expects the request to look like
	// the one the page made -- its own header says Referer, cookies *and*
	// User-Agent -- and the shell was building that context with the Referer
	// alone. The comment beside it said "and this browser's own User-Agent"
	// while the line was not there.
	//
	// **The third field is still missing a supplier**, and this is the seam
	// it would arrive through. `stream_context::cookies` is filled only from
	// a learned extractor's headers; nothing reads the engine's cookie jar,
	// so an ordinary detected stream goes upstream with no `Cookie` header at
	// all. Android could answer in one call -- `CookieManager.getCookie(url)`
	// -- and the desktop cannot, which is what makes it a design question
	// rather than an omission. See project.md.
	//
	// Empty means "this backend cannot say", and a caller must then leave the
	// field alone rather than invent one: a wrong User-Agent is worse than
	// the transport's default, which is at least honest about being a
	// library.
	virtual QString user_agent() const { return QString(); }

	// **The cookies the page carried, for a `Cookie` header the CDN expects.**
	// A naked stream URL frequently answers 403 because the CDN wants the same
	// context the page had (architecture doc sec 11.3), and `local_proxy` already
	// replays whatever `stream_context::cookies` holds -- it was the observing
	// half that did not exist, so the field was always empty for a stream found
	// by detection.
	//
	// Empty by default and empty where a backend cannot answer, which is the
	// same discipline as `user_agent()` above: `local_proxy` sets the header
	// only when this is non-empty, so an unknowing backend sends no cookies
	// rather than wrong ones.
	virtual QString cookie_header_for(const QUrl &) const { return QString(); }

	// Forget the observed cookies. Part of "Clear browsing data": the mirror
	// below is a record of where somebody has been, and a clear that left it
	// standing would be the same fault this tree has already found twice in
	// other caches.
	virtual void forget_cookies() {}

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
	//
	// **`why` is the next step of that same argument.** Saying a download
	// failed and not why leaves the person with nothing to do about it, and
	// the reasons are not interchangeable: a full disk, a directory that
	// refuses the write, a network that timed out and a transfer the engine
	// blocked want four different responses. Qt reports it as
	// `interruptReasonString()` and nothing was reading it. Empty when `ok`,
	// and empty when a backend cannot say -- which is how a backend says it
	// cannot, rather than being made to invent a sentence.
	// **A struct rather than a parameter list**, and the reason is that this
	// is the second widening. It carried `(url, path, finished, ok)`, grew
	// `why` when a failure that said nothing proved useless, and now needs an
	// identity and a byte count -- because the shell no longer merely narrates
	// a page download, it adopts it as a job (architecture doc sec 11.2: one
	// manager fed by two sources). A field added later does not touch every
	// implementer of this interface, and Android implements it too.
	struct engine_download {
		// Stable across every note for one download, and the backend's to
		// mint. The shell keys its job on this: `url` cannot, because two
		// downloads of one address are two downloads, and `path` cannot,
		// because it is not known until the engine has chosen a name.
		quint64 id       = 0;
		QUrl    url;
		QString path;
		qint64  received = 0;
		qint64  total    = -1;   // -1 while unknown, and it may stay unknown
		bool    finished = false;
		bool    ok       = false;
		// Only when finished and not ok. Empty where a backend cannot say,
		// which is how it says so rather than inventing a sentence.
		QString why;
	};
	using download_note = std::function<void(const engine_download &d)>;
	virtual void set_download_handler(download_note fn) = 0;

	// **Stop a download the engine is running.** The shell cannot: the
	// transfer belongs to the engine for the reasons the factory records --
	// a `blob:` has no url to refetch and a cookie-bound one gets a login
	// page -- so a Cancel in the downloads window has to come back here.
	//
	// False when the id is unknown, which is the ordinary answer for a
	// download that has already finished. A backend with no page downloads to
	// cancel inherits that answer rather than being made to pretend.
	virtual bool cancel_engine_download(quint64 id) {
		Q_UNUSED(id)
		return false;
	}

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

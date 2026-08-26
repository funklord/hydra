// SPDX-License-Identifier: GPL-3.0-or-later
#include "qtwebengine_factory.h"
#include "qtwebengine_view.h"
#include "request_filter.h"
#include "qtwebengine_interceptor.h"

#include <QWebEngineDownloadRequest>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QNetworkCookie>
#include <QTimer>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineCookieStore>

namespace {

// The schemes main() registered, remembered so the constructor knows what to
// install a handler for. A file-scope list is acceptable here because scheme
// registration is itself process-wide and once-only in Qt.
QStringList g_custom_schemes;

}  // namespace

// Receives every request for a registered custom scheme. Chromium calls this on
// the UI thread, so handing the URL straight to the shell is safe.
class magnet_scheme_handler : public QWebEngineUrlSchemeHandler {
public:
	void requestStarted(QWebEngineUrlRequestJob *job) override {
		const QUrl url = job->requestUrl();
		// Abort rather than fail with an error: the navigation must leave no
		// trace in the page, because from the user's point of view clicking the
		// link started a download and did not go anywhere.
		job->fail(QWebEngineUrlRequestJob::RequestAborted);
		if (on_url)
			on_url(url);
	}

	std::function<void(const QUrl &)> on_url;
};

void qtwebengine_factory::register_url_schemes(const QStringList &schemes) {
	for (const QString &name : schemes) {
		QWebEngineUrlScheme scheme(name.toUtf8());
		// A magnet link is all query and no host, so Path is the right syntax.
		scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
		// It is not a document and must never be treated as an origin that can
		// reach anything.
		scheme.setFlags(QWebEngineUrlScheme::NoAccessAllowed);
		QWebEngineUrlScheme::registerScheme(scheme);
		g_custom_schemes << name;
	}
}

namespace {

// A name in `dir` that is not taken, keeping the extension where there is one.
//
// Chromium hands over a suggested name and nothing else; writing it blind is
// how a second download of the same file destroys the first. Numbered rather
// than timestamped so that a person can see which came first.
QString free_path(const QString &dir, const QString &suggested) {
	const QString base = suggested.isEmpty() ? QStringLiteral("download")
		                                          : suggested;
	QString path = dir + "/" + base;
	if (!QFile::exists(path))
		return path;
	const int dot = base.lastIndexOf('.');
	// A leading dot is the whole name, not an extension, so `.bashrc` numbers
	// as `.bashrc (1)` rather than growing a suffix in front of itself.
	const QString stem = dot > 0 ? base.left(dot) : base;
	const QString ext  = dot > 0 ? base.mid(dot) : QString();
	for (int n = 1; n < 1000; ++n) {
		path = QString("%1/%2 (%3)%4").arg(dir, stem).arg(n).arg(ext);
		if (!QFile::exists(path))
			return path;
	}
	return dir + "/" + base;
}

}  // namespace

qtwebengine_factory::qtwebengine_factory(request_filter *filter)
  : m_filter(filter) {
	// One shared profile for every view (architecture doc sec 6).
	// **A named profile, because the default one forgets everything.**
	//
	// `QWebEngineProfile::defaultProfile()` is off-the-record in Qt 6: cookies,
	// localStorage, IndexedDB and the http cache all live in memory and die
	// with the process. Nothing here ever asked for that -- it was inherited by
	// taking the default -- and the cost was invisible in the way this project
	// keeps meeting: log in, quit, come back, and you are logged out, with
	// nothing said. It contradicted the tree, which persists tabs and their
	// history, so a restored tab reloaded a site you were signed into and
	// showed you signed out. It also contradicted the consent blocker, whose
	// whole design rests on an answer sticking, and an answer is a cookie.
	//
	// Constructing one with a storage name makes it persistent. The name is
	// the directory: Qt puts it under `AppDataLocation/QtWebEngine/<name>`,
	// which for this app is beside the tree file and the state directory
	// rather than somewhere else on the disk.
	//
	// Owned here, and safe to own: `main()` declares the factory *before* the
	// window, so the window and every page in it are destroyed first. A
	// profile outliving its pages is the requirement, and declaration order is
	// what satisfies it -- that comment in `main()` is load-bearing.
	m_profile = new QWebEngineProfile(QStringLiteral("hydra"));
	// Stated rather than left to the default, because the default is the thing
	// that was wrong before and a reader deserves to see the intent.
	m_profile->setPersistentCookiesPolicy(
	  QWebEngineProfile::AllowPersistentCookies);
	m_profile->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
	// **The policy engine is the only decider, and a persistent profile was
	// about to quietly take that away.** `QWebEnginePage::permissionRequested`
	// fires only while a permission's state is `Ask`, and
	// `qtwebengine_view.cpp` answers every one of those by calling the shell's
	// decider and then `grant()`. Off the record, a grant died with the
	// process, so the question came back next time and the engine stored
	// nothing the shield did not know about.
	//
	// A named profile's default is to write that grant to disk. The engine
	// would then stop asking: a site the user later blocks in the shield keeps
	// the camera it was once given, `HYDRA_PERM_DEBUG` prints nothing, and
	// there is no screen anywhere in this app that lists or clears what
	// Chromium decided to remember. Two authorities, one of them invisible.
	//
	// `AskEveryTime` is not a new policy -- it is the behaviour that has always
	// been here, now that it has to be asked for rather than inherited from a
	// profile that could not remember anything.
	m_profile->setPersistentPermissionsPolicy(
	  QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);

	// **A page asking to save something.** Nothing was connected to this, so
	// Chromium asked and got no answer, and Qt cancels an unaccepted request --
	// silently. Every download a page started did nothing at all, which is
	// indistinguishable from a page that ignored the click.
	//
	// The engine keeps the transfer rather than handing the url to
	// `download_manager`. That manager fetches a url itself, which is right
	// for a magnet or a stream the media dialog found, and wrong here: a page
	// download may be a `blob:` the page built in memory, or a url that means
	// nothing without the session's cookies. Refetching from outside the
	// engine gets a login page.
	QObject::connect(m_profile, &QWebEngineProfile::downloadRequested,
	                  m_profile, [this](QWebEngineDownloadRequest *d) {
		if (!d)
			return;
		const QString dir =
		  QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
		QDir().mkpath(dir);
		const QString path = free_path(dir, d->suggestedFileName());
		d->setDownloadDirectory(QFileInfo(path).absolutePath());
		d->setDownloadFileName(QFileInfo(path).fileName());

		const QUrl url = d->url();
		if (m_download_note)
			m_download_note(url, path, /*finished=*/false, /*ok=*/false);
		// Reported when it ends as well, either way: this feature exists
		// because a download that fails without saying so looks exactly like
		// one that was never started.
		QObject::connect(d, &QWebEngineDownloadRequest::isFinishedChanged, d,
		                  [this, d, url, path] {
			if (m_download_note)
				m_download_note(url, path, /*finished=*/true,
				                 d->state() == QWebEngineDownloadRequest::DownloadCompleted);
		});
		d->accept();
	});

	m_interceptor = new qtwebengine_interceptor(m_filter);
	m_profile->setUrlRequestInterceptor(m_interceptor);

	// The cookie filter is the same decision as any other request, so it goes
	// through the same shared filter; only unpacking Qt's FilterRequest is
	// specific to this backend.
	request_filter *f = m_filter;
	m_profile->cookieStore()->setCookieFilter(
	  [f](const QWebEngineCookieStore::FilterRequest &r) {
		  return f->allow_cookie(r.firstPartyUrl.host(), r.thirdParty);
	  });

	// Links that are not pages (sec 11.4). Installed per scheme main() registered;
	// with none registered this does nothing and such links behave as before.
	if (!g_custom_schemes.isEmpty()) {
		m_scheme_handler = new magnet_scheme_handler;
		m_scheme_handler->on_url = [this](const QUrl &u) {
			if (m_external)
				m_external(u);
		};
		for (const QString &name : g_custom_schemes)
			m_profile->installUrlSchemeHandler(name.toUtf8(), m_scheme_handler);
	}
}

void qtwebengine_factory::set_external_url_handler(external_url_handler fn) {
	m_external = std::move(fn);
}

void qtwebengine_factory::set_download_handler(download_note fn) {
	m_download_note = std::move(fn);
}

namespace {

// One clear in flight.
//
// It has to outlive the call that started it, because not one of the three
// stores empties synchronously and the three answer in three different ways:
// `clearHttpCache()` answers with `clearHttpCacheCompleted`, `deleteAllCookies()`
// answers with one `cookieRemoved` per cookie and no "that was the last one",
// and `clearAllVisitedLinks()` answers with nothing whatsoever. So this owns
// the connections, the counting and the two timers, and reports once.
//
// **Counting the removals is what makes the report evidence rather than a
// claim.** "The call returned" says only that Qt accepted the request. A
// number that came from watching cookies leave is a measurement, and it is the
// difference between a dialog that says "cleared" and one that says what went.
class clear_run : public QObject {
public:
	clear_run(QWebEngineProfile *profile, web_view_factory::clear_note done)
		: QObject(profile), m_profile(profile), m_done(std::move(done)) {}

	void start(const web_view_factory::browsing_data &what) {
		using state = web_view_factory::clear_state;

		// Both timers exist before anything is asked to clear. A removal
		// notification arriving during `deleteAllCookies()` would otherwise
		// reach a null settle timer, and the ordering that makes that
		// impossible is Chromium's rather than ours.
		m_settle = new QTimer(this);
		m_settle->setSingleShot(true);
		connect(m_settle, &QTimer::timeout, this, [this] {
			m_settled = true;
			m_cookies_pending = false;
			m_report.cookies = m_report.cookies_removed > 0
				? web_view_factory::clear_state::done
				// Nothing was seen to go. That is the truthful answer whether the
				// jar was empty or the store never told us, and the two cannot be
				// told apart from here, so the note says so rather than the state
				// claiming more than was observed.
				: web_view_factory::clear_state::unconfirmed;
			if (m_report.cookies_removed == 0)
				m_report.notes << QStringLiteral(
					"Cookies: none were seen to go. Either there were none, or "
					"the store had none loaded to report on.");
			maybe_report();
		});

		// **A deadline, so that a report always arrives.** A clear that never
		// answers leaves a dialog saying "Clearing..." for ever, which is the
		// silence this feature was added to remove, wearing a progress
		// message.
		auto *deadline = new QTimer(this);
		deadline->setSingleShot(true);
		connect(deadline, &QTimer::timeout, this, [this] { report_now(); });
		deadline->start(k_deadline_ms);

		if (what.visited_links) {
			// No completion signal and no count anywhere in the Qt API, so
			// this is `unconfirmed` for ever. Saying `done` would be inventing
			// a fact about work nobody can observe.
			m_profile->clearAllVisitedLinks();
			m_report.visited_links = state::unconfirmed;
			m_report.notes << QStringLiteral(
				"Visited links: the engine was told to drop them. Qt reports no "
				"completion for this one, so it is not confirmed here.");
		}

		if (what.cache) {
			m_cache_pending = true;
			connect(m_profile, &QWebEngineProfile::clearHttpCacheCompleted,
				       this, [this] {
				m_cache_pending = false;
				m_report.cache = state::done;
				maybe_report();
			});
			m_profile->clearHttpCache();
		}

		if (what.cookies) {
			m_cookies_pending = true;
			m_report.cookies_removed = 0;
			QWebEngineCookieStore *store = m_profile->cookieStore();
			connect(store, &QWebEngineCookieStore::cookieRemoved, this,
				       [this](const QNetworkCookie &) {
				++m_report.cookies_removed;
				// Each removal pushes the settle window out, so a long jar
				// does not get cut off part-way through being counted.
				m_settle->start(k_settle_ms);
			});
			// The store only notifies about a jar it has loaded; a profile
			// whose pages have not been visited this run may not have loaded
			// one yet, and deleting from it would then be silent and
			// uncountable.
			store->loadAllCookies();
			store->deleteAllCookies();
		}

		if (m_cookies_pending)
			m_settle->start(k_settle_ms);
		else
			m_settled = true;

		// Nothing asked for, or nothing that waits: answer at once rather than
		// after the settle window, so a caller is not made to wait on work that
		// was never started.
		if (!m_cookies_pending && !m_cache_pending)
			QTimer::singleShot(0, this, [this] { report_now(); });
	}

private:
	// Long enough that a jar of a few hundred cookies is counted in one window
	// -- the removals arrive in a burst -- and short enough that a person who
	// pressed a button gets an answer while still looking at it.
	static constexpr int k_settle_ms   = 500;
	static constexpr int k_deadline_ms = 10000;

	void maybe_report() {
		if (m_settled && !m_cache_pending)
			report_now();
	}

	void report_now() {
		if (m_reported)
			return;
		m_reported = true;
		using state = web_view_factory::clear_state;
		// Whatever was still in flight when the deadline arrived is reported
		// as unconfirmed, not as done. It may well have finished afterwards;
		// what is certain is that nobody here saw it.
		if (m_cache_pending) {
			m_report.cache = state::unconfirmed;
			m_report.notes << QStringLiteral(
				"Cached files: the engine did not report the clear finished "
				"within ten seconds. It may still be running.");
		}
		if (m_cookies_pending) {
			m_report.cookies = state::unconfirmed;
			m_report.notes << QStringLiteral(
				"Cookies: still being removed after ten seconds; the count is "
				"what had gone by then.");
		}
		if (m_done)
			m_done(m_report);
		deleteLater();
	}

	QWebEngineProfile           *m_profile = nullptr;
	web_view_factory::clear_note m_done;
	web_view_factory::clear_report m_report;
	QTimer *m_settle = nullptr;
	bool m_cache_pending   = false;
	bool m_cookies_pending = false;
	bool m_settled         = false;
	bool m_reported        = false;
};

}  // namespace

void qtwebengine_factory::clear_browsing_data(const browsing_data &what,
                                               clear_note done) {
	if (!m_profile) {
		clear_report r;
		r.notes << QStringLiteral("There is no web engine profile to clear.");
		if (done)
			done(r);
		return;
	}

	// **What this does not clear, said out loud rather than left to be
	// discovered.** localStorage, IndexedDB and service-worker storage are
	// where a lot of what people mean by "still logged in" actually lives, and
	// Qt 6.8 exposes no call that empties them: `QWebEngineProfile` offers
	// exactly `clearHttpCache`, `clearAllVisitedLinks` and `clearVisitedLinks`,
	// and `QWebEngineCookieStore` offers `deleteAllCookies` and
	// `deleteSessionCookies`. Chromium's own BrowsingDataRemover is not
	// wrapped. Deleting the files instead was rejected: on Linux an unlink
	// succeeds against a database the engine still has open, so the engine
	// keeps writing to a file nothing can reach and the directory comes back
	// inconsistent -- a corrupted profile in exchange for a checkbox.
	//
	// So the note names the gap and the directory, because a person who needs
	// it gone can quit and delete it, and cannot do that without being told.
	if (what.cookies) {
		auto *run = new clear_run(m_profile, [this, done](
		    const clear_report &r) {
			clear_report out = r;
			out.notes << QString(
			  "Site data (localStorage, IndexedDB, service workers) is not "
			  "cleared: Qt 6.8 has no call for it, and deleting the files "
			  "under a running engine corrupts the profile. It is in %1.")
			  .arg(m_profile ? m_profile->persistentStoragePath() : QString());
			if (done)
				done(out);
		});
		run->start(what);
		return;
	}

	auto *run = new clear_run(m_profile, std::move(done));
	run->start(what);
}


qtwebengine_factory::~qtwebengine_factory() {
	// Hooks off first: the interceptor is about to be deleted and the profile
	// must not be left holding a pointer to it for even one statement.
	if (m_profile) {
		m_profile->setUrlRequestInterceptor(nullptr);
		m_profile->cookieStore()->setCookieFilter(nullptr);
	}
	delete m_interceptor;
	// **Ours now, so we delete it** -- it used to be Qt's default profile,
	// which must not be deleted. Reaching here means the window and every page
	// went first, which `main()`'s declaration order guarantees.
	delete m_profile;
	m_profile = nullptr;
}

web_view_backend *qtwebengine_factory::create_view(QWidget *parent) {
	return new qtwebengine_view(m_profile, parent);
}

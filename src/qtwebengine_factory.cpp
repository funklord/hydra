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
	m_profile = QWebEngineProfile::defaultProfile();

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


qtwebengine_factory::~qtwebengine_factory() {
	// The profile outlives us (it is Qt's default one), so take our hooks back
	// off it before the interceptor goes away.
	if (m_profile) {
		m_profile->setUrlRequestInterceptor(nullptr);
		m_profile->cookieStore()->setCookieFilter(nullptr);
	}
	delete m_interceptor;
}

web_view_backend *qtwebengine_factory::create_view(QWidget *parent) {
	return new qtwebengine_view(m_profile, parent);
}

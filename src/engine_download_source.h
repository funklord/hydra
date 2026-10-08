#pragma once

#include "download_source.h"
#include "web_view_factory.h"

#include <QHash>

// A source standing for a download the ENGINE is running.
//
// **It transfers nothing, and that is the point.** `qtwebengine_factory`
// records why the engine keeps a page download rather than handing the url to
// the manager: it may be a `blob:` the page built in memory, or a url that
// means nothing without the session's cookies, so refetching from outside the
// engine gets a login page. The transfer therefore cannot move here -- only
// the bookkeeping can, which is exactly what `download_manager::adopt` exists
// for and says so: "the transport is running, so finished() must be able to
// retire it and sweep() must never try to start it".
//
// Architecture doc sec 11.2 asks for "one manager fed by two sources --
// page-initiated downloads via QWebEngineProfile::downloadRequested, and
// detector-initiated media saves". The second was wired and the first was
// not, so a page download appeared in the status bar for twelve seconds and
// nowhere else: not in the downloads window, and not in the history, because
// there was never a row. That is what this closes.
//
// **No Q_OBJECT and no moc.** It declares no signals or slots of its own and
// emits `download_source`'s inherited `progressed()` and `finished()`, which
// is what lets it be a header with no translation unit of its own -- the same
// shape `test/fake_download_source.h` uses.
class engine_download_source : public download_source {
public:
	explicit engine_download_source(web_view_factory *factory,
	                                 QObject *parent = nullptr)
	    : download_source(parent), m_factory(factory) {}

	QString id() const override { return QStringLiteral("engine"); }
	QString display_name() const override { return QStringLiteral("Page"); }

	// Defaults, deliberately: the engine resumes nothing across a restart and
	// exposes no pause, so claiming either would offer the downloads window a
	// button that calls an empty body -- the fault `pausable` was added to
	// stop. Cancel is not a capability; it is the override below.
	source_capabilities capabilities() const override { return {}; }

	// **Never accepts a url.** Nothing may be routed here, because there is
	// nothing to start: a job arrives only through `adopt`, already running.
	// Saying so rather than returning a bare false, since the manager hands
	// this reason to whoever asked.
	bool accepts(const QUrl &, QString *why_not = nullptr) const override {
		if (why_not)
			*why_not = QStringLiteral("a page download is the page's to begin");
		return false;
	}

	// Unreachable while `accepts` is false, and it refuses rather than
	// asserting: a source whose `start` cannot be called is one line away
	// from a source whose `start` is called by a path nobody predicted.
	bool start(const download_request &, QString *error) override {
		if (error)
			*error = QStringLiteral("a page download is already under way");
		return false;
	}

	// Back to the engine, which is the only thing holding the transfer.
	void cancel(int job_id) override {
		if (!m_factory)
			return;
		const quint64 eid = m_engine_ids.value(job_id, 0);
		if (eid)
			m_factory->cancel_engine_download(eid);
	}

	// Told by the shell once, when it adopts the job the first note produced.
	void adopted(int job_id, quint64 engine_id) {
		m_engine_ids.insert(job_id, engine_id);
	}

	// And on every note after that. Progress while it runs, and the terminal
	// answer once, which is what retires the job and writes the history.
	void report(int job_id, const web_view_factory::engine_download &d) {
		if (!d.finished) {
			download_progress p;
			p.received = d.received;
			p.total    = d.total;
			p.path     = d.path;
			emit progressed(job_id, p);
			return;
		}
		// Dropped before the signal, not after: `on_finished` is synchronous
		// and a Cancel arriving from the window during it would otherwise
		// reach an engine id whose request has gone.
		m_engine_ids.remove(job_id);
		emit finished(job_id, d.ok, d.why);
	}

	// Whether a note belongs to a job this source already knows.
	int job_for(quint64 engine_id) const {
		for (auto it = m_engine_ids.cbegin(); it != m_engine_ids.cend(); ++it)
			if (it.value() == engine_id)
				return it.key();
		return 0;
	}

private:
	web_view_factory   *m_factory = nullptr;
	QHash<int, quint64> m_engine_ids;
};

#include "download_manager.h"

#include <QStandardPaths>
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

download_manager::download_manager(QObject *parent) : QObject(parent) {
	m_dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

void download_manager::add_source(download_source *source) {
	if (!source || m_sources.contains(source))
		return;
	source->setParent(this);
	m_sources.push_back(source);
	connect(source, &download_source::progressed, this, &download_manager::on_progress);
	connect(source, &download_source::finished,   this, &download_manager::on_finished);
}

download_source *download_manager::source_by_id(const QString &id) const {
	for (download_source *s : m_sources)
		if (s->id() == id)
			return s;
	return nullptr;
}

download_source *download_manager::source_for(const QUrl &url) const {
	for (download_source *s : m_sources)
		if (s->accepts(url))
			return s;
	return nullptr;
}

download_job *download_manager::find(int id) {
	for (download_job &j : m_jobs)
		if (j.id == id)
			return &j;
	return nullptr;
}

int download_manager::live_count(const QString &source_id) const {
	int n = 0;
	for (const download_job &j : m_jobs)
		if (j.source_id == source_id && m_live.contains(j.id))
			++n;
	return n;
}

bool download_manager::has_consent(const QString &source_id) const {
	return m_consented.contains(source_id);
}

void download_manager::set_consent(const QString &source_id, bool granted) {
	if (granted)
		m_consented.insert(source_id);
	else
		m_consented.remove(source_id);
	pump();
}

int download_manager::enqueue(const QUrl &url, const QString &node_id,
                               QString *error,
                               const QMap<QString, QString> &headers) {
	// **The same address twice is one download, not two.**
	//
	// Reported from use: clicking a magnet link sometimes produced two rows
	// in the downloads list and sometimes one. There was no check here at
	// all -- a source accepted the url and a job was built, however many
	// times it was asked -- so two calls made two jobs of the same transfer.
	// Why the engine's external-url handler fires twice for some links and
	// once for others is a separate question and still open; this is the
	// half that can be answered where the jobs are made.
	//
	// **Only while the existing one is still going.** `is_terminal` is done,
	// failed or cancelled, and a job in one of those states is history: asking
	// for that address again is a retry somebody meant, and refusing it would
	// make a failed download unrepeatable.
	//
	// The existing id rather than 0, because 0 is this function's failure
	// value and the caller turns it into "Nothing here can download that".
	// A second click is not an error -- it is a request to see the download
	// that is already running, which is what the caller does with an id.
	for (const download_job &j : m_jobs)
		if (j.url == url && !j.terminal())
			return j.id;

	// First source that accepts wins. If none does, report the most specific
	// reason offered rather than a generic refusal -- the sources know why.
	download_source *chosen = nullptr;
	QString first_reason;
	for (download_source *s : m_sources) {
		QString why_not;
		if (s->accepts(url, &why_not)) {
			chosen = s;
			break;
		}
		if (first_reason.isEmpty() && !why_not.isEmpty())
			first_reason = why_not;
	}
	if (!chosen) {
		if (error)
			*error = first_reason.isEmpty()
			             ? QString("Nothing here can download that address.")
			             : first_reason;
		return 0;
	}

	const source_capabilities caps = chosen->capabilities();

	download_job job;
	job.id                   = m_next_id++;
	job.url                  = url;
	job.node_id              = node_id;
	job.source_id            = chosen->id();
	job.status               = download_state::queued;
	job.public_participation = caps.public_participation;
	job.headers              = headers;

	m_jobs.push_back(job);
	emit changed();

	// Ask before the first publicly-observable job of this source, not after.
	if (caps.public_participation && !has_consent(job.source_id))
		emit consent_required(job.source_id, caps.participation_note, job.id);

	pump();
	return job.id;
}

int download_manager::adopt(download_source *source, const QUrl &url,
                             const QString &node_id) {
	if (!source)
		return 0;
	if (!m_sources.contains(source))
		add_source(source);

	download_job job;
	job.id                   = m_next_id++;
	job.url                  = url;
	job.node_id              = node_id;
	job.source_id            = source->id();
	job.status               = download_state::running;
	job.public_participation = source->capabilities().public_participation;

	m_jobs.push_back(job);
	// Live from the outset: the transport is running, so finished() must be
	// able to retire it and sweep() must never try to start it.
	m_live.insert(job.id);
	emit changed();
	return job.id;
}

void download_manager::pump() {
	// A source may fail synchronously inside start(), which lands in
	// on_finished() and calls back in here mid-iteration. Rather than reason
	// about what that does to the loop, coalesce: the inner call asks for
	// another sweep and the outer one performs it.
	if (m_pumping) {
		m_pump_again = true;
		return;
	}
	m_pumping = true;
	do {
		m_pump_again = false;
		sweep();
	} while (m_pump_again);
	m_pumping = false;
}

void download_manager::sweep() {
	for (download_job &j : m_jobs) {
		if (j.status != download_state::queued)
			continue;
		download_source *s = source_by_id(j.source_id);
		if (!s)
			continue;
		const source_capabilities caps = s->capabilities();
		if (caps.public_participation && !has_consent(j.source_id))
			continue;   // held until the user is told and agrees
		if (live_count(j.source_id) >= qMax(1, caps.max_concurrent))
			continue;

		download_request req;
		req.id        = j.id;
		req.url       = j.url;
		req.directory = m_dir;
		req.node_id   = j.node_id;
		req.headers   = j.headers;

		QString error;
		m_live.insert(j.id);
		if (!s->start(req, &error)) {
			m_live.remove(j.id);
			j.status = download_state::failed;
			j.error  = error;
			emit changed();
			continue;
		}
		// A source may have moved the job along synchronously inside start();
		// only claim "running" if it did not say otherwise.
		if (j.status == download_state::queued)
			j.status = download_state::running;
		emit changed();
	}
}

void download_manager::on_progress(int id, const download_progress &p) {
	download_job *j = find(id);
	if (!j || j->terminal())
		return;
	j->received = p.received;
	if (p.total >= 0)
		j->total = p.total;
	if (!p.path.isEmpty())
		j->path = p.path;
	if (!p.files.isEmpty())
		j->files = p.files;
	j->detail = p.detail;
	j->status = p.state;
	emit changed();
}

void download_manager::on_finished(int id, bool ok, const QString &message) {
	m_live.remove(id);
	if (download_job *j = find(id)) {
		if (j->status == download_state::cancelled) {
			// A cancellation is the user's decision and final. A source that
			// reports success afterwards is describing what it managed before
			// stopping, not undoing the cancel -- and this used to overwrite it,
			// so cancelling a capture showed as "Complete". HTTP never exposed
			// it because aborting a reply reports failure; a capture reports
			// success whenever any bytes were written.
		} else if (ok) {
			j->status = download_state::done;
		} else {
			j->status = download_state::failed;
			j->error  = message;
		}
	}
	emit changed();
	persist_history();
	pump();
}

void download_manager::cancel(int id) {
	download_job *j = find(id);
	if (!j || j->terminal())
		return;
	if (m_live.contains(id)) {
		j->status = download_state::cancelled;
		if (download_source *s = source_by_id(j->source_id))
			s->cancel(id);           // finished() cleans up and pumps
	} else {
		j->status = download_state::cancelled;
		emit changed();
		persist_history();
	}
}

bool download_manager::forget(int id) {
	for (int i = 0; i < m_jobs.size(); ++i) {
		if (m_jobs[i].id != id)
			continue;
		if (!m_jobs[i].terminal())
			return false;   // a running download does not vanish from the list
		m_jobs.removeAt(i);
		emit changed();
		persist_history();
		return true;
	}
	return false;
}

int download_manager::forget_finished() {
	int removed = 0;
	// Backwards, so removing one does not shift an index still to be
	// checked. terminal() is the same test forget() uses, so a live job --
	// queued or running -- is left exactly as Remove leaves it.
	for (int i = m_jobs.size() - 1; i >= 0; --i) {
		if (m_jobs[i].terminal()) {
			m_jobs.removeAt(i);
			++removed;
		}
	}
	if (removed > 0) {
		emit changed();
		persist_history();
	}
	return removed;
}

static QString state_name(download_state s) {
	switch (s) {
	case download_state::done:      return QStringLiteral("done");
	case download_state::failed:    return QStringLiteral("failed");
	case download_state::cancelled: return QStringLiteral("cancelled");
	default:                        return QString();
	}
}

static download_state state_from_name(const QString &n) {
	if (n == QLatin1String("done"))      return download_state::done;
	if (n == QLatin1String("failed"))    return download_state::failed;
	if (n == QLatin1String("cancelled")) return download_state::cancelled;
	return download_state::queued;   // a non-terminal sentinel, rejected on load
}

void download_manager::persist_history() {
	if (!m_history_path.isEmpty())
		save_history(m_history_path);
}

void download_manager::save_history(const QString &path) const {
	QList<const download_job *> hist;
	for (const download_job &j : m_jobs)
		if (j.terminal())
			hist << &j;
	// Bound the file: keep the most recent, so a long-lived profile does not
	// accumulate a download record without limit.
	const int cap = 200;
	const int from = hist.size() > cap ? hist.size() - cap : 0;
	QJsonArray arr;
	for (int i = from; i < hist.size(); ++i) {
		const download_job &j = *hist[i];
		QJsonObject o;
		o.insert("url",    j.url.toString());
		o.insert("source", j.source_id);
		o.insert("path",   j.path);
		o.insert("node",   j.node_id);
		o.insert("received", double(j.received));
		o.insert("total",    double(j.total));
		o.insert("status", state_name(j.status));
		if (!j.error.isEmpty())
			o.insert("error", j.error);
		o.insert("public", j.public_participation);
		arr.append(o);
	}
	// Atomic, so a crash mid-write cannot leave a half-file that fails to parse
	// and loses the whole history.
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return;
	f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
	f.commit();
}

void download_manager::load_history(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return;
	const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
	for (const QJsonValue &v : arr) {
		const QJsonObject o = v.toObject();
		download_job j;
		j.status = state_from_name(o.value("status").toString());
		if (!j.terminal())
			continue;   // only history is restored, never a fake running row
		j.id        = m_next_id++;
		j.url       = QUrl(o.value("url").toString());
		j.source_id = o.value("source").toString();
		j.path      = o.value("path").toString();
		j.node_id   = o.value("node").toString();
		j.received  = qint64(o.value("received").toDouble());
		j.total     = qint64(o.value("total").toDouble(-1));
		j.error     = o.value("error").toString();
		j.public_participation = o.value("public").toBool();
		m_jobs.push_back(j);
	}
	emit changed();
}

void download_manager::pause(int id) {
	download_job *j = find(id);
	if (!j || !m_live.contains(id))
		return;
	download_source *s = source_by_id(j->source_id);
	if (!s || !s->capabilities().pausable)
		return;
	s->pause(id);
}

void download_manager::unpause(int id) {
	download_job *j = find(id);
	if (!j)
		return;
	download_source *s = source_by_id(j->source_id);
	if (!s || !s->capabilities().pausable)
		return;
	if (m_live.contains(id)) {
		s->unpause(id);
	} else if (j->status == download_state::paused) {
		j->status = download_state::queued;
		pump();
	}
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "download_manager.h"

#include <QStandardPaths>

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

int download_manager::enqueue(const QUrl &url, const QString &node_id, QString *error) {
	// First source that accepts wins. If none does, report the most specific
	// reason offered rather than a generic refusal — the sources know why.
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
		if (ok) {
			j->status = download_state::done;
		} else if (j->status != download_state::cancelled) {
			j->status = download_state::failed;
			j->error  = message;
		}
	}
	emit changed();
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
	}
}

void download_manager::pause(int id) {
	download_job *j = find(id);
	if (!j || !m_live.contains(id))
		return;
	download_source *s = source_by_id(j->source_id);
	if (!s || !s->capabilities().resumable)
		return;
	s->pause(id);
}

void download_manager::unpause(int id) {
	download_job *j = find(id);
	if (!j)
		return;
	download_source *s = source_by_id(j->source_id);
	if (!s || !s->capabilities().resumable)
		return;
	if (m_live.contains(id)) {
		s->unpause(id);
	} else if (j->status == download_state::paused) {
		j->status = download_state::queued;
		pump();
	}
}

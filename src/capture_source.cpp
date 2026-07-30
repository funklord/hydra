// SPDX-License-Identifier: GPL-3.0-or-later
#include "capture_source.h"

#include <QFileInfo>

void capture_source::began(int job_id, const QString &path) {
	m_path = path;
	download_progress p;
	p.state    = download_state::running;
	p.path     = path;
	p.received = 0;
	// No total, and there will not be one: a recording's length is not known
	// until it stops. The list draws that as a busy bar, which is the honest
	// picture rather than a percentage of an invented figure.
	p.total    = -1;
	p.detail   = "waiting for the page to start playing";
	emit progressed(job_id, p);
}

void capture_source::progressed_to(int job_id, qint64 bytes, const QString &detail) {
	download_progress p;
	p.state    = download_state::running;
	p.path     = m_path;
	p.received = bytes;
	p.total    = -1;
	p.detail   = detail;
	emit progressed(job_id, p);
}

void capture_source::ended(int job_id, bool ok, const QString &message) {
	if (ok) {
		// Now the length is known, so say it: a finished recording should show
		// as complete rather than as something still in flight.
		const qint64 size = QFileInfo(m_path).size();
		download_progress p;
		p.state    = download_state::done;
		p.path     = m_path;
		p.received = size;
		p.total    = size;
		emit progressed(job_id, p);
	}
	emit finished(job_id, ok, message);
}

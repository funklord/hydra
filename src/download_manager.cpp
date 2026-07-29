// SPDX-License-Identifier: GPL-3.0-or-later
#include "download_manager.h"
#include "media_detector.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

download_manager::download_manager(QObject *parent) : QObject(parent) {
	m_net = new QNetworkAccessManager(this);
	m_dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

download_job *download_manager::find(int id) {
	for (download_job &j : m_jobs)
		if (j.id == id)
			return &j;
	return nullptr;
}

int download_manager::enqueue(const QUrl &url, const QString &node_id, QString *error) {
	bool saveable = false;
	const media_kind kind = media_detector::classify(url, &saveable);
	if (kind == media_kind::hls || kind == media_kind::dash) {
		// Fetching a manifest would save the playlist text, not the video.
		// Refuse plainly rather than produce a file that looks like a failure.
		if (error)
			*error = "Streamed media (HLS/DASH) cannot be saved yet — segment "
			         "assembly is not implemented. Try \"Watch in player\".";
		return 0;
	}
	if (!saveable) {
		if (error)
			*error = "That URL does not look like a saveable file.";
		return 0;
	}

	QDir().mkpath(m_dir);

	download_job job;
	job.id      = m_next_id++;
	job.url     = url;
	job.node_id = node_id;

	QString name = QFileInfo(url.path()).fileName();
	if (name.isEmpty())
		name = "download";
	// Never let a remote path escape the download directory.
	name = QFileInfo(name).fileName();
	job.path = QDir(m_dir).filePath(name);

	m_jobs.push_back(job);
	emit changed();
	pump();
	return job.id;
}

void download_manager::pump() {
	if (m_active)
		return;
	for (download_job &j : m_jobs) {
		if (j.status == download_job::queued) {
			start(j);
			return;
		}
	}
}

void download_manager::start(download_job &job) {
	job.status = download_job::running;
	m_active   = job.id;

	QNetworkRequest req(job.url);

	// Resume: if a partial file is already there, ask for the rest.
	QFileInfo fi(job.path);
	const qint64 have = fi.exists() ? fi.size() : 0;
	if (have > 0) {
		req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(have) + "-");
		job.received = have;
	}

	m_file = new QFile(job.path, this);
	if (!m_file->open(have > 0 ? (QIODevice::Append | QIODevice::WriteOnly)
	                           : (QIODevice::Truncate | QIODevice::WriteOnly))) {
		job.status = download_job::failed;
		job.error  = "Cannot write " + job.path;
		delete m_file;
		m_file = nullptr;
		m_active = 0;
		emit changed();
		pump();
		return;
	}

	m_reply = m_net->get(req);
	QNetworkReply *reply = m_reply;
	const int id = job.id;

	connect(reply, &QNetworkReply::readyRead, this, [this, reply, id] {
		if (m_file)
			m_file->write(reply->readAll());
		if (download_job *j = find(id))
			j->received = m_file ? m_file->size() : j->received;
		emit changed();
	});
	connect(reply, &QNetworkReply::downloadProgress, this,
	         [this, id](qint64 got, qint64 total) {
		if (download_job *j = find(id)) {
			// With a resumed request the server reports only the remaining
			// bytes, so add back what was already on disk.
			const qint64 base = (j->status == download_job::running && total > 0)
			                        ? j->received - got : 0;
			j->total = (total > 0) ? total + qMax<qint64>(0, base) : -1;
		}
		emit changed();
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply, id] {
		reply->deleteLater();
		if (m_file) {
			m_file->write(reply->readAll());
			m_file->close();
			delete m_file;
			m_file = nullptr;
		}
		if (download_job *j = find(id)) {
			if (j->status == download_job::cancelled) {
				// leave as cancelled
			} else if (reply->error() != QNetworkReply::NoError) {
				j->status = download_job::failed;
				j->error  = reply->errorString();
			} else {
				j->status   = download_job::done;
				j->received = QFileInfo(j->path).size();
				j->total    = j->received;
			}
		}
		m_active = 0;
		emit changed();
		pump();
	});
}

void download_manager::cancel(int id) {
	download_job *j = find(id);
	if (!j)
		return;
	if (j->status == download_job::running && m_reply) {
		j->status = download_job::cancelled;
		m_reply->abort();          // finished() cleans up and pumps
	} else if (j->status == download_job::queued) {
		j->status = download_job::cancelled;
		emit changed();
	}
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

struct download_job {
	int      id = 0;
	QUrl     url;
	QString  path;          // destination on disk
	QString  node_id;       // the tree node it came from (§11.2), may be empty
	qint64   received = 0;
	qint64   total    = -1; // -1 until Content-Length is known
	enum state { queued, running, done, failed, cancelled } status = queued;
	QString  error;
};

// One queue fed by two sources (architecture doc §11.2): media the detector
// found, and downloads the page itself initiated. Jobs are organised against
// the tab tree — a download belongs to the node it came from.
//
// Deliberately modest for now: direct files only, one at a time, with resume
// via a Range request when a partial file is already on disk. Segmented-stream
// assembly and the ffmpeg remux (§11.2) are the next increment and are not
// here; the manager refuses a manifest rather than downloading the playlist
// text and calling it a video.
class download_manager : public QObject {
	Q_OBJECT
public:
	explicit download_manager(QObject *parent = nullptr);

	void set_directory(const QString &dir) { m_dir = dir; }
	QString directory() const { return m_dir; }

	// Returns the job id, or 0 if the request was refused (see `error`).
	int enqueue(const QUrl &url, const QString &node_id, QString *error);
	void cancel(int id);

	const QList<download_job> &jobs() const { return m_jobs; }

signals:
	void changed();   // any job's state or progress moved

private:
	void pump();      // start the next queued job if idle
	void start(download_job &job);
	download_job *find(int id);

	QNetworkAccessManager *m_net = nullptr;
	QPointer<QNetworkReply> m_reply;
	QFile  *m_file = nullptr;
	int     m_active = 0;      // job id currently running, 0 if idle
	int     m_next_id = 1;
	QString m_dir;
	QList<download_job> m_jobs;
};

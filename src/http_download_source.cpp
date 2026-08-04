// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_download_source.h"
#include "media_detector.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

http_download_source::http_download_source(QObject *parent)
  : download_source(parent) {
	m_net = new QNetworkAccessManager(this);
}

http_download_source::~http_download_source() {
	// abort() delivers finished() synchronously, which lands in the lambda in
	// wire() and calls teardown() — which removes from m_transfers and deletes
	// the transfer. Doing that while iterating m_transfers invalidates the
	// iterator and double-frees. So empty the map first: the callbacks then
	// find nothing and return, and this loop owns every transfer outright.
	const QList<transfer *> orphans = m_transfers.values();
	m_transfers.clear();
	for (transfer *t : orphans) {
		if (t->reply) {
			t->reply->disconnect(this);
			t->reply->abort();
		}
		delete t->file;
		delete t;
	}
}

bool http_download_source::accepts(const QUrl &url, QString *why_not) const {
	const QString scheme = url.scheme().toLower();
	if (scheme != "http" && scheme != "https") {
		if (why_not)
			*why_not = "Not an HTTP address.";
		return false;
	}

	bool saveable = false;
	const media_kind kind = media_detector::classify(url, &saveable);
	if (kind == media_kind::hls || kind == media_kind::dash) {
		// Fetching a manifest would save the playlist text, not the video.
		// Refuse plainly rather than produce a file that looks like a failure.
		if (why_not)
			*why_not = "Streamed media (HLS/DASH) cannot be saved yet — segment "
			           "assembly is not implemented. Try \"Watch in player\".";
		return false;
	}
	if (!saveable) {
		if (why_not)
			*why_not = "That URL does not look like a saveable file.";
		return false;
	}
	return true;
}

bool http_download_source::start(const download_request &req, QString *error) {
	if (m_transfers.contains(req.id)) {
		if (error)
			*error = "That job is already running.";
		return false;
	}

	QDir().mkpath(req.directory);

	QString name = QFileInfo(req.url.path()).fileName();
	if (name.isEmpty())
		name = "download";
	// Never let a remote path escape the download directory.
	name = QFileInfo(name).fileName();

	auto *t = new transfer;
	t->id   = req.id;
	t->path = QDir(req.directory).filePath(name);

	QNetworkRequest net_req(req.url);

	// Whatever this address needs to be served at all. Range is ours and is
	// set below, so a caller cannot overwrite the resume offset by naming it.
	for (auto it = req.headers.cbegin(); it != req.headers.cend(); ++it) {
		if (it.key().isEmpty() || it.value().isEmpty())
			continue;
		if (it.key().compare("Range", Qt::CaseInsensitive) == 0)
			continue;
		net_req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
	}

	// Resume: if a partial file is already there, ask for the rest.
	const QFileInfo fi(t->path);
	const qint64 have = fi.exists() ? fi.size() : 0;
	if (have > 0) {
		net_req.setRawHeader("Range",
		                      QByteArray("bytes=") + QByteArray::number(have) + "-");
		t->base = have;
	}

	t->file = new QFile(t->path);
	if (!t->file->open(have > 0 ? (QIODevice::Append | QIODevice::WriteOnly)
	                            : (QIODevice::Truncate | QIODevice::WriteOnly))) {
		if (error)
			*error = "Cannot write " + t->path;
		delete t->file;
		delete t;
		return false;
	}

	t->reply = m_net->get(net_req);
	m_transfers.insert(t->id, t);
	wire(t);

	download_progress p;
	p.state    = download_state::running;
	p.path     = t->path;
	p.received = t->base;
	emit progressed(t->id, p);
	return true;
}

void http_download_source::wire(transfer *t) {
	QNetworkReply *reply = t->reply;
	const int id = t->id;

	// Did the server actually honour the Range we asked for?
	//
	// **It does not have to, and many do not** — a server with no Range support
	// answers 200 with the whole body, which is a correct HTTP response and a
	// disaster for a file opened in Append: the complete body lands after the
	// bytes already on disk and the result is a file of exactly twice the right
	// size, reported as 100% because the byte count is whatever is on disk.
	// Measured, on a phone, by downloading the same 195 KiB file twice and
	// watching the download list say 390.6 KiB.
	//
	// Only 206 means "the rest of it". Anything else means "all of it", so the
	// resume is abandoned and the file starts again from nothing.
	const auto honour_range = [this, reply, id] {
		transfer *t = find(id);
		if (!t || !t->file || t->base == 0 || t->range_checked)
			return;
		t->range_checked = true;
		const int code =
		  reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (code == 206)
			return;
		t->file->seek(0);
		t->file->resize(0);
		t->base = 0;
	};

	connect(reply, &QNetworkReply::metaDataChanged, this, honour_range);

	connect(reply, &QNetworkReply::readyRead, this, [this, reply, id, honour_range] {
		transfer *t = find(id);
		if (!t || !t->file)
			return;
		// Before the first write, in case metaDataChanged never arrived.
		honour_range();
		t->file->write(reply->readAll());
		download_progress p;
		p.state    = download_state::running;
		p.path     = t->path;
		p.received = t->file->size();
		emit progressed(id, p);
	});

	connect(reply, &QNetworkReply::downloadProgress, this,
	         [this, id](qint64 got, qint64 total) {
		transfer *t = find(id);
		if (!t)
			return;
		download_progress p;
		p.state    = download_state::running;
		p.path     = t->path;
		p.received = t->file ? t->file->size() : t->base;
		// With a resumed request the server reports only the remaining bytes,
		// so add back what was already on disk.
		Q_UNUSED(got)
		p.total = (total > 0) ? total + t->base : -1;
		emit progressed(id, p);
	});

	connect(reply, &QNetworkReply::finished, this, [this, reply, id, honour_range] {
		transfer *t = find(id);
		reply->deleteLater();
		if (!t)
			return;
		if (t->file) {
			honour_range();
			t->file->write(reply->readAll());
		}
		if (t->cancelled)
			teardown(t, false, "Cancelled.");
		else if (reply->error() != QNetworkReply::NoError)
			teardown(t, false, reply->errorString());
		else
			teardown(t, true, QString());
	});
}

void http_download_source::teardown(transfer *t, bool ok, const QString &message) {
	if (t->file) {
		t->file->close();
		delete t->file;
		t->file = nullptr;
	}
	if (ok) {
		download_progress p;
		p.state    = download_state::done;
		p.path     = t->path;
		p.received = QFileInfo(t->path).size();
		p.total    = p.received;
		emit progressed(t->id, p);
	}
	const int id = t->id;
	m_transfers.remove(id);
	delete t;
	emit finished(id, ok, message);
}

void http_download_source::cancel(int id) {
	transfer *t = find(id);
	if (!t)
		return;
	t->cancelled = true;
	if (t->reply)
		t->reply->abort();   // finished() does the teardown
	else
		teardown(t, false, "Cancelled.");
}

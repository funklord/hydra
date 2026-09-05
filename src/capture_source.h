#pragma once

#include "download_source.h"

// A recording in progress, as a download (architecture doc sec 11.6).
//
// A media capture is a download in every sense the user cares about: bytes
// accumulating into a file, with progress, a destination and a reason to stop
// it. It is not a download in one sense only -- nothing here fetches anything.
// The page drives the transfer and the shell watches the proxy's byte count.
//
// That is why it is adopted rather than enqueued: `download_manager::adopt()`
// registers a job whose transport is already running, and everything after
// that point is ordinary. The downloads window shows a capture beside an HTTP
// file and a torrent, with the same progress bar and the same Cancel, and
// needs to know nothing about how any of them work.
//
// `streamable` is true and means it: the file is written strictly
// front-to-back, so Watch can play a recording while it is still recording,
// through the same proxy that is receiving it.
class capture_source : public download_source {
	Q_OBJECT
public:
	explicit capture_source(QObject *parent = nullptr) : download_source(parent) {}

	QString id() const override { return "capture"; }
	QString display_name() const override { return "Media capture"; }

	source_capabilities capabilities() const override {
		source_capabilities c;
		c.streamable = true;    // appended in order, so it plays as it grows
		c.resumable  = false;   // a recording cannot be resumed, only restarted
		return c;
	}

	// Never chosen by enqueue(): a capture is begun by the shell, not by
	// handing the manager an address.
	bool accepts(const QUrl &, QString *why_not = nullptr) const override {
		if (why_not)
			*why_not = "Captures are started from Tools ▸ Media ▸ Capture "
			             "Playing Video.";
		return false;
	}
	bool start(const download_request &, QString *error) override {
		if (error)
			*error = "A capture is not started this way.";
		return false;
	}

	// Driven by the shell as the proxy's byte count moves.
	void began(int job_id, const QString &path);
	void progressed_to(int job_id, qint64 bytes, const QString &detail);
	void ended(int job_id, bool ok, const QString &message);

	// Cancel comes from the downloads window, so the shell has to hear about
	// it and stop the recording -- the source cannot stop the page by itself.
	void cancel(int id) override { emit stop_requested(id); }

signals:
	void stop_requested(int job_id);

private:
	QString m_path;
};

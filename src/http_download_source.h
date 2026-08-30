#pragma once

#include "download_source.h"

#include <QHash>
#include <QPointer>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

// The transport the download manager used to contain (architecture doc sec 11.2):
// direct files over HTTP, with resume via a Range request when a partial file
// is already on disk. Behaviour is unchanged by the move -- this is the same
// code behind the sec 11.4 seam, not a reimplementation.
//
// It refuses HLS and DASH manifests, because fetching one saves the playlist
// text rather than the video. Segment assembly lives in `hls_assembler`, which
// is a separate path today; folding it in as a second source is the obvious
// next use of this interface and is not done here.
//
// Transfers are keyed by job id rather than a single active reply, so the
// serial behaviour is a *policy* (`max_concurrent = 1`) the manager enforces
// and can relax, instead of a structural limit baked into the transport.
class http_download_source : public download_source {
	Q_OBJECT
public:
	explicit http_download_source(QObject *parent = nullptr);
	~http_download_source() override;

	QString id() const override { return "http"; }
	QString display_name() const override { return "Direct download"; }

	source_capabilities capabilities() const override {
		source_capabilities c;
		c.resumable      = true;   // Range against what is already on disk
		c.max_concurrent = 1;      // unchanged from before the seam
		c.streamable     = true;   // written strictly front-to-back
		return c;
	}

	bool accepts(const QUrl &url, QString *why_not = nullptr) const override;
	bool start(const download_request &req, QString *error) override;
	void cancel(int id) override;

private:
	struct transfer {
		int      id = 0;
		QFile   *file = nullptr;
		QPointer<QNetworkReply> reply;
		qint64   base = 0;        // bytes already on disk when we resumed
		QString  path;
		bool     cancelled = false;
		// Whether the answer to "did the server honour our Range" has been read
		// yet. Asked once per transfer, from whichever of metaDataChanged,
		// readyRead or finished happens first -- a small reply can arrive
		// complete before anything has had a chance to look at its headers.
		bool     range_checked = false;
	};

	void wire(transfer *t);
	void teardown(transfer *t, bool ok, const QString &message);
	transfer *find(int id) const { return m_transfers.value(id, nullptr); }

	QNetworkAccessManager *m_net = nullptr;
	QHash<int, transfer *> m_transfers;
};

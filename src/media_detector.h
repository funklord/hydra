// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "request_filter.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QUrl>

// What kind of saveable thing a URL looks like (architecture doc §11.1).
enum class media_kind {
	direct,      // a file: .mp4/.webm/.mkv/.mp3/.m4a/.pdf …
	hls,         // an .m3u8 manifest
	dash,        // an .mpd manifest
	segment,     // .ts / .m4s — betrays a manifest we may not have seen
};

struct media_item {
	media_kind kind = media_kind::direct;
	QUrl       url;
	QString    site_host;   // the page it was found on
	QString    label;       // filename or manifest name, for the list
	int        hits = 0;    // segment counts tell us which stream is playing
};

// The first interceptor consumer (architecture doc §10/§11): it watches the
// request stream and classifies anything saveable, grouped by the page that
// requested it.
//
// Detection is URL-shaped only. The interceptor sees requests, not responses,
// so real Content-Types and manifest bodies are not available here — that is
// the optional local proxy's job (§10), and this degrades to extension and
// path heuristics without it. Obfuscated manifests are still betrayed by their
// segment requests, which is why segments are tracked at all.
//
// Thread note: on_request() arrives off the UI thread; everything here is
// guarded by a mutex and the UI reads snapshots.
class media_detector : public QObject, public request_observer {
	Q_OBJECT
public:
	explicit media_detector(QObject *parent = nullptr);

	void on_request(const request_context &ctx, const request_decision &d) override;

	// Snapshot for a page, best candidate first.
	QList<media_item> items_for(const QString &site_host) const;
	int count_for(const QString &site_host) const;

	// The primary stream: the manifest whose segments are actively being
	// fetched, else the first manifest, else the first direct file (§11.3).
	media_item primary_for(const QString &site_host) const;

	void clear_site(const QString &site_host);

	// Add something found by other means — the yt-dlp handoff (§11.5), which
	// resolves a page authoritatively rather than guessing from URL shape.
	// Deduplicated by URL, so asking twice does not double the list.
	void add_item(const QString &site_host, const media_item &item);

	static media_kind classify(const QUrl &url, bool *saveable);

signals:
	// Queued to the UI thread by Qt because the emit happens off it.
	void site_updated(const QString &site_host, int count);

private:
	mutable QMutex m_lock;
	QHash<QString, QList<media_item>> m_by_site;
};

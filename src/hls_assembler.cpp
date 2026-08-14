// SPDX-License-Identifier: GPL-3.0-or-later
#include "hls_assembler.h"

#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

hls_assembler::hls_assembler(QObject *parent) : QObject(parent) {
	m_net = new QNetworkAccessManager(this);
}

hls_assembler::~hls_assembler() {
	stop();
}

QNetworkReply *hls_assembler::get(const QUrl &url, const QByteArray &range) {
	QNetworkRequest req(url);
	// The same context injection the proxy does -- a CDN that 403s a naked
	// stream URL will 403 our segment fetches too (sec 11.3).
	if (!m_ctx.referer.isEmpty())
		req.setRawHeader("Referer", m_ctx.referer.toUtf8());
	if (!m_ctx.user_agent.isEmpty())
		req.setRawHeader("User-Agent", m_ctx.user_agent.toUtf8());
	if (!m_ctx.cookies.isEmpty())
		req.setRawHeader("Cookie", m_ctx.cookies.toUtf8());
	if (!range.isEmpty())
		req.setRawHeader("Range", range);
	return m_net->get(req);
}

void hls_assembler::start(const QUrl &manifest, const stream_context &ctx,
                           const QString &output_path) {
	stop();
	m_ctx       = ctx;
	m_path      = output_path;
	m_written   = 0;
	m_index     = 0;
	m_finished  = false;
	m_stopped   = false;
	m_redirects = 0;

	m_file = new QFile(m_path, this);
	if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		emit failed("Cannot write " + m_path);
		delete m_file;
		m_file = nullptr;
		return;
	}
	fetch_manifest(manifest);
}

void hls_assembler::stop() {
	m_stopped = true;
	if (m_reply && m_reply->isRunning())
		m_reply->abort();
	if (m_file) {
		m_file->close();
		delete m_file;
		m_file = nullptr;
	}
}

void hls_assembler::fetch_manifest(const QUrl &url) {
	m_reply = get(url);
	QNetworkReply *reply = m_reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
		reply->deleteLater();
		if (m_stopped)
			return;
		if (reply->error() != QNetworkReply::NoError) {
			emit failed("Manifest fetch failed: " + reply->errorString());
			return;
		}
		m_playlist = hls::parse(reply->readAll(), url);

		if (m_playlist.is_master) {
			const hls_variant *v = hls::best_variant(m_playlist);
			if (!v) {
				emit failed("Master playlist listed no variants.");
				return;
			}
			// One hop only: a master pointing at another master is malformed,
			// and following it forever is how a fetch loop happens.
			if (++m_redirects > 1) {
				emit failed("Master playlist points at another master playlist.");
				return;
			}
			fetch_manifest(v->url);
			return;
		}

		if (m_playlist.segments.isEmpty()) {
			emit failed("Playlist listed no segments.");
			return;
		}
		next_segment();
	});
}

void hls_assembler::next_segment() {
	if (m_stopped)
		return;
	if (m_index >= m_playlist.segments.size()) {
		// A live playlist keeps growing, so "ran out of segments" is only the
		// end for VOD. Re-polling a live list is the next increment; for now
		// say plainly that what we captured is what there is.
		if (m_file) {
			m_file->flush();
			m_file->close();
		}
		m_finished = true;
		emit completed();
		return;
	}

	const hls_segment seg = m_playlist.segments.at(m_index);
	QByteArray range;
	if (seg.byte_length > 0) {
		const qint64 off = (seg.byte_offset >= 0) ? seg.byte_offset : 0;
		range = "bytes=" + QByteArray::number(off) + "-" +
		        QByteArray::number(off + seg.byte_length - 1);
	}

	m_reply = get(seg.url, range);
	QNetworkReply *reply = m_reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		if (m_stopped)
			return;
		if (reply->error() != QNetworkReply::NoError) {
			emit failed(QString("Segment %1 failed: %2")
			                .arg(m_index).arg(reply->errorString()));
			return;
		}
		const QByteArray body = reply->readAll();
		if (m_file) {
			m_file->write(body);
			m_file->flush();   // so a reader can play what has landed so far
			m_written += body.size();
		}
		++m_index;
		emit progress(m_written, m_index, m_playlist.segments.size());
		next_segment();
	});
}

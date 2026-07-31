// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_probe.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <memory>

namespace {

bool starts_with_ci(const QByteArray &b, const char *lit) {
	return b.left(int(qstrlen(lit))).toLower() == QByteArray(lit).toLower();
}

// Leading whitespace and a UTF-8 BOM are both common in served manifests and
// neither says anything about what the file is.
QByteArray trim_front(const QByteArray &b) {
	QByteArray s = b;
	if (s.startsWith("\xEF\xBB\xBF"))
		s = s.mid(3);
	int i = 0;
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
		++i;
	return s.mid(i);
}

}  // namespace

stream_probe::stream_probe(QObject *parent)
	: QObject(parent), m_net(new QNetworkAccessManager(this)) {}

probe_result stream_probe::classify(const QString &content_type,
                                     const QByteArray &head) {
	probe_result r;
	r.content_type = content_type;
	r.head = head;

	const QByteArray body = trim_front(head);
	const QString ct = content_type.toLower();

	// What the header would have us believe, kept separately so the two can be
	// compared rather than one silently winning.
	QString by_header;
	if (ct.contains("mpegurl"))                       by_header = "hls";
	else if (ct.contains("dash+xml"))                 by_header = "dash";
	else if (ct.startsWith("video/") || ct.startsWith("audio/"))
		by_header = "direct";

	// What the bytes say. This wins, because the disguise is precisely a
	// truthful-looking header over a manifest.
	QString by_body;
	if (starts_with_ci(body, "#EXTM3U"))
		by_body = "hls";
	else if (body.contains("<MPD") || body.contains("urn:mpeg:dash:schema"))
		by_body = "dash";
	else if (body.size() >= 12 && body.mid(4, 4) == "ftyp")
		by_body = "direct";                       // ISO-BMFF: mp4, m4s, mov
	else if (body.startsWith("\x1A\x45\xDF\xA3"))
		by_body = "direct";                       // Matroska / WebM
	else if (body.startsWith("OggS") || body.startsWith("ID3") ||
	          body.startsWith("\xFF\xFB"))
		by_body = "direct";

	r.kind = !by_body.isEmpty() ? by_body : by_header;
	r.disagreed = !by_body.isEmpty() && !by_header.isEmpty() &&
	               by_body != by_header;

	if (r.kind.isEmpty()) {
		r.reason = content_type.isEmpty()
			? QString("nothing in the opening bytes identifies this as a stream")
			: QString("served as %1, and the opening bytes do not identify a "
			           "stream either").arg(content_type);
	} else if (r.disagreed) {
		r.reason = QString("the body is %1, though it is served as %2")
		               .arg(r.kind.toUpper(), content_type);
	} else if (by_body.isEmpty()) {
		r.reason = QString("served as %1").arg(content_type);
	} else if (by_header.isEmpty() && !content_type.isEmpty()) {
		// The measured case: a master playlist behind `text/plain`.
		r.reason = QString("the body is %1, though it is served as %2")
		               .arg(r.kind.toUpper(), content_type);
		r.disagreed = true;
	} else {
		r.reason = QString("the body is %1").arg(r.kind.toUpper());
	}
	return r;
}

void stream_probe::probe(const QUrl &url, const stream_context &ctx,
                          std::function<void(const probe_result &)> done) {
	QNetworkRequest req(url);
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                  QNetworkRequest::NoLessSafeRedirectPolicy);
	// Ask for the opening bytes only. A server that ignores Range sends the
	// whole thing, so the read is capped below as well.
	req.setRawHeader("Range",
	                  QByteArray("bytes=0-") + QByteArray::number(k_sniff_bytes - 1));
	if (!ctx.referer.isEmpty())
		req.setRawHeader("Referer", ctx.referer.toUtf8());
	if (!ctx.user_agent.isEmpty())
		req.setRawHeader("User-Agent", ctx.user_agent.toUtf8());
	if (!ctx.cookies.isEmpty())
		req.setRawHeader("Cookie", ctx.cookies.toUtf8());
	for (auto it = ctx.extra.cbegin(); it != ctx.extra.cend(); ++it)
		req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

	QNetworkReply *reply = m_net->get(req);
	// Shared, not owned by the lambda that runs first. `readyRead`, `finished`
	// and the timeout can all arrive, and an earlier version deleted this guard
	// inside the first one and then let the next read the freed memory to decide
	// whether it had already run — which duly let some replies through twice.
	auto fired = std::make_shared<bool>(false);

	auto finish = [reply, done, fired](bool reached) {
		if (*fired) return;
		*fired = true;
		probe_result r;
		if (reached) {
			const QByteArray head = reply->read(k_sniff_bytes);
			const QString ct =
				reply->header(QNetworkRequest::ContentTypeHeader).toString();
			r = classify(ct.section(';', 0, 0).trimmed(), head);
			r.reached = true;
			r.status =
				reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			// A 403 is the CDN refusing the context, not a statement about the
			// content, and must not read as "this is not a stream".
			if (r.status >= 400) {
				r.kind.clear();
				r.reason = QString("the server answered %1, so what it is could "
				                    "not be established").arg(r.status);
			}
		} else {
			r.reason = QString("could not be reached (%1)")
			               .arg(reply->errorString());
		}
		done(r);
		reply->deleteLater();
	};

	// Enough bytes have arrived to decide; no reason to pull the rest.
	QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, finish] {
		if (reply->bytesAvailable() >= k_sniff_bytes)
			finish(true);
	});
	QObject::connect(reply, &QNetworkReply::finished, reply, [reply, finish] {
		finish(reply->error() == QNetworkReply::NoError ||
		        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid());
	});
	QTimer::singleShot(k_timeout_ms, reply, [reply, finish] {
		if (reply->isRunning()) { reply->abort(); finish(false); }
	});
}

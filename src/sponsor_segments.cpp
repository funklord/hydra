#include "sponsor_segments.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

#include <algorithm>

namespace {

// Eleven characters of the base64url alphabet. Checked rather than assumed:
// what this returns is hashed, sent and compared, and a url is a page-supplied
// string.
bool looks_like_id(const QString &s) {
	if (s.size() != 11)
		return false;
	for (const QChar c : s) {
		if (c.isLetterOrNumber() && c.unicode() < 128)
			continue;
		if (c == QLatin1Char('-') || c == QLatin1Char('_'))
			continue;
		return false;
	}
	return true;
}

// The host families that carry an id this understands. Suffix-matched, so
// `m.youtube.com` and `www.youtube-nocookie.com` are included without listing
// every spelling.
bool known_host(const QString &host) {
	for (const char *h : { "youtube.com", "youtube-nocookie.com", "youtu.be" }) {
		const QString name = QString::fromLatin1(h);
		if (host == name || host.endsWith(QLatin1Char('.') + name))
			return true;
	}
	return false;
}

}  // namespace

namespace sponsor_segments {

QString video_id(const QUrl &url) {
	const QString host = url.host().toLower();
	if (!known_host(host))
		return QString();

	// A watch page carries it in the query; every other form carries it as the
	// last path element. `youtu.be/<id>` is the short link, `/embed/<id>` is
	// what an iframe on somebody else's site loads, and `/shorts/<id>` is the
	// vertical player -- all three are a path.
	const QString path = url.path();
	if (path == QLatin1String("/watch")) {
		const QString v = QUrlQuery(url).queryItemValue(QStringLiteral("v"));
		return looks_like_id(v) ? v : QString();
	}
	const QStringList parts =
	  path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	if (parts.isEmpty())
		return QString();
	// For a short link the id is the only element; for the others it follows
	// the word that says which player. Taking the last element covers all of
	// them, and the validity check is what stops `/embed` alone or
	// `/results?search_query=...` being read as an id.
	const QString last = parts.last();
	return looks_like_id(last) ? last : QString();
}

QString hash_prefix(const QString &video_id) {
	if (!looks_like_id(video_id))
		return QString();
	const QByteArray sum = QCryptographicHash::hash(video_id.toLatin1(),
	                                                 QCryptographicHash::Sha256);
	return QString::fromLatin1(sum.toHex().left(4));
}

QStringList skipped_categories() {
	return { QStringLiteral("sponsor"), QStringLiteral("selfpromo"),
	          QStringLiteral("interaction") };
}

QList<sponsor_segment> parse(const QByteArray &json, const QString &video_id) {
	QList<sponsor_segment> out;
	if (!looks_like_id(video_id))
		return out;
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
	if (err.error != QJsonParseError::NoError || !doc.isArray())
		return out;

	const QStringList wanted = skipped_categories();
	for (const QJsonValue &v : doc.array()) {
		const QJsonObject entry = v.toObject();
		// **The filter that makes the prefix query honest.** The answer holds
		// every video sharing the prefix; anything not this one is somebody
		// else's video and its times mean nothing here.
		if (entry.value(QStringLiteral("videoID")).toString() != video_id)
			continue;
		for (const QJsonValue &sv : entry.value(QStringLiteral("segments"))
		                                 .toArray()) {
			const QJsonObject seg = sv.toObject();
			// Only the ones meant to be skipped outright. The service also
			// carries segments to mute, to mark a point of interest, or to
			// describe a whole video, and treating any of those as a skip
			// would jump over something nobody asked to lose.
			if (seg.value(QStringLiteral("actionType")).toString() !=
			    QLatin1String("skip"))
				continue;
			const QString category =
			  seg.value(QStringLiteral("category")).toString();
			if (!wanted.contains(category))
				continue;
			const QJsonArray at = seg.value(QStringLiteral("segment")).toArray();
			if (at.size() != 2)
				continue;
			sponsor_segment s;
			s.from     = at.at(0).toDouble(-1);
			s.to       = at.at(1).toDouble(-1);
			s.category = category;
			// A segment that does not move forward is not one. Zero-length and
			// reversed both arrive from real submissions, and a zero-length
			// one would have the script seek to where it already is -- which
			// on some players restarts the decoder, so it is not harmless.
			if (s.from < 0 || s.to <= s.from)
				continue;
			out.push_back(s);
		}
	}
	std::sort(out.begin(), out.end(),
	           [](const sponsor_segment &a, const sponsor_segment &b) {
		return a.from < b.from;
	});
	return out;
}

}  // namespace sponsor_segments

// SPDX-License-Identifier: GPL-3.0-or-later
#include "hls_playlist.h"

#include <QHash>

namespace {

// Attribute lists look like BANDWIDTH=123,RESOLUTION=1x2,CODECS="a,b" -- the
// quoted value can contain commas, so a plain split on ',' is wrong.
QHash<QString, QString> parse_attributes(const QString &s) {
	QHash<QString, QString> out;
	QString key, value;
	bool in_value = false, quoted = false;
	auto flush = [&] {
		if (!key.trimmed().isEmpty())
			out.insert(key.trimmed().toUpper(), value.trimmed());
		key.clear();
		value.clear();
		in_value = false;
	};
	for (int i = 0; i < s.size(); ++i) {
		const QChar c = s.at(i);
		if (c == '"') { quoted = !quoted; continue; }
		if (!quoted && c == '=' && !in_value) { in_value = true; continue; }
		if (!quoted && c == ',') { flush(); continue; }
		(in_value ? value : key).append(c);
	}
	flush();
	return out;
}

}  // namespace

double hls_playlist::total_duration() const {
	double d = 0.0;
	for (const hls_segment &s : segments)
		d += s.duration;
	return d;
}

namespace hls {

hls_playlist parse(const QByteArray &text, const QUrl &base) {
	hls_playlist out;

	double pending_duration = 0.0;
	bool   have_duration    = false;
	qint64 pending_offset   = -1;
	qint64 pending_length   = -1;
	hls_variant pending_variant;
	bool   have_variant     = false;

	const QList<QByteArray> lines = text.split('\n');
	for (const QByteArray &raw : lines) {
		const QString line = QString::fromUtf8(raw).trimmed();
		if (line.isEmpty())
			continue;

		if (line.startsWith("#EXT-X-ENDLIST")) {
			out.is_live = false;   // a complete VOD list
			continue;
		}
		if (line.startsWith("#EXT-X-MEDIA-SEQUENCE:")) {
			out.media_sequence = line.section(':', 1).toInt();
			continue;
		}
		if (line.startsWith("#EXT-X-TARGETDURATION:")) {
			out.target_duration = line.section(':', 1).toDouble();
			continue;
		}
		if (line.startsWith("#EXT-X-BYTERANGE:")) {
			// len[@offset]
			const QString v = line.section(':', 1);
			pending_length = v.section('@', 0, 0).toLongLong();
			const QString off = v.section('@', 1, 1);
			pending_offset = off.isEmpty() ? -1 : off.toLongLong();
			continue;
		}
		if (line.startsWith("#EXT-X-STREAM-INF:")) {
			const auto attrs = parse_attributes(line.section(':', 1));
			pending_variant = hls_variant{};
			pending_variant.bandwidth  = attrs.value("BANDWIDTH").toInt();
			pending_variant.resolution = attrs.value("RESOLUTION");
			pending_variant.codecs     = attrs.value("CODECS");
			have_variant = true;
			out.is_master = true;
			continue;
		}
		if (line.startsWith("#EXTINF:")) {
			pending_duration = line.section(':', 1).section(',', 0, 0).toDouble();
			have_duration = true;
			continue;
		}
		if (line.startsWith('#'))
			continue;   // a tag we don't need

		// A bare line is a URI, belonging to whichever tag preceded it.
		const QUrl resolved = base.isValid() ? base.resolved(QUrl(line)) : QUrl(line);
		if (have_variant) {
			pending_variant.url = resolved;
			out.variants.push_back(pending_variant);
			have_variant = false;
		} else if (have_duration) {
			hls_segment seg;
			seg.url         = resolved;
			seg.duration    = pending_duration;
			seg.byte_offset = pending_offset;
			seg.byte_length = pending_length;
			// An omitted offset is not zero. RFC 8216 sec 4.3.2.2: the sub-range
			// begins at the byte after the previous segment's sub-range, and
			// that previous segment is required to be a slice of the same
			// resource. Byte-range playlists say `1000@0` once and then only
			// lengths, so reading the omission as zero fetches the first slice
			// again for every segment -- an assembled file that is wrong without
			// being empty, which is the worst way to be wrong.
			//
			// Resolved here rather than in the assembler because this is where
			// the previous segment is known; downstream sees concrete offsets
			// and needs no rule of its own.
			if (seg.byte_length > 0 && seg.byte_offset < 0 && !out.segments.isEmpty()) {
				const hls_segment &prev = out.segments.last();
				if (prev.byte_length > 0 && prev.byte_offset >= 0 &&
				    prev.url == seg.url)
					seg.byte_offset = prev.byte_offset + prev.byte_length;
			}
			out.segments.push_back(seg);
			have_duration  = false;
			pending_offset = -1;
			pending_length = -1;
		}
	}

	// A master playlist has no segments of its own; live/VOD is a property of
	// the media playlists it points at.
	if (out.is_master)
		out.is_live = false;
	return out;
}

const hls_variant *best_variant(const hls_playlist &p) {
	const hls_variant *best = nullptr;
	for (const hls_variant &v : p.variants)
		if (!best || v.bandwidth > best->bandwidth)
			best = &v;
	return best;
}

}  // namespace hls

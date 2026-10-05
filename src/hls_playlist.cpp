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
		// **`#EXT-X-MAP` is the initialisation segment, and it was read by
		// nothing.** fMP4 HLS -- which is most modern HLS, because it is what
		// lets one set of segments serve HLS and DASH alike -- puts the `moov`
		// box in a separate file and names it here. Concatenating the fragments
		// without it produces a file with no initialisation at all: unplayable,
		// written out, and reported as a finished save, with ffmpeg then
		// failing to rewrap it and pointing at a container problem that is not
		// the cause.
		//
		// DASH's equivalent has always been handled -- `dash_representation`
		// carries an `init` and `assemble_dash` pushes it in front of the
		// segments -- so this is the same field and the same prepend for the
		// other grammar, not new machinery.
		//
		// **A second, different MAP is refused.** The tag may appear again
		// mid-playlist when the initialisation changes, and a stream whose
		// initialisation changes part way through is not one file. Taking the
		// first and ignoring the rest would be the silent-wrong-output shape
		// this whole section is about.
		if (line.startsWith("#EXT-X-MAP:")) {
			const QString v = line.section(':', 1);
			QUrl       uri;
			qint64     off = -1, len = -1;
			for (const QString &attr : v.split(',')) {
				const QString a = attr.trimmed();
				if (a.startsWith("URI=", Qt::CaseInsensitive)) {
					QString raw = a.mid(4).trimmed();
					if (raw.startsWith('"') && raw.endsWith('"') &&
					     raw.size() >= 2)
						raw = raw.mid(1, raw.size() - 2);
					if (!raw.isEmpty())
						uri = base.isEmpty() ? QUrl(raw) : base.resolved(QUrl(raw));
				} else if (a.startsWith("BYTERANGE=", Qt::CaseInsensitive)) {
					QString raw = a.mid(10).trimmed();
					if (raw.startsWith('"') && raw.endsWith('"') &&
					     raw.size() >= 2)
						raw = raw.mid(1, raw.size() - 2);
					bool len_ok = false, off_ok = true;
					len = raw.section('@', 0, 0).trimmed().toLongLong(&len_ok);
					const QString at = raw.section('@', 1, 1).trimmed();
					off = at.isEmpty() ? 0 : at.toLongLong(&off_ok);
					if (!len_ok || !off_ok || len <= 0) {
						if (out.error.isEmpty())
							out.error = "an EXT-X-MAP byte range that cannot be "
							             "read (" + raw + ")";
						len = off = -1;
					}
				}
			}
			if (!uri.isValid() || uri.isEmpty()) {
				if (out.error.isEmpty())
					out.error = "an EXT-X-MAP names no usable URI";
				continue;
			}
			if (!out.init.isEmpty() &&
			     (out.init != uri || out.init_offset != off ||
			      out.init_length != len)) {
				if (out.error.isEmpty())
					out.error = "the initialisation segment changes part way "
					             "through, which is not one file";
				continue;
			}
			out.init        = uri;
			out.init_offset = off;
			out.init_length = len;
			continue;
		}

		// **An encrypted playlist is refused, not concatenated.**
		// `#EXT-X-KEY:METHOD=AES-128,URI="..."` says every segment from here on
		// is AES-encrypted, and this engine does not decrypt: it would fetch
		// them, concatenate the ciphertext, write it out and report a saved
		// file. What the person gets is noise and a confusing complaint from
		// ffmpeg about a container, with nothing anywhere saying the stream was
		// encrypted. Most commercial HLS is.
		//
		// `METHOD=NONE` is the tag turning encryption *off* for the segments
		// that follow and is not a refusal -- a playlist may carry it after an
		// encrypted stretch, and refusing it would reject a clear stream for
		// saying so.
		//
		// Decrypting is a feature: the key has to be fetched, and AES-128-CBC
		// needs the IV, which is either the tag's or the segment's sequence
		// number. Refusing by name is what this `error` field is for, and it is
		// the difference between "we cannot do this" and a file that is not the
		// programme.
		if (line.startsWith("#EXT-X-KEY:")) {
			const QString v = line.section(':', 1);
			// The attribute list is comma-separated; METHOD is the first by
			// specification but read by name rather than by position.
			QString method;
			for (const QString &attr : v.split(',')) {
				const QString a = attr.trimmed();
				if (a.startsWith("METHOD=", Qt::CaseInsensitive)) {
					method = a.mid(7).trimmed();
					break;
				}
			}
			if (!method.isEmpty() &&
			     method.compare("NONE", Qt::CaseInsensitive) != 0) {
				if (out.error.isEmpty())
					out.error = "the segments are encrypted (METHOD=" + method +
					             "), which this cannot decrypt";
			}
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
			// len[@offset], and both are read with their status rather than
			// guessed at: `toLongLong()` answers 0 for what it cannot read, a
			// length of 0 means "the whole file" to the assembler, and the
			// result is a slice request silently turned into a whole-file one.
			// A manifest this cannot understand is refused instead.
			const QString v = line.section(':', 1);
			const QString off = v.section('@', 1, 1);
			bool len_ok = false, off_ok = true;
			const qint64 len = v.section('@', 0, 0).trimmed().toLongLong(&len_ok);
			const qint64 at =
			  off.isEmpty() ? -1 : off.trimmed().toLongLong(&off_ok);
			if (!len_ok || !off_ok || len <= 0 || (!off.isEmpty() && at < 0)) {
				if (out.error.isEmpty())
					out.error = QStringLiteral("byte range not understood: %1")
					              .arg(line.trimmed());
				continue;
			}
			pending_length = len;
			pending_offset = at;
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

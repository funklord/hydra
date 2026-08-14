// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

// One quality variant from a master playlist -- what sec 11.2 means by "stream
// quality variants from the manifest".
struct hls_variant {
	QUrl    url;
	int     bandwidth = 0;      // bits/sec, the primary ranking key
	QString resolution;         // "1920x1080", when advertised
	QString codecs;
};

struct hls_segment {
	QUrl   url;
	double duration = 0.0;
	// #EXT-X-BYTERANGE: a segment that is a slice of a larger file. Both -1
	// when the segment is a whole file.
	qint64 byte_offset = -1;
	qint64 byte_length = -1;
};

// A parsed HLS playlist.
struct hls_playlist {
	bool   is_master = false;   // a list of variants rather than of segments
	bool   is_live   = true;    // no #EXT-X-ENDLIST means the list still grows
	int    media_sequence = 0;
	double target_duration = 0.0;
	QList<hls_variant> variants;
	QList<hls_segment> segments;

	double total_duration() const;
};

// Parsing only -- no network, no state. The assembler and the media list both
// need to read manifests, and the parsing is where the fiddly cases live
// (relative URIs, byte ranges, VOD versus live), so it is separated out and
// tested on its own.
namespace hls {

// `base` is the manifest's own URL; relative URIs resolve against it.
hls_playlist parse(const QByteArray &text, const QUrl &base);

// Highest bandwidth wins -- the sec 11.3 heuristic for the primary stream.
// Returns nullptr when there are no variants.
const hls_variant *best_variant(const hls_playlist &p);

}  // namespace hls

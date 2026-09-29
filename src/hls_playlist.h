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
	// **Why a parser that cannot fail needed somewhere to say so.** A
	// `#EXT-X-BYTERANGE` whose numbers cannot be read used to come out as
	// length 0 and offset 0, because `toLongLong()` answers 0 for a value it
	// cannot read -- and length 0 means "the whole file" to the assembler,
	// which builds no `Range` header for it. So a malformed manifest produced
	// a *wrong file*: whole media where a slice was meant, concatenated with
	// its neighbours, with nothing anywhere saying the manifest had not been
	// understood.
	//
	// Only the byte range sets this, because only it decides which bytes are
	// fetched. `BANDWIDTH` that cannot be read leaves a variant at 0 and
	// changes which stream is picked, which is a worse choice rather than a
	// wrong file; the durations and the media sequence are read by nothing
	// that decides anything. Empty means the manifest parsed.
	QString error;

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

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

// One representation: a single encoding of one stream, which is DASH's unit of
// choice in the way a variant is HLS's. A DASH manifest normally carries video
// and audio as *separate* representations, which is the whole difference from
// HLS for anything downstream -- see the note on `best_video` below.
struct dash_representation {
	QString id;
	int     bandwidth = 0;      // bits/sec, the primary ranking key
	int     width     = 0;      // 0 when not advertised
	int     height    = 0;
	QString codecs;
	QString mime_type;          // "video/mp4", "audio/mp4", ...

	// The initialisation segment, where the addressing names one. Empty for a
	// single-file representation, which needs none.
	QUrl init;
	// Media segments, in presentation order.
	QList<QUrl> segments;

	bool is_video() const { return mime_type.startsWith("video/"); }
	bool is_audio() const { return mime_type.startsWith("audio/"); }
};

// A parsed MPD.
struct dash_manifest {
	// MPD@type: "dynamic" means the list still grows, which is DASH's word for
	// what HLS says by omitting `#EXT-X-ENDLIST`. Default "static".
	bool   is_live = false;
	double duration = 0.0;      // mediaPresentationDuration, in seconds
	QList<dash_representation> representations;

	// **Why a parser needs somewhere to say it did not understand.** The same
	// reason `hls_playlist` has one, and the same failure: a segment list that
	// is wrong rather than absent produces a *wrong file* -- media assembled
	// out of the wrong pieces, or truncated -- with nothing anywhere saying the
	// manifest had not been read. Absent is recoverable and wrong is not, so
	// anything this cannot address is refused here rather than turned into a
	// shorter list.
	//
	// Set only by what decides which bytes are fetched: an addressing mode this
	// does not implement, a template whose count cannot be derived, and a
	// number or timestamp that will not parse. A `bandwidth` that cannot be
	// read leaves a representation at 0 and changes which stream is picked,
	// which is a worse choice rather than a wrong file. Empty means the
	// manifest parsed.
	QString error;
};

// Parsing only -- no network, no state, exactly as `hls_playlist` is separated
// from `hls_assembler`. The fiddly cases are all here: four addressing modes,
// template identifiers with printf-style widths, and BaseURL resolution down
// four levels of the document.
namespace dash {

// `base` is the manifest's own URL; relative URLs resolve against it.
dash_manifest parse(const QByteArray &xml, const QUrl &base);

// Highest bandwidth of its kind wins, which is `hls::best_variant`'s heuristic
// per stream. Null when the manifest carries none of that kind.
//
// **Two calls rather than one, because DASH separates the streams.** An HLS
// variant is usually muxed, so picking one is picking the programme; a DASH
// manifest hands out video and audio apart, and assembling only the video is
// how a silent file gets produced by something that looks like it worked.
const dash_representation *best_video(const dash_manifest &m);
const dash_representation *best_audio(const dash_manifest &m);

}  // namespace dash

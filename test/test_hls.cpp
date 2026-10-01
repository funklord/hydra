// The HLS manifest parser (architecture doc sec 11.3).
//
// Its own header said it was "separated out and tested on its own". It was
// separated out. This is the other half, written after noticing the claim was
// unaccompanied -- and it is worth having because this is a parser for a text
// format that arrives from a CDN, which is to say from nobody trustworthy, and
// everything downstream of it (the quality list, the assembler, the media
// dialog) believes what it says.
#include "hls_playlist.h"

#include <QCoreApplication>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	const QUrl base("https://cdn.example/v/hls/master.m3u8");

	section("a master playlist: the quality variants");
	{
		const QByteArray text =
		  "#EXTM3U\n"
		  "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\"\n"
		  "low/index.m3u8\n"
		  "#EXT-X-STREAM-INF:BANDWIDTH=2400000,RESOLUTION=1280x720\n"
		  "mid/index.m3u8\n"
		  "#EXT-X-STREAM-INF:BANDWIDTH=5200000,RESOLUTION=1920x1080\n"
		  "https://other.example/high/index.m3u8\n";
		const hls_playlist p = hls::parse(text, base);

		check(p.is_master, "it is recognised as a master");
		check(!p.is_live, "and a master is never live in its own right");
		check(p.variants.size() == 3,
		      QString("all three variants are found (%1)").arg(p.variants.size()));
		check(p.segments.isEmpty(), "and it has no segments of its own");

		check(p.variants[0].bandwidth == 800000, "bandwidth is read");
		check(p.variants[0].resolution == "640x360", "resolution too");
		check(p.variants[0].codecs == "avc1.4d401e,mp4a.40.2",
		      QString("and a quoted CODECS keeps its commas (%1)")
		          .arg(p.variants[0].codecs));

		check(p.variants[0].url.toString() ==
		          "https://cdn.example/v/hls/low/index.m3u8",
		      QString("a relative variant resolves against the manifest (%1)")
		          .arg(p.variants[0].url.toString()));
		check(p.variants[2].url.toString() ==
		          "https://other.example/high/index.m3u8",
		      "and an absolute one is left alone, even on another host");

		const hls_variant *best = hls::best_variant(p);
		check(best && best->bandwidth == 5200000,
		      "the best variant is the widest, not the last");
	}

	section("a media playlist: the segments");
	{
		const QByteArray text =
		  "#EXTM3U\r\n"                       // CRLF, as served by many CDNs
		  "#EXT-X-TARGETDURATION:10\r\n"
		  "#EXT-X-MEDIA-SEQUENCE:42\r\n"
		  "#EXTINF:9.009,\r\n"
		  "seg0.ts\r\n"
		  "#EXTINF:8.5,a title with, a comma\r\n"
		  "seg1.ts\r\n"
		  "#EXT-X-ENDLIST\r\n";
		const hls_playlist p = hls::parse(text, base);

		check(!p.is_master, "not a master");
		check(!p.is_live, "ENDLIST means the list is complete");
		check(p.media_sequence == 42, "the media sequence is read");
		check(qFuzzyCompare(p.target_duration, 10.0), "and the target duration");
		check(p.segments.size() == 2,
		      QString("both segments are found despite CRLF (%1)").arg(p.segments.size()));
		check(qFuzzyCompare(p.segments[0].duration, 9.009),
		      "a fractional duration survives");
		check(qFuzzyCompare(p.segments[1].duration, 8.5),
		      "and a title after the comma is not mistaken for one");
		check(qFuzzyCompare(p.total_duration(), 17.509),
		      QString("the total is the sum (%1)").arg(p.total_duration()));
		check(p.segments[0].url.toString() == "https://cdn.example/v/hls/seg0.ts",
		      "segments resolve against the manifest too");
	}

	section("live versus complete");
	{
		const QByteArray live =
		  "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nseg0.ts\n";
		check(hls::parse(live, base).is_live,
		      "no ENDLIST means the list is still growing");
	}

	section("byte ranges: a playlist that is slices of one file");
	{
		// The shape a byte-range playlist actually has: one resource, each
		// segment a sub-range, and **the offset omitted after the first**.
		// RFC 8216 sec 4.3.2.2: a missing offset means the byte after the previous
		// sub-range -- not zero. Treating it as zero fetches the first slice over
		// and over and assembles a file that is wrong without being empty.
		const QByteArray text =
		  "#EXTM3U\n"
		  "#EXT-X-TARGETDURATION:4\n"
		  "#EXTINF:4,\n"
		  "#EXT-X-BYTERANGE:1000@0\n"
		  "all.mp4\n"
		  "#EXTINF:4,\n"
		  "#EXT-X-BYTERANGE:2000\n"
		  "all.mp4\n"
		  "#EXTINF:4,\n"
		  "#EXT-X-BYTERANGE:1500\n"
		  "all.mp4\n"
		  "#EXT-X-ENDLIST\n";
		const hls_playlist p = hls::parse(text, base);

		check(p.segments.size() == 3, "three sub-ranges");
		check(p.segments[0].byte_offset == 0 && p.segments[0].byte_length == 1000,
		      "the first states its own offset");
		check(p.segments[1].byte_offset == 1000,
		      QString("the second continues from where the first ended (%1, wanted 1000)")
		          .arg(p.segments[1].byte_offset));
		check(p.segments[2].byte_offset == 3000,
		      QString("and the third from where the second ended (%1, wanted 3000)")
		          .arg(p.segments[2].byte_offset));
		check(p.segments[1].byte_length == 2000 && p.segments[2].byte_length == 1500,
		      "lengths are read whether or not an offset came with them");
	}

	section("byte ranges: a range does not leak onto a later segment");
	{
		const QByteArray text =
		  "#EXTM3U\n"
		  "#EXTINF:4,\n"
		  "#EXT-X-BYTERANGE:1000@0\n"
		  "part.mp4\n"
		  "#EXTINF:4,\n"
		  "whole.ts\n";
		const hls_playlist p = hls::parse(text, base);
		check(p.segments.size() == 2, "two segments");
		check(p.segments[1].byte_length == -1 && p.segments[1].byte_offset == -1,
		      "a segment with no range of its own is a whole file, not the "
		      "previous segment's slice");
	}

	section("what a hostile or broken manifest gets");
	{
		check(hls::parse("", base).segments.isEmpty(), "empty text parses to nothing");
		check(!hls::parse("", base).is_master, "and is not a master");

		const hls_playlist junk = hls::parse("this is not a playlist at all\n", base);
		check(junk.segments.isEmpty(),
		      "a bare line with no EXTINF before it is not a segment");

		// Tags we do not handle must not be mistaken for ones we do. #EXT-X-MEDIA
		// carries its URI as an attribute and has no following line; treating it
		// like #EXT-X-STREAM-INF would invent a variant.
		const QByteArray with_media =
		  "#EXTM3U\n"
		  "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en\",URI=\"audio/en.m3u8\"\n"
		  "#EXT-X-STREAM-INF:BANDWIDTH=1000,AUDIO=\"a\"\n"
		  "v/index.m3u8\n";
		const hls_playlist m = hls::parse(with_media, base);
		check(m.variants.size() == 1,
		      QString("#EXT-X-MEDIA is not counted as a variant (%1)")
		          .arg(m.variants.size()));
		check(m.variants[0].url.toString() == "https://cdn.example/v/hls/v/index.m3u8",
		      "and the real variant still gets the URI that follows it");

		// An I-FRAME variant also carries URI= inline and has no following line.
		const QByteArray iframe =
		  "#EXTM3U\n"
		  "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=90000,URI=\"iframe.m3u8\"\n"
		  "#EXT-X-STREAM-INF:BANDWIDTH=1000\n"
		  "v/index.m3u8\n";
		const hls_playlist f = hls::parse(iframe, base);
		check(f.variants.size() == 1,
		      QString("an I-frame variant does not steal the next URI (%1)")
		          .arg(f.variants.size()));
		check(f.variants[0].bandwidth == 1000,
		      "and the variant that is left is the real one");

		// **A byte range whose numbers cannot be read is refused, not guessed
		// at.** `toLongLong()` answers 0 for what it cannot read, a length of 0
		// means "the whole file" to the assembler -- it builds no Range header
		// for one -- and the result was a slice request silently turned into a
		// whole-file one, concatenated with its neighbours and reported
		// complete. So the parse says it did not understand the manifest, and
		// the assembler refuses it.
		const QByteArray bad_range =
		  "#EXTM3U\n"
		  "#EXT-X-BYTERANGE:abc@def\n"
		  "#EXTINF:4,\n"
		  "one.ts\n"
		  "#EXT-X-ENDLIST\n";
		const hls_playlist br = hls::parse(bad_range, base);
		check(!br.error.isEmpty(),
		      QString("an unreadable byte range is reported (%1)").arg(br.error));
		check(br.error.contains("abc@def"),
		      "naming the line, which is the only thing that can be acted on");

		// Each shape that used to come out as zero, and zero is the value that
		// means something else.
		for (const QByteArray &spec : { QByteArray("#EXT-X-BYTERANGE:0\n"),
		                                 QByteArray("#EXT-X-BYTERANGE:\n"),
		                                 QByteArray("#EXT-X-BYTERANGE:100@x\n"),
		                                 QByteArray("#EXT-X-BYTERANGE:@500\n") }) {
			const QByteArray text = "#EXTM3U\n" + spec +
			                         "#EXTINF:4,\none.ts\n#EXT-X-ENDLIST\n";
			const hls_playlist p = hls::parse(text, base);
			check(!p.error.isEmpty(),
			       QString("%1 is refused")
			         .arg(QString::fromUtf8(spec).trimmed()));
		}

		// The control: a byte range that reads correctly says nothing, and the
		// section above already proves it is honoured. Without this the check
		// would pass for a parser that called every manifest broken.
		const QByteArray good_range =
		  "#EXTM3U\n"
		  "#EXT-X-BYTERANGE:1000@2000\n"
		  "#EXTINF:4,\n"
		  "one.ts\n"
		  "#EXT-X-ENDLIST\n";
		const hls_playlist gr = hls::parse(good_range, base);
		check(gr.error.isEmpty(),
		      QString("a byte range that reads is not reported (%1)").arg(gr.error));
		check(gr.segments.size() == 1 && gr.segments[0].byte_length == 1000 &&
		          gr.segments[0].byte_offset == 2000,
		      "and is the slice it says");
	}

	// **An encrypted playlist is refused rather than concatenated.** Nothing
	// read `#EXT-X-KEY`, so the segments were fetched, the ciphertext was
	// concatenated and written out, and the save was reported as done: what the
	// person gets is noise and a complaint from ffmpeg about a container, with
	// nothing saying the stream was encrypted. Most commercial HLS is.
	section("an encrypted playlist is refused by name");
	{
		const QByteArray enc =
		  "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
		  "#EXT-X-KEY:METHOD=AES-128,URI=\"https://k.example/k\",IV=0x0\n"
		  "#EXTINF:4.0,\n/s0.ts\n#EXTINF:4.0,\n/s1.ts\n#EXT-X-ENDLIST\n";
		const hls_playlist p = hls::parse(enc, QUrl("https://c.example/v.m3u8"));
		check(!p.error.isEmpty(),
		      QString("refused (%1)").arg(p.error.isEmpty()
		        ? QStringLiteral("parsed happily") : p.error));
		check(p.error.contains("encrypted") && p.error.contains("AES-128"),
		      QString("and the message names what it cannot do, with the "
		               "method (%1)").arg(p.error));

		// **METHOD=NONE is the tag turning encryption off and is not a
		// refusal.** A playlist may carry it after an encrypted stretch, and
		// refusing it would reject a clear stream for saying it is clear --
		// which is the half a one-sided check would miss.
		const QByteArray cleared =
		  "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
		  "#EXT-X-KEY:METHOD=NONE\n"
		  "#EXTINF:4.0,\n/s0.ts\n#EXT-X-ENDLIST\n";
		const hls_playlist c = hls::parse(cleared, QUrl("https://c.example/v.m3u8"));
		check(c.error.isEmpty(),
		      QString("METHOD=NONE parses (%1)").arg(c.error));
		check(c.segments.size() == 1,
		      QString("and its segments are there (%1)").arg(c.segments.size()));

		// Read by name rather than by position, so an attribute list that puts
		// METHOD second is still understood.
		const QByteArray reordered =
		  "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
		  "#EXT-X-KEY:URI=\"https://k.example/k\",METHOD=SAMPLE-AES\n"
		  "#EXTINF:4.0,\n/s0.ts\n#EXT-X-ENDLIST\n";
		const hls_playlist r = hls::parse(reordered, QUrl("https://c.example/v.m3u8"));
		check(!r.error.isEmpty() && r.error.contains("SAMPLE-AES"),
		      QString("METHOD after URI is still found (%1)")
		          .arg(r.error.isEmpty() ? QStringLiteral("parsed happily")
		                                  : r.error));
	}

	section("no base url");
	{
		const QByteArray text = "#EXTM3U\n#EXTINF:4,\nhttps://a.example/s.ts\n";
		const hls_playlist p = hls::parse(text, QUrl());
		check(p.segments.size() == 1 &&
		          p.segments[0].url.toString() == "https://a.example/s.ts",
		      "an absolute segment url needs no base");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

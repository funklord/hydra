// The DASH manifest parser (architecture doc sec 11.2/sec 11.3, where the proxy
// "does the manifest work" for a player that cannot).
//
// It is worth testing for the reason `test_hls` gives about its own subject and
// one more. An MPD arrives from a CDN, which is to say from nobody
// trustworthy, and everything downstream believes what it says -- and unlike a
// playlist it is a *program* for computing segment names rather than a list of
// them, so the ways it can be wrong are arithmetic as well as textual. A
// manifest that yields the wrong count produces a file that plays and is
// truncated, which is the failure this parser refuses rather than rounds.
#include "dash_manifest.h"

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

	const QUrl base("https://cdn.example/v/manifest.mpd");

	section("a template with a timeline: the count is read, not derived");
	{
		// `r="2"` is three segments, not two -- the attribute is the number of
		// *additional* repeats, and reading it as a total is the classic way to
		// lose a segment off every entry.
		const QByteArray xml =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD type=\"static\" mediaPresentationDuration=\"PT10S\">\n"
		  "  <Period>\n"
		  "    <AdaptationSet mimeType=\"video/mp4\" codecs=\"avc1.4d401f\">\n"
		  "      <SegmentTemplate timescale=\"1000\"\n"
		  "         initialization=\"$RepresentationID$/init.mp4\"\n"
		  "         media=\"$RepresentationID$/seg-$Number%05d$.m4s\"\n"
		  "         startNumber=\"1\">\n"
		  "        <SegmentTimeline>\n"
		  "          <S t=\"0\" d=\"2000\" r=\"2\"/>\n"
		  "          <S d=\"1500\"/>\n"
		  "        </SegmentTimeline>\n"
		  "      </SegmentTemplate>\n"
		  "      <Representation id=\"v720\" bandwidth=\"1200000\""
		  " width=\"1280\" height=\"720\"/>\n"
		  "    </AdaptationSet>\n"
		  "  </Period>\n"
		  "</MPD>\n";
		const dash_manifest m = dash::parse(xml, base);

		check(m.error.isEmpty(),
		      QString("it parses (%1)")
		          .arg(m.error.isEmpty() ? QString("no error") : m.error));
		check(!m.is_live, "a static MPD is not live");
		check(qAbs(m.duration - 10.0) < 0.001,
		      QString("PT10S is ten seconds (%1)").arg(m.duration));
		check(m.representations.size() == 1,
		      QString("one representation (%1)").arg(m.representations.size()));
		if (m.representations.isEmpty()) {
			std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
			return 1;
		}
		const dash_representation &r = m.representations.first();
		check(r.id == "v720" && r.bandwidth == 1200000,
		      "the id and bandwidth are the representation's own");
		check(r.width == 1280 && r.height == 720,
		      "and the resolution it advertises");
		check(r.mime_type == "video/mp4" && r.codecs == "avc1.4d401f",
		      "inherited from the AdaptationSet, which is where they sit");
		check(r.init == QUrl("https://cdn.example/v/v720/init.mp4"),
		      QString("the init segment resolves against the manifest (%1)")
		          .arg(r.init.toString()));
		check(r.segments.size() == 4,
		      QString("four segments: r=2 is three, plus the odd last one (%1)")
		          .arg(r.segments.size()));
		check(r.segments.value(0) ==
		          QUrl("https://cdn.example/v/v720/seg-00001.m4s"),
		      QString("$Number%05d$ is padded, not printed bare (%1)")
		          .arg(r.segments.value(0).toString()));
		check(r.segments.value(3) ==
		          QUrl("https://cdn.example/v/v720/seg-00004.m4s"),
		      QString("and numbering runs on across timeline entries (%1)")
		          .arg(r.segments.value(3).toString()));
	}

	section("$Time$ is the timeline's clock, not the segment's index");
	{
		// The distinction that matters: with `t=0 d=2000 r=1`, the second
		// segment's `$Time$` is 2000. An implementation that interpolated the
		// index would ask for `.../1.m4s` and get a 404 on every segment but
		// the first, which is the shape of bug that looks like a network fault.
		const QByteArray xml =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD mediaPresentationDuration=\"PT4S\"><Period>\n"
		  "  <AdaptationSet mimeType=\"video/mp4\">\n"
		  "    <SegmentTemplate timescale=\"1000\" media=\"t/$Time$.m4s\">\n"
		  "      <SegmentTimeline><S t=\"0\" d=\"2000\" r=\"1\"/></SegmentTimeline>\n"
		  "    </SegmentTemplate>\n"
		  "    <Representation id=\"v\" bandwidth=\"1\"/>\n"
		  "  </AdaptationSet>\n"
		  "</Period></MPD>\n";
		const dash_manifest m = dash::parse(xml, base);
		const dash_representation &r = m.representations.first();
		check(m.error.isEmpty() && r.segments.size() == 2,
		      QString("two segments (%1)").arg(r.segments.size()));
		check(r.segments.value(0) == QUrl("https://cdn.example/v/t/0.m4s"),
		      QString("the first is at time 0 (%1)")
		          .arg(r.segments.value(0).toString()));
		check(r.segments.value(1) == QUrl("https://cdn.example/v/t/2000.m4s"),
		      QString("the second at 2000, the first's start plus its duration, "
		               "not its index (%1)")
		          .arg(r.segments.value(1).toString()));
	}

	section("a template with a duration: the count is arithmetic");
	{
		// Seven seconds of two-second segments is four, not three: the last one
		// is short and it is still a segment. Truncating here is how a file
		// ends a second early and plays perfectly.
		const QByteArray xml =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD mediaPresentationDuration=\"PT7S\"><Period>\n"
		  "  <AdaptationSet mimeType=\"audio/mp4\">\n"
		  "    <SegmentTemplate timescale=\"1\" duration=\"2\" media=\"a/$Number$.m4s\"/>\n"
		  "    <Representation id=\"a1\" bandwidth=\"128000\"/>\n"
		  "  </AdaptationSet>\n"
		  "</Period></MPD>\n";
		const dash_manifest m = dash::parse(xml, base);
		const dash_representation &r = m.representations.first();
		check(m.error.isEmpty(), QString("it parses (%1)").arg(m.error));
		check(r.segments.size() == 4,
		      QString("ceil(7/2) is four, and the fourth is the short one (%1)")
		          .arg(r.segments.size()));
		check(r.segments.value(0) == QUrl("https://cdn.example/v/a/1.m4s"),
		      QString("numbering starts at startNumber's default of 1 (%1)")
		          .arg(r.segments.value(0).toString()));
		check(r.is_audio() && !r.is_video(), "and it knows which stream it is");
	}

	section("an explicit list, and a BaseURL that moves the whole document");
	{
		const QByteArray xml =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD type=\"dynamic\">\n"
		  "  <BaseURL>https://other.example/x/</BaseURL>\n"
		  "  <Period><AdaptationSet mimeType=\"video/mp4\">\n"
		  "    <Representation id=\"v1\" bandwidth=\"900000\">\n"
		  "      <SegmentList>\n"
		  "        <Initialization sourceURL=\"init.mp4\"/>\n"
		  "        <SegmentURL media=\"s1.m4s\"/>\n"
		  "        <SegmentURL media=\"s2.m4s\"/>\n"
		  "      </SegmentList>\n"
		  "    </Representation>\n"
		  "  </AdaptationSet></Period>\n"
		  "</MPD>\n";
		const dash_manifest m = dash::parse(xml, base);
		const dash_representation &r = m.representations.first();
		check(m.error.isEmpty(), QString("it parses (%1)").arg(m.error));
		check(m.is_live, "type=\"dynamic\" is DASH's word for a growing list");
		check(r.segments.size() == 2,
		      QString("both listed segments (%1)").arg(r.segments.size()));
		check(r.segments.value(0) == QUrl("https://other.example/x/s1.m4s"),
		      QString("resolved against the document's BaseURL rather than the "
		               "manifest's own address (%1)")
		          .arg(r.segments.value(0).toString()));
		check(r.init == QUrl("https://other.example/x/init.mp4"),
		      QString("and so is the init (%1)").arg(r.init.toString()));
	}

	// **An encrypted MPD is refused by name**, which is `#EXT-X-KEY`'s fault in
	// the other grammar -- and it was still here an hour after that one was
	// fixed. Without this the ciphertext is fetched, concatenated, written out
	// and reported as a saved file.
	section("an encrypted representation is refused, by scheme");
	{
		const QByteArray xml =
		  "<?xml version=\"1.0\"?>\n<MPD>\n"
		  "  <Period><AdaptationSet mimeType=\"video/mp4\">\n"
		  "    <ContentProtection schemeIdUri=\"urn:mpeg:dash:mp4protection:"
		  "2011\" value=\"cenc\"/>\n"
		  "    <Representation id=\"v1\" bandwidth=\"900000\">\n"
		  "      <SegmentList>\n"
		  "        <SegmentURL media=\"s1.m4s\"/>\n"
		  "      </SegmentList>\n"
		  "    </Representation>\n"
		  "  </AdaptationSet></Period>\n</MPD>\n";
		const dash_manifest m = dash::parse(xml, base);
		check(!m.error.isEmpty() && m.error.contains("encrypted"),
		      QString("refused (%1)").arg(m.error.isEmpty()
		        ? QStringLiteral("parsed happily") : m.error));
		check(m.error.contains("mp4protection"),
		      QString("and the message names the scheme (%1)").arg(m.error));

		// **On the AdaptationSet, inherited by its Representation.** The level
		// stack pushes a copy of its parent, which is what makes that work --
		// and what keeps the next case honest.
		const int segs = m.representations.isEmpty()
		                   ? 0 : m.representations.first().segments.size();
		check(segs == 0,
		      QString("and no segment list was built from it (%1)").arg(segs));

		// **A clear manifest is not condemned by the tag existing in the
		// grammar**, which is the check that stops this from being a refusal of
		// every MPD.
		const QByteArray clear =
		  "<?xml version=\"1.0\"?>\n<MPD>\n"
		  "  <Period><AdaptationSet mimeType=\"video/mp4\">\n"
		  "    <Representation id=\"v1\" bandwidth=\"900000\">\n"
		  "      <SegmentList>\n"
		  "        <SegmentURL media=\"s1.m4s\"/>\n"
		  "      </SegmentList>\n"
		  "    </Representation>\n"
		  "  </AdaptationSet></Period>\n</MPD>\n";
		const dash_manifest c = dash::parse(clear, base);
		check(c.error.isEmpty() && !c.representations.isEmpty() &&
		        c.representations.first().segments.size() == 1,
		      QString("a clear manifest still parses (%1)").arg(c.error));
	}

	// **A SegmentURL carrying `mediaRange` is refused, not approximated.**
	// It says the segment is bytes a..b of one file, which is DASH's
	// `#EXT-X-BYTERANGE`. `dash_representation::segments` is a list of urls
	// with nowhere to put a range, so dropping it would make every entry name
	// the same whole file -- fetched once per segment, with no `Range` header
	// for `hls_assembler`'s length check to catch, and N copies written out as
	// a finished save.
	section("a SegmentURL with a byte range is refused by name");
	{
		const QByteArray xml =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD>\n"
		  "  <BaseURL>https://other.example/x/</BaseURL>\n"
		  "  <Period><AdaptationSet mimeType=\"video/mp4\">\n"
		  "    <Representation id=\"v1\" bandwidth=\"900000\">\n"
		  "      <SegmentList>\n"
		  "        <SegmentURL media=\"all.m4s\" mediaRange=\"0-999\"/>\n"
		  "        <SegmentURL media=\"all.m4s\" mediaRange=\"1000-1999\"/>\n"
		  "      </SegmentList>\n"
		  "    </Representation>\n"
		  "  </AdaptationSet></Period>\n"
		  "</MPD>\n";
		const dash_manifest m = dash::parse(xml, base);
		check(!m.error.isEmpty() && m.error.contains("mediaRange"),
		      QString("refused, and the message names the attribute (%1)")
		          .arg(m.error.isEmpty() ? QStringLiteral("(parsed happily)")
		                                  : m.error));
		// **The consequence, which is what makes the refusal worth having.**
		// Dropping the range leaves two entries naming one file; refusing
		// leaves nothing for the assembler to fetch twice.
		const int segs = m.representations.isEmpty()
		                   ? 0 : m.representations.first().segments.size();
		check(segs == 0,
		      QString("and no segment list was built from it (%1)").arg(segs));
	}

	section("one file, addressed by SegmentBase");
	{
		// The `<Initialization range=...>` here is a byte range of the same
		// file, not another URL. Fetching the file whole already carries it,
		// which is why nothing reads the range and why that is correct.
		const QByteArray xml =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD mediaPresentationDuration=\"PT5S\"><Period>\n"
		  "  <AdaptationSet mimeType=\"video/mp4\">\n"
		  "    <Representation id=\"v\" bandwidth=\"1\">\n"
		  "      <BaseURL>whole.mp4</BaseURL>\n"
		  "      <SegmentBase indexRange=\"0-900\">\n"
		  "        <Initialization range=\"0-800\"/>\n"
		  "      </SegmentBase>\n"
		  "    </Representation>\n"
		  "  </AdaptationSet>\n"
		  "</Period></MPD>\n";
		const dash_manifest m = dash::parse(xml, base);
		const dash_representation &r = m.representations.first();
		check(m.error.isEmpty(), QString("it parses (%1)").arg(m.error));
		check(r.segments.size() == 1 &&
		          r.segments.value(0) == QUrl("https://cdn.example/v/whole.mp4"),
		      QString("one segment, the file itself (%1)")
		          .arg(r.segments.value(0).toString()));
		check(r.init.isEmpty(),
		      "and no separate init, because the range is inside that file");
	}

	section("what it refuses, because refusing is the whole point");
	{
		// **Each of these would otherwise produce a wrong file rather than no
		// file**, which is the distinction `dash_manifest::error` exists for.
		const QByteArray no_length =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD><Period><AdaptationSet mimeType=\"video/mp4\">\n"
		  "  <SegmentTemplate duration=\"2\" timescale=\"1\" media=\"s$Number$.m4s\"/>\n"
		  "  <Representation id=\"v\" bandwidth=\"1\"/>\n"
		  "</AdaptationSet></Period></MPD>\n";
		const dash_manifest a = dash::parse(no_length, base);
		check(!a.error.isEmpty(),
		      "a segment duration with no manifest length is refused, not "
		      "truncated");
		check(a.representations.value(0).segments.isEmpty(),
		      "and it yields no segments rather than some");

		const QByteArray open_repeat =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD type=\"dynamic\"><Period><AdaptationSet mimeType=\"video/mp4\">\n"
		  "  <SegmentTemplate timescale=\"1000\" media=\"s$Number$.m4s\">\n"
		  "    <SegmentTimeline><S t=\"0\" d=\"2000\" r=\"-1\"/></SegmentTimeline>\n"
		  "  </SegmentTemplate>\n"
		  "  <Representation id=\"v\" bandwidth=\"1\"/>\n"
		  "</AdaptationSet></Period></MPD>\n";
		check(!dash::parse(open_repeat, base).error.isEmpty(),
		      "r=\"-1\" repeats to the end of the period, which is refused "
		      "rather than guessed");

		const QByteArray unknown_id =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD mediaPresentationDuration=\"PT4S\"><Period>\n"
		  "  <AdaptationSet mimeType=\"video/mp4\">\n"
		  "  <SegmentTemplate timescale=\"1\" duration=\"2\""
		  " media=\"s$SubNumber$.m4s\"/>\n"
		  "  <Representation id=\"v\" bandwidth=\"1\"/>\n"
		  "</AdaptationSet></Period></MPD>\n";
		check(!dash::parse(unknown_id, base).error.isEmpty(),
		      "an identifier from a later edition of the spec is refused "
		      "rather than left in the url");

		const QByteArray not_xml = "this is not a manifest at all";
		check(!dash::parse(not_xml, base).error.isEmpty(),
		      "and something that is not XML says so");

		const QByteArray bad_number =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD mediaPresentationDuration=\"PT4S\"><Period>\n"
		  "  <AdaptationSet mimeType=\"video/mp4\">\n"
		  "  <SegmentTemplate timescale=\"lots\" duration=\"2\""
		  " media=\"s$Number$.m4s\"/>\n"
		  "  <Representation id=\"v\" bandwidth=\"1\"/>\n"
		  "</AdaptationSet></Period></MPD>\n";
		check(!dash::parse(bad_number, base).error.isEmpty(),
		      "a timescale that is not a number is refused, because it is a "
		      "divisor and 0 would silently change every count");

		const QByteArray empty_duration =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD mediaPresentationDuration=\"PT\"><Period>\n"
		  "  <AdaptationSet mimeType=\"video/mp4\">\n"
		  "  <SegmentTemplate timescale=\"1\" duration=\"2\""
		  " media=\"s$Number$.m4s\"/>\n"
		  "  <Representation id=\"v\" bandwidth=\"1\"/>\n"
		  "</AdaptationSet></Period></MPD>\n";
		check(!dash::parse(empty_duration, base).error.isEmpty(),
		      "and a bare \"PT\" is not a duration of zero, which would read as "
		      "a live stream");
	}

	section("picking a stream, which DASH makes two questions");
	{
		// **An HLS variant is usually muxed and a DASH representation is not.**
		// Assembling only the video is how a silent file gets produced by
		// something that looks like it worked, so the picker answers per kind
		// and the caller has to ask twice.
		const QByteArray xml =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD mediaPresentationDuration=\"PT4S\"><Period>\n"
		  "  <AdaptationSet mimeType=\"video/mp4\">\n"
		  "    <SegmentTemplate timescale=\"1\" duration=\"2\" media=\"v$Number$-$Bandwidth$.m4s\"/>\n"
		  "    <Representation id=\"lo\" bandwidth=\"500000\" width=\"640\" height=\"360\"/>\n"
		  "    <Representation id=\"hi\" bandwidth=\"3000000\" width=\"1920\" height=\"1080\"/>\n"
		  "  </AdaptationSet>\n"
		  "  <AdaptationSet mimeType=\"audio/mp4\">\n"
		  "    <SegmentTemplate timescale=\"1\" duration=\"2\" media=\"a$Number$.m4s\"/>\n"
		  "    <Representation id=\"aac\" bandwidth=\"128000\"/>\n"
		  "  </AdaptationSet>\n"
		  "</Period></MPD>\n";
		const dash_manifest m = dash::parse(xml, base);
		check(m.error.isEmpty(), QString("it parses (%1)").arg(m.error));
		check(m.representations.size() == 3,
		      QString("three representations across two sets (%1)")
		          .arg(m.representations.size()));
		const dash_representation *v = dash::best_video(m);
		const dash_representation *a = dash::best_audio(m);
		check(v && v->id == "hi",
		      QString("the highest-bandwidth video wins (%1)")
		          .arg(v ? v->id : QString("none")));
		check(a && a->id == "aac",
		      QString("and the audio is found separately (%1)")
		          .arg(a ? a->id : QString("none")));
		check(v && v->segments.value(0) ==
		          QUrl("https://cdn.example/v/v1-3000000.m4s"),
		      QString("$Bandwidth$ is the representation's own (%1)")
		          .arg(v ? v->segments.value(0).toString() : QString()));

		// A manifest with no audio at all must answer null rather than handing
		// back a video representation, which is the mistake a single `best()`
		// invites.
		const QByteArray video_only =
		  "<?xml version=\"1.0\"?>\n"
		  "<MPD mediaPresentationDuration=\"PT2S\"><Period>\n"
		  "  <AdaptationSet mimeType=\"video/mp4\">\n"
		  "    <SegmentTemplate timescale=\"1\" duration=\"2\" media=\"v$Number$.m4s\"/>\n"
		  "    <Representation id=\"v\" bandwidth=\"1\"/>\n"
		  "  </AdaptationSet></Period></MPD>\n";
		const dash_manifest vo = dash::parse(video_only, base);
		check(dash::best_video(vo) != nullptr, "a video-only manifest has video");
		check(dash::best_audio(vo) == nullptr,
		      "and no audio, said as null rather than as the video");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

#include "dash_manifest.h"

#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

#include <cmath>

namespace {

// ISO 8601 durations, which is how an MPD states every length it has:
// `PT1H2M3.5S`, and the date part `P1DT...` a long recording can carry.
//
// **Returns false rather than 0 for something it cannot read**, because 0 is a
// meaningful duration here -- it is what a live manifest with no stated length
// looks like -- and a caller that cannot tell "no duration" from "unreadable
// duration" is the sentinel collision `hls_playlist` already paid for.
bool parse_iso_duration(const QString &text, double *out) {
	static const QRegularExpression re(
	  "\\AP(?:(\\d+(?:\\.\\d+)?)Y)?(?:(\\d+(?:\\.\\d+)?)M)?"
	  "(?:(\\d+(?:\\.\\d+)?)D)?"
	  "(?:T(?:(\\d+(?:\\.\\d+)?)H)?(?:(\\d+(?:\\.\\d+)?)M)?"
	  "(?:(\\d+(?:\\.\\d+)?)S)?)?\\z");
	const QRegularExpressionMatch m = re.match(text.trimmed());
	if (!m.hasMatch())
		return false;
	// A bare "P" matches the pattern and states nothing; so does "PT". Neither
	// is a duration, and accepting them as zero is how an empty field becomes a
	// live stream.
	bool any = false;
	double total = 0.0;
	static const double weight[6] = { 365.0 * 86400.0, 30.0 * 86400.0, 86400.0,
	                                   3600.0, 60.0, 1.0 };
	for (int i = 0; i < 6; ++i) {
		const QString part = m.captured(i + 1);
		if (part.isEmpty())
			continue;
		bool ok = false;
		const double v = part.toDouble(&ok);
		if (!ok)
			return false;
		total += v * weight[i];
		any = true;
	}
	if (!any)
		return false;
	*out = total;
	return true;
}

// `$Number$`, `$Time$`, `$RepresentationID$`, `$Bandwidth$`, and `$$` for a
// literal dollar. Each may carry a printf width -- `$Number%05d$` -- which real
// manifests use and which a naive replace gets wrong by producing an unpadded
// name the server does not have.
QString expand(const QString &tmpl, const QString &rep_id, int bandwidth,
                qint64 number, qint64 time, bool *ok) {
	*ok = true;
	QString out;
	int i = 0;
	while (i < tmpl.size()) {
		const QChar c = tmpl.at(i);
		if (c != QLatin1Char('$')) {
			out += c;
			++i;
			continue;
		}
		const int close = tmpl.indexOf(QLatin1Char('$'), i + 1);
		if (close < 0) {          // an unterminated identifier: not a template
			*ok = false;
			return QString();
		}
		const QString body = tmpl.mid(i + 1, close - i - 1);
		i = close + 1;
		if (body.isEmpty()) {     // `$$`
			out += QLatin1Char('$');
			continue;
		}
		QString name = body;
		QString format;
		const int pct = body.indexOf(QLatin1Char('%'));
		if (pct >= 0) {
			name   = body.left(pct);
			format = body.mid(pct);
		}
		const auto pad = [&format](qint64 v) {
			if (format.isEmpty())
				return QString::number(v);
			// The only form DASH allows is %0<width>d. Anything else is a
			// manifest this does not understand rather than one to guess at.
			static const QRegularExpression fr("\\A%0(\\d+)d\\z");
			const QRegularExpressionMatch fm = fr.match(format);
			if (!fm.hasMatch())
				return QString();
			return QString("%1").arg(v, fm.captured(1).toInt(), 10,
			                          QLatin1Char('0'));
		};
		if (name == QLatin1String("RepresentationID")) {
			out += rep_id;
		} else if (name == QLatin1String("Bandwidth")) {
			const QString s = pad(bandwidth);
			if (s.isEmpty()) { *ok = false; return QString(); }
			out += s;
		} else if (name == QLatin1String("Number")) {
			const QString s = pad(number);
			if (s.isEmpty()) { *ok = false; return QString(); }
			out += s;
		} else if (name == QLatin1String("Time")) {
			const QString s = pad(time);
			if (s.isEmpty()) { *ok = false; return QString(); }
			out += s;
		} else {
			*ok = false;          // an identifier from a later edition of the spec
			return QString();
		}
	}
	return out;
}

// One `<S>` in a `<SegmentTimeline>`.
struct timeline_entry {
	qint64 start    = 0;
	qint64 duration = 0;
	int    repeat   = 0;   // @r; -1 means "until the period ends"
};

struct seg_template {
	bool    present = false;
	QString media;
	QString initialization;
	qint64  timescale    = 1;
	qint64  duration     = 0;   // 0 = not stated
	qint64  start_number = 1;
	bool    has_timeline = false;
	QList<timeline_entry> timeline;
};

struct seg_list {
	bool        present = false;
	QUrl        init;
	QList<QUrl> media;
};

// Everything a Representation inherits from the elements above it. DASH lets
// mimeType, codecs, the resolution and the whole addressing sit on the
// AdaptationSet and be overridden per Representation, so the parse carries a
// stack of these and each level starts as a copy of its parent.
struct level {
	QUrl         base;
	QString      mime;
	QString      codecs;
	QString      id;
	int          bandwidth = 0;
	int          width  = 0;
	int          height = 0;
	double       period_duration = 0.0;
	seg_template tmpl;
	seg_list     list;
	bool         segment_base = false;
};

// **A ceiling on how many segment URLs a manifest can ask for.** The count in
// the @duration case is arithmetic on numbers the manifest supplies, so a
// hostile or broken one can ask for as many as it likes; an hour of two-second
// segments is 1800, and a ten-hour recording 18000. Anything past this is
// refused rather than built, because the alternative is a loop whose length a
// remote file chose.
constexpr qint64 k_segment_ceiling = 100000;

// The whole of the addressing, in one place, because which of the four modes a
// Representation uses is a property of the document and not of the code path
// that got here. Refuses rather than shortens: see `dash_manifest::error`.
template <typename Refuse>
void build_segments(const level &l, dash_representation *rep, Refuse refuse) {
	// **An explicit list wins**, being the one mode that states the answer
	// rather than describing how to compute it.
	if (l.list.present) {
		rep->init     = l.list.init;
		rep->segments = l.list.media;
		if (rep->segments.isEmpty())
			refuse("a SegmentList names no segments");
		return;
	}

	if (l.tmpl.present) {
		if (l.tmpl.media.isEmpty()) {
			refuse("a SegmentTemplate names no media");
			return;
		}
		bool ok = true;
		if (!l.tmpl.initialization.isEmpty()) {
			const QString s = expand(l.tmpl.initialization, l.id, l.bandwidth,
			                          0, 0, &ok);
			if (!ok) {
				refuse("an initialization template uses an identifier this "
				        "does not implement");
				return;
			}
			rep->init = l.base.resolved(QUrl(s));
		}

		// A timeline states each segment's start and length, so the count is
		// read rather than derived, which is why it is preferred where both are
		// present.
		if (l.tmpl.has_timeline) {
			if (l.tmpl.timeline.isEmpty()) {
				refuse("a SegmentTimeline carries no entries");
				return;
			}
			qint64 number = l.tmpl.start_number;
			qint64 built  = 0;
			for (const timeline_entry &e : l.tmpl.timeline) {
				// @r = -1 is "repeat until the period ends", which needs a
				// period length this may not have and is how a live manifest
				// states an open end. Refused rather than guessed.
				if (e.repeat < 0) {
					refuse("a SegmentTimeline entry repeats to the end of the "
					        "period, which this does not derive");
					return;
				}
				qint64 t = e.start;
				for (int k = 0; k <= e.repeat; ++k) {
					if (++built > k_segment_ceiling) {
						refuse("this manifest asks for more segments than will "
						        "be built");
						return;
					}
					const QString s = expand(l.tmpl.media, l.id, l.bandwidth,
					                          number, t, &ok);
					if (!ok) {
						refuse("a media template uses an identifier this does "
						        "not implement");
						return;
					}
					rep->segments.push_back(l.base.resolved(QUrl(s)));
					++number;
					t += e.duration;
				}
			}
			return;
		}

		// No timeline: every segment is the same length and the count comes
		// from the period's. **Both halves are required.** A template stating a
		// duration against a manifest stating no length gives no count at all,
		// and the tempting answer -- build what you can -- is a truncated file
		// that looks complete.
		if (l.tmpl.duration > 0) {
			if (l.period_duration <= 0.0) {
				refuse("a SegmentTemplate states a segment duration but the "
				        "manifest states no length, so the count cannot be "
				        "derived");
				return;
			}
			const double per = double(l.tmpl.duration) / double(l.tmpl.timescale);
			if (per <= 0.0) {
				refuse("a SegmentTemplate segment duration is not positive");
				return;
			}
			const double exact = l.period_duration / per;
			if (exact > double(k_segment_ceiling)) {
				refuse("this manifest asks for more segments than will be "
				        "built");
				return;
			}
			// Ceiling, because a period that is not a whole number of segments
			// still has a last, shorter one.
			const qint64 count = qint64(std::ceil(exact));
			for (qint64 i = 0; i < count; ++i) {
				const QString s =
				  expand(l.tmpl.media, l.id, l.bandwidth,
				          l.tmpl.start_number + i, i * l.tmpl.duration, &ok);
				if (!ok) {
					refuse("a media template uses an identifier this does not "
					        "implement");
					return;
				}
				rep->segments.push_back(l.base.resolved(QUrl(s)));
			}
			return;
		}

		refuse("a SegmentTemplate has neither a timeline nor a segment "
		        "duration");
		return;
	}

	// **SegmentBase, or nothing at all: the representation is one file.** Its
	// `<Initialization range=...>` names a byte range of that same file rather
	// than a separate URL, so there is nothing to prepend -- fetching the file
	// whole already carries it, which is why the range is read by nothing here
	// and why that is correct rather than a gap.
	if (l.segment_base || !l.base.isEmpty()) {
		rep->segments.push_back(l.base);
		return;
	}
	refuse("a Representation states no addressing this understands");
}

}  // namespace

namespace dash {

dash_manifest parse(const QByteArray &xml, const QUrl &base) {
	dash_manifest out;
	QXmlStreamReader r(xml);

	QList<level> stack;
	level root;
	root.base = base;
	stack.push_back(root);

	const auto refuse = [&out](const QString &why) {
		if (out.error.isEmpty())
			out.error = why;
	};

	// The element currently collecting a SegmentTimeline, so `<S>` knows which
	// template to append to.
	auto read_number = [&refuse](const QXmlStreamAttributes &a, const char *name,
	                              qint64 fallback, bool *ok) -> qint64 {
		*ok = true;
		if (!a.hasAttribute(QLatin1String(name)))
			return fallback;
		bool good = false;
		const qint64 v = a.value(QLatin1String(name)).toLongLong(&good);
		if (!good) {
			*ok = false;
			refuse(QString("%1 is not a number in this manifest").arg(name));
			return fallback;
		}
		return v;
	};

	while (!r.atEnd()) {
		r.readNext();
		if (r.hasError())
			break;

		if (r.isStartElement()) {
			const QString name = r.name().toString();
			const QXmlStreamAttributes a = r.attributes();

			if (name == QLatin1String("MPD")) {
				out.is_live = a.value("type").toString() == QLatin1String("dynamic");
				if (a.hasAttribute("mediaPresentationDuration")) {
					double d = 0.0;
					if (parse_iso_duration(
					        a.value("mediaPresentationDuration").toString(), &d))
						out.duration = d;
					else
						refuse("mediaPresentationDuration is not a duration");
				}
				stack.push_back(stack.last());
				stack.last().period_duration = out.duration;
				continue;
			}

			if (name == QLatin1String("BaseURL")) {
				// Resolved against the level above, which is what makes a chain
				// of them compose the way the spec says.
				const QString text = r.readElementText().trimmed();
				if (!text.isEmpty())
					stack.last().base = stack.last().base.resolved(QUrl(text));
				continue;
			}

			if (name == QLatin1String("Period")) {
				stack.push_back(stack.last());
				if (a.hasAttribute("duration")) {
					double d = 0.0;
					if (parse_iso_duration(a.value("duration").toString(), &d))
						stack.last().period_duration = d;
					else
						refuse("a Period duration is not a duration");
				}
				continue;
			}

			if (name == QLatin1String("AdaptationSet") ||
			     name == QLatin1String("Representation")) {
				stack.push_back(stack.last());
				level &l = stack.last();
				if (a.hasAttribute("mimeType"))
					l.mime = a.value("mimeType").toString();
				if (a.hasAttribute("codecs"))
					l.codecs = a.value("codecs").toString();
				bool ok = true;
				if (a.hasAttribute("width"))
					l.width = int(read_number(a, "width", l.width, &ok));
				if (a.hasAttribute("height"))
					l.height = int(read_number(a, "height", l.height, &ok));
				// Identity, which the templates below interpolate. Read here
				// because a pull parser cannot go back for it at the end
				// element, and inherited so an AdaptationSet stating a
				// bandwidth is not lost.
				if (a.hasAttribute("id"))
					l.id = a.value("id").toString();
				if (a.hasAttribute("bandwidth"))
					l.bandwidth = int(read_number(a, "bandwidth", l.bandwidth, &ok));
				continue;
			}

			if (name == QLatin1String("SegmentTemplate")) {
				level &l = stack.last();
				seg_template t = l.tmpl;     // inherited, then overridden
				t.present = true;
				if (a.hasAttribute("media"))
					t.media = a.value("media").toString();
				if (a.hasAttribute("initialization"))
					t.initialization = a.value("initialization").toString();
				bool ok = true;
				t.timescale    = read_number(a, "timescale", t.timescale, &ok);
				t.duration     = read_number(a, "duration", t.duration, &ok);
				t.start_number = read_number(a, "startNumber", t.start_number, &ok);
				if (t.timescale <= 0) {
					refuse("a SegmentTemplate timescale is not positive");
					t.timescale = 1;
				}
				l.tmpl = t;
				continue;
			}

			if (name == QLatin1String("SegmentTimeline")) {
				stack.last().tmpl.present      = true;
				stack.last().tmpl.has_timeline = true;
				stack.last().tmpl.timeline.clear();
				continue;
			}

			if (name == QLatin1String("S")) {
				seg_template &t = stack.last().tmpl;
				if (!t.has_timeline)
					continue;            // an `S` outside a timeline is not ours
				timeline_entry e;
				bool ok = true;
				// @t is optional after the first: the previous entry's end.
				const qint64 previous_end =
				  t.timeline.isEmpty()
				    ? 0
				    : t.timeline.last().start +
				        t.timeline.last().duration *
				          (t.timeline.last().repeat + 1);
				e.start    = read_number(a, "t", previous_end, &ok);
				e.duration = read_number(a, "d", 0, &ok);
				e.repeat   = int(read_number(a, "r", 0, &ok));
				if (e.duration <= 0)
					refuse("a SegmentTimeline entry has no usable duration");
				t.timeline.push_back(e);
				continue;
			}

			if (name == QLatin1String("SegmentList")) {
				stack.last().list.present = true;
				stack.last().list.media.clear();
				continue;
			}

			if (name == QLatin1String("SegmentURL")) {
				if (!stack.last().list.present)
					continue;
				const QString media = a.value("media").toString();
				// **A range we would drop is a refusal, not a segment.**
				// `mediaRange` says this segment is bytes a..b of `media`,
				// which is DASH's `#EXT-X-BYTERANGE`: several segments inside
				// one file. `dash_representation::segments` is a list of urls
				// with nowhere to carry a range, so honouring it is a feature
				// rather than a parse -- and ignoring it is worse than either.
				// Every SegmentURL would name the same whole file, the
				// assembler would fetch it once per segment with no `Range`
				// header to check against, and the output would be N copies
				// reported as a finished save. That is the shape the
				// byte-range check in `hls_assembler` exists for, and it
				// cannot see this one because no range is ever sent.
				if (a.hasAttribute(QLatin1String("mediaRange"))) {
					refuse("a SegmentURL carries mediaRange, which names a byte "
					        "range of one file -- this addressing is not "
					        "supported rather than approximated");
					continue;
				}
				if (media.isEmpty())
					refuse("a SegmentURL names no media");
				else
					stack.last().list.media.push_back(
					  stack.last().base.resolved(QUrl(media)));
				continue;
			}

			if (name == QLatin1String("SegmentBase")) {
				stack.last().segment_base = true;
				continue;
			}

			if (name == QLatin1String("Initialization") ||
			     name == QLatin1String("Initialisation")) {
				level &l = stack.last();
				if (a.hasAttribute("sourceURL"))
					l.list.init =
					  l.base.resolved(QUrl(a.value("sourceURL").toString()));
				continue;
			}
			continue;
		}

		if (!r.isEndElement())
			continue;

		const QString name = r.name().toString();
		if (name == QLatin1String("Representation")) {
			const level l = stack.last();
			dash_representation rep;
			rep.id        = l.id;
			rep.bandwidth = l.bandwidth;
			rep.mime_type = l.mime;
			rep.codecs    = l.codecs;
			rep.width     = l.width;
			rep.height    = l.height;
			build_segments(l, &rep, refuse);
			out.representations.push_back(rep);
			stack.removeLast();
			continue;
		}
		if (name == QLatin1String("AdaptationSet") ||
		     name == QLatin1String("Period") || name == QLatin1String("MPD"))
			stack.removeLast();
	}

	if (r.hasError())
		refuse("the manifest is not well-formed XML: " + r.errorString());
	return out;
}

const dash_representation *best_video(const dash_manifest &m) {
	const dash_representation *best = nullptr;
	for (const dash_representation &r : m.representations)
		if (r.is_video() && (!best || r.bandwidth > best->bandwidth))
			best = &r;
	return best;
}

const dash_representation *best_audio(const dash_manifest &m) {
	const dash_representation *best = nullptr;
	for (const dash_representation &r : m.representations)
		if (r.is_audio() && (!best || r.bandwidth > best->bandwidth))
			best = &r;
	return best;
}

}  // namespace dash

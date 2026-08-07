// SPDX-License-Identifier: GPL-3.0-or-later
#include "extractor_dialog.h"
#include "ai_provider.h"
#include "extractor_signals.h"
#include "stream_probe.h"
#include "extractor_helpers.h"

#include <QDialogButtonBox>
#include <QHash>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QThread>
#include <QCoreApplication>
#include <QtGlobal>

#include <algorithm>

namespace {

const char *k_system_prompt =
	"You write small JavaScript functions that pick the video stream out of a "
	"list of requests a web page made.\n\n"
	"Reply with JavaScript and nothing else — no prose, no markdown fences.\n"
	"Assign to `extract`:\n\n"
	"  extract = function (page, requests) {\n"
	"    // page:     { url, host }\n"
	"    // requests: [ { url, type, order, seen }, ... ]\n"
	"    //   type is what the browser fetched it as: 'script', 'image' or\n"
	"    //   'other'. It is never 'hls' or 'dash' — that is your conclusion,\n"
	"    //   and it goes in `kind` below.\n"
	"    //   seen is how many times the page fetched that shape: 1 for a\n"
	"    //   manifest, many for a segment. It is the same number as the\n"
	"    //   table's `seen` column, so you may test it in code.\n"
	"    return { url: one of the request urls, kind: 'hls'|'dash'|'direct',\n"
	"             headers: { Referer: page.url } };   // or null\n"
	"  };\n\n"
	"Rules:\n"
	"1. Return a url **taken from the requests array**. Never build one, never "
	"guess one: a returned url that was not requested is rejected outright.\n"
	"2. Match on shape, not on an exact string — the next visit will have "
	"different ids, tokens and numbers. Prefer a stable path fragment.\n"
	"3. **Write two tests, not one.** First match the address the notes below "
	"the table identify as a stream. Then, separately, fall back to a manifest "
	"extension (`.m3u8`, `.mpd`). Return whichever matches, notes first.\n"
	"   `seen === 1` narrows a search; it never picks on its own. **Row 0 is the "
	"page itself** — fetched once, type `other` — so `seen === 1 && type === "
	"'other'` finds the page before it finds anything else, and the page is "
	"never the stream. Skip any request whose url is `page.url`.\n"
	"   Both are needed because sites differ and you cannot tell which kind you "
	"are looking at: a master playlist is routinely disguised with an innocuous "
	"extension and a query string, which only the notes will catch — and plenty "
	"of other sites serve a plain `.m3u8`, which the extension test catches for "
	"free. A function carrying only one of the two tests is wrong on half the "
	"sites this has been measured against.\n"
	"4. The evidence is a table: `order | type | seen | url`. `seen` is how many "
	"times the page fetched that shape — a high count means segments, and a "
	"manifest is fetched once. Some rows have a note below the table saying what "
	"that address turned out to serve; read those notes to decide *which* row is "
	"the stream, then find that row again by a stable part of its url. Do not "
	"match on `order`: it is this visit's position in the list and will be a "
	"different number next time.\n"
	"5. Prefer the manifest over any individual segment.\n"
	"6. Return null if nothing in the list is a stream. Returning nothing is a "
	"clean answer; a guess is not.\n"
	"7. Anything fetched as a `script` or an `image` is page furniture, not the "
	"stream — returning one is rejected, so do not fall back to it.\n";

// How much an address reads like a playlist. Two tiers, because "master" and
// "m3u8" name the thing itself while "index" and "hls" merely sit near it -- on
// one measured site `index.php?…do=getVideo` is an API and `master.txt` is the
// manifest, and asking the wrong one first wastes the only question that host
// gets.
int playlistish(const QUrl &u) {
	const QString s = (u.host() + u.path()).toLower();
	for (const char *strong : { "m3u8", "master", "playlist", "manifest", ".mpd" })
		if (s.contains(QLatin1String(strong)))
			return 2;
	for (const char *weak : { "index", "/hls", "/m3/", "stream" })
		if (s.contains(QLatin1String(weak)))
			return 1;
	return 0;
}

// The shared one, so the fold the user reads, the ranking that spends the probe
// budget and the gate's segment rule cannot drift apart.
QString shape_of(const QString &url) {
	return site_extractor::shape_of(QUrl(url));
}

}  // namespace

QString extractor_dialog::summarise(const QList<evidence_request> &evidence,
                                     int *kept) {
	QStringList lines;
	QHash<QString, int> seen_shape;
	QHash<QString, int> repeats;
	QList<QString> order;
	QStringList urls, kinds;
	QList<int> orders;

	for (const evidence_request &r : evidence) {
		const QString shape = shape_of(r.url.toString());
		if (seen_shape.contains(shape)) {
			repeats[shape]++;
			continue;
		}
		seen_shape.insert(shape, r.order);
		order << shape;
		// What the server said this really is, where it was asked. This is the
		// channel the measured site tells the truth on, so it belongs beside
		// the address -- but in a **column of its own, with the url last**.
		//
		// It used to be appended to the url as `   -> …`, and that cost the
		// whole mechanism. Measured on kisskh: four runs in five wrote
		// `url.includes('->')`, reading the note as part of the address it was
		// printed against, so those branches matched nothing and both hits came
		// from an `.m3u8` extension fallback instead -- on the one site where a
		// fallback works. Two rounds of prose had already been spent telling the
		// model the url ends where the note begins; the layout was saying the
		// opposite the whole time, and layout won. With the url last, nothing
		// trails it and there is nothing to mistake for part of it.
		urls << r.url.toString().left(300);
		orders << r.order;
		kinds << r.kind;
	}
	// Say what was folded away rather than silently dropping it: a reader
	// should be able to tell a page that fetched one thing from a page that
	// fetched it four hundred times. As a count in its own column, for the same
	// reason the note is one -- it used to be appended after the url, and
	// anything printed after an address is something that can be read as part
	// of it.
	for (int i = 0; i < order.size(); ++i)
		lines << QString("%1 | %2 | %3 | %4")
		             .arg(orders[i], 4).arg(kinds[i], -6)
		             .arg(1 + repeats.value(order[i]), 4).arg(urls[i]);
	if (kept)
		*kept = lines.size();
	return lines.join('\n');
}

QList<evidence_request> extractor_dialog::candidates(
    const QList<evidence_request> &evidence, const QUrl &page, int max) {
	// One per shape, because probing sixty numbered segments learns the same
	// thing sixty times.
	QHash<QString, int> repeats;      // shape -> extra sightings
	QHash<QString, int> host_repeats; // host  -> extra sightings, all shapes
	QList<evidence_request> firsts;
	const QString page_norm =
	  page.adjusted(QUrl::RemoveFragment | QUrl::StripTrailingSlash).toString();

	for (const evidence_request &r : evidence) {
		// Furniture the gate refuses anyway, and the document itself. Spending
		// a request on either would be spending it to be told what is known.
		if (r.kind == "image" || r.kind == "script")
			continue;
		// Only things that can be fetched. A websocket address cannot answer
		// "what do you serve", and spending a question on one is spending it to
		// learn nothing — two of ten went that way on a real capture.
		if (r.url.scheme() != "http" && r.url.scheme() != "https")
			continue;
		if (r.url.adjusted(QUrl::RemoveFragment | QUrl::StripTrailingSlash)
		        .toString() == page_norm)
			continue;
		const QString shape = shape_of(r.url.toString());
		if (repeats.contains(shape)) {
			repeats[shape]++;
			host_repeats[r.url.host()]++;
			continue;
		}
		repeats.insert(shape, 0);
		host_repeats.contains(r.url.host()) ? void()
		                                     : void(host_repeats.insert(r.url.host(), 0));
		firsts << r;
	}

	// Ranking, and "fetched once" alone is not enough: analytics beacons and
	// stylesheets are fetched once too, and they arrive first, so a budget
	// ordered on that spends itself on trackers before reaching the video —
	// measured, on a real capture, where the ten questions went to Google
	// Analytics, Yandex and a CSS file.
	//
	// The signal that does work is already in the evidence. **The host that
	// served a flood of near-identical requests is the media host**, so a
	// request fetched *once* on *that* host is the manifest, and a request
	// fetched once on a host that never repeated anything is a beacon. Segments
	// come next, since knowing one is `video/mp4` tells the model what the
	// flood is; unrelated one-offs come last.
	// By how big the host's flood is, not by whether it has one. "Any repeat"
	// was the first attempt and it fails on real traffic: analytics beacons
	// repeat two or three times, so a tracker host scored exactly the same as
	// the CDN serving forty segments, and the manifest came sixth of eight.
	// A flood is a matter of degree, so the ranking has to be too.
	std::stable_sort(firsts.begin(), firsts.end(),
	                  [&](const evidence_request &a, const evidence_request &b) {
		const int flood_a = host_repeats.value(a.url.host());
		const int flood_b = host_repeats.value(b.url.host());
		if (flood_a != flood_b)
			return flood_a > flood_b;         // the busiest host first
		// Within a host, ask about the thing that looks like a playlist first.
		//
		// This is a *lexical* guess and it is placed here deliberately, because
		// this is the one place where being wrong is nearly free: it decides
		// which addresses we spend a 2 KB question on, not what the model may
		// answer and not what the gate will accept. The words are HLS's own —
		// master playlist, index/media playlist, `.m3u8` — so they are not one
		// site's vocabulary.
		//
		// Checked against all three captures rather than tuned until two passed,
		// which is how the rule this replaces came to be wrong on the second
		// site: `cf-master.…txt`, `master.txt` and `…_index.m3u8` are the three
		// manifests, and each is the first thing asked about on its host.
		const int look_a = playlistish(a.url);
		const int look_b = playlistish(b.url);
		if (look_a != look_b)
			return look_a > look_b;
		// Then: what was fetched once is the manifest, what repeated is one of
		// its segments.
		const bool rep_a = repeats.value(shape_of(a.url.toString())) > 0;
		const bool rep_b = repeats.value(shape_of(b.url.toString())) > 0;
		return rep_a < rep_b;
	});
	// And then spread the budget *across* hosts rather than spending it down one
	// ranked list, which is the fix a second site forced.
	//
	// The flood rule above was derived from one capture and is wrong on the
	// next. On a site whose ad networks are busier than its player, the busiest
	// host is an ad network: measured, and comprehensively — all ten questions
	// went to beacons, fonts and a favicon, and the host actually serving the
	// video was never asked about at all. The media host there served fourteen
	// requests of fourteen *different* shapes, so it never looked like a flood.
	//
	// Ranking cannot tell those apart from inside one list, so the budget is
	// dealt round-robin: every host's best candidate before any host's second.
	// A host that is wrong costs one question instead of all of them, and the
	// ordering above still decides who goes first and what each host offers.
	QList<QString> host_order;
	QHash<QString, QList<evidence_request>> by_host;
	for (const evidence_request &r : firsts) {
		const QString h = r.url.host();
		if (!by_host.contains(h))
			host_order << h;
		by_host[h].append(r);
	}
	QList<evidence_request> dealt;
	for (int round = 0; ; ++round) {
		bool any = false;
		for (const QString &h : host_order) {
			const QList<evidence_request> &list = by_host[h];
			if (round >= list.size())
				continue;
			any = true;
			dealt << list[round];
			if (max >= 0 && dealt.size() >= max)
				return dealt;
		}
		if (!any)
			break;
	}
	return dealt;
}

QString extractor_dialog::transcript_text(const QList<helper_call> &calls) {
	// The point of this is that the question stops being "read this code and
	// predict what it does" and becomes "here is what it did". So it is written
	// for someone deciding whether to trust it, not for a log: a refused call
	// is marked in the margin rather than merely worded differently, because a
	// refusal is the one line that must not be skimmed past.
	QStringList lines;
	for (const helper_call &c : calls) {
		const QString mark = c.allowed ? QStringLiteral("  ")
		                                : QStringLiteral("! ");
		QString target = c.target;
		if (target.size() > 96)
			target = target.left(60) + "…" + target.right(35);
		lines << QString("%1%2  %3\n        %4")
		             .arg(mark, c.verb.leftJustified(4), target, c.outcome);
	}
	if (lines.isEmpty())
		return QString();
	return lines.join('\n');
}

void extractor_dialog::use_helpers(helper_host *helpers) { m_helpers = helpers; }

void extractor_dialog::show_transcript() {
	const QString text =
	  m_helpers ? transcript_text(m_helpers->transcript()) : QString();
	const bool any = !text.isEmpty();
	m_transcript->setPlainText(text);
	m_transcript->setVisible(any);
	m_transcript_label->setVisible(any);
	if (!any)
		return;
	m_transcript_label->setText(
	  m_helpers->breached()
	    ? QString("What it did — and where it was stopped:")
	    : QString("What it did (%1 %2, %3 bytes):")
	          .arg(m_helpers->calls_used())
	          .arg(m_helpers->calls_used() == 1 ? "call" : "calls")
	          .arg(m_helpers->bytes_used()));
}

QString extractor_dialog::strip_fences(const QString &reply) {
	QString s = reply.trimmed();
	if (!s.startsWith("```"))
		return s;
	// Drop the opening fence and its language tag, then anything from the
	// closing fence onwards.
	const int first_nl = s.indexOf('\n');
	if (first_nl < 0)
		return QString();
	s = s.mid(first_nl + 1);
	const int close = s.lastIndexOf("```");
	if (close >= 0)
		s = s.left(close);
	return s.trimmed();
}

extractor_dialog::extractor_dialog(extractor_signals *signals_source,
                                    extractor_store *store, ai_provider *provider,
                                    const QString &site_host, const QUrl &page_url,
                                    QWidget *parent)
  : QDialog(parent), m_signals(signals_source), m_store(store),
    m_provider(provider), m_site(site_host), m_page(page_url) {
	setWindowTitle("Learn this site");
	resize(860, 620);
	build_ui();

	m_evidence = m_signals->evidence_for(m_site);
	rebuild_payload();

	connect(m_provider, &ai_provider::finished, this, &extractor_dialog::on_reply);
	connect(m_provider, &ai_provider::failed,   this, &extractor_dialog::on_failed);

	if (m_evidence.isEmpty()) {
		m_status->setText("No requests recorded for this page yet. Many sites "
		                  "fetch nothing until their player starts, so press "
		                  "play first, then try again.");
		m_send->setEnabled(false);
		return;
	}

	// Ask the server about the likely candidates before anyone asks the model.
	// This happens on open rather than on Send for two reasons: the payload
	// pane is a promise about what will be sent, so it has to be complete
	// before Send is available; and these requests go to the site the page
	// already talked to, not to the provider, so the "nothing leaves until you
	// press Send" rule is untouched.
	probe_candidates();
}

void extractor_dialog::rebuild_payload() {
	int kept = 0;
	const QString folded = summarise(m_evidence, &kept);

	QString payload = "Page: " + m_page.toString() + "\n";
	payload += "Host: " + m_site + "\n\n";
	payload += QString("Requests this page made (%1 shown, %2 seen):\n")
	               .arg(kept).arg(m_evidence.size());
	// Name the columns. The middle one is the browser's resource type, and
	// unlabelled it reads as anything the reader likes.
	payload += "order | type | seen | url\n";
	payload += folded;
	// **A legend, not a column.** Third arrangement of the same fact, and the
	// first two each fixed the previous symptom and produced the next one: the
	// note appended to the url gave `url.includes('->')`, and the note as a
	// column named `serves` gave `request.serves` -- undefined at run time, in
	// eight runs of ten across both sites. Anything printed *as part of a row*
	// reads as a property of that row.
	//
	// So it is printed away from the rows entirely, keyed by the order number,
	// and phrased as something that was found out rather than something a
	// request carries. The join is done by eye, here, now -- which is the only
	// place it can be done, since nothing is probed on a later visit.
	if (!m_served.isEmpty()) {
		payload += "\n\nWhat some of those addresses turned out to serve, found "
		            "by fetching them with this page's context. This is the "
		            "server's own answer, so it outranks the file extension, "
		            "which on many sites is chosen to mislead:\n";
		for (const evidence_request &r : m_evidence) {
			const QString what = m_served.value(r.url.toString());
			if (what.isEmpty())
				continue;
			payload += QString("  request %1 turned out to be %2\n")
			               .arg(r.order).arg(what);
		}
		payload += "Addresses not listed here were never asked, which is not the "
		            "same as being asked and found ordinary.";
	}
	// Where that instruction goes decides whether it is read at all. Measured:
	// with the note above and nothing in the tail, five runs in five wrote
	// `endsWith('.m3u8')` over a list whose manifest was annotated HLS on the
	// line beside it — the annotation was present, correct, and ignored. That is
	// the same displacement the three findings below record, so it gets the same
	// remedy: say it after the evidence, in the paragraph the model demonstrably
	// acts on.
	//
	// The second half is the load-bearing one, and the first attempt at it was
	// too subtle. Saying only that the notes "are not there on a later visit"
	// moved three runs in five to `url.includes('-> application/vnd.apple.
	// mpegurl')` — the annotation was finally being read, and read as part of
	// the url, because the fold prints it on the same line and nothing said
	// otherwise. So it now names the fields a request actually has and says
	// plainly that the url ends where the note begins. The note exists only
	// while the extractor is being written; a stored script runs on later visits
	// where nothing has been probed, so it is evidence for *which* address to
	// match, never something the script may test for.
	// The contract again, after the evidence rather than only before it, and the
	// exact wording is measured rather than chosen. On the synthetic set this is
	// redundant; on evidence captured from a real page — eighteen times longer,
	// and shaped exactly like something one summarises — the model answered five
	// times out of five with prose and no extract() at all. Three findings, five
	// runs each:
	//
	//   * "reply in JavaScript" alone moved four in five from prose to code, and
	//     all four wrote `extract(url)` over a single address. Restating the
	//     format while omitting the shape displaced the shape.
	//   * A skeleton restoring the shape was copied out verbatim, placeholders
	//     included, so `url: <one of those urls>` came back as a syntax error.
	//     It is prose now, with nothing that can be pasted.
	//   * The note about `kind` is not padding. It named two vocabularies
	//     sharing one field name, and two runs in five returned
	//     `request.kind === 'hls'`, true of nothing. The request side is called
	//     `type` now, and this says so.
	payload += "\n\nNow write the extractor for the requests above. Assign to "
	           "`extract` a function of two arguments, `page` and `requests`, "
	           "where `requests` is the list above and each entry has `url`, "
	           "`type`, `order` and `seen`.\n\n"
	           "`type` is what the browser fetched it as — `script`, `image` or "
	           "`other` — and never `hls` or `dash`, so testing for those "
	           "matches nothing. Anything fetched as a `script` or an `image` "
	           "is page furniture and will be rejected, so it is not a useful "
	           "fallback either.\n\n";
	if (!m_served.isEmpty())
		payload += "The notes under the table settle what the extension cannot: "
		            "an address noted as HLS is the manifest however it is named, "
		            "and one noted as video is a segment however it is named.\n\n"
		            "More than one note can say so, and they are "
		            "not alternatives. If any note says HLS or DASH, "
		            "that address is the manifest and it is the answer — return it, with "
		            "`kind` `hls` or `dash`. Everything the manifest describes "
		            "reaches you as a separate request: an initialisation "
		            "segment, then the numbered media segments. Those are pieces "
		            "of that same stream and returning one gives a few seconds of "
		            "video or none at all. Only return a piece when no line is "
		            "noted as a manifest.\n\n"
		            "Those notes are for you, reading this now. They are not "
		            "data your function will get: there is no field holding what "
		            "an address serves. It cannot be otherwise — your function "
		            "runs on later visits where nothing has been fetched to ask, "
		            "so anything it could read would be empty.\n\n"
		            "`seen` is the exception and it is real: it counts how many "
		            "times *this* page fetched that shape, which a later visit "
		            "can count for itself. Test it in code freely — a high count "
		            "is a segment. But `seen === 1` is true of the page's own "
		            "address too, which is the first row, so it narrows a search "
		            "and never decides one. It is what an address serves that you "
		            "must decide now, not how often it was asked for.\n\n"
		            "Which means the work happens in two steps, and only the "
		            "second one is code. **Now:** read the notes and decide which "
		            "address is the stream. **In your function:** find that "
		            "address again by a stable part of its url. Its ids, tokens "
		            "and position in the list will all be different next time; "
		            "the shape of its path will not.\n\n";
	// The correction, last of all, for the reason every other instruction in
	// this payload sits where it does: measured, the model acts on the tail.
	if (!m_last_refused_reason.isEmpty()) {
		payload += "One more thing, and it is the most important. **A previous "
		            "attempt was rejected.** This is what it returned:\n\n";
		payload += m_last_refused_source + "\n\n";
		payload += "It was refused because: " + m_last_refused_reason + "\n\n";
		payload += "Do not send that again. The reason above says what to "
		            "change; everything else about the task is unchanged.\n\n";
	}

	payload += "Return an object whose `url` is one of the addresses above "
	           "copied exactly, whose `kind` is `hls`, `dash` or `direct`, and "
	           "whose `headers` sets `Referer` to `page.url` — or return null "
	           "if none of them is a stream. Reply with that JavaScript and "
	           "nothing else: no summary of this list, no explanation, no "
	           "markdown fences.";
	m_payload->setPlainText(payload);
}

void extractor_dialog::build_ui() {
	auto *outer = new QVBoxLayout(this);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	m_status->setText(
	  m_provider->is_external()
	    ? QString("<b>%1</b> — external provider. This is the list of "
	               "addresses this page requested; read it before sending, "
	               "and nothing leaves until you press Send.")
	          .arg(m_provider->name())
	    : QString("<b>%1</b> — local provider; nothing leaves this machine.")
	          .arg(m_provider->name()));
	outer->addWidget(m_status);

	m_pages = new QStackedWidget(this);

	// Named, not merely ordered. Drivers used to find these by position — "the
	// pane built second" — which broke silently the moment a pane was added,
	// and reported the wrong text rather than failing.
	m_payload = new QPlainTextEdit(this);
	m_payload->setObjectName("payload");
	m_payload->setReadOnly(true);
	// **Wrapped, which it was not.** This pane holds two kinds of text: a
	// column-aligned request table, and paragraphs of prose explaining what is
	// about to be sent. `NoWrap` was chosen for the table -- its columns line
	// up only if nothing reflows -- and it left every prose line running off
	// the right edge behind a horizontal scrollbar.
	//
	// Photographing this dialog for the first time is what showed it, and the
	// trade goes the other way once seen: this is the pane somebody reads to
	// decide whether to send anything to a model at all, and a paragraph that
	// has to be scrolled sideways line by line is one nobody reads. The table
	// rows are short and survive wrapping; a long url now folds instead of
	// disappearing, which is the smaller loss.
	m_payload->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	m_pages->addWidget(m_payload);

	auto *result_page = new QWidget(this);
	auto *rl = new QVBoxLayout(result_page);
	rl->setContentsMargins(0, 0, 0, 0);
	m_result = new QLabel(result_page);
	m_result->setObjectName("verdict");
	m_result->setWordWrap(true);
	m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
	rl->addWidget(m_result);
	m_script = new QPlainTextEdit(result_page);
	m_script->setObjectName("proposal");
	m_script->setReadOnly(true);
	rl->addWidget(m_script, 1);

	// What the script actually did, when it was allowed to do anything
	// (§11.5.1). Hidden entirely on the pure tier, where there is nothing to
	// say and an empty box would only ask a question that has no answer.
	m_transcript_label = new QLabel("What it did:", result_page);
	m_transcript_label->setObjectName("transcript_label");
	rl->addWidget(m_transcript_label);
	m_transcript = new QPlainTextEdit(result_page);
	m_transcript->setObjectName("transcript");
	m_transcript->setReadOnly(true);
	m_transcript->setLineWrapMode(QPlainTextEdit::NoWrap);
	rl->addWidget(m_transcript, 1);
	m_transcript_label->hide();
	m_transcript->hide();

	m_pages->addWidget(result_page);

	outer->addWidget(m_pages, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	m_send  = buttons->addButton("&Send", QDialogButtonBox::ActionRole);
	m_apply = buttons->addButton("&Use This Extractor", QDialogButtonBox::AcceptRole);
	m_apply->setEnabled(false);
	connect(m_send,  &QPushButton::clicked, this, &extractor_dialog::on_send);
	connect(m_apply, &QPushButton::clicked, this, &extractor_dialog::on_accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);
}

void extractor_dialog::on_send() {
	m_send->setEnabled(false);
	m_status->setText(QString("Asking %1…").arg(m_provider->name()));
	m_provider->send(QString::fromLatin1(k_system_prompt), m_payload->toPlainText());
}

void extractor_dialog::on_failed(const QString &error) {
	m_send->setEnabled(true);
	m_status->setText("<b>Failed:</b> " + error.toHtmlEscaped());
}

void extractor_dialog::on_reply(const QString &text) {
	m_send->setEnabled(true);
	m_proposal = strip_fences(text);
	m_script->setPlainText(m_proposal);
	m_pages->setCurrentIndex(1);

	// Judged before it is offered, never after. The gate is the whole reason
	// this is safe to run at all (§11.5) — but it is judged on another thread,
	// because a helper-tier script blocks on the network and blocking here
	// would freeze the window for the whole deadline. The pure tier takes the
	// same path: a runaway script is bounded by the watchdog either way, and one
	// path is easier to reason about than two.
	m_apply->setEnabled(false);
	m_status->setText("Judging the proposal…");

	const QString  src  = m_proposal;
	const QUrl     page = m_page;
	const auto     ev   = m_evidence;
	helper_host   *hs   = m_helpers;
	// Copied, not referenced: this is read on the judging thread while the probe
	// callbacks that fill it run on this one.
	const QSet<QString> manifests = m_manifests;
	QPointer<extractor_dialog> self(this);

	QThread *t = QThread::create([src, page, ev, hs, manifests, self] {
		const extractor_verdict v =
		  site_extractor::check(src, page, ev, hs, &manifests);
		// Back to the UI thread, and only if the dialog is still there. A
		// closed dialog leaves this to find a null QPointer and stop, rather
		// than writing into freed widgets.
		QMetaObject::invokeMethod(qApp, [self, v] {
			if (self)
				self->on_judged(v);
		}, Qt::QueuedConnection);
	});
	connect(t, &QThread::finished, t, &QObject::deleteLater);
	t->start();
}

// The other half of on_reply, on the UI thread with a verdict in hand.
void extractor_dialog::on_judged(const extractor_verdict &verdict) {
	m_verdict = verdict;
	m_apply->setEnabled(m_verdict.usable);

	// Shown whatever the verdict. A rejected proposal's transcript is the more
	// interesting one: it is where a script that reached somewhere it should not
	// have is visible, and hiding it on rejection would hide exactly that.
	show_transcript();

	if (!m_verdict.usable) {
		m_result->setText("<b>Not usable.</b> " + m_verdict.message.toHtmlEscaped() +
		                   (m_verdict.invented
		                        ? "<br><br>A script that returns an address the "
		                          "page never asked for is refused outright — "
		                          "there is no safe way to accept one."
		                        : QString()) +
		                   (m_verdict.is_piece
		                        ? "<br><br>An initialisation or media segment plays "
		                          "as a fraction of a second or as nothing at all. "
		                          "The playlist beside it is what names them in "
		                          "order, so that is the address worth keeping."
		                        : QString()));
		// Remembered for the next send. A rejected attempt plus the reason it
		// was rejected is far more than the first attempt had, and the gate's
		// messages are written to be acted on -- "the script reads a `serves`
		// value at run time ... decide from it now and match the address" is an
		// instruction, not just a complaint.
		if (m_script)
			m_last_refused_source = m_script->toPlainText().left(1200);
		m_last_refused_reason = m_verdict.message;
		rebuild_payload();   // so the next send carries the correction
		m_status->setText("The proposal was rejected. Sending again now tells the "
		                   "model what was wrong with it.");
		return;
	}

	m_result->setText(
	  QString("<b>Accepted.</b> %1<br><br>It picks:<br><tt>%2</tt><br><br>"
	           "That address is one this page really requested. Using it "
	           "stores the script for <b>%3</b>, and it will run on this site "
	           "from now on.")
	      .arg(m_verdict.message.toHtmlEscaped(),
	           m_verdict.result.url.toString().left(200).toHtmlEscaped(),
	           m_site.toHtmlEscaped()));
	m_status->setText("Reviewed and validated. Nothing is saved until you accept.");

	confirm_by_fetching();
}

// The evidence the model gets is urls, types and order — and on the site this
// was measured against, the url is the one channel that lies. Its manifest
// wears `.txt` and its segments wear `.woff2`, while the server, asked
// directly, answers `application/vnd.apple.mpegurl` and `video/mp4`. Four
// rounds of prompt work could not find the stream in the addresses because the
// answer was never in them. So ask first, and send what came back.
void extractor_dialog::probe_candidates() {
	if (!m_probe)
		m_probe = new stream_probe(this);

	const QList<evidence_request> picks =
	  candidates(m_evidence, m_page, k_max_probes);
	if (picks.isEmpty())
		return;

	stream_context ctx;
	ctx.referer = m_page.toString();

	m_pending = int(picks.size());
	m_send->setEnabled(false);
	m_status->setText(QString("Asking the server what %1 of these addresses "
	                           "actually serve…").arg(m_pending));

	for (const evidence_request &r : picks) {
		const QString url = r.url.toString();
		m_probe->probe(r.url, ctx, [this, url](const probe_result &res) {
			if (qEnvironmentVariableIsSet("HYDRA_PROBE_DEBUG"))
				qWarning("probe %s -> reached=%d status=%d kind=%s (%s)",
				          qPrintable(url.left(80)), int(res.reached), res.status,
				          qPrintable(res.kind), qPrintable(res.reason.left(90)));
			// Only a positive identification is worth a line in the payload. An
			// unreachable address or a refused one says nothing about what it
			// is, and writing "unknown" beside it would read as a finding.
			if (res.reached && !res.kind.isEmpty()) {
				m_served.insert(url, res.content_type.isEmpty()
				  ? res.kind.toUpper()
				  : QString("%1 (%2)").arg(res.content_type, res.kind.toUpper()));
				// A playlist is the one answer the gate can act on, so it is kept
				// separately and normalised the way the gate compares urls.
				if (res.kind == "hls" || res.kind == "dash")
					m_manifests.insert(QUrl(url)
					  .adjusted(QUrl::RemoveFragment | QUrl::StripTrailingSlash)
					  .toString());
			} else if (res.reached && res.status >= 400) {
				m_served.insert(url, QString("%1, not established")
				                          .arg(res.status));
			}
			if (--m_pending > 0)
				return;

			rebuild_payload();
			m_send->setEnabled(true);
			int found = 0;
			for (const QString &v : std::as_const(m_served))
				if (!v.contains("not established")) ++found;
			m_status->setText(found
			  ? QString("%1 of those addresses said what they serve, and that "
			             "is in the list below. Nothing leaves until you press "
			             "Send.").arg(found)
			  : QString("The server did not say what any of them serve. The "
			             "list below is the addresses alone."));
		});
	}
}

// §11.5's last clause, and §10's content-type tier: the gate proves the address
// was observed, which is not the same as proving it is a stream. So fetch its
// opening bytes with the page's own context and look.
//
// Held to advisory except when it *contradicts* the proposal. The tier is
// optional by design — no network, a CDN that refuses the context, a 403 — and
// none of those are evidence the pick is wrong, so none of them may block an
// accept. A body that is plainly a web page is different, and does.
void extractor_dialog::confirm_by_fetching() {
	if (!m_probe)
		m_probe = new stream_probe(this);

	stream_context ctx;
	ctx.referer = m_page.toString();
	for (auto it = m_verdict.result.headers.cbegin();
	      it != m_verdict.result.headers.cend(); ++it) {
		const QString name = it.key().toLower();
		if (name == "referer")         ctx.referer = it.value();
		else if (name == "user-agent") ctx.user_agent = it.value();
		else if (name == "cookie")     ctx.cookies = it.value();
		else                            ctx.extra.insert(it.key(), it.value());
	}

	const QUrl picked = m_verdict.result.url;
	m_status->setText("Reviewed and validated. Checking what that address "
	                   "actually serves…");

	m_probe->probe(picked, ctx, [this, picked](const probe_result &r) {
		if (m_verdict.result.url != picked)
			return;                       // a newer proposal has overtaken this
		QString note;
		if (!r.reached) {
			note = QString("Could not check what it serves: %1. The pick stands "
			                "on the request log alone.").arg(r.reason);
		} else if (r.kind.isEmpty() && r.status < 400) {
			note = QString("<b>It does not look like a stream:</b> %1.")
			           .arg(r.reason);
			m_apply->setEnabled(false);
		} else if (r.kind.isEmpty()) {
			note = QString("Could not confirm: %1. The pick stands on the "
			                "request log alone.").arg(r.reason);
		} else {
			note = QString("<b>Confirmed as %1</b> — %2.")
			           .arg(r.kind.toUpper(), r.reason);
			if (r.disagreed)
				note += " That is the disguise this exists to see through.";
		}
		m_result->setText(m_result->text() + "<br><br>" + note);
		m_status->setText(m_apply->isEnabled()
		  ? "Reviewed, validated and checked. Nothing is saved until you accept."
		  : "The address does not serve a stream, so it cannot be accepted.");
	});
}

void extractor_dialog::on_accept() {
	if (!m_verdict.usable || !m_store)
		return;
	m_store->set_for(m_site, m_proposal,
	                  QString("picks %1").arg(m_verdict.result.kind));
	accept();
}

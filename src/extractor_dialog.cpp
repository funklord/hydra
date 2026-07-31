// SPDX-License-Identifier: GPL-3.0-or-later
#include "extractor_dialog.h"
#include "ai_provider.h"
#include "extractor_signals.h"
#include "stream_probe.h"

#include <QDialogButtonBox>
#include <QHash>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

const char *k_system_prompt =
	"You write small JavaScript functions that pick the video stream out of a "
	"list of requests a web page made.\n\n"
	"Reply with JavaScript and nothing else — no prose, no markdown fences.\n"
	"Assign to `extract`:\n\n"
	"  extract = function (page, requests) {\n"
	"    // page:     { url, host }\n"
	"    // requests: [ { url, type, order }, ... ]\n"
	"    //   type is what the browser fetched it as: 'script', 'image' or\n"
	"    //   'other'. It is never 'hls' or 'dash' — that is your conclusion,\n"
	"    //   and it goes in `kind` below.\n"
	"    return { url: one of the request urls, kind: 'hls'|'dash'|'direct',\n"
	"             headers: { Referer: page.url } };   // or null\n"
	"  };\n\n"
	"Rules:\n"
	"1. Return a url **taken from the requests array**. Never build one, never "
	"guess one: a returned url that was not requested is rejected outright.\n"
	"2. Match on shape, not on an exact string — the next visit will have "
	"different ids, tokens and numbers. Prefer a stable path fragment.\n"
	"3. A manifest may be disguised: master playlists are routinely served with "
	"innocuous extensions and query strings, so do not decide by extension "
	"alone.\n"
	"4. A line ending `(+N more like this)` is a request the page repeated N "
	"further times — those are segments. A manifest is fetched once.\n"
	"5. Prefer the manifest over any individual segment.\n"
	"6. Return null if nothing in the list is a stream. Returning nothing is a "
	"clean answer; a guess is not.\n"
	"7. Anything fetched as a `script` or an `image` is page furniture, not the "
	"stream — returning one is rejected, so do not fall back to it.\n";

// Requests that differ only in a run of digits are the same request repeated —
// segment 0001, 0002, and so on.
QString shape_of(const QString &url) {
	static const QRegularExpression digits("[0-9]{2,}");
	QString s = url;
	s.replace(digits, "#");
	return s;
}

}  // namespace

QString extractor_dialog::summarise(const QList<evidence_request> &evidence,
                                     int *kept) {
	QStringList lines;
	QHash<QString, int> seen_shape;
	QHash<QString, int> repeats;
	QList<QString> order;

	for (const evidence_request &r : evidence) {
		const QString shape = shape_of(r.url.toString());
		if (seen_shape.contains(shape)) {
			repeats[shape]++;
			continue;
		}
		seen_shape.insert(shape, r.order);
		order << shape;
		lines << QString("%1 | %2 | %3")
		             .arg(r.order, 4).arg(r.kind, -6)
		             .arg(r.url.toString().left(300));
	}
	// Say what was folded away rather than silently dropping it: a reader
	// should be able to tell a page that fetched one thing from a page that
	// fetched it four hundred times.
	for (int i = 0; i < order.size(); ++i) {
		const int extra = repeats.value(order[i]);
		if (extra > 0)
			lines[i] += QString("   (+%1 more like this)").arg(extra);
	}
	if (kept)
		*kept = lines.size();
	return lines.join('\n');
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
	int kept = 0;
	const QString folded = summarise(m_evidence, &kept);

	QString payload = "Page: " + m_page.toString() + "\n";
	payload += "Host: " + m_site + "\n\n";
	payload += QString("Requests this page made (%1 shown, %2 seen):\n")
	               .arg(kept).arg(m_evidence.size());
	// Name the columns. The middle one is the browser's resource type, and
	// unlabelled it reads as anything the reader likes.
	payload += "order | type | url\n";
	payload += folded;
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
	           "`type` and `order`.\n\n"
	           "`type` is what the browser fetched it as — `script`, `image` or "
	           "`other` — and never `hls` or `dash`, so testing for those "
	           "matches nothing. Anything fetched as a `script` or an `image` "
	           "is page furniture and will be rejected, so it is not a useful "
	           "fallback either.\n\n"
	           "Return an object whose `url` is one of the addresses above "
	           "copied exactly, whose `kind` is `hls`, `dash` or `direct`, and "
	           "whose `headers` sets `Referer` to `page.url` — or return null "
	           "if none of them is a stream. Reply with that JavaScript and "
	           "nothing else: no summary of this list, no explanation, no "
	           "markdown fences.";
	m_payload->setPlainText(payload);

	connect(m_provider, &ai_provider::finished, this, &extractor_dialog::on_reply);
	connect(m_provider, &ai_provider::failed,   this, &extractor_dialog::on_failed);

	if (m_evidence.isEmpty()) {
		m_status->setText("No requests recorded for this page yet. Many sites "
		                  "fetch nothing until their player starts, so press "
		                  "play first, then try again.");
		m_send->setEnabled(false);
	}
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

	m_payload = new QPlainTextEdit(this);
	m_payload->setReadOnly(true);
	m_payload->setLineWrapMode(QPlainTextEdit::NoWrap);
	m_pages->addWidget(m_payload);

	auto *result_page = new QWidget(this);
	auto *rl = new QVBoxLayout(result_page);
	rl->setContentsMargins(0, 0, 0, 0);
	m_result = new QLabel(result_page);
	m_result->setWordWrap(true);
	m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
	rl->addWidget(m_result);
	m_script = new QPlainTextEdit(result_page);
	m_script->setReadOnly(true);
	rl->addWidget(m_script, 1);
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
	// this is safe to run at all (§11.5).
	m_verdict = site_extractor::check(m_proposal, m_page, m_evidence);
	m_apply->setEnabled(m_verdict.usable);

	if (!m_verdict.usable) {
		m_result->setText("<b>Not usable.</b> " + m_verdict.message.toHtmlEscaped() +
		                   (m_verdict.invented
		                        ? "<br><br>A script that returns an address the "
		                          "page never asked for is refused outright — "
		                          "there is no safe way to accept one."
		                        : QString()));
		m_status->setText("The proposal was rejected. You can send again.");
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

#include "extractor_helpers.h"

#include <QHostAddress>
#include "stream_probe.h"

#include <QRegularExpression>

QString helper_allowlist::normalise(const QUrl &url) {
	return url.adjusted(QUrl::RemoveFragment | QUrl::StripTrailingSlash).toString();
}

void helper_allowlist::observe(const QList<evidence_request> &evidence) {
	for (const evidence_request &r : evidence)
		m_allowed.insert(normalise(r.url));
}

bool helper_allowlist::is_local(const QUrl &url) {
	const QString host = url.host().toLower();
	if (host.isEmpty())
		return false;
	// The name, which never reaches a resolver.
	if (host == QLatin1String("localhost") ||
	     host == QLatin1String("localhost.localdomain") ||
	     host.endsWith(QLatin1String(".localhost")))
		return true;
	// And the literal address, where Qt already knows these ranges. A *name*
	// that resolves to a private address is deliberately not caught: asking a
	// resolver here would be a lookup whose answer can change between the
	// check and the fetch, and a gate that depends on that is worse than one
	// whose limits are written down. See the note in `allows`.
	const QHostAddress addr(host);
	if (addr.isNull())
		return false;
	return addr.isLoopback() || addr.isLinkLocal() || addr.isPrivateUse();
}

bool helper_allowlist::allows(const QUrl &url) const {
	if (!url.isValid() || url.isEmpty())
		return false;
	// Only what a page can actually fetch. A `file:` or a custom scheme in a
	// document is not something this tier will follow.
	const QString scheme = url.scheme().toLower();
	if (scheme != "http" && scheme != "https")
		return false;

	// **A public page must not reach the machine through this tier.** The set
	// grows from strings scraped out of fetched documents, so a page can put
	// `http://127.0.0.1:8080/...` in its own manifest and a generated script
	// can then ask for it -- and the answer would be handed back to the script
	// as text. The page could issue that request itself and could *not* read
	// the response; this tier reads it. That is the escalation, and it turns a
	// media helper into a way to probe whatever listens on the user's machine,
	// this browser's own loopback proxy included.
	//
	// Allowed only when the page is itself local, which is how a dev server or
	// a media box on the LAN keeps working. **What is not covered, written
	// down rather than implied:** a *name* that resolves to a private address
	// passes this, because resolving it here would be a lookup whose answer can
	// change between the check and the fetch.
	if (is_local(url) && url.host().toLower() != m_page_host)
		return false;

	return m_allowed.contains(normalise(url));
}

int helper_allowlist::learn_from(const QUrl &base, const QByteArray &body) {
	// Deliberately crude, and crude in the safe direction. This looks for
	// things that *appear* to be references and resolves them; it does not
	// parse HLS, XML or HTML, because a parser that understood one format would
	// quietly refuse to follow the others. Over-collecting here costs nothing:
	// an address only becomes reachable if a script also asks for it, and a
	// string that was never a real reference is one no script has reason to
	// name.
	static const QRegularExpression absolute(
	  R"((https?://[^\s"'<>\\)\]]+))");
	// A manifest line is bare: `index-f1-v1-a1.txt?k=...` on its own line, or a
	// quoted URI attribute.
	static const QRegularExpression relative(
	  R"((?:^|["'\s,=(])([A-Za-z0-9._~!$&*+;@%-][A-Za-z0-9._~!$&*+;@%/-]*\.[A-Za-z0-9]{1,8}(?:\?[^\s"'<>\\)\]]*)?))",
	  QRegularExpression::MultilineOption);

	const QString text = QString::fromUtf8(body);
	int learned = 0;

	auto add = [&](const QUrl &candidate) {
		if (!candidate.isValid() || candidate.isEmpty())
			return;
		const QString scheme = candidate.scheme().toLower();
		if (scheme != "http" && scheme != "https")
			return;
		const QString key = normalise(candidate);
		if (m_allowed.contains(key))
			return;
		m_allowed.insert(key);
		++learned;
	};

	auto it = absolute.globalMatch(text);
	while (it.hasNext())
		add(QUrl(it.next().captured(1)));

	auto rel = relative.globalMatch(text);
	while (rel.hasNext()) {
		const QString ref = rel.next().captured(1).trimmed();
		if (ref.isEmpty() || ref.startsWith("http"))
			continue;
		add(base.resolved(QUrl(ref)));
	}
	return learned;
}

helper_host::helper_host(helper_allowlist *allow, helper_fetcher fetch,
                          helper_budget budget, QObject *parent)
  : QObject(parent), m_allow(allow), m_fetch(std::move(fetch)),
    m_budget(budget) {}

void helper_host::begin() { m_clock.start(); }

bool helper_host::permit(const QString &verb, const QUrl &url,
                          helper_call *out) {
	out->verb   = verb;
	out->target = url.toString().left(300);

	// Budgets first: a call refused for cost never touches the allowlist, and
	// never reaches the network.
	// Two phrasings on purpose: the transcript line is terse and reads in a
	// list, while the breach is a clause the gate completes into a sentence.
	if (m_calls.size() >= m_budget.max_calls) {
		out->outcome = QString("refused: the budget of %1 calls is spent")
		                   .arg(m_budget.max_calls);
		m_breach = QString("spent its budget of %1 helper calls")
		               .arg(m_budget.max_calls);
		return false;
	}
	if (m_clock.isValid() && m_clock.elapsed() > m_budget.deadline_ms) {
		out->outcome = QString("refused: past the %1 ms deadline")
		                   .arg(m_budget.deadline_ms);
		m_breach = QString("ran past its %1 ms deadline").arg(m_budget.deadline_ms);
		return false;
	}
	if (m_bytes >= m_budget.max_bytes) {
		out->outcome = QString("refused: the budget of %1 bytes is spent")
		                   .arg(m_budget.max_bytes);
		m_breach = QString("spent its budget of %1 bytes").arg(m_budget.max_bytes);
		return false;
	}
	if (!m_allow || !m_allow->allows(url)) {
		// The invariant, and the reason this tier is safe at all.
		out->outcome = "refused: that address was never observed, and no fetched "
		                "document referred to it";
		m_breach = QString("reached for %1, which was never observed and which no "
		                    "fetched document referred to")
		               .arg(url.toString().left(120));
		return false;
	}
	out->allowed = true;
	return true;
}

void helper_host::finish(helper_call call) { m_calls << call; }

void helper_host::log(const QString &message) {
	// A note costs nothing to serve, so it must not compete with fetches for
	// the call budget -- a script that explained itself well spent its last slot
	// on the explanation and was refused for it. Bounded separately, because an
	// unbounded log is still its own denial of service.
	helper_call c;
	c.verb   = "log";
	c.target = message.left(300);
	if (m_notes >= k_max_notes) {
		c.outcome = QString("refused: more than %1 notes").arg(k_max_notes);
		m_breach = QString("wrote more than %1 notes").arg(k_max_notes);
	} else {
		++m_notes;
		c.allowed = true;
		c.outcome = "noted";
	}
	finish(c);
}

QVariantMap helper_host::head(const QString &url) {
	const QUrl target(url);
	helper_call c;
	if (!permit("head", target, &c)) { finish(c); return {}; }

	const fetch_result r = m_fetch(target, stream_probe::k_sniff_bytes,
	                                m_budget.deadline_ms);
	m_bytes += r.body.size();
	c.status = r.status;
	c.bytes  = r.body.size();

	QVariantMap out;
	if (!r.reached) {
		c.outcome = QString("unreachable: %1").arg(r.error.left(120));
		finish(c);
		return out;
	}
	const probe_result p = stream_probe::classify(r.content_type, r.body);
	out.insert("status", r.status);
	out.insert("type", r.content_type);
	out.insert("kind", r.status >= 400 ? QString() : p.kind);
	c.outcome = r.status >= 400
	  ? QString("%1, so what it is could not be established").arg(r.status)
	  : QString("%1%2").arg(r.content_type,
	                         p.kind.isEmpty() ? QString()
	                                          : QString("  (%1)").arg(p.kind.toUpper()));
	finish(c);
	return out;
}

QString helper_host::text(const QString &url) {
	const QUrl target(url);
	helper_call c;
	if (!permit("text", target, &c)) { finish(c); return {}; }

	const qint64 room = m_budget.max_bytes - m_bytes;
	const fetch_result r = m_fetch(target, room, m_budget.deadline_ms);
	m_bytes += r.body.size();
	c.status = r.status;
	c.bytes  = r.body.size();

	if (!r.reached) {
		c.outcome = QString("unreachable: %1").arg(r.error.left(120));
		finish(c);
		return {};
	}
	if (r.status >= 400) {
		c.outcome = QString("%1, no body").arg(r.status);
		finish(c);
		return {};
	}

	// The body is what extends the allowlist -- this is the "follow" half of
	// follow-not-fabricate, and the only way the set ever grows.
	const int learned = m_allow ? m_allow->learn_from(target, r.body) : 0;
	c.outcome = QString("%1 bytes%2")
	                .arg(r.body.size())
	                .arg(learned ? QString(", %1 new addresses it may now follow")
	                                   .arg(learned)
	                              : QString());
	finish(c);
	return QString::fromUtf8(r.body);
}

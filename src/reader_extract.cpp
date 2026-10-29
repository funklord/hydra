#include "reader_extract.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace reader_extract {

namespace {

// Read-only over the live document: hidden elements are known only there, and
// a clone would have no computed style. Nothing is written into the page.
const char *k_source = R"JS(
(function () {
  var NEG = /(^|[\s_-])(comments?|meta|footer|footnotes?|sidebar|side-bar|nav|navbar|menu|share|sharing|social|related|recommend\w*|promo\w*|sponsor\w*|advert\w*|ad|ads|banner|popup|modal|cookie\w*|consent|newsletter|subscribe|signup|breadcrumbs?|pagination|pager|widget|outbrain|taboola|disqus|masthead|toolbar|tags|byline-share|skip|references?|reflist|refbegin|citations?|navbox|catlinks|mw-editsection|printfooter|noprint)($|[\s_-])/i;
  var POS = /(^|[\s_-])(article|body|content|entry|main|post|text|story|blog|prose|hentry|page-content)($|[\s_-])/i;
  var DROP = { SCRIPT: 1, STYLE: 1, NOSCRIPT: 1, TEMPLATE: 1, IFRAME: 1, OBJECT: 1,
    EMBED: 1, FORM: 1, BUTTON: 1, INPUT: 1, SELECT: 1, TEXTAREA: 1, NAV: 1,
    ASIDE: 1, FOOTER: 1, SVG: 1, CANVAS: 1, DIALOG: 1, LINK: 1, META: 1 };
  var ROLES = /^(navigation|banner|contentinfo|complementary|dialog|alert|menu|menubar|search)$/;
  var KEEP = { P: 'p', DIV: 'div', SECTION: 'div', ARTICLE: 'div', MAIN: 'div',
    H1: 'h2', H2: 'h2', H3: 'h3', H4: 'h4', H5: 'h5', H6: 'h6', UL: 'ul', OL: 'ol',
    LI: 'li', DL: 'dl', DT: 'dt', DD: 'dd', BLOCKQUOTE: 'blockquote', PRE: 'pre',
    CODE: 'code', EM: 'em', I: 'em', STRONG: 'strong', B: 'strong', U: 'u', S: 's',
    SUB: 'sub', SUP: 'sup', SMALL: 'small', MARK: 'mark', BR: 'br', HR: 'hr',
    A: 'a', IMG: 'img', FIGURE: 'figure', FIGCAPTION: 'figcaption', TABLE: 'table',
    THEAD: 'thead', TBODY: 'tbody', TFOOT: 'tfoot', TR: 'tr', TD: 'td', TH: 'th',
    CAPTION: 'caption', Q: 'q', CITE: 'cite', ABBR: 'abbr', TIME: 'time' };
  var LIMIT = 1500000;

  function meta(sel) {
    var m = document.querySelector(sel);
    return m ? (m.getAttribute('content') || '').trim() : '';
  }
  function text(n) { return (n.textContent || '').replace(/\s+/g, ' ').trim(); }
  function label(n) { return (n.className && n.className.baseVal === undefined ? n.className : '') + ' ' + (n.id || ''); }
  function hidden(n) {
    if (n.hidden || n.getAttribute('aria-hidden') === 'true') return true;
    var s = window.getComputedStyle(n);
    return s.display === 'none' || s.visibility === 'hidden';
  }
  function chrome(n) {
    if (DROP[n.tagName]) return true;
    var r = n.getAttribute('role');
    if (r && ROLES.test(r)) return true;
    var l = label(n);
    return NEG.test(l) && !POS.test(l);
  }
  function link_density(n) {
    var all = text(n).length;
    if (!all) return 0;
    var a = n.getElementsByTagName('a'), l = 0;
    for (var i = 0; i < a.length; i++) l += text(a[i]).length;
    return l / all;
  }

  var body = document.body;
  if (!body) return JSON.stringify({ ok: false, why: 'The page has no body.' });

  // Score: every paragraph-like block gives its ancestors credit.
  // Indexed by a Map rather than a mark on the node: on Android this runs in
  // the page's own world, where anything set on an element is the page's to
  // see. A list search instead made scoring quadratic on a long page.
  var scores = [], nodes = [];
  var index = typeof Map === 'function' ? new Map() : null;
  function find(n) {
    if (index) { var v = index.get(n); return v === undefined ? -1 : v; }
    return nodes.indexOf(n);
  }
  function score_of(n) {
    var i = find(n);
    if (i >= 0) return i;
    var s = 0, t = n.tagName;
    if (t === 'DIV' || t === 'ARTICLE' || t === 'MAIN' || t === 'SECTION') s += 5;
    else if (t === 'PRE' || t === 'TD' || t === 'BLOCKQUOTE') s += 3;
    else if (t === 'UL' || t === 'OL' || t === 'FORM' || t === 'DL') s -= 3;
    else if (/^H[1-6]$/.test(t) || t === 'TH') s -= 5;
    if (t === 'ARTICLE' || n.getAttribute('itemprop') === 'articleBody') s += 20;
    var l = label(n);
    if (POS.test(l)) s += 25;
    if (NEG.test(l)) s -= 25;
    nodes.push(n); scores.push(s);
    if (index) index.set(n, nodes.length - 1);
    return nodes.length - 1;
  }
  // A div holding only inline content is a paragraph too: plenty of pages
  // write their text into divs separated by <br>, and would otherwise score
  // nothing at all.
  var BLOCK = /^(P|DIV|SECTION|ARTICLE|TABLE|UL|OL|DL|PRE|BLOCKQUOTE|FIGURE|H[1-6]|HEADER|FOOTER|NAV|ASIDE|FORM|MAIN)$/;
  // Not <li>: a list item is part of an article but rarely its prose, and a
  // page's references -- long items, many commas -- outscored Wikipedia's
  // own paragraphs when they counted. Lists inside the article are kept
  // anyway, since the chosen container is emitted whole.
  var paras = Array.prototype.slice.call(body.querySelectorAll('p, pre, td, blockquote'));
  var divs = body.getElementsByTagName('div');
  for (var d = 0; d < divs.length; d++) {
    var inline = true;
    for (var e = divs[d].firstElementChild; e; e = e.nextElementSibling)
      if (BLOCK.test(e.tagName)) { inline = false; break; }
    if (inline) paras.push(divs[d]);
  }
  for (var i = 0; i < paras.length; i++) {
    var p = paras[i];
    var t = text(p);
    if (t.length < 25) continue;
    var skip = false;
    for (var a = p; a && a !== body; a = a.parentElement)
      if (chrome(a)) { skip = true; break; }
    if (skip) continue;
    var gain = 1 + t.split(/[,\uFF0C\u3001]/).length - 1 + Math.min(Math.floor(t.length / 100), 3);
    var up = p.parentElement, level = 0;
    while (up && up !== document.documentElement && level < 3) {
      var k = score_of(up);
      scores[k] += level === 0 ? gain : level === 1 ? gain / 2 : gain / 3;
      up = up.parentElement; level++;
    }
  }
  var best = null, best_score = 0;
  for (var j = 0; j < nodes.length; j++) {
    var f = scores[j] * (1 - link_density(nodes[j]));
    scores[j] = f;
    if (f > best_score) { best_score = f; best = nodes[j]; }
  }
  if (!best || text(best).length < 250)
    return JSON.stringify({ ok: false, why: 'No article was found on this page.' });

  // The best container, and siblings that score or read like its paragraphs.
  var parts = [];
  var parent = best.parentElement || body;
  var floor = Math.max(10, best_score * 0.2);
  for (var c = parent.firstElementChild; c; c = c.nextElementSibling) {
    if (c === best) { parts.push(c); continue; }
    if (chrome(c)) continue;
    var ix = find(c);
    if (ix >= 0 && scores[ix] >= floor) { parts.push(c); continue; }
    if (c.tagName === 'P') {
      var ct = text(c), ld = link_density(c);
      if ((ct.length > 80 && ld < 0.25) || (ct.length > 0 && ld === 0 && /\.( |$)/.test(ct)))
        parts.push(c);
    }
  }

  // Rebuilt, never copied: tags from the allow-list, a few attributes, text
  // escaped. Hidden things, chrome and link lists are left out.
  var title = meta('meta[property="og:title"]') || meta('meta[name="twitter:title"]');
  if (!title) { var h = body.querySelector('h1'); title = h ? text(h) : ''; }
  if (!title) title = (document.title || '').trim();
  // "Bread - Wikipedia": the last segment is the site's name, which the
  // reader page shows on its own line. Dropped only when it names the site
  // or the host, so a title that merely contains a dash keeps it.
  var site = meta('meta[property="og:site_name"]') || location.hostname;
  var squash = function (s) { return s.toLowerCase().replace(/[^a-z0-9\u00c0-\uffff]/g, ''); };
  var cut = title.match(/^(.*\S)\s+[|\-\u2013\u2014]\s+([^|\-\u2013\u2014]+)$/);
  if (cut && cut[2].length < 60) {
    var tail = squash(cut[2]);
    if (tail && (squash(site).indexOf(tail) >= 0 || squash(location.hostname).indexOf(tail) >= 0))
      title = cut[1];
  }
  var out = [], size = 0, words = 0, dropped_title = false;
  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }
  function safe_url(u) { return /^(https?:|mailto:)/i.test(u) ? u : ''; }
  function emit(s) { if (size < LIMIT) { out.push(s); size += s.length; } }
  function walk(n) {
    if (size >= LIMIT) return;
    if (n.nodeType === 3) {
      var v = n.nodeValue;
      if (v) { emit(esc(v)); words += (v.match(/\S+/g) || []).length; }
      return;
    }
    if (n.nodeType !== 1) return;
    if (chrome(n) || hidden(n)) return;
    var t = n.tagName, tag = KEEP[t];
    if ((t === 'DIV' || t === 'SECTION' || t === 'UL' || t === 'OL') &&
        link_density(n) > 0.5 && text(n).length < 400)
      return;
    if (/^H[1-6]$/.test(t) && !dropped_title && text(n) === title) {
      dropped_title = true;
      return;
    }
    if (t === 'IMG') {
      var src = safe_url(n.currentSrc || n.src || n.getAttribute('data-src') || '');
      var w = n.naturalWidth || parseInt(n.getAttribute('width') || '0', 10);
      if (src && (w === 0 || w >= 60))
        emit('<img src="' + esc(src) + '" alt="' + esc(n.getAttribute('alt') || '') + '">');
      return;
    }
    if (t === 'BR' || t === 'HR') { emit('<' + tag + '>'); return; }
    var open = '';
    if (tag === 'a') {
      var href = safe_url(n.href || '');
      open = href ? '<a href="' + esc(href) + '">' : '';
    } else if (tag === 'td' || tag === 'th') {
      open = '<' + tag;
      var cs = n.getAttribute('colspan'), rs = n.getAttribute('rowspan');
      if (cs && /^\d+$/.test(cs)) open += ' colspan="' + cs + '"';
      if (rs && /^\d+$/.test(rs)) open += ' rowspan="' + rs + '"';
      open += '>';
    } else if (tag) {
      open = '<' + tag + '>';
    }
    emit(open);
    for (var ch = n.firstChild; ch; ch = ch.nextSibling) walk(ch);
    if (open) emit('</' + (tag === 'a' ? 'a' : tag) + '>');
  }
  for (var q = 0; q < parts.length; q++) walk(parts[q]);

  var byline = meta('meta[name="author"]') || meta('meta[property="article:author"]');
  if (!byline || /^https?:/i.test(byline)) {
    var b = body.querySelector('[rel="author"], [itemprop="author"], .byline, .author');
    byline = b ? text(b) : '';
  }
  if (byline.length > 120) byline = '';
  var root = document.documentElement;
  return JSON.stringify({
    ok: true, title: title, byline: byline,
    site: site,
    lang: root.getAttribute('lang') || '', dir: (root.getAttribute('dir') || body.getAttribute('dir') || '').toLowerCase(),
    html: out.join(''), words: words
  });
})()
)JS";

}  // namespace

QString source() {
	return QString::fromUtf8(k_source);
}

bool parse(const QString &json, article *out, QString *why) {
	const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
	if (!doc.isObject()) {
		if (why)
			*why = json.isEmpty() ? QStringLiteral("This page could not be read.")
			                      : QStringLiteral("The page's answer could not be read.");
		return false;
	}
	const QJsonObject o = doc.object();
	if (!o.value("ok").toBool()) {
		if (why)
			*why = o.value("why").toString(QStringLiteral("No article was found on this page."));
		return false;
	}
	article a;
	a.title  = o.value("title").toString().left(400);
	a.byline = o.value("byline").toString().left(120);
	a.site   = o.value("site").toString().left(200);
	a.lang   = o.value("lang").toString().left(35);
	a.dir    = o.value("dir").toString() == QLatin1String("rtl") ? QStringLiteral("rtl")
	                                                             : QStringLiteral("ltr");
	a.html   = o.value("html").toString();
	a.words  = o.value("words").toInt();
	if (a.html.trimmed().isEmpty()) {
		if (why)
			*why = QStringLiteral("No article was found on this page.");
		return false;
	}
	if (out)
		*out = a;
	return true;
}

}  // namespace reader_extract

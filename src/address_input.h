#pragma once

#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QStringList>

#include <functional>

class QString;
class QUrl;

// The address bar itself, which exists as a class for one reason: to tell the
// on-screen keyboard what its action key should do.
//
// **Measured on a Galaxy Note 9: that key reads *Next*.** Tapping it moves
// focus out of the field rather than navigating, so the address sits there,
// nothing loads and the keyboard closes -- and a return afterwards does
// nothing either, the field no longer having focus. Input-method *hints* do
// not reach this; they describe the text, not the key.
//
// What does reach it is `Qt::ImEnterKeyType`. Qt's Android plugin builds the
// Android `imeOptions` from it -- `imeOptionsFromEnterKeyType` in its jar --
// and `QWidget` answers it through `inputMethodQuery`, which is why this is a
// subclass rather than a call. `EnterKeyGo` is the one Android renders as
// **Go** and delivers as an action the field can act on.
class address_line : public QLineEdit {
	Q_OBJECT
public:
	using QLineEdit::QLineEdit;

	QVariant inputMethodQuery(Qt::InputMethodQuery query) const override {
		if (query == Qt::ImEnterKeyType)
			return int(Qt::EnterKeyGo);
		return QLineEdit::inputMethodQuery(query);
	}

signals:
	// Escape, with something typed: the person has changed their mind. The
	// window puts the page's own address back.
	void abandoned();
	// The field took focus -- the moment the window offers what is on the
	// clipboard (`main_window::offer_clipboard`).
	void focused();
	// What is in the field as somebody sees it, the word the keyboard is still
	// composing included. `text()` leaves that word out until it is
	// committed, so on a phone the suggestions followed one word behind.
	void typing(const QString &shown);

protected:
	// **The word being composed counts as typed.** Gboard holds the current
	// word as a composition until a space or a punctuation mark, and Qt keeps
	// it out of `text()` and sends no `textEdited` until then -- so `hacker`
	// typed on a phone suggested nothing, and the field read as empty and
	// offered the clipboard instead (emulator, 2026-10-30).
	void inputMethodEvent(QInputMethodEvent *event) override {
		QLineEdit::inputMethodEvent(event);
		QString shown = text();
		const QString pre = event->preeditString();
		if (!pre.isEmpty())
			shown.insert(qBound(0, cursorPosition(), int(shown.size())), pre);
		emit typing(shown);
	}

	// **The escape hatch from a half-typed address, and it is needed now in a
	// way it was not before.** `update_address` leaves a modified field alone
	// so that a redirect cannot eat what somebody is typing -- which means
	// nothing puts the true address back either, and an abandoned edit would
	// sit in the bar describing a page the window is no longer on. Every
	// browser answers that with Escape.
	//
	// **Only when something has been typed.** Escape has other jobs in this
	// window -- leaving kiosk is the one that matters -- and swallowing the
	// key whenever the bar happens to hold focus would take one of them away.
	// `isModified()` is the same question `update_address` asks, so the key is
	// captured in exactly the state the guard creates and passed on otherwise.
	void keyPressEvent(QKeyEvent *event) override {
		if (event->key() == Qt::Key_Escape && isModified()) {
			emit abandoned();
			event->accept();
			return;
		}
		QLineEdit::keyPressEvent(event);
	}

	// **Tapping the bar selects what is in it, which is what every browser
	// does and this did not.** Measured: a tap put focus in the field and the
	// cursor at character 30 of a URL with nothing selected, while `Ctrl+L`
	// on the same field selected the lot. Reaching for the bar to go
	// somewhere new meant clearing a long address by hand -- on a phone, on a
	// touch keyboard, which is where it hurts.
	//
	// **Armed on focus, spent on the release.** A press cannot ask whether it
	// is the one that focused the field: Qt grants focus BEFORE delivering
	// the press, so `hasFocus()` in `mousePressEvent` is already true --
	// measured, with a fixture that had genuinely cleared focus first. And
	// selecting in `focusInEvent` is undone a moment later, because
	// `QLineEdit::mousePressEvent` puts the cursor where the click landed and
	// drops the selection. The reason is the only thing that says a click
	// brought focus here, and the release is the only moment left to act on
	// it.
	//
	// **`MouseFocusReason` alone.** Tab focus already selects the field, so
	// re-selecting on a later click would take away the cursor placement a
	// click is for; `ShortcutFocusReason` is Ctrl+L, which selects
	// explicitly; and accepting `ActiveWindowFocusReason` would select the
	// whole address on the first click after alt-tabbing back to the window,
	// for a keystroke nobody made.
	void focusInEvent(QFocusEvent *event) override {
		QLineEdit::focusInEvent(event);
		if (event->reason() == Qt::MouseFocusReason)
			m_select_on_release = true;
		emit focused();
	}

	void mouseReleaseEvent(QMouseEvent *event) override {
		QLineEdit::mouseReleaseEvent(event);
		if (!m_select_on_release)
			return;
		m_select_on_release = false;
		// **A drag is somebody choosing their own selection**, and replacing
		// it with everything would be worse than not selecting at all. Only a
		// plain click, which leaves nothing selected, takes the whole field.
		if (!hasSelectedText())
			selectAll();
	}

private:
	// Armed by a focus-in the mouse caused, spent on the release that
	// follows -- so the first tap selects and a second, the field already
	// focused, places the cursor as a second click should.
	bool m_select_on_release = false;
};

// **Where an address is edited on a phone**: the whole width of the window,
// as many lines as the address needs up to five, and then it scrolls. Asked
// for by the holder on 2026-10-30, after a one-line box a third of a phone's
// width showed a long link a few dozen characters at a time and moving
// through it meant dragging a cursor nobody could see the ends of.
//
// The toolbar's `address_line` is still the address bar; this takes over
// from it while somebody is typing (`main_window::open_address_editor`), and
// hands what was typed back to it to be navigated. Plain text, wrapped at any
// character -- an address has no spaces to wrap at -- and never more than one
// paragraph: Return submits rather than breaking a line, and a pasted line
// break becomes nothing.
//
// **What is pasted is cleaned as it lands** (`cleaner`): the lists' tracking
// parameters taken off and a tracking redirect skipped, so a link copied out
// of Facebook arrives without its `fbclid`. That covers each way a paste
// arrives -- the window's own Paste rows, Ctrl+V, and the keyboard's clipboard
// chip, which comes as committed text rather than a paste.
class address_editor : public QPlainTextEdit {
	Q_OBJECT
public:
	explicit address_editor(QWidget *parent = nullptr) : QPlainTextEdit(parent) {
		setWordWrapMode(QTextOption::WrapAnywhere);
		setTabChangesFocus(true);
		setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
		setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		connect(this, &QPlainTextEdit::textChanged, this, [this] { emit typing(shown()); });
	}

	// The cleaning applied to anything pasted; identity when unset.
	std::function<QString(const QString &)> cleaner;

	QString text() const { return toPlainText().simplified(); }
	// Filled with an address the person did not write: not an edit.
	void set_text(const QString &t) {
		setPlainText(t);
		document()->setModified(false);
	}
	// Whether the person has changed the text since it was last filled.
	bool edited() const { return document()->isModified(); }

	// Puts `t` at the cursor, over the selection, cleaned as a paste is.
	// **A pasted link replaces the page's address; it is never appended to
	// it.** Inserted at the cursor, a tap that placed the cursor in the
	// address and a paste made "chrome://blankhttps://..." (holder,
	// 2026-10-31: "nothing should be appended to a written url"). Into text
	// the person has edited, it goes where they put the cursor, since that
	// text is theirs.
	void paste_text(const QString &t) {
		if (edited()) {
			insertPlainText(clean(t));
		} else {
			setPlainText(clean(t));
			moveCursor(QTextCursor::End);
		}
		document()->setModified(true);
	}

	QVariant inputMethodQuery(Qt::InputMethodQuery query) const override {
		if (query == Qt::ImEnterKeyType)
			return int(Qt::EnterKeyGo);
		return QPlainTextEdit::inputMethodQuery(query);
	}

signals:
	void submitted();          // Return, or the keyboard's Go
	void abandoned();          // Escape
	void typing(const QString &shown);
	void left();               // focus went elsewhere

protected:
	void keyPressEvent(QKeyEvent *event) override {
		if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
			emit submitted();
			event->accept();
			return;
		}
		if (event->key() == Qt::Key_Escape) {
			emit abandoned();
			event->accept();
			return;
		}
		QPlainTextEdit::keyPressEvent(event);
	}

	void insertFromMimeData(const QMimeData *source) override {
		if (source && source->hasText())
			paste_text(source->text());
	}

	// The keyboard's clipboard chip commits the clipboard as typed text, so a
	// link arriving whole in one commit is treated as the paste it is.
	void inputMethodEvent(QInputMethodEvent *event) override {
		const QString commit = event->commitString();
		if (commit.contains(QLatin1String("://")) || commit.contains(QLatin1Char('\n')))
			event->setCommitString(clean(commit), event->replacementStart(),
			                       event->replacementLength());
		QPlainTextEdit::inputMethodEvent(event);
		m_preedit = event->preeditString();
		emit typing(shown());
	}

	void focusOutEvent(QFocusEvent *event) override {
		QPlainTextEdit::focusOutEvent(event);
		// A popup the editor opened itself -- the keyboard's own -- is not
		// somebody leaving.
		if (event->reason() != Qt::PopupFocusReason)
			emit left();
	}

private:
	QString clean(const QString &t) const {
		QString one = t;
		one.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '));
		one = one.trimmed();
		return cleaner ? cleaner(one) : one;
	}
	// The field as seen, the word still being composed included.
	QString shown() const {
		QString s = toPlainText();
		if (!m_preedit.isEmpty())
			s.insert(qBound(0, textCursor().position(), int(s.size())), m_preedit);
		return s.simplified();
	}
	QString m_preedit;
};

// What the address bar does with what somebody typed.
//
// Until this existed the answer was "assume it is an address", so anything
// that was not one went to `QUrl::fromUserInput` and came back either invalid
// -- silently doing nothing -- or as a guess like `http://weather tomorrow`.
// A browser whose address bar only accepts addresses is missing the half of it
// people use most.

// Whether typed text is meant as an address rather than as terms to search
// for.
//
// **The interesting direction is the one that leaks.** Guessing "search" for
// something that was an address is a wasted keystroke; guessing "search" for
// an *intranet* address sends a private hostname to a third party, and that
// cannot be taken back. So every rule below is written to keep a plausible
// host out of the search box, and the residual ambiguity is spent the other
// way.
//
// An address is any of:
//
//   * text carrying a scheme -- `https:`, `file:`, `magnet:`, `about:`.
//     Whitespace does not override this: somebody who typed a scheme meant an
//     address, and the engine can encode the rest.
//   * a path, by its first character: `/`, `./`, `../` or `~/`.
//   * an IPv4 or IPv6 literal, with or without a port.
//   * `localhost`, with or without a port.
//   * any single token carrying a dot whose last label looks like a
//     top-level domain -- two or more characters, all letters.
//
// Everything else is search. That last rule is what keeps `3.14` and
// `1.5x faster` out of the address bar: a dot alone is not a hostname, and a
// final label of digits is not a TLD. It is also what keeps
// `internal.corp.example/secret` out of a search engine, which is the case
// worth being careful about.
//
// A bare word with no dot -- `wiki`, `router` -- is searched, which is what
// every browser does and is the one leak this accepts: the word itself goes
// out, and it is a word rather than a path. `localhost` is named explicitly
// because it is the one such host that is always real.
bool looks_like_address(const QString &text);

// **The host to ask the local resolver about before searching**, or empty.
//
// A single-label host with a path -- `nas/share`, `printer/status?job=1` --
// cannot be told from `AC/DC` or `and/or` by its shape, and searching it
// sends a private hostname and path to a third party. But the machine can
// tell them apart: a host on this network resolves and a word does not
// (measured here: `debian-nabbe` resolved, and `router`, `ac`, `tcp`, `and`
// and `km` did not). So for exactly that shape this names the label, and
// the caller asks the resolver -- which sees every host this browser loads
// anyway -- and goes to the address when it answers, to the search when it
// does not.
//
// Empty for everything `looks_like_address` already decides, for a bare word
// with no path (the one leak the rule above accepts), and for a label with
// no letter in it: `24/7` would be read by the resolver as the IPv4 address
// 0.0.0.24.
QString host_to_ask(const QString &text);

// What the address bar shows for a page's address: the address, except for
// a blank page, which has none to show. `about:blank` is what a new tab
// holds, and Android's WebView reports it on some devices as
// `chrome://blank`; either one left in the field became the start of
// whatever was typed or pasted next -- "chrome://blankhttps://..." -- and
// was then searched for or loaded as nonsense (reported from the phone,
// 2026-10-31). Every browser shows an empty bar there.
QString shown_address(const QString &url);

// **The url an address typed without a scheme goes to: https first.**
// `QUrl::fromUserInput` makes `example.org` into `http://example.org`, so a
// typed address went out in the clear and was redirected only if the site
// chose to -- measured on the phone, 2026-10-31. Every current browser tries
// https first. `upgraded` says when this chose https, so the caller can fall
// back to http once if the site has none. Left as fromUserInput has it: an
// explicit scheme, an explicit port (a service on a port is rarely https),
// an IP literal, `localhost`, and a single label (an intranet name).
QUrl typed_url(const QString &text, bool *upgraded = nullptr);

// The search url for `terms` under `tmpl`, where `tmpl` carries a single `%1`
// standing for the encoded terms. Returns an invalid QUrl if the template has
// no `%1`, so a mistyped setting fails visibly rather than searching for
// nothing.
QUrl search_url(const QString &terms, const QString &tmpl);

// What `hydra <argument>` was asked to open, as a page -- or an invalid QUrl
// when the argument is a tree path.
//
// **The distinction is the scheme as written, and it has to be**, because
// `QUrl::fromUserInput` *invents* one: hand it `./tree.txt` and it hands back
// `file:///.../tree.txt`, so a classifier built on the parsed url cannot tell
// a path from a `file:` uri and would silently change what
// `hydra ./tree.txt` has always meant. Parsed with `QUrl(raw)` instead, which
// invents nothing: a path has no scheme and a `file:` uri always did.
//
// `http` and `https` are pages, and so is `file:` -- the desktop entry claims
// `text/html` and passes `%U`, so a file manager opening a local page hands
// over `file:///home/me/doc.html`. Before this that was read as a tree path,
// refused for naming no existing directory, and the browser came up on the
// personal tree with the page nowhere: measured, and it is what "hydra cannot
// open my html file" looks like from outside.
//
// An `http`/`https` argument that also names an existing file stays a tree
// path, which is the older rule and is left alone.
QUrl argument_url(const QString &raw);

// What a whole command line means, which is `argument_url` applied to each
// argument plus the two rules that decide the rest.
//
// **`Exec=hydra %U` promises a list** -- a file manager with three pages
// selected passes all three -- and only the first argument was ever classified
// or opened, so the others were discarded without a word. The rules keep a
// single argument behaving exactly as it always has:
//
//   * anything `argument_url` accepts is a page, and they open in the order
//     given;
//   * **an argument carrying any other scheme is still something to open**,
//     not a tree. `hydra magnet:?xt=...` used to become a tree path, be
//     refused by `load_tree` for being a url, and be dropped -- with a torrent
//     engine in the same process. The scheme test is `load_tree`'s own: more
//     than one character, which spares a Windows drive letter;
//   * the FIRST argument, when it carries no scheme, is the tree;
//   * any later argument that carries no scheme cannot be a second tree, so it
//     is reported rather than dropped.
//
// Here rather than in `main()` because these are rules with cases, and rules
// with cases belong somewhere a test can reach -- `main()` links into no test
// in this tree.
struct argument_plan {
	// Everything to open, in the order given: pages and any other scheme
	// something in this browser can take. Which of the two an entry is,
	// `renders_as_page` decides at the moment it is opened -- one answer, in
	// one place, shared with the engines (see `scheme_rules.h`).
	QStringList opens;
	QString     tree;         // empty when the first argument is not a path
	QStringList not_paths;    // later arguments that are neither, to report
};
argument_plan plan_arguments(const QStringList &args);

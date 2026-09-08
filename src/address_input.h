#pragma once

#include <QKeyEvent>
#include <QMouseEvent>
#include <QLineEdit>

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

protected:
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

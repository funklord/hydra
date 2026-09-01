#pragma once

#include <QDialog>

#include "policy.h"

class QCheckBox;

// "This site wants to use your camera." -- and the answer.
//
// **Nothing asked, before this.** The policy model had `allow` and `block` and
// nothing between them, so every capability a page can request had to be
// decided in advance, by someone who had not yet met the page. Camera and
// microphone were therefore blocked globally, which is the right default and
// the wrong end state: the refusal reached the page as a `NotAllowedError` and
// reached the person as nothing at all. A video call simply did not work, and
// the browser never said that it had been the one to stop it.
//
// So this is the third answer. It names the site, says plainly what is being
// asked for in the words of the thing rather than the words of the setting
// ("use your camera", not "Camera"), and offers to remember.
//
// **Closing the dialog blocks.** Escape, the window button, a click elsewhere
// that dismisses it -- every one of those is a refusal, not a deferral. A
// permission prompt that treats "go away" as "ask me again in a moment" is how
// a page nags someone into agreeing, and the page must never be able to profit
// from the dialog being dismissed.
class permission_dialog : public QDialog {
	Q_OBJECT
public:
	permission_dialog(const QString &host, policy::feature f,
	                   bool encrypted, QWidget *parent = nullptr);

	// True when the person asked for the answer to stick to this site.
	// Unchecked by default -- see the note in the implementation about why the
	// convenient default is the wrong one here.
	bool remember() const;

	// What the buttons mean, so the caller does not have to know which
	// `QDialog` result code stands for which answer.
	bool granted() const { return m_granted; }

private:
	QCheckBox *m_remember = nullptr;
	bool       m_granted  = false;
};

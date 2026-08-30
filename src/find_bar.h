#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;

// Find on this page: the strip that appears above the status bar.
//
// **It knows nothing about the engine.** It emits what somebody typed and what
// direction they asked for, and it is told how many matches there were. That
// keeps the one piece of this that is engine-specific -- `findText` and its
// result -- behind the WebViewBackend seam, and it means the bar can be driven
// by a test without a page.
class find_bar : public QWidget {
	Q_OBJECT
public:
	explicit find_bar(QWidget *parent = nullptr);

	// Show, take focus, and select whatever is already there, so that pressing
	// the shortcut a second time replaces the old term rather than appending
	// to it -- which is what every other search field does.
	void begin();

	// `matches` of nothing is not the same as nothing typed yet: the first is
	// worth saying, the second is not.
	void set_result(int matches, int active);

	// Say nothing rather than something stale. A count belongs to the page it
	// was counted on, and survives a switch to another one as a lie.
	void clear_result();

	QString text() const;

signals:
	// `fresh` distinguishes a new term from stepping through the current one.
	// The engine restarts its search on a new term and advances on a repeat,
	// and only the bar knows which just happened.
	void search(const QString &text, bool forward, bool fresh);
	void dismissed();

protected:
	void keyPressEvent(QKeyEvent *event) override;

private:
	void step(bool forward);

	QLineEdit *m_input = nullptr;
	QLabel    *m_count = nullptr;
};

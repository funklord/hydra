// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The shell, stood up the same way three times.
//
// `try_navigate` grew from fifteen checks to sixty-nine and from one subject to
// three, at which point it covered everything and nothing well: a failure said
// "try_navigate" and meant any of moving between pages, what the window says
// about the page, or the tools that act on it. This is the part all three
// share, so that splitting them costs a preamble rather than a copy.
//
// Header-only and in `live/` because it is scaffolding for the drivers that
// need a real window: it writes pages, builds the shell around them, and hands
// back the widgets a driver reaches for. Nothing here reaches into private
// members -- everything is found the way a person finds it, by placeholder,
// tooltip or object name.
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"

#include <QAction>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QProgressBar>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <cstdio>

namespace shell {

inline int g_pass = 0, g_fail = 0;

inline void check(bool ok, const QString &what) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(what)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(what)); }
}
inline void section(const char *n) { std::printf("\n== %s ==\n", n); }
inline void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}
inline int report() {
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

// Found by the tooltip somebody reads, not by position in the toolbar.
inline QAction *toolbar_action(QWidget *w, const QString &tip) {
	for (QAction *a : w->findChildren<QAction *>())
		if (a->toolTip() == tip)
			return a;
	return nullptr;
}

inline bool write_page(const QString &path, const QString &body) {
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	f.write(QString("<!doctype html><title>%1</title><p>%1</p>\n")
		            .arg(body).toUtf8());
	return true;
}

// Wait until the address bar shows what was asked for. Load times vary, and a
// fixed sleep either wastes seconds or fails on a busy machine.
inline bool wait_for(QLineEdit *address, const QString &fragment, int ms = 12000) {
	for (int waited = 0; waited < ms; waited += 200) {
		spin(200);
		if (address->text().contains(fragment))
			return true;
	}
	return false;
}

// An enabled state follows `loadFinished`, which can land a beat after the url
// changes, so a check made the instant the address bar updates is a race.
inline void settle(QAction *a, bool want) {
	for (int i = 0; i < 25 && a->isEnabled() != want; ++i)
		spin(200);
}

// Two local pages, a tree holding both, and the window around them.
//
// **Local files rather than a server**, because what is being driven is the
// shell: history, chrome and page tools all behave the same whatever served
// the document, and a file:// pair needs no network, no port and no cleanup.
struct fixture {
	QString         out, one, two, tree;
	policy_engine   policy;
	request_filter  filter;
	qtwebengine_factory factory;
	main_window     window;

	QAction   *back = nullptr, *fwd = nullptr, *reload = nullptr;
	QLineEdit *address = nullptr;
	QTreeView *tv = nullptr;

	explicit fixture(const QString &fallback_dir)
		    : out(qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
		              ? QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT"))
		              : fallback_dir),
		      filter(&policy), factory(&filter),
		      window(&factory, &policy, &filter) {
		QDir(out).removeRecursively();
		QDir().mkpath(out);
		one  = out + "/one.html";
		two  = out + "/two.html";
		tree = out + "/tree.txt";
		write_page(one, "one");
		write_page(two, "two");

		QFile tf(tree);
		if (tf.open(QIODevice::WriteOnly | QIODevice::Truncate))
			tf.write(QString("- [f0] folder | Work\n"
				                  "  - [a1] unopened | One | %1\n"
				                  "  - [a2] unopened | Two | %2\n")
				             .arg(QUrl::fromLocalFile(one).toString(),
				                   QUrl::fromLocalFile(two).toString()).toUtf8());
		tf.close();

		window.load_tree(tree);
		window.resize(1000, 700);
		window.show();
		spin(1200);

		back    = toolbar_action(&window, "Back");
		fwd     = toolbar_action(&window, "Forward");
		reload  = toolbar_action(&window, "Reload");
		tv      = window.findChild<QTreeView *>();
		for (QLineEdit *e : window.findChildren<QLineEdit *>())
			if (e->placeholderText() == "Address")
				address = e;
	}

	// Open the tab at `row` under the first folder, the way a click does, and
	// **return only once the page has actually arrived**.
	//
	// The address bar updates on `url_changed`, which is when a navigation
	// commits -- well before `loadFinished`. Returning there left the next
	// section running against a load still in flight: the progress bar was up,
	// so "no bar once the page has arrived" failed, and a second navigation
	// started on top of the first swallowed its own failure message. Waiting
	// for the bar to go is waiting for the same thing a person waits for.
	bool open_tab(int row, const QString &expect) {
		emit tv->activated(tv->model()->index(row, 0, tv->model()->index(0, 0)));
		if (!wait_for(address, expect))
			return false;
		wait_idle();
		return true;
	}

	// Until nothing is loading, or until it is clearly not going to settle.
	void wait_idle(int ms = 8000) {
		QProgressBar *bar = window.findChild<QProgressBar *>("load_progress");
		for (int waited = 0; waited < ms; waited += 100) {
			if (!bar || !bar->isVisible())
				return;
			spin(100);
		}
	}
};

}  // namespace shell

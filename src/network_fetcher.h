// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "extractor_helpers.h"
#include "local_proxy.h"

#include <QObject>
#include <QUrl>

class QThread;

Q_DECLARE_METATYPE(fetch_result)

// The real fetcher behind the sec 11.5.1 helper tier: the one component in it that
// touches a network, and the only one that touches a thread.
//
// **Why a thread at all.** A helper call is synchronous, because QJSEngine is;
// the network is not. Something has to bridge that, and the two obvious ways
// are both wrong. Spinning a nested QEventLoop on the *calling* thread would
// re-enter whatever that thread was doing -- on the UI thread that means
// delivering paint events and, worse, user input in the middle of a running
// script. Polling with `processEvents` has the same defect and burns a core.
//
// So the request runs on a thread of its own, which owns the
// QNetworkAccessManager and spins its nested loop there, where re-entrancy
// reaches nothing. The call marshals across with `Qt::BlockingQueuedConnection`
// -- genuinely synchronous, and without a hand-rolled condition variable to get
// subtly wrong.
//
// **The caller blocks.** That is the contract, and it is why the script should
// not be run on the UI thread once this is wired into the dialog: a slow CDN
// would freeze the window for the deadline. The budget's deadline bounds it.
//
// **It must not be called from its own thread.** BlockingQueuedConnection
// between a thread and itself deadlocks; `fetch` refuses that case and returns
// a failure rather than hanging, because a deadlock in a review dialog is
// indistinguishable from a hang and impossible to diagnose from the outside.
class network_fetcher : public QObject {
	Q_OBJECT
public:
	// `ctx` is the page's own request context -- the same one the proxy injects,
	// because a naked fetch of a CDN's stream URLs comes back 403 (sec 11.3).
	explicit network_fetcher(stream_context ctx = {}, QObject *parent = nullptr);
	~network_fetcher() override;

	// Blocking. Safe from any thread except this object's own worker.
	fetch_result fetch(const QUrl &url, qint64 max_bytes, int timeout_ms);

	// Bound for handing to a helper_host.
	helper_fetcher as_function();

private:
	class worker;
	QThread *m_thread = nullptr;
	worker  *m_worker = nullptr;
};

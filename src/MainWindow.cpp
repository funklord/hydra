#include "MainWindow.h"
#include "TabTreeModel.h"
#include "TreeSortProxy.h"
#include "StateStore.h"
#include "PolicyEngine.h"
#include "RequestInterceptor.h"
#include "SitePolicyDialog.h"
#include "Policy.h"
#include "Node.h"

#include <QSplitter>
#include <QTreeView>
#include <QStackedWidget>
#include <QToolBar>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QDataStream>
#include <QCloseEvent>
#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineHistory>
#include <QWebEngineSettings>
#include <QWebEngineCookieStore>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    // One shared profile. The request interceptor, cookie filter, and download
    // handler attach here in later steps (architecture doc §6/§7/§10).
    profile_ = QWebEngineProfile::defaultProfile();

    // --- Security spine: PolicyEngine + interceptor + cookie filter -------
    policy_      = new PolicyEngine(this);
    interceptor_ = new RequestInterceptor(policy_, this);
    profile_->setUrlRequestInterceptor(interceptor_);

    PolicyEngine* pe = policy_;
    profile_->cookieStore()->setCookieFilter(
        [pe](const QWebEngineCookieStore::FilterRequest& r) {
            const QString site = r.firstPartyUrl.host();
            if (r.thirdParty &&
                !pe->isAllowed(policy::Feature::ThirdPartyCookies, site))
                return false;
            return pe->isAllowed(policy::Feature::Cookies, site);
        });

    model_ = new TabTreeModel(this);
    proxy_ = new TreeSortProxy(this);
    proxy_->setSourceModel(model_);

    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(1500);   // debounce structural saves
    connect(saveTimer_, &QTimer::timeout, this, &MainWindow::flushTree);

    // --- Toolbar --------------------------------------------------------
    QToolBar* bar = addToolBar("Main");
    bar->setMovable(false);

    QAction* backAct   = bar->addAction("◀");
    QAction* fwdAct    = bar->addAction("▶");
    QAction* reloadAct = bar->addAction("↻");
    connect(backAct,   &QAction::triggered, this, &MainWindow::goBack);
    connect(fwdAct,    &QAction::triggered, this, &MainWindow::goForward);
    connect(reloadAct, &QAction::triggered, this, &MainWindow::reloadPage);

    address_ = new QLineEdit(this);
    address_->setPlaceholderText("Address");
    address_->setClearButtonEnabled(true);
    connect(address_, &QLineEdit::returnPressed, this, &MainWindow::navigateToAddress);
    bar->addWidget(address_);

    QAction* shieldAct = bar->addAction("Shield");
    shieldAct->setToolTip("Site controls");
    connect(shieldAct, &QAction::triggered, this, &MainWindow::openSiteControls);

    bar->addSeparator();
    bar->addWidget(new QLabel(" Sort: ", this));
    sortBox_ = new QComboBox(this);
    sortBox_->addItem("Tree order");
    sortBox_->addItem("Title A–Z");
    sortBox_->addItem("Newest");
    sortBox_->addItem("Recently seen");
    connect(sortBox_, SIGNAL(currentIndexChanged(int)), this, SLOT(onSortModeChanged(int)));
    bar->addWidget(sortBox_);

    search_ = new QLineEdit(this);
    search_->setPlaceholderText("Search tree");
    search_->setClearButtonEnabled(true);
    search_->setMaximumWidth(200);
    connect(search_, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);
    bar->addWidget(search_);

    // --- Central splitter ----------------------------------------------
    tree_ = new QTreeView(this);
    tree_->setModel(proxy_);
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->expandAll();
    connect(tree_, &QTreeView::activated, this, &MainWindow::onTreeActivated);
    connect(tree_, &QTreeView::clicked,   this, &MainWindow::onTreeActivated);
    connect(tree_, &QTreeView::customContextMenuRequested,
            this, &MainWindow::onTreeContextMenu);

    stack_ = new QStackedWidget(this);
    QLabel* placeholder = new QLabel("Select a tab from the tree", this);
    placeholder->setAlignment(Qt::AlignCenter);
    stack_->addWidget(placeholder);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(tree_);
    splitter->addWidget(stack_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 900});
    setCentralWidget(splitter);

    setWindowTitle("Hydra");
    resize(1180, 760);
}

bool MainWindow::loadTree(const QString& path) {
    treePath_ = path;
    const QString dir = QFileInfo(path).absolutePath();
    delete state_;
    state_ = new StateStore(dir + "/state");

    policyPath_ = dir + "/policy.json";
    policy_->load(policyPath_);   // no-op if the file doesn't exist yet

    const bool ok = model_->load(path);
    tree_->expandAll();
    return ok;
}

QWebEngineView* MainWindow::currentView() const {
    return qobject_cast<QWebEngineView*>(stack_->currentWidget());
}

void MainWindow::openNode(Node* node) {
    if (!node || node->url.isEmpty())
        return;  // folders and empty nodes have nothing to show

    node->lastSeen = QDateTime::currentDateTime();

    QWebEngineView* view = viewsById_.value(node->id, nullptr);
    if (!view) {
        view = new QWebEngineView(this);
        QWebEnginePage* page = new QWebEnginePage(profile_, view);
        view->setPage(page);
        viewsById_.insert(node->id, view);
        stack_->addWidget(view);

        applyPolicy(page, QUrl::fromUserInput(node->url).host());

        connect(view, &QWebEngineView::urlChanged, this, [this, view, page](const QUrl& u) {
            applyPolicy(page, u.host());
            if (view == currentView())
                updateAddress(u.toString());
        });

        // Feature permissions (geo/cam/mic/notifications) decided from policy.
        // Note: featurePermissionRequested is deprecated in Qt 6.8+ but still
        // functional; migrate to QWebEnginePermission when targeting only 6.8+.
        connect(page, &QWebEnginePage::featurePermissionRequested, this,
                [this, page](const QUrl& origin, QWebEnginePage::Feature f) {
            policy::Feature pf;
            switch (f) {
                case QWebEnginePage::Geolocation:
                    pf = policy::Feature::Geolocation; break;
                case QWebEnginePage::MediaAudioCapture:
                    pf = policy::Feature::Microphone; break;
                case QWebEnginePage::MediaVideoCapture:
                case QWebEnginePage::MediaAudioVideoCapture:
                    pf = policy::Feature::Camera; break;
                case QWebEnginePage::Notifications:
                    pf = policy::Feature::Notifications; break;
                default:
                    page->setFeaturePermission(origin, f,
                        QWebEnginePage::PermissionDeniedByUser);
                    return;
            }
            const bool grant = policy_->isAllowed(pf, origin.host());
            page->setFeaturePermission(origin, f,
                grant ? QWebEnginePage::PermissionGrantedByUser
                      : QWebEnginePage::PermissionDeniedByUser);
        });

        if (node->type == NodeType::SuspendedTab && state_ && state_->hasState(node->id)) {
            // Restore navigation history from the suspended blob.
            QByteArray blob = state_->load(node->id);
            QDataStream ds(&blob, QIODevice::ReadOnly);
            ds >> *view->history();
            state_->remove(node->id);
        } else {
            view->setUrl(QUrl::fromUserInput(node->url));
        }
    }

    node->type = NodeType::OpenTab;
    model_->refreshNode(node);
    stack_->setCurrentWidget(view);
    updateAddress(view->url().toString());
    touchLru(node->id);
    enforceLiveCap(node->id);
    markDirty();
}

void MainWindow::suspendNode(Node* node) {
    if (!node)
        return;
    QWebEngineView* view = viewsById_.value(node->id, nullptr);
    if (!view)
        return;

    // Serialize navigation history to a blob keyed by node id.
    QByteArray blob;
    {
        QDataStream ds(&blob, QIODevice::WriteOnly);
        ds << *view->history();
    }
    if (state_)
        state_->save(node->id, blob);

    if (view == currentView())
        stack_->setCurrentIndex(0);  // back to placeholder
    stack_->removeWidget(view);
    viewsById_.remove(node->id);
    lru_.removeAll(node->id);
    view->deleteLater();

    node->type = NodeType::SuspendedTab;
    model_->refreshNode(node);
    markDirty();
}

void MainWindow::touchLru(const QString& id) {
    lru_.removeAll(id);
    lru_.prepend(id);
}

void MainWindow::enforceLiveCap(const QString& keepId) {
    while (viewsById_.size() > kMaxLiveViews) {
        QString victim;
        for (auto it = lru_.crbegin(); it != lru_.crend(); ++it) {
            if (*it != keepId && viewsById_.contains(*it)) {
                victim = *it;
                break;
            }
        }
        if (victim.isEmpty())
            break;
        if (Node* n = model_->nodeById(victim))
            suspendNode(n);
        else
            break;
    }
}

void MainWindow::onTreeActivated(const QModelIndex& proxyIndex) {
    if (!proxyIndex.isValid())
        return;
    Node* node = model_->nodeForIndex(proxy_->mapToSource(proxyIndex));
    openNode(node);
}

void MainWindow::onTreeContextMenu(const QPoint& pos) {
    const QModelIndex proxyIndex = tree_->indexAt(pos);
    if (!proxyIndex.isValid())
        return;
    Node* node = model_->nodeForIndex(proxy_->mapToSource(proxyIndex));
    if (!node || node->isFolder())
        return;

    QMenu menu(this);
    QAction* openA = menu.addAction("Open");
    QAction* susA  = menu.addAction("Suspend");
    susA->setEnabled(viewsById_.contains(node->id));

    QAction* chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (chosen == openA)
        openNode(node);
    else if (chosen == susA)
        suspendNode(node);
}

void MainWindow::onSortModeChanged(int comboIndex) {
    using SM = TreeSortProxy::SortMode;
    static const SM modes[] = { SM::TreeOrder, SM::TitleAsc,
                                SM::NewestCreated, SM::RecentlySeen };
    if (comboIndex >= 0 && comboIndex < 4)
        proxy_->setSortMode(modes[comboIndex]);
    tree_->expandAll();
}

void MainWindow::onSearchChanged(const QString& text) {
    proxy_->setSearchText(text);
    if (!text.trimmed().isEmpty())
        tree_->expandAll();
}

void MainWindow::navigateToAddress() {
    if (QWebEngineView* view = currentView())
        view->setUrl(QUrl::fromUserInput(address_->text()));
}

void MainWindow::goBack() {
    if (QWebEngineView* v = currentView())
        v->triggerPageAction(QWebEnginePage::Back);
}

void MainWindow::goForward() {
    if (QWebEngineView* v = currentView())
        v->triggerPageAction(QWebEnginePage::Forward);
}

void MainWindow::reloadPage() {
    if (QWebEngineView* v = currentView())
        v->triggerPageAction(QWebEnginePage::Reload);
}

void MainWindow::markDirty() {
    saveTimer_->start();  // debounced flushTree()
}

void MainWindow::flushTree() {
    if (!treePath_.isEmpty())
        model_->save(treePath_);
}

void MainWindow::updateAddress(const QString& url) {
    address_->setText(url);
}

void MainWindow::applyPolicy(QWebEnginePage* page, const QString& host) {
    if (!page)
        return;
    QWebEngineSettings* s = page->settings();
    using F = policy::Feature;
    s->setAttribute(QWebEngineSettings::JavascriptEnabled,
                    policy_->isAllowed(F::JavaScript, host));
    s->setAttribute(QWebEngineSettings::AutoLoadImages,
                    policy_->isAllowed(F::Images, host));
    s->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture,
                    !policy_->isAllowed(F::Autoplay, host));
    s->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,
                    policy_->isAllowed(F::Popups, host));
}

void MainWindow::openSiteControls() {
    if (!policyDialog_) {
        policyDialog_ = new SitePolicyDialog(policy_, this);
        connect(policyDialog_, &SitePolicyDialog::policyChanged,
                this, &MainWindow::onPolicyChanged);
    }
    QString host;
    if (QWebEngineView* v = currentView())
        host = v->url().host();
    policyDialog_->setHost(host);
    policyDialog_->move(address_->mapToGlobal(QPoint(0, address_->height() + 2)));
    policyDialog_->show();
    policyDialog_->raise();
}

void MainWindow::onPolicyChanged() {
    if (!policyPath_.isEmpty())
        policy_->save(policyPath_);
    if (QWebEngineView* v = currentView()) {
        applyPolicy(v->page(), v->url().host());
        v->triggerPageAction(QWebEnginePage::Reload);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Persist live tabs as suspended blobs so they restore next launch,
    // then write the tree structure.
    const QList<QString> liveIds = viewsById_.keys();
    for (const QString& id : liveIds)
        if (Node* n = model_->nodeById(id))
            suspendNode(n);
    if (!treePath_.isEmpty())
        model_->save(treePath_);
    if (!policyPath_.isEmpty())
        policy_->save(policyPath_);
    QMainWindow::closeEvent(event);
}

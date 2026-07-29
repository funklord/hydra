#pragma once

#include <QMainWindow>
#include <QHash>
#include <QStringList>
#include <QString>

class QTreeView;
class QStackedWidget;
class QLineEdit;
class QComboBox;
class QTimer;
class QPoint;
class QCloseEvent;
class QWebEngineView;
class QWebEngineProfile;
class QWebEnginePage;
class TabTreeModel;
class TreeSortProxy;
class StateStore;
class PolicyEngine;
class RequestInterceptor;
class SitePolicyDialog;
struct Node;

// The shell: a splitter with the tab tree on the left and a stack of
// chrome-less web views on the right (architecture doc §6). Tabs follow a
// lifecycle — unopened -> open (live view) -> suspended (history blob) — with a
// small cap on how many views stay live at once (§5.4).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    bool loadTree(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onTreeActivated(const QModelIndex& proxyIndex);
    void onTreeContextMenu(const QPoint& pos);
    void onSortModeChanged(int comboIndex);
    void onSearchChanged(const QString& text);
    void navigateToAddress();
    void goBack();
    void goForward();
    void reloadPage();
    void flushTree();
    void openSiteControls();
    void onPolicyChanged();

private:
    QWebEngineView* currentView() const;
    void openNode(Node* node);          // create/restore a live view and show it
    void suspendNode(Node* node);       // serialize + tear down the live view
    void touchLru(const QString& id);
    void enforceLiveCap(const QString& keepId);
    void markDirty();
    void updateAddress(const QString& url);
    void applyPolicy(QWebEnginePage* page, const QString& host);

    static constexpr int kMaxLiveViews = 4;

    TabTreeModel*   model_ = nullptr;
    TreeSortProxy*  proxy_ = nullptr;
    QTreeView*      tree_  = nullptr;
    QStackedWidget* stack_ = nullptr;

    QLineEdit*      address_ = nullptr;
    QComboBox*      sortBox_ = nullptr;
    QLineEdit*      search_  = nullptr;

    QWebEngineProfile*               profile_ = nullptr;
    StateStore*                      state_   = nullptr;
    PolicyEngine*                    policy_  = nullptr;
    RequestInterceptor*              interceptor_ = nullptr;
    SitePolicyDialog*                policyDialog_ = nullptr;
    QTimer*                          saveTimer_ = nullptr;
    QString                          treePath_;
    QString                          policyPath_;

    QHash<QString, QWebEngineView*>  viewsById_;   // node id -> live view
    QStringList                      lru_;         // most-recent id at front
};

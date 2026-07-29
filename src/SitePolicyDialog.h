#pragma once

#include <QDialog>
#include <QVector>
#include <QString>

class QComboBox;
class QLabel;
class PolicyEngine;

// The compact per-site editor that drops down from the address-bar shield
// (architecture doc §7.4). A scope selector (this host / *.domain / global) and
// one tri-state combo per feature, all writing straight into the PolicyEngine.
class SitePolicyDialog : public QDialog {
    Q_OBJECT
public:
    SitePolicyDialog(PolicyEngine* engine, QWidget* parent = nullptr);

    void setHost(const QString& host);

signals:
    void policyChanged();

private slots:
    void onScopeChanged();
    void onFeatureChanged(int featureIndex);

private:
    QString currentPattern() const;   // empty string means "global"
    void    repopulate();

    PolicyEngine*         engine_;
    QString               host_;
    QLabel*               hostLabel_ = nullptr;
    QComboBox*            scope_     = nullptr;
    QVector<QComboBox*>   combos_;
    bool                  populating_ = false;
};

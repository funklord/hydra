#pragma once

#include "Policy.h"

#include <QObject>
#include <QString>
#include <QVector>

// Holds all per-site rules and resolves the effective decision for a feature on
// a host (architecture doc §7.1/§7.2). Consulted by the interceptor, the cookie
// filter, per-page settings, and permission handling. Thread note: reads happen
// from the interceptor which Qt may call off the UI thread; the rule set is
// only mutated from the UI thread and reads tolerate a stale snapshot.
class PolicyEngine : public QObject {
    Q_OBJECT
public:
    struct Rule {
        QString pattern;   // exact host, "*.domain.tld", or "*"
        quint64 bits = 0;  // packed per-feature Settings
    };

    explicit PolicyEngine(QObject* parent = nullptr);

    // Effective decision. true = feature permitted, false = blocked.
    bool isAllowed(policy::Feature f, const QString& host) const;
    policy::Setting effectiveSetting(policy::Feature f, const QString& host) const;

    // Global default per feature (always Allow or Block, never Default).
    policy::Setting globalDefault(policy::Feature f) const;
    void            setGlobalDefault(policy::Feature f, policy::Setting s);

    // Per-pattern rule editing.
    policy::Setting settingFor(const QString& pattern, policy::Feature f) const;
    void            setSetting(const QString& pattern, policy::Feature f, policy::Setting s);

    const QVector<Rule>& rules() const { return rules_; }

    bool load(const QString& path);
    bool save(const QString& path) const;

    // Best-effort registrable domain (last two labels; no public-suffix list).
    static QString etldPlusOne(const QString& host);

signals:
    void changed();

private:
    static bool matchPattern(const QString& pattern, const QString& host, int& specificity);
    Rule*       findRule(const QString& pattern);
    const Rule* findRule(const QString& pattern) const;

    QVector<Rule> rules_;
    quint64       globalDefaults_ = 0;
};

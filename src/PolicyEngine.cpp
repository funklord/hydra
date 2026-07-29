#include "PolicyEngine.h"

#include <QFile>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

using policy::Feature;
using policy::Setting;

namespace {

const char* settingName(Setting s) {
    switch (s) {
        case Setting::Allow: return "allow";
        case Setting::Block: return "block";
        default:             return "default";
    }
}

Setting settingFromName(const QString& n) {
    if (n == "allow") return Setting::Allow;
    if (n == "block") return Setting::Block;
    return Setting::Default;
}

}  // namespace

PolicyEngine::PolicyEngine(QObject* parent) : QObject(parent) {
    // Sensible privacy-leaning defaults (architecture doc §7.2). Flip
    // JavaScript to Block here to run in per-site "whitelist mode".
    setGlobalDefault(Feature::JavaScript,        Setting::Allow);
    setGlobalDefault(Feature::Cookies,           Setting::Allow);
    setGlobalDefault(Feature::ThirdPartyCookies, Setting::Block);
    setGlobalDefault(Feature::Ads,               Setting::Block);
    setGlobalDefault(Feature::Popups,            Setting::Block);
    setGlobalDefault(Feature::Images,            Setting::Allow);
    setGlobalDefault(Feature::Autoplay,          Setting::Block);
    setGlobalDefault(Feature::Geolocation,       Setting::Block);
    setGlobalDefault(Feature::Camera,            Setting::Block);
    setGlobalDefault(Feature::Microphone,        Setting::Block);
    setGlobalDefault(Feature::Notifications,     Setting::Block);
    setGlobalDefault(Feature::Referer,           Setting::Allow);
}

QString PolicyEngine::etldPlusOne(const QString& host) {
    const QStringList labels = host.split('.', Qt::SkipEmptyParts);
    if (labels.size() <= 2)
        return host;
    return labels.mid(labels.size() - 2).join('.');
}

bool PolicyEngine::matchPattern(const QString& pattern, const QString& host, int& specificity) {
    if (pattern == "*") {
        specificity = 0;
        return true;
    }
    if (pattern.startsWith("*.")) {
        const QString dom = pattern.mid(2);
        if (host == dom || host.endsWith("." + dom)) {
            specificity = dom.count('.') + 1;   // more labels = more specific
            return true;
        }
        return false;
    }
    if (pattern == host) {
        specificity = host.count('.') + 100;    // exact beats any wildcard
        return true;
    }
    return false;
}

PolicyEngine::Rule* PolicyEngine::findRule(const QString& pattern) {
    for (Rule& r : rules_)
        if (r.pattern == pattern)
            return &r;
    return nullptr;
}

const PolicyEngine::Rule* PolicyEngine::findRule(const QString& pattern) const {
    for (const Rule& r : rules_)
        if (r.pattern == pattern)
            return &r;
    return nullptr;
}

Setting PolicyEngine::effectiveSetting(Feature f, const QString& host) const {
    Setting best = Setting::Default;
    int bestSpec = -1;
    for (const Rule& r : rules_) {
        const Setting s = policy::getSetting(r.bits, f);
        if (s == Setting::Default)
            continue;
        int spec = -1;
        if (matchPattern(r.pattern, host, spec) && spec > bestSpec) {
            bestSpec = spec;
            best = s;
        }
    }
    if (best != Setting::Default)
        return best;
    return globalDefault(f);
}

bool PolicyEngine::isAllowed(Feature f, const QString& host) const {
    return effectiveSetting(f, host) != Setting::Block;
}

Setting PolicyEngine::globalDefault(Feature f) const {
    const Setting s = policy::getSetting(globalDefaults_, f);
    return (s == Setting::Block) ? Setting::Block : Setting::Allow;
}

void PolicyEngine::setGlobalDefault(Feature f, Setting s) {
    const Setting norm = (s == Setting::Block) ? Setting::Block : Setting::Allow;
    globalDefaults_ = policy::withSetting(globalDefaults_, f, norm);
    emit changed();
}

Setting PolicyEngine::settingFor(const QString& pattern, Feature f) const {
    const Rule* r = findRule(pattern);
    return r ? policy::getSetting(r->bits, f) : Setting::Default;
}

void PolicyEngine::setSetting(const QString& pattern, Feature f, Setting s) {
    Rule* r = findRule(pattern);
    if (!r) {
        rules_.push_back({pattern, 0});
        r = &rules_.last();
    }
    r->bits = policy::withSetting(r->bits, f, s);
    emit changed();
}

bool PolicyEngine::load(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;
    const QJsonObject root = doc.object();

    const QJsonObject gd = root.value("globalDefaults").toObject();
    for (auto it = gd.begin(); it != gd.end(); ++it) {
        const Feature f = policy::featureFromName(it.key());
        if (f != Feature::Count)
            setGlobalDefault(f, settingFromName(it.value().toString()));
    }

    rules_.clear();
    const QJsonArray arr = root.value("rules").toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        Rule rule;
        rule.pattern = o.value("pattern").toString();
        if (rule.pattern.isEmpty())
            continue;
        const QJsonObject settings = o.value("settings").toObject();
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            const Feature f = policy::featureFromName(it.key());
            if (f != Feature::Count)
                rule.bits = policy::withSetting(rule.bits, f, settingFromName(it.value().toString()));
        }
        rules_.push_back(rule);
    }
    emit changed();
    return true;
}

bool PolicyEngine::save(const QString& path) const {
    QJsonObject gd;
    for (int i = 0; i < policy::featureCount(); ++i) {
        const Feature f = static_cast<Feature>(i);
        gd.insert(policy::featureName(f), settingName(globalDefault(f)));
    }

    QJsonArray arr;
    for (const Rule& r : rules_) {
        QJsonObject settings;
        for (int i = 0; i < policy::featureCount(); ++i) {
            const Feature f = static_cast<Feature>(i);
            const Setting s = policy::getSetting(r.bits, f);
            if (s != Setting::Default)
                settings.insert(policy::featureName(f), settingName(s));
        }
        if (settings.isEmpty())
            continue;  // don't persist empty rules
        QJsonObject o;
        o.insert("pattern", r.pattern);
        o.insert("settings", settings);
        arr.append(o);
    }

    QJsonObject root;
    root.insert("globalDefaults", gd);
    root.insert("rules", arr);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

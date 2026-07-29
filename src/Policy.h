#pragma once

#include <QString>
#include <QtGlobal>

// Per-site security features and the tri-state setting model (architecture
// doc §7.1). Each rule packs one 2-bit Setting per Feature into a quint64.
namespace policy {

enum class Feature : int {
    JavaScript = 0,
    Cookies,
    ThirdPartyCookies,
    Ads,
    Popups,
    Images,
    Autoplay,
    Geolocation,
    Camera,
    Microphone,
    Notifications,
    Referer,
    Count
};

enum class Setting : quint8 { Default = 0, Allow = 1, Block = 2 };

inline int featureCount() { return static_cast<int>(Feature::Count); }

// Stable machine name (JSON keys).
const char* featureName(Feature f);
// Human label (UI).
const char* featureLabel(Feature f);
// Parse a machine name; returns Feature::Count on failure.
Feature     featureFromName(const QString& name);

// --- 2-bit packing into a quint64 -----------------------------------------
inline Setting getSetting(quint64 bits, Feature f) {
    const int shift = 2 * static_cast<int>(f);
    return static_cast<Setting>((bits >> shift) & 0x3ULL);
}

inline quint64 withSetting(quint64 bits, Feature f, Setting s) {
    const int shift = 2 * static_cast<int>(f);
    bits &= ~(0x3ULL << shift);
    bits |= (static_cast<quint64>(s) & 0x3ULL) << shift;
    return bits;
}

}  // namespace policy

#include "SitePolicyDialog.h"
#include "PolicyEngine.h"
#include "Policy.h"

#include <QVBoxLayout>
#include <QGridLayout>
#include <QComboBox>
#include <QLabel>
#include <QFrame>
#include <QFont>

using policy::Feature;
using policy::Setting;

namespace {

int settingToIndex(Setting s) {
    switch (s) {
        case Setting::Allow: return 1;
        case Setting::Block: return 2;
        default:             return 0;  // Default
    }
}

Setting indexToSetting(int i) {
    switch (i) {
        case 1:  return Setting::Allow;
        case 2:  return Setting::Block;
        default: return Setting::Default;
    }
}

}  // namespace

SitePolicyDialog::SitePolicyDialog(PolicyEngine* engine, QWidget* parent)
    : QDialog(parent), engine_(engine) {
    setWindowFlags(Qt::Popup);
    setWindowTitle("Site controls");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    hostLabel_ = new QLabel(this);
    QFont hf = hostLabel_->font();
    hf.setBold(true);
    hostLabel_->setFont(hf);
    outer->addWidget(hostLabel_);

    scope_ = new QComboBox(this);
    scope_->addItem("This host");
    scope_->addItem("This domain");
    scope_->addItem("Global default");
    connect(scope_, SIGNAL(currentIndexChanged(int)), this, SLOT(onScopeChanged()));
    outer->addWidget(scope_);

    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    outer->addWidget(line);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(4);
    combos_.resize(policy::featureCount());
    for (int i = 0; i < policy::featureCount(); ++i) {
        const Feature f = static_cast<Feature>(i);
        grid->addWidget(new QLabel(policy::featureLabel(f), this), i, 0);

        auto* combo = new QComboBox(this);
        combo->addItem("Default");
        combo->addItem("Allow");
        combo->addItem("Block");
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, i](int) { onFeatureChanged(i); });
        combos_[i] = combo;
        grid->addWidget(combo, i, 1);
    }
    outer->addLayout(grid);
}

void SitePolicyDialog::setHost(const QString& host) {
    host_ = host;
    hostLabel_->setText(host.isEmpty() ? QStringLiteral("(no page)") : host);
    scope_->setItemText(1, "This domain (*." + PolicyEngine::etldPlusOne(host) + ")");
    repopulate();
}

QString SitePolicyDialog::currentPattern() const {
    switch (scope_->currentIndex()) {
        case 0:  return host_;
        case 1:  return "*." + PolicyEngine::etldPlusOne(host_);
        default: return QString();  // global
    }
}

void SitePolicyDialog::repopulate() {
    populating_ = true;
    const bool global = (scope_->currentIndex() == 2);
    const QString pattern = currentPattern();
    for (int i = 0; i < policy::featureCount(); ++i) {
        const Feature f = static_cast<Feature>(i);
        Setting s = global ? engine_->globalDefault(f)
                           : engine_->settingFor(pattern, f);
        combos_[i]->setCurrentIndex(settingToIndex(s));
    }
    populating_ = false;
}

void SitePolicyDialog::onScopeChanged() {
    repopulate();
}

void SitePolicyDialog::onFeatureChanged(int featureIndex) {
    if (populating_ || featureIndex < 0 || featureIndex >= combos_.size())
        return;
    const Feature f = static_cast<Feature>(featureIndex);
    const Setting s = indexToSetting(combos_[featureIndex]->currentIndex());

    if (scope_->currentIndex() == 2) {
        // Global default: Default is not meaningful, treat it as Allow.
        engine_->setGlobalDefault(f, s == Setting::Default ? Setting::Allow : s);
    } else {
        engine_->setSetting(currentPattern(), f, s);
    }
    emit policyChanged();
}

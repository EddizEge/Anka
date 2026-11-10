// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_nx2perf.h"

#include <cstdlib>
#include <string>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStringList>

#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "ui_configure_nx2perf.h"
#include "yuzu/nx2perf_session.h"
#include "yuzu_cmd/anka_core_route.h"

namespace Perf = Core::Nx2PerfSettings;

namespace {

/// The second core's game, as the texts about it name it
QString SecondCoreGame() {
    return QString::fromUtf8(AnkaCoreRoute::Text::GameName);
}

} // Anonymous namespace

ConfigureNx2Perf::ConfigureNx2Perf(QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigureNx2Perf>()} {
    ui->setupUi(this);

    using S = Perf::Switches;
    rows = {
        {ui->split_gpu_thread,
         ui->split_gpu_thread_text,
         &S::split_gpu_thread,
         {"EDEN_NX2_BACK_STAGES", "EDEN_NX2_BACK_CUTS"}},
        {ui->acquire_peek, ui->acquire_peek_text, &S::acquire_peek, {"EDEN_NX2_ACQUIRE_PEEK"}},
        {ui->jit_same, ui->jit_same_text, &S::jit_same, {"EDEN_NX2_JIT_SAME"}},
        {ui->front_macro_lean,
         ui->front_macro_lean_text,
         &S::front_macro_lean,
         {"EDEN_NX2_FRONT_MACRO_LEAN"}},
        {ui->fg_timed, ui->fg_timed_text, &S::fg_timed, {"EDEN_NX2_FG_TIMED"}},
        {ui->osd, ui->osd_text, &S::osd, {"EDEN_NX2_OSD"}},
        {ui->dk_dyn, ui->dk_dyn_text, &S::dk_dyn, {"EDEN_NX2_DK_DYN"}},
        {ui->dk_pacing,
         ui->dk_pacing_text,
         &S::dk_pacing,
         {"EDEN_NX2_PRESENT_INTERVAL_WORD", "EDEN_NX2_MODE_PACE", "EDEN_NX2_PACE_SPIN_US"}},
    };
    BuildSecondCoreOptions();
    RetranslateSecondCoreOptions();

    SetConfiguration();
    MarkSetOutside();
    ShowSecondCoreNotes();
    ui->preset->setToolTip(tr(AnkaCoreRoute::Text::PresetOnSecondCore).arg(SecondCoreGame()));

    // The preset's item order is Perf::Preset's (configure_nx2perf.ui).
    connect(ui->preset, &QComboBox::activated, this,
            [this](int index) { ChoosePreset(static_cast<Perf::Preset>(index)); });
    for (const Row& row : rows) {
        connect(row.box, &QCheckBox::toggled, this, [this] { BoxChanged(); });
    }
    // The item order is Perf::FrameGen's; "Other" only shows the Graphics tab's own setup.
    if (auto* model = qobject_cast<QStandardItemModel*>(ui->frame_gen->model())) {
        model->item(static_cast<int>(Perf::FrameGen::Other))->setEnabled(false);
    }
    connect(ui->frame_gen, &QComboBox::activated, this,
            [this](int index) { ChooseFrameGen(static_cast<Perf::FrameGen>(index)); });
}

ConfigureNx2Perf::~ConfigureNx2Perf() = default;

void ConfigureNx2Perf::SetReactiveFlushing(std::function<bool(bool, bool)> func) {
    reactive_flushing = std::move(func);
}

void ConfigureNx2Perf::SetGameRunning(bool running) {
    game_running = running;
}

void ConfigureNx2Perf::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureNx2Perf::RetranslateUI() {
    ui->retranslateUi(this);
    ShowPreset();
    ShowFrameGen();
    MarkSetOutside();
    // The notes go under the texts as they are now, the conflict's line included
    ShowDkDynConflict();
    ShowSecondCoreNotes();
    RetranslateSecondCoreOptions();
}

void ConfigureNx2Perf::ShowDkDynConflict() {
    using Reason = Core::Nx2DkDynConflictInfo::Reason;
    const auto conflict = Core::Nx2DkDynConflict();
    const QString mod = QString::fromStdString(conflict.mod);
    QString why;
    switch (conflict.reason) {
    case Reason::None:
        return;
    case Reason::Zone:
        why = tr("Off in the running game: the mod \"%1\" changes the frame rate code it patches; "
                 "both at once run the game too fast. Turn the mod off in Add-Ons (Properties) to "
                 "use it.")
                  .arg(mod);
        break;
    case Reason::Cave:
        why = tr("Off in the running game: the mod \"%1\" puts its own code in the free space past "
                 "the game's code that dynamic FPS writes to; using it would break the mod. Turn "
                 "the mod off in Add-Ons (Properties) to use it.")
                  .arg(mod);
        break;
    case Reason::CaveInUse:
        why = tr("Off in the running game: the free space past the game's code that dynamic FPS "
                 "writes to is not empty (a mod replacing the game's code, e.g. its exefs/main?). "
                 "The log has the address. Turn such mods off in Add-Ons (Properties) to use it.");
        break;
    case Reason::Name:
        why = tr("Off in the running game: the mod \"%1\" is named like a frame rate mod; both at "
                 "once run the game too fast. Turn the mod off in Add-Ons (Properties) to use it.")
                  .arg(mod);
        break;
    case Reason::AnyMod:
        why = tr("Off in the running game: EDEN_NX2_DK_DYN_MOD_GUARD=all turns it off with any mod "
                 "that changes the game's code, here \"%1\". Turn the mod off in Add-Ons "
                 "(Properties), or unset MOD_GUARD (auto), to use it.")
                  .arg(mod);
        break;
    case Reason::Cheat:
        why = tr("Off in the running game: the cheat [%1] in \"%2\" reads like a frame rate cheat; "
                 "both at once run the game too fast. Turn \"%2\" off in Add-Ons (Properties; its "
                 "other cheats go off too) or delete the [%1] entry from its cheats file to use "
                 "it.")
                  .arg(QString::fromStdString(conflict.cheat), mod);
        break;
    }
    ui->dk_dyn_text->setText(ui->dk_dyn_text->text() + QStringLiteral("\n") + why);
}

void ConfigureNx2Perf::SetConfiguration() {
    Show(UISettings::values.nx2perf);
    ShowSecondCoreOptions(UISettings::values.second_core);
    ShowDkDynConflict();
    // A choice waiting for the game to stop (Later) is shown, not the running game's setting
    const auto waiting = Nx2PerfSession::Pending().frame_gen;
    frame_gen_shown = waiting.value_or(Nx2PerfSession::CurrentFrameGen());
    ShowFrameGen();
    if (Nx2PerfSession::HasPendingGraphics()) {
        ShowPending();
    }
}

Perf::Switches ConfigureNx2Perf::Current() const {
    Perf::Switches switches{};
    for (const Row& row : rows) {
        switches.*row.field = row.box->isChecked();
    }
    switches.preset = preset;
    return switches;
}

void ConfigureNx2Perf::Show(const Perf::Switches& switches) {
    for (const Row& row : rows) {
        const QSignalBlocker blocker{row.box};
        row.box->setChecked(switches.*row.field);
    }
    preset = switches.preset;
    ShowPreset();
}

void ConfigureNx2Perf::ShowPreset() {
    {
        const QSignalBlocker blocker{ui->preset};
        ui->preset->setCurrentIndex(static_cast<int>(preset));
    }
    switch (preset) {
    case Perf::Preset::Recommended:
        ui->preset_text->setText(tr("All settings on, Reactive Flushing off (Graphics, Advanced): "
                                    "the setup Anka was measured with (GPU Mode Fast, the default). Like "
                                    "start-recommended.bat."));
        break;
    case Perf::Preset::Reactive:
        ui->preset_text->setText(
            tr("All settings on, Reactive Flushing and barrier feedback loops on, RX_RT_FIT=3. "
               "Experimental: on our PC faster in open areas, slower at the opening. Like "
               "start-reactive-experimental.bat."));
        break;
    case Perf::Preset::Safe:
        ui->preset_text->setText(tr("All settings below off, and the GPU shortcuts Anka keeps on by "
                                    "default off too (the variables start-safe.bat sets to 0). "
                                    "Frame generation off; other graphics settings such as "
                                    "Reactive Flushing stay as they are."));
        break;
    case Perf::Preset::Custom:
        ui->preset_text->setText(tr("Your own choice of the settings below."));
        break;
    }
}

void ConfigureNx2Perf::ChoosePreset(Perf::Preset chosen) {
    // Custom and Recommended change no graphics setting: a choice still waiting keeps its note
    ShowStatus();
    Show(Perf::FromPreset(chosen, Current()));
    if (const auto frame_gen = Perf::FrameGenFor(chosen)) {
        ChooseFrameGen(*frame_gen);
    }
    const auto reactive = Perf::ReactiveFlushing(chosen);
    if (!reactive) {
        return;
    }
    const bool barrier_on = Perf::BarrierFeedbackLoops(chosen).value_or(false);
    if (game_running) {
        pending_reactive = *reactive;
        pending_barrier = Perf::BarrierFeedbackLoops(chosen);
        ShowPending();
        return;
    }
    if (!reactive_flushing || !reactive_flushing(*reactive, barrier_on)) {
        ui->status->setText(tr("Reactive Flushing (Graphics, Advanced) can only be changed with no "
                               "game running; set it there afterwards."));
    }
}

void ConfigureNx2Perf::BoxChanged() {
    const Perf::Switches switches = Perf::AfterBoxChange(Current());
    if (switches.preset != preset) {
        preset = switches.preset;
        ShowPreset();
    }
}

void ConfigureNx2Perf::ShowFrameGen() {
    {
        const QSignalBlocker blocker{ui->frame_gen};
        ui->frame_gen->setCurrentIndex(static_cast<int>(frame_gen_shown));
    }
    QString text;
    switch (frame_gen_shown) {
    case Perf::FrameGen::Off:
        text = tr("Off.");
        break;
    case Perf::FrameGen::Fixed2x:
        text = tr("FSR3, one generated frame per real frame. On a 60 Hz screen it holds the game "
                  "at 30 fps.");
        break;
    case Perf::FrameGen::Dynamic:
        text = tr("FSR3 aiming at 120 fps, at most the screen's refresh rate: 2x to 4x, only while "
                  "the screen can show the frames and the game's own frame rate holds.");
        break;
    case Perf::FrameGen::Other:
        text = tr("Set in the Graphics tab.");
        break;
    }
    ui->frame_gen_text->setText(
        text + QLatin1Char(' ') +
        tr("Generated frames need a screen faster than the game's frame rate; on a 60 Hz screen "
           "they only help below 30 fps."));
}

void ConfigureNx2Perf::ChooseFrameGen(Perf::FrameGen chosen) {
    if (chosen == Perf::FrameGen::Other) {
        ShowFrameGen();
        return;
    }
    frame_gen_choice = chosen;
    frame_gen_shown = chosen;
    ShowFrameGen();
    ShowStatus();
}

bool ConfigureNx2Perf::Waiting() const {
    return game_running &&
           (Nx2PerfSession::HasPendingGraphics() || pending_reactive.has_value() ||
            pending_barrier.has_value() ||
            (frame_gen_choice && *frame_gen_choice != Nx2PerfSession::CurrentFrameGen()));
}

void ConfigureNx2Perf::ShowStatus() {
    if (Waiting()) {
        ShowPending();
    } else {
        ui->status->clear();
    }
}

void ConfigureNx2Perf::ShowPending() {
    ui->status->setText(tr("A game is running: Reactive Flushing and frame generation cannot change "
                           "in it. Anka applies them when it restarts (Restart Now) or when the "
                           "game stops."));
}

void ConfigureNx2Perf::ApplyConfiguration() {
    UISettings::values.nx2perf = Current();
    UISettings::values.second_core = CurrentSecondCoreOptions();
    if (game_running) {
        Nx2PerfSession::AddPendingGraphics({.reactive_flushing = pending_reactive,
                                            .barrier_feedback_loops = pending_barrier,
                                            .frame_gen = frame_gen_choice});
    } else if (frame_gen_choice) {
        Nx2PerfSession::ApplyFrameGen(*frame_gen_choice);
    }
}

void ConfigureNx2Perf::MarkSetOutside() {
    for (const Row& row : rows) {
        QStringList outside;
        for (const std::string_view name : row.names) {
            if (Nx2PerfSession::SetOutside(name)) {
                // The value now: the program never sets one that was there at its start.
                const std::string key{name};
                const char* value = std::getenv(key.c_str());
                outside << QStringLiteral("%1=%2").arg(QString::fromStdString(key),
                                                       QString::fromUtf8(value ? value : ""));
            }
        }
        // All set outside: the box changes nothing, its value is theirs.
        const bool all_outside = outside.size() == static_cast<qsizetype>(row.names.size());
        row.box->setEnabled(!all_outside);
        QStringList tip;
        if (all_outside) {
            tip << tr("Set outside the program (your environment, a .bat of your own "
                      "or nx2_switches.txt), so this is used whatever the box says: "
                      "%1")
                       .arg(outside.join(QStringLiteral(", ")));
        } else if (!outside.isEmpty()) {
            tip << tr("Partly set outside the program, and that part is used: %1. "
                      "The box sets the rest.")
                       .arg(outside.join(QStringLiteral(", ")));
        }
        row.box->setToolTip(tip.join(QStringLiteral("\n\n")));
    }
}

void ConfigureNx2Perf::ShowSecondCoreNotes() {
    // The second core takes only the settings its table has (yuzu_cmd/anka_core_route.h). A quiet
    // line under the text, in the placeholder colour: a fact about the other core, not a warning,
    // and the box stays usable (every setting applies on the main core).
    const QString colour = palette().color(QPalette::PlaceholderText).name();
    for (const Row& row : rows) {
        if (AnkaCoreRoute::ReachesSecondCore(KeyName(row.field))) {
            continue;
        }
        const QString text =
            row.text->text().toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        row.text->setTextFormat(Qt::RichText);
        row.text->setText(QStringLiteral("%1<br><span style=\"color:%2\">%3</span>")
                              .arg(text, colour,
                                   tr(AnkaCoreRoute::Text::NoEffectOnSecondCore)
                                       .arg(SecondCoreGame())
                                       .toHtmlEscaped()));
    }
}

std::string_view ConfigureNx2Perf::KeyName(bool Perf::Switches::*field) {
    for (const Perf::Key& key : Perf::Keys) {
        if (key.field == field) {
            return key.name;
        }
    }
    return {};
}

void ConfigureNx2Perf::BuildSecondCoreOptions() {
    // The rows of the game group above, from the table: a box with its text under it (indented as
    // theirs), or a label and a number / choice on one line with the text under it (as Preset)
    using AnkaCoreRoute::OptionKind;
    for (const auto& option : AnkaCoreRoute::SecondCoreOptions) {
        OptionRow row{};
        switch (option.kind) {
        case OptionKind::Toggle:
            row.input = new QCheckBox(ui->second_core);
            ui->second_core_layout->addWidget(row.input);
            break;
        case OptionKind::Number: {
            auto* spin = new QSpinBox(ui->second_core);
            spin->setRange(option.min, option.max);
            row.input = spin;
            break;
        }
        case OptionKind::Choice: {
            auto* choice = new QComboBox(ui->second_core);
            for (const auto& item : option.choices) {
                choice->addItem(QString{}, item.value);
            }
            row.input = choice;
            break;
        }
        }
        if (option.kind != OptionKind::Toggle) {
            row.label = new QLabel(ui->second_core);
            row.label->setBuddy(row.input);
            auto* line = new QHBoxLayout;
            line->addWidget(row.label);
            line->addWidget(row.input);
            line->addStretch();
            ui->second_core_layout->addLayout(line);
        }
        row.text = new QLabel(ui->second_core);
        row.text->setWordWrap(true);
        row.text->setBuddy(row.input);
        if (option.kind == OptionKind::Toggle) {
            row.text->setIndent(22);
        }
        ui->second_core_layout->addWidget(row.text);
        option_rows.push_back(row);
    }
}

void ConfigureNx2Perf::RetranslateSecondCoreOptions() {
    namespace Text = AnkaCoreRoute::Text;
    // Titled as the main core's game group above it
    ui->second_core->setTitle(tr(Text::OptionsTitle).arg(SecondCoreGame()));
    for (std::size_t i = 0; i < option_rows.size(); ++i) {
        const auto& option = AnkaCoreRoute::SecondCoreOptions[i];
        const OptionRow& row = option_rows[i];
        if (row.label) {
            row.label->setText(tr(option.label));
        } else {
            static_cast<QCheckBox*>(row.input)->setText(tr(option.label));
        }
        if (option.kind == AnkaCoreRoute::OptionKind::Choice) {
            auto* choice = static_cast<QComboBox*>(row.input);
            for (std::size_t j = 0; j < option.choices.size(); ++j) {
                choice->setItemText(static_cast<int>(j), tr(option.choices[j].label));
            }
        }
        row.text->setText(tr(option.text));
    }
}

void ConfigureNx2Perf::ShowSecondCoreOptions(const AnkaCoreRoute::SecondCoreValues& values) {
    using AnkaCoreRoute::OptionKind;
    for (std::size_t i = 0; i < option_rows.size(); ++i) {
        const auto& option = AnkaCoreRoute::SecondCoreOptions[i];
        const int value = AnkaCoreRoute::ValidOption(option, values[i]);
        QWidget* input = option_rows[i].input;
        switch (option.kind) {
        case OptionKind::Toggle:
            static_cast<QCheckBox*>(input)->setChecked(value != 0);
            break;
        case OptionKind::Number:
            static_cast<QSpinBox*>(input)->setValue(value);
            break;
        case OptionKind::Choice: {
            auto* choice = static_cast<QComboBox*>(input);
            choice->setCurrentIndex(choice->findData(value));
            break;
        }
        }
    }
}

AnkaCoreRoute::SecondCoreValues ConfigureNx2Perf::CurrentSecondCoreOptions() const {
    using AnkaCoreRoute::OptionKind;
    AnkaCoreRoute::SecondCoreValues values = AnkaCoreRoute::SecondCoreDefaults();
    for (std::size_t i = 0; i < option_rows.size(); ++i) {
        const auto& option = AnkaCoreRoute::SecondCoreOptions[i];
        const QWidget* input = option_rows[i].input;
        switch (option.kind) {
        case OptionKind::Toggle:
            values[i] = static_cast<const QCheckBox*>(input)->isChecked() ? 1 : 0;
            break;
        case OptionKind::Number:
            values[i] = static_cast<const QSpinBox*>(input)->value();
            break;
        case OptionKind::Choice:
            values[i] = static_cast<const QComboBox*>(input)->currentData().toInt();
            break;
        }
        values[i] = AnkaCoreRoute::ValidOption(option, values[i]);
    }
    return values;
}

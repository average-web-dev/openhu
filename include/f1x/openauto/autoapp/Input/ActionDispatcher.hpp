/*
*  This file is part of OpenHU.
*  Copyright (C) 2026 Leon Strauß
*
*  OpenHU is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.
*
*  OpenHU is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with OpenHU. If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <functional>

#include <QHash>
#include <QObject>
#include <QStringList>
#include <f1x/openauto/autoapp/Audio/VolumeController.hpp>
#include <f1x/openauto/autoapp/Shell/ShellController.hpp>
#include <f1x/openauto/autoapp/Service/AudioFocusPolicy.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace input
{

// Named actions ("media.next", "volume.up", "call.answer", ...) from any input source: keyboard,
// steering wheel/CAN plugins and the API. Each action is executed as an Android Auto key or a shell
// function and is also published as "input.action" so plugins can implement their own actions
// (e.g. "camera.toggle").
class ActionDispatcher: public QObject
{
    Q_OBJECT

public:
    enum class Phase
    {
        Click,      // press + release
        Press,
        Release
    };

    ActionDispatcher(audio::VolumeController& volume, shell::ShellController& shell, QObject* parent = nullptr);

    // False if the action is unknown and no plugin may handle it either (empty name).
    Q_INVOKABLE bool trigger(const QString& action, const QString& phase = QStringLiteral("click"));
    bool trigger(const QString& action, Phase phase);

    // Built-in actions with a short description.
    static QVariantMap actions();

    // aa.reconnect: ends a wireless (TCP) session, true when there was one; otherwise the phones on USB are reset
    void setNetworkSessionEnder(std::function<bool()> ender) { networkSessionEnder_ = std::move(ender); }

    // Keyboard -> actions according to the key map (defaults + ~/.openhu/keymap.json).
    void installKeyboardHandler();
    QVariantMap keymap() const;

    // Key map editing (settings page). Keys use Qt names ("N", "Volume Up", "F5").
    Q_INVOKABLE QVariantList keymapList() const;
    Q_INVOKABLE void setKeyAction(const QString& key, const QString& action);
    Q_INVOKABLE void removeKey(const QString& key);
    Q_INVOKABLE void resetKeymap();
    // The next key press is reported as keyCaptured() instead of triggering an action.
    Q_INVOKABLE void captureKey(bool enabled);

signals:
    void keymapChanged();
    void keyCaptured(const QString& key);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    bool sendAaKey(const QString& action, Phase phase);
    // the media source that started last is a local one (plugin), not Android Auto
    bool localMediaSource() const;
    void loadKeymap();
    void saveOverrides() const;
    static QString keyName(int key);

    audio::VolumeController& volume_;
    shell::ShellController& shell_;
    service::AudioFocusPolicy mediaSources_;
    QHash<int, QString> keymap_;
    QVariantMap overrides_;  // differences to the defaults, "" removes a default key
    bool capturing_ = false;
    std::function<bool()> networkSessionEnder_;
};

}
}
}
}

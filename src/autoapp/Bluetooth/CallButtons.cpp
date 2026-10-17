/*
*  This file is part of openauto project.
*
*  openauto is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  openauto is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with openauto. If not, see <http://www.gnu.org/licenses/>.
*/

#include <f1x/openauto/autoapp/Bluetooth/CallButtons.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace bt
{

bool CallButtons::isRinging(const QString& state)
{
    return state == QLatin1String("incoming");
}

bool CallButtons::isInCall(const QString& state)
{
    return state == QLatin1String("in_call") || state == QLatin1String("on_hold")
           || state == QLatin1String("conferenced") || state == QLatin1String("muted");
}

void CallButtons::setCallState(const QString& state, long long nowMs)
{
    if(state == state_)
    {
        return;
    }
    if(isInCall(state) && !isInCall(state_))
    {
        callStartMs_ = nowMs;
    }
    state_ = state;
    changedMs_ = nowMs;
}

CallButtons::Decision CallButtons::decide(const QString& command, long long nowMs) const
{
    const bool play = command == QLatin1String("play");
    const bool playPause = command == QLatin1String("playpause");
    const bool pauseOrStop = command == QLatin1String("pause") || command == QLatin1String("stop");
    const bool settled = nowMs - changedMs_ >= cSettleMs;

    if(isRinging(state_))
    {
        if(!play && !playPause)
        {
            return {QString(), QStringLiteral("ringing: only play answers")};
        }
        if(!settled)
        {
            return {QString(), QStringLiteral("ringing for less than %1 ms").arg(cSettleMs)};
        }
        return {QStringLiteral("call.answer"), QStringLiteral("ringing")};
    }
    if(isInCall(state_))
    {
        if(!pauseOrStop && !playPause)
        {
            return {QString(), QStringLiteral("in a call: only pause/stop end it")};
        }
        if(nowMs - callStartMs_ < cEndAfterMs)
        {
            return {QString(), QStringLiteral("call started less than %1 ms ago (the radio switching to the phone)")
                                   .arg(cEndAfterMs)};
        }
        if(!settled)
        {
            return {QString(), QStringLiteral("call state changed less than %1 ms ago").arg(cSettleMs)};
        }
        return {QStringLiteral("call.end"), QStringLiteral("in a call")};
    }

    if(command == QLatin1String("next"))
    {
        return {QStringLiteral("media.next"), QString()};
    }
    if(command == QLatin1String("previous"))
    {
        return {QStringLiteral("media.previous"), QString()};
    }
    if(play)
    {
        return {QStringLiteral("media.play"), QString()};
    }
    if(pauseOrStop)
    {
        return {QStringLiteral("media.pause"), QString()};
    }
    return {QStringLiteral("media.play_pause"), QString()};
}

}
}
}
}

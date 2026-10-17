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

#pragma once

#include <QString>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace bt
{

// What a button of the car radio (AVRCP command) does, also during a call (#112).
// Car radios send pause/stop by themselves when they switch to their phone source at the start of a call (and
// play when they switch back); taken as the hang-up button that ended every call the user started. So:
// - ringing: play or play/pause answers, from cSettleMs after the ringing started; pause/stop do nothing (no
//   rejecting by accident)
// - in a call: pause, stop or play/pause hang up, only once the call has lasted cEndAfterMs and the call state
//   has not changed for cSettleMs (hold, mute); play, next and previous do nothing
// - else: media control (play, pause, play/pause, next, previous)
class CallButtons
{
public:
    static constexpr long long cSettleMs = 2000;
    static constexpr long long cEndAfterMs = 5000;

    // callState of "phone.status" (idle, incoming, in_call, on_hold, conferenced, muted, inactive, unknown); nowMs
    // from a monotonic clock
    void setCallState(const QString& state, long long nowMs);

    struct Decision
    {
        QString action;  // action to trigger (call.answer, call.end, media.*), empty: ignored
        QString reason;  // for the log
    };
    Decision decide(const QString& command, long long nowMs) const;

    static bool isRinging(const QString& state);
    static bool isInCall(const QString& state);

private:
    QString state_ = QStringLiteral("idle");
    long long changedMs_ = 0;    // last change of the call state
    long long callStartMs_ = 0;  // start of the current call (first in-call state)
};

}
}
}
}

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

// The car radio's buttons during a call: the radio's own pause at the start of a call must not hang up (#112)

#include <QtTest>
#include <f1x/openauto/autoapp/Bluetooth/CallButtons.hpp>

using f1x::openauto::autoapp::bt::CallButtons;

class TestCallButtons: public QObject
{
    Q_OBJECT

private slots:
    void mediaWithoutCall()
    {
        CallButtons buttons;
        QCOMPARE(buttons.decide("play", 0).action, QString("media.play"));
        QCOMPARE(buttons.decide("pause", 0).action, QString("media.pause"));
        QCOMPARE(buttons.decide("stop", 0).action, QString("media.pause"));
        QCOMPARE(buttons.decide("playpause", 0).action, QString("media.play_pause"));
        QCOMPARE(buttons.decide("next", 0).action, QString("media.next"));
        QCOMPARE(buttons.decide("previous", 0).action, QString("media.previous"));
    }

    // The report: a call the user started, the radio switches to its phone source and sends pause/stop
    void radiosOwnPauseDoesNotHangUp()
    {
        CallButtons buttons;
        buttons.setCallState("in_call", 10000);
        for(const char* command: {"pause", "stop", "playpause"})
        {
            const auto decision = buttons.decide(command, 10300);
            QVERIFY2(decision.action.isEmpty(), command);
            QVERIFY(!decision.reason.isEmpty());
        }
        QVERIFY(buttons.decide("pause", 10000 + CallButtons::cEndAfterMs - 1).action.isEmpty());
        // later: the user's button
        QCOMPARE(buttons.decide("pause", 10000 + CallButtons::cEndAfterMs).action, QString("call.end"));
        QCOMPARE(buttons.decide("playpause", 20000).action, QString("call.end"));
        QCOMPARE(buttons.decide("stop", 20000).action, QString("call.end"));
    }

    void playNextPreviousDoNothingInACall()
    {
        CallButtons buttons;
        buttons.setCallState("in_call", 0);
        for(const char* command: {"play", "next", "previous"})
        {
            QVERIFY2(buttons.decide(command, 60000).action.isEmpty(), command);
        }
    }

    // answered by the phone: the call counts from then on
    void answeredCallCountsFromTheAnswer()
    {
        CallButtons buttons;
        buttons.setCallState("incoming", 0);
        buttons.setCallState("in_call", 30000);
        QVERIFY(buttons.decide("pause", 31000).action.isEmpty());
        QCOMPARE(buttons.decide("pause", 35000).action, QString("call.end"));
    }

    void holdAndMuteKeepTheCallButSettle()
    {
        CallButtons buttons;
        buttons.setCallState("in_call", 0);
        buttons.setCallState("on_hold", 20000);
        // the radio reacts to the hold too
        QVERIFY(buttons.decide("pause", 20500).action.isEmpty());
        QCOMPARE(buttons.decide("pause", 20000 + CallButtons::cSettleMs).action, QString("call.end"));
        buttons.setCallState("muted", 30000);
        QCOMPARE(buttons.decide("stop", 33000).action, QString("call.end"));  // the call is older than 5 s
        buttons.setCallState("in_call", 40000);
        QCOMPARE(buttons.decide("stop", 42000).action, QString("call.end"));
    }

    void ringingAnswersOnlyWithPlay()
    {
        CallButtons buttons;
        buttons.setCallState("incoming", 1000);
        // the radio switching to its phone source
        QVERIFY(buttons.decide("play", 1200).action.isEmpty());
        QVERIFY(buttons.decide("pause", 5000).action.isEmpty());
        QVERIFY(buttons.decide("stop", 5000).action.isEmpty());
        QVERIFY(buttons.decide("next", 5000).action.isEmpty());
        QCOMPARE(buttons.decide("play", 1000 + CallButtons::cSettleMs).action, QString("call.answer"));
        QCOMPARE(buttons.decide("playpause", 5000).action, QString("call.answer"));
    }

    void afterTheCallMediaAgain()
    {
        CallButtons buttons;
        buttons.setCallState("in_call", 0);
        buttons.setCallState("idle", 60000);
        QCOMPARE(buttons.decide("play", 60100).action, QString("media.play"));
        // a new call starts its own time
        buttons.setCallState("in_call", 70000);
        QVERIFY(buttons.decide("pause", 71000).action.isEmpty());
    }

    void sameStateAgainChangesNothing()
    {
        CallButtons buttons;
        buttons.setCallState("in_call", 0);
        buttons.setCallState("in_call", 9000);  // phone.status arrives again with the duration
        QCOMPARE(buttons.decide("pause", 9100).action, QString("call.end"));
    }
};

QTEST_GUILESS_MAIN(TestCallButtons)
#include "test_callbuttons.moc"

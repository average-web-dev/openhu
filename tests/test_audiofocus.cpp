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

// When OpenHU takes the audio focus from the phone (#79)

#include <QtTest>
#include <f1x/openauto/autoapp/Service/AudioFocusPolicy.hpp>

using f1x::openauto::autoapp::service::AudioFocusPolicy;

namespace
{
QVariantMap status(const QString& source, const QString& state)
{
    return {{"source", source}, {"state", state}};
}
}

class TestAudioFocus: public QObject
{
    Q_OBJECT

private slots:
    void radioWhileSpotifyPlays()
    {
        // tested with a Fairphone 4: radio chosen in Android Auto while Spotify plays
        AudioFocusPolicy policy(status("androidauto", "playing"));
        QVERIFY(policy.update(status("radio", "playing")));
        // the phone still reports "playing" until the focus loss arrived, the radio keeps sending updates (RDS)
        QVERIFY(!policy.update(status("androidauto", "playing")));
        QVERIFY(!policy.update(status("radio", "playing")));
        QVERIFY(!policy.update(status("androidauto", "paused")));
        QVERIFY(!policy.update(status("radio", "playing")));
    }

    void spotifyTakesOverAndRadioComesBack()
    {
        AudioFocusPolicy policy;
        QVERIFY(policy.update(status("radio", "playing")));
        // play in Spotify: Android Auto starts, the radio stops by itself (without publishing "stopped")
        QVERIFY(!policy.update(status("androidauto", "paused")));
        QVERIFY(!policy.update(status("androidauto", "playing")));
        // the radio again: a new start, the focus goes to the head unit again
        QVERIFY(policy.update(status("radio", "playing")));
    }

    void mediaKeysOwner()
    {
        AudioFocusPolicy policy(status("androidauto", "paused"));
        QCOMPARE(policy.owner(), QString("androidauto"));
        policy.update(status("radio", "playing"));
        QCOMPARE(policy.owner(), QString("radio"));
        // stopped radio keeps the keys (play resumes it), late phone reports do not take them
        policy.update(status("androidauto", "paused"));
        policy.update(status("radio", "stopped"));
        QCOMPARE(policy.owner(), QString("radio"));
        policy.update(status("androidauto", "playing"));
        QCOMPARE(policy.owner(), QString("androidauto"));
    }

    void stopAndPlayAgain()
    {
        AudioFocusPolicy policy;
        QVERIFY(policy.update(status("radio", "playing")));
        QVERIFY(!policy.update(status("radio", "stopped")));
        QVERIFY(policy.update(status("radio", "playing")));
        // another local source
        QVERIFY(policy.update(status("usb", "playing")));
    }

    void connectWhileRadioPlays()
    {
        // a connection starts while the radio already plays: no change, no focus loss
        AudioFocusPolicy policy(status("radio", "playing"));
        QVERIFY(!policy.update(status("radio", "playing")));
        QVERIFY(!policy.update(QVariantMap()));
        QVERIFY(!policy.update(status("androidauto", "stopped")));
    }
};

QTEST_GUILESS_MAIN(TestAudioFocus)
#include "test_audiofocus.moc"

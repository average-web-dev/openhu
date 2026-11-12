#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <f1x/openauto/autoapp/Core/EventBus.hpp>
#include <f1x/openauto/autoapp/Input/ActionDispatcher.hpp>
#include "TestHome.hpp"

using f1x::openauto::autoapp::audio::VolumeController;
using f1x::openauto::autoapp::core::EventBus;
using f1x::openauto::autoapp::input::ActionDispatcher;
using f1x::openauto::autoapp::shell::ShellController;

namespace
{

QVariantMap readJson(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object().toVariantMap() : QVariantMap();
}

}

class TestActions: public QObject
{
    Q_OBJECT

private slots:
    void shellActionsNavigate()
    {
        TestHome home;
        VolumeController volume;
        ShellController shell;
        ActionDispatcher actions(volume, shell);
        QSignalSpy navigate(&shell, &ShellController::navigateRequested);

        QVERIFY(actions.trigger("ui.settings"));
        QVERIFY(actions.trigger("ui.home", "press"));
        // once per click/press: the release does nothing
        QVERIFY(actions.trigger("ui.home", "release"));
        // without Android Auto on screen "back" goes to the home page
        QVERIFY(actions.trigger("ui.back"));

        QCOMPARE(navigate.count(), 3);
        QCOMPARE(navigate.at(0).at(0).toString(), QString("settings"));
        QCOMPARE(navigate.at(1).at(0).toString(), QString("home"));
        QCOMPARE(navigate.at(2).at(0).toString(), QString("home"));
    }

    // aa.reconnect: a wireless session is ended (the phone comes back by itself), no USB reset then
    void reconnectEndsWirelessSession()
    {
        TestHome home;
        VolumeController volume;
        ShellController shell;
        ActionDispatcher actions(volume, shell);
        QSignalSpy toasts(&shell, &ShellController::toastRequested);
        int ended = 0;
        bool wireless = true;
        actions.setNetworkSessionEnder([&]() {
            ++ended;
            return wireless;
        });

        QVERIFY(actions.trigger("aa.reconnect", "press"));
        QVERIFY(actions.trigger("aa.reconnect", "release"));
        QCOMPARE(ended, 1);
        QCOMPARE(toasts.count(), 1);
        QCOMPARE(toasts.at(0).at(0).toString(), qtTrId("actions.reconnecting_phone"));

        // no wireless session: the phones on USB are reset (none in the test)
        wireless = false;
        QVERIFY(actions.trigger("aa.reconnect"));
        QCOMPARE(ended, 2);
        QCOMPARE(toasts.count(), 2);
    }

    void everyActionIsPublished()
    {
        TestHome home;
        VolumeController volume;
        ShellController shell;
        ActionDispatcher actions(volume, shell);

        QVariantMap received;
        auto connection = connect(&EventBus::instance(), &EventBus::event, this,
                                  [&received](const QString& topic, const QVariantMap& data) {
                                      if(topic == "input.action")
                                      {
                                          received = data;
                                      }
                                  });

        // unknown actions are left to plugins (camera.toggle)
        QVERIFY(actions.trigger("camera.toggle", "press"));
        QCOMPARE(received.value("action").toString(), QString("camera.toggle"));
        QCOMPARE(received.value("phase").toString(), QString("press"));
        // one-shot: a reconnecting plugin must not get the last action again
        QVERIFY(EventBus::instance().last("input.action").isEmpty());
        disconnect(connection);
    }

    void invalidActions()
    {
        TestHome home;
        VolumeController volume;
        ShellController shell;
        ActionDispatcher actions(volume, shell);

        QVERIFY(!actions.trigger(QString()));
        // Android Auto keys fail without a connected phone
        QVERIFY(!actions.trigger("media.next"));
        QVERIFY(ActionDispatcher::actions().contains("media.next"));
    }

    void mediaKeysGoToTheCurrentSource()
    {
        TestHome home;
        VolumeController volume;
        ShellController shell;
        ActionDispatcher actions(volume, shell);
        QVariantMap received;
        auto connection = connect(&EventBus::instance(), &EventBus::event, this,
                                  [&received](const QString& topic, const QVariantMap& data) {
            if(topic == "input.action")
            {
                received = data;
            }
        });

        // a local source (radio plugin): handled by it through input.action, not sent to the phone
        EventBus::instance().publish("media.status", {{"state", "playing"}, {"source", "radio"}});
        QVERIFY(actions.trigger("media.next"));
        QTRY_COMPARE(received.value("action").toString(), QString("media.next"));
        QVERIFY(actions.trigger("media.play_pause"));
        // also after the local source stopped: play resumes it
        EventBus::instance().publish("media.status", {{"state", "stopped"}, {"source", "radio"}});
        QVERIFY(actions.trigger("media.play"));
        // other Android Auto keys still go to the phone (none connected: fails)
        QVERIFY(!actions.trigger("voice"));

        // the radio plays again, the phone still reports "paused" afterwards (focus loss): the keys stay with the radio
        EventBus::instance().publish("media.status", {{"state", "playing"}, {"source", "radio"}});
        EventBus::instance().publish("media.status", {{"state", "paused"}, {"source", "androidauto"}});
        QTRY_VERIFY(actions.trigger("media.next"));

        // Android Auto music starts: the phone gets the key (none connected: fails)
        EventBus::instance().publish("media.status", {{"state", "playing"}, {"source", "androidauto"}});
        QTRY_VERIFY(!actions.trigger("media.next"));
        disconnect(connection);
    }

    void defaultKeymap()
    {
        TestHome home;
        VolumeController volume;
        ShellController shell;
        ActionDispatcher actions(volume, shell);

        const QVariantMap keymap = actions.keymap();
        QCOMPARE(keymap.value("N").toString(), QString("media.next"));
        QCOMPARE(keymap.value("Volume Up").toString(), QString("volume.up"));
        QCOMPARE(keymap.value("Esc").toString(), QString("ui.back"));
        QVERIFY(!QFile::exists(home.path(".openhu/keymap.json")));
    }

    void editKeymap()
    {
        TestHome home;
        VolumeController volume;
        ShellController shell;
        {
            ActionDispatcher actions(volume, shell);
            QSignalSpy changed(&actions, &ActionDispatcher::keymapChanged);

            actions.setKeyAction("F5", "camera.toggle");
            actions.setKeyAction("N", "media.previous");
            actions.removeKey("H");
            QCOMPARE(changed.count(), 3);

            const QVariantMap overrides = readJson(home.path(".openhu/keymap.json"));
            QCOMPARE(overrides.value("F5").toString(), QString("camera.toggle"));
            QCOMPARE(overrides.value("N").toString(), QString("media.previous"));
            QCOMPARE(overrides.value("H").toString(), QString());

            bool customFound = false;
            for(const auto& entry: actions.keymapList())
            {
                const QVariantMap map = entry.toMap();
                if(map.value("key") == "F5")
                {
                    customFound = map.value("custom").toBool();
                }
            }
            QVERIFY(customFound);
        }

        // a new dispatcher (next start) reads the overrides
        ActionDispatcher actions(volume, shell);
        QVariantMap keymap = actions.keymap();
        QCOMPARE(keymap.value("F5").toString(), QString("camera.toggle"));
        QCOMPARE(keymap.value("N").toString(), QString("media.previous"));
        QVERIFY(!keymap.contains("H"));

        // back to the default value: no override left for that key
        actions.setKeyAction("N", "media.next");
        QVERIFY(!readJson(home.path(".openhu/keymap.json")).contains("N"));

        actions.resetKeymap();
        QVERIFY(!QFile::exists(home.path(".openhu/keymap.json")));
        QCOMPARE(actions.keymap().value("H").toString(), QString("aa.home"));
        QVERIFY(!actions.keymap().contains("F5"));
    }

    void keymapFileNamesAreNormalized()
    {
        TestHome home;
        QDir().mkpath(home.path(".openhu"));
        QFile file(home.path(".openhu/keymap.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"volume up": "media.next", "f6": "camera.toggle"})");
        file.close();

        VolumeController volume;
        ShellController shell;
        ActionDispatcher actions(volume, shell);
        QCOMPARE(actions.keymap().value("Volume Up").toString(), QString("media.next"));
        QCOMPARE(actions.keymap().value("F6").toString(), QString("camera.toggle"));
    }
};

QTEST_MAIN(TestActions)
#include "test_actions.moc"

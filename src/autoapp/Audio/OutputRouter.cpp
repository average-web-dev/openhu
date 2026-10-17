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

#include <memory>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <f1x/openauto/autoapp/Audio/OutputRouter.hpp>
#include <f1x/openauto/autoapp/Core/ChildProcesses.hpp>
#include <f1x/openauto/Common/Log.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace audio
{

namespace
{

// E8:78:29:C0:E5:2D and E8_78_29_C0_E5_2D compare equal
QString normalized(QString name)
{
    return name.toUpper().replace(':', '_');
}

bool isA2dp(const QString& profile)
{
    return profile.startsWith(QLatin1String("a2dp"), Qt::CaseInsensitive);
}

std::optional<quint32> indexOf(const QList<Sink>& sinks, const QString& name)
{
    for(const Sink& sink: sinks)
    {
        if(sink.name == name)
        {
            return sink.index;
        }
    }
    return std::nullopt;
}

// Where the streams go back to when the Bluetooth sink "from" stops being the output
struct Back
{
    QString target;
    bool setDefault = false;
};
Back backTo(const QList<Sink>& sinks, const QString& from, const QString& previous, const QString& current)
{
    // the Bluetooth sink gone: the default is a fallback of the audio server, not a choice
    const bool fromGone = !indexOf(sinks, from);
    if(current == from || fromGone)
    {
        if(!previous.isEmpty() && indexOf(sinks, previous))
        {
            return {previous, current != previous};
        }
        // the audio server had switched to the Bluetooth sink before OpenHU knew the output before: an output
        // that is not Bluetooth
        if(current == from)
        {
            for(const Sink& sink: sinks)
            {
                if(!sink.name.startsWith(QLatin1String("bluez")))
                {
                    return {sink.name, true};
                }
            }
        }
    }
    // chosen meanwhile (or the output before is gone): the streams go to the default
    if(!current.isEmpty() && current != from && indexOf(sinks, current))
    {
        return {current, false};
    }
    return {};
}

QByteArray runBlocking(const QStringList& arguments)
{
    QProcess process;
    core::startChild(process, "pactl", arguments);
    process.waitForFinished(2000);
    return process.readAllStandardOutput();
}

}

bool BluetoothCard::needsA2dp() const
{
    return !a2dpProfile.isEmpty() && !isA2dp(activeProfile);
}

std::optional<BluetoothCard> findBluetoothCard(const QByteArray& cardsJson, const QString& address)
{
    const QJsonDocument document = QJsonDocument::fromJson(cardsJson);
    const QString id = normalized(address);
    for(const QJsonValue& value: document.array())
    {
        const QJsonObject object = value.toObject();
        const QString name = object.value("name").toString();
        if(!name.startsWith(QLatin1String("bluez")) || id.isEmpty() || !normalized(name).contains(id))
        {
            continue;
        }
        BluetoothCard card;
        card.name = name;
        card.activeProfile = object.value("active_profile").toString();
        // "profiles": {"a2dp-sink": {"sinks": 1, "sources": 0, "priority": 18, "available": true}, ...}
        // PipeWire: a2dp-sink, a2dp-sink-sbc, a2dp-sink-aac, ...; PulseAudio: a2dp_sink
        double best = -1;
        const QJsonObject profiles = object.value("profiles").toObject();
        for(auto it = profiles.begin(); it != profiles.end(); ++it)
        {
            const QJsonObject profile = it.value().toObject();
            if(!isA2dp(it.key()) || profile.value("sinks").toInt() < 1 || !profile.value("available").toBool(true))
            {
                continue;
            }
            const double priority = profile.value("priority").toDouble();
            if(priority > best)
            {
                best = priority;
                card.a2dpProfile = it.key();
            }
        }
        return card;
    }
    return std::nullopt;
}

QList<Sink> parseShortSinks(const QByteArray& output)
{
    QList<Sink> sinks;
    for(const QString& line: QString::fromUtf8(output).split('\n', Qt::SkipEmptyParts))
    {
        bool ok = false;
        Sink sink;
        sink.index = line.section('\t', 0, 0).toUInt(&ok);
        sink.name = line.section('\t', 1, 1);
        if(ok && !sink.name.isEmpty())
        {
            sinks.append(sink);
        }
    }
    return sinks;
}

QString findBluetoothSink(const QList<Sink>& sinks, const QString& address)
{
    const QString id = normalized(address);
    for(const Sink& sink: sinks)
    {
        if(!id.isEmpty() && sink.name.startsWith(QLatin1String("bluez")) && normalized(sink.name).contains(id))
        {
            return sink.name;
        }
    }
    return QString();
}

bool followsOutput(const SinkInput& input)
{
    if(input.module)
    {
        return false;
    }
    return input.binary != QLatin1String("pipewire") && input.binary != QLatin1String("pipewire-pulse")
           && input.binary != QLatin1String("wireplumber");
}

OutputRouter::OutputRouter(QObject* parent)
    : QObject(parent)
    , ownPid_(QCoreApplication::applicationPid())
    , ownBinary_(QFileInfo(QCoreApplication::applicationFilePath()).fileName())
{
}

void OutputRouter::setOwnProcess(qint64 pid, const QString& binary)
{
    ownPid_ = pid;
    ownBinary_ = binary;
}

QString OutputRouter::sink() const
{
    return sink_;
}

bool OutputRouter::busy() const
{
    return busy_;
}

void OutputRouter::setFollowDelay(int ms)
{
    followDelayMs_ = ms;
}

void OutputRouter::setStreamFilter(std::function<bool(const SinkInput&)> filter)
{
    streamFilter_ = std::move(filter);
}

void OutputRouter::pactl(const QList<QStringList>& commands, std::function<void(const Outputs&)> done)
{
    auto outputs = std::make_shared<Outputs>(commands.size());
    auto left = std::make_shared<int>(int(commands.size()));
    const int generation = generation_;
    for(int i = 0; i < commands.size(); ++i)
    {
        auto* process = new QProcess(this);
        const auto finished = [this, process, outputs, left, i, generation, done, command = commands[i]]() {
            (*outputs)[i] = process->readAllStandardOutput();
            if(process->exitStatus() != QProcess::NormalExit || process->exitCode() != 0)
            {
                OPENAUTO_LOG(debug) << "[OutputRouter] pactl " << command.join(' ').toStdString() << " failed: "
                                    << process->readAllStandardError().trimmed().toStdString();
            }
            process->deleteLater();
            if(--*left == 0 && generation == generation_)
            {
                done(*outputs);
            }
        };
        connect(process, &QProcess::finished, this, finished);
        connect(process, &QProcess::errorOccurred, this, [finished](QProcess::ProcessError error) {
            if(error == QProcess::FailedToStart)
            {
                finished();
            }
        });
        core::startChild(*process, "pactl", commands[i]);
    }
    if(commands.isEmpty())
    {
        done(*outputs);
    }
}

void OutputRouter::rememberDefault()
{
    this->pactl({{"get-default-sink"}}, [this](const Outputs& outputs) {
        const QString current = QString::fromUtf8(outputs[0]).trimmed();
        if(sink_.isEmpty() && !busy_ && !current.isEmpty() && !current.startsWith(QLatin1String("bluez")))
        {
            previous_ = current;
        }
    });
}

void OutputRouter::route(const QString& address)
{
    if(busy_)
    {
        return;
    }
    busy_ = true;
    this->pactl({{"-f", "json", "list", "cards"}, {"list", "short", "sinks"}},
                [this, address](const Outputs& outputs) { this->onListed(address, outputs); });
}

void OutputRouter::onListed(const QString& address, const Outputs& outputs)
{
    const auto card = findBluetoothCard(outputs[0], address);
    const QList<Sink> sinks = parseShortSinks(outputs[1]);
    const QString sink = findBluetoothSink(sinks, address);
    // what the audio server shows of the device, logged when it changes (a radio that never offers A2DP)
    const QString seen = (card ? QString("card %1, profile %2, A2DP profile %3")
                                     .arg(card->name, card->activeProfile,
                                          card->a2dpProfile.isEmpty() ? QString("none (yet)") : card->a2dpProfile)
                               : QString("no card"))
                         + ", sink " + (sink.isEmpty() ? QString("none") : sink);
    if(seen != seen_)
    {
        seen_ = seen;
        OPENAUTO_LOG(info) << "[OutputRouter] " << address.toStdString() << ": " << seen.toStdString();
    }
    // a few tries; a card that refuses A2DP plays through the sink it has
    if(card && card->needsA2dp() && profileSwitches_ < 3)
    {
        ++profileSwitches_;
        OPENAUTO_LOG(info) << "[OutputRouter] " << card->name.toStdString() << ": profile "
                           << card->activeProfile.toStdString() << " -> " << card->a2dpProfile.toStdString();
        // the A2DP sink appears a moment later: the next route()
        this->pactl({{"set-card-profile", card->name, card->a2dpProfile}}, [this](const Outputs&) { this->finish(); });
        return;
    }
    // a car radio connected hands-free only: its A2DP may come (CarAudio asks for it), the headset sink is the
    // last resort (mono, telephone quality)
    if(card && !isA2dp(card->activeProfile) && card->a2dpProfile.isEmpty() && headsetWaits_ < 15)
    {
        ++headsetWaits_;
        this->finish();
        return;
    }
    if(sink.isEmpty() || sink == sink_)
    {
        if(!sink.isEmpty())
        {
            emit routed(sink);  // already the output
        }
        this->finish();
        return;
    }
    this->switchTo(sink, sinks);
}

void OutputRouter::switchTo(const QString& sink, const QList<Sink>& sinks)
{
    this->pactl({{"get-default-sink"}}, [this, sink, sinks](const Outputs& outputs) {
        const QString current = QString::fromUtf8(outputs[0]).trimmed();
        if(!current.startsWith(QLatin1String("bluez")) && !current.isEmpty())
        {
            previous_ = current;
        }
        sink_ = sink;
        profileSwitches_ = 0;
        headsetWaits_ = 0;
        OPENAUTO_LOG(info) << "[OutputRouter] output " << sink.toStdString() << " (before: " << current.toStdString()
                           << ")";
        this->pactl({{"set-default-sink", sink}}, [this, sink, sinks, current](const Outputs&) {
            // what follows the default moves by itself; the rest (explicit sink: Qt's outputs) after a moment
            const int generation = generation_;
            QTimer::singleShot(followDelayMs_, this, [this, sink, sinks, current, generation]() {
                if(generation != generation_)
                {
                    return;
                }
                // the audio server may have switched to the Bluetooth sink itself: the streams are on the one before
                const QString from = current.startsWith(QLatin1String("bluez")) ? previous_ : current;
                this->moveStreams(sinks, from, {}, sink, true, [this, sink]() {
                    emit routed(sink);
                    this->finish();
                });
            });
        });
    });
}

QList<quint32> OutputRouter::streamsToMove(const QList<SinkInput>& inputs, const QList<Sink>& sinks,
                                           const QString& from, const QSet<quint32>& also, const QString& to) const
{
    const auto fromIndex = indexOf(sinks, from);
    const auto toIndex = indexOf(sinks, to);
    QList<quint32> result;
    for(const SinkInput& input: inputs)
    {
        if(!followsOutput(input) || (streamFilter_ && !streamFilter_(input)) || (toIndex && input.sink == *toIndex))
        {
            continue;
        }
        // OpenHU's own streams (Android Auto) always go to the output, wherever they play (the audio server may have
        // made the Bluetooth sink the default before OpenHU looked, then the output before is not known)
        const bool own = input.pid != 0 && input.pid == ownPid_ && (input.binary.isEmpty() || input.binary == ownBinary_);
        if(own || (fromIndex && input.sink == *fromIndex) || also.contains(input.index))
        {
            result.append(input.index);
        }
    }
    return result;
}

void OutputRouter::moveStreams(const QList<Sink>& sinks, const QString& from, const QSet<quint32>& also,
                               const QString& to, bool remember, std::function<void()> done)
{
    this->pactl({{"-f", "json", "list", "sink-inputs"}, {"-f", "json", "list", "clients"}},
                [this, sinks, from, also, to, remember, done](const Outputs& outputs) {
                    const QList<quint32> streams =
                        this->streamsToMove(parseSinkInputs(outputs[0], outputs[1]), sinks, from, also, to);
                    QList<QStringList> commands;
                    for(const quint32 index: streams)
                    {
                        commands.append({"move-sink-input", QString::number(index), to});
                        if(remember)
                        {
                            moved_.insert(index);
                        }
                    }
                    if(!streams.isEmpty())
                    {
                        OPENAUTO_LOG(info) << "[OutputRouter] " << streams.size() << " stream(s) to " << to.toStdString();
                    }
                    this->pactl(commands, [done](const Outputs&) { done(); });
                });
}

void OutputRouter::restore()
{
    if(busy_)
    {
        restorePending_ = true;
        return;
    }
    if(sink_.isEmpty())
    {
        return;
    }
    busy_ = true;
    this->doRestore();
}

void OutputRouter::doRestore()
{
    const QString from = sink_;
    const QString previous = previous_;
    const QSet<quint32> moved = moved_;
    sink_.clear();
    moved_.clear();
    profileSwitches_ = 0;
    headsetWaits_ = 0;
    seen_.clear();
    this->pactl({{"get-default-sink"}, {"list", "short", "sinks"}}, [this, from, previous, moved](const Outputs& outputs) {
        const QString current = QString::fromUtf8(outputs[0]).trimmed();
        const QList<Sink> sinks = parseShortSinks(outputs[1]);
        const Back back = backTo(sinks, from, previous, current);
        if(back.target.isEmpty())
        {
            OPENAUTO_LOG(info) << "[OutputRouter] output before (" << previous.toStdString() << ") is gone";
            emit restored();
            this->finish();
            return;
        }
        OPENAUTO_LOG(info) << "[OutputRouter] output back to " << back.target.toStdString();
        const QList<QStringList> commands =
            back.setDefault ? QList<QStringList>{{"set-default-sink", back.target}} : QList<QStringList>{};
        const QString to = back.target;
        this->pactl(commands, [this, sinks, from, moved, to](const Outputs&) {
            const int generation = generation_;
            QTimer::singleShot(followDelayMs_, this, [this, sinks, from, moved, to, generation]() {
                if(generation != generation_)
                {
                    return;
                }
                this->moveStreams(sinks, from, moved, to, false, [this]() {
                    emit restored();
                    this->finish();
                });
            });
        });
    });
}

void OutputRouter::finish()
{
    busy_ = false;
    if(restorePending_)
    {
        restorePending_ = false;
        this->restore();
    }
}

void OutputRouter::restoreNow()
{
    ++generation_;  // nothing that still runs continues
    busy_ = false;
    restorePending_ = false;
    if(sink_.isEmpty())
    {
        return;
    }
    const QString from = sink_;
    const QSet<quint32> moved = moved_;
    sink_.clear();
    moved_.clear();
    const QList<Sink> sinks = parseShortSinks(runBlocking({"list", "short", "sinks"}));
    const Back back = backTo(sinks, from, previous_, QString::fromUtf8(runBlocking({"get-default-sink"})).trimmed());
    if(back.target.isEmpty())
    {
        return;
    }
    const QString to = back.target;
    if(back.setDefault)
    {
        runBlocking({"set-default-sink", to});
    }
    const QList<quint32> streams = this->streamsToMove(
        parseSinkInputs(runBlocking({"-f", "json", "list", "sink-inputs"}), runBlocking({"-f", "json", "list", "clients"})),
        sinks, from, moved, to);
    for(const quint32 index: streams)
    {
        runBlocking({"move-sink-input", QString::number(index), to});
    }
    OPENAUTO_LOG(info) << "[OutputRouter] output back to " << to.toStdString();
}

}
}
}
}

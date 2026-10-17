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

#include <algorithm>
#include <cmath>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <f1x/openauto/autoapp/Audio/SinkInputs.hpp>

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

// pactl writes numbers of properties and indices as strings or numbers depending on the field
qint64 number(const QJsonValue& value)
{
    if(value.isDouble())
    {
        return qint64(value.toDouble());
    }
    bool ok = false;
    const qint64 result = value.toString().toLongLong(&ok);
    return ok ? result : 0;
}

QJsonArray array(const QByteArray& json)
{
    const QJsonDocument document = QJsonDocument::fromJson(json);
    return document.isArray() ? document.array() : QJsonArray();
}

struct Client
{
    qint64 pid = 0;
    QString binary;
};

}

QList<SinkInput> parseSinkInputs(const QByteArray& sinkInputsJson, const QByteArray& clientsJson)
{
    QHash<qint64, Client> clients;
    for(const QJsonValue& value: array(clientsJson))
    {
        const QJsonObject client = value.toObject();
        const QJsonObject properties = client.value("properties").toObject();
        clients.insert(number(client.value("index")),
                       Client{number(properties.value("application.process.id")),
                              properties.value("application.process.binary").toString()});
    }

    QList<SinkInput> inputs;
    for(const QJsonValue& value: array(sinkInputsJson))
    {
        const QJsonObject object = value.toObject();
        const QJsonObject properties = object.value("properties").toObject();
        SinkInput input;
        input.index = quint32(number(object.value("index")));
        input.sink = quint32(number(object.value("sink")));
        // "volume": {"front-left": {"value": 65536, "value_percent": "100%", ...}, ...}, in channel map order
        const QJsonObject volume = object.value("volume").toObject();
        const QStringList channels = object.value("channel_map").toString().split(',', Qt::SkipEmptyParts);
        for(const QString& channel: channels)
        {
            if(volume.contains(channel))
            {
                input.volumes.append(quint32(number(volume.value(channel).toObject().value("value"))));
            }
        }
        if(input.volumes.size() != volume.size())
        {
            // channel names repeat (aux channels) or no map: the object's order then
            input.volumes.clear();
            for(auto it = volume.begin(); it != volume.end(); ++it)
            {
                input.volumes.append(quint32(number(it.value().toObject().value("value"))));
            }
        }
        input.pid = number(properties.value("application.process.id"));
        input.binary = properties.value("application.process.binary").toString();
        const auto client = clients.constFind(number(object.value("client")));
        if(client != clients.constEnd())
        {
            if(input.pid == 0)
            {
                input.pid = client->pid;
            }
            if(input.binary.isEmpty())
            {
                input.binary = client->binary;
            }
        }
        input.appName = properties.value("application.name").toString();
        input.mediaName = properties.value("media.name").toString();
        const QJsonValue owner = object.value("owner_module");
        input.module = !owner.isNull() && !owner.isUndefined() && owner.toString() != QLatin1String("n/a");
        inputs.append(input);
    }
    return inputs;
}

bool isLocalSource(const SinkInput& input, qint64 ownPid, const QString& ownBinary)
{
    if(input.module || input.volumes.isEmpty())
    {
        return false;
    }
    // filter chains and loopbacks of PipeWire itself
    if(input.binary == QLatin1String("pipewire") || input.binary == QLatin1String("pipewire-pulse")
       || input.binary == QLatin1String("wireplumber"))
    {
        return false;
    }
    // OpenHU's own streams; the binary as well, the pid alone could be another process in another pid namespace
    // (OpenHU in a container next to the host's applications)
    if(input.pid != 0 && input.pid == ownPid && (input.binary.isEmpty() || ownBinary.isEmpty() || input.binary == ownBinary))
    {
        return false;
    }
    return true;
}

QList<quint32> scaledVolumes(const QList<quint32>& volumes, double loudness)
{
    const double factor = std::cbrt(std::clamp(loudness, 0.0, 1.0));
    QList<quint32> scaled;
    scaled.reserve(volumes.size());
    for(const quint32 volume: volumes)
    {
        scaled.append(quint32(std::lround(volume * factor)));
    }
    return scaled;
}

}
}
}
}

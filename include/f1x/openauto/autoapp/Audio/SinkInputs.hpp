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

#include <QByteArray>
#include <QList>
#include <QString>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace audio
{

// One playback stream of the audio server ("pactl -f json list sink-inputs", PulseAudio >= 16 / pipewire-pulse)
struct SinkInput
{
    quint32 index = 0;
    quint32 sink = 0;      // index of the sink it plays on
    // raw volume per channel (65536 = 100 %)
    QList<quint32> volumes;
    // application.process.id of the stream, else of its client; 0 if unknown
    qint64 pid = 0;
    QString binary;        // application.process.binary
    QString appName;       // application.name
    QString mediaName;     // media.name
    bool module = false;   // created by a module of the audio server (loopback, filter), not an application

    // key that survives the stream (a new stream of the same application after a restart)
    QString key() const { return appName + QLatin1Char('\n') + mediaName; }
};

// Streams from "pactl -f json list sink-inputs"; the clients ("pactl -f json list clients") give the process of
// streams that do not carry it themselves (native PipeWire clients). Empty for output that is no JSON list.
QList<SinkInput> parseSinkInputs(const QByteArray& sinkInputsJson, const QByteArray& clientsJson = QByteArray());

// Streams that ducking lowers: everything except OpenHU's own (its Android Auto audio outputs, same process) and the
// audio server's internal streams (loopbacks and filters, they carry Android Auto's sound too).
bool isLocalSource(const SinkInput& input, qint64 ownPid, const QString& ownBinary);

// Volumes scaled to a loudness factor (linear amplitude 0..1, like Android's ducking): PulseAudio/PipeWire volumes
// are cubic, 20 % loudness is 58 % on the volume scale (cbrt(0.2) = 0.585, -14 dB).
QList<quint32> scaledVolumes(const QList<quint32>& volumes, double loudness);

}
}
}
}

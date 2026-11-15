/*
  Copyright (C) 2026 Kieren (DCC file transfers)

  You may use this file under the terms of BSD license as follows:

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR
  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "dcctransfer.h"

#include <QDir>
#include <QFileInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>

// Keep at most this much queued in the socket, and read the file in chunks.
static const qint64 WriteBufferSize = 256 * 1024;
static const qint64 ChunkSize = 64 * 1024;
// Report progress to the UI at most this often.
static const int ProgressMsecs = 250;

DccTransfer::DccTransfer(int id, Direction direction, QObject* parent) : QObject(parent),
    m_id(id), m_direction(direction), m_status(direction == Incoming ? Offered : Waiting),
    m_port(0), m_size(0), m_transferred(0), m_sent(0), m_acked(0), m_draining(false),
    m_socket(0), m_server(0), m_elapsedAtEnd(-1),
    m_connectSecs(60), m_waitSecs(300), m_stallSecs(120), m_drainSecs(30)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(onTimeout()));
    m_progressTimer.setSingleShot(true);
    m_progressTimer.setInterval(ProgressMsecs);
    connect(&m_progressTimer, SIGNAL(timeout()), this, SLOT(emitProgress()));
}

DccTransfer::~DccTransfer()
{
    if (isActive())
        cleanup(true);
}

qint64 DccTransfer::speed() const
{
    qint64 msecs = m_elapsedAtEnd >= 0 ? m_elapsedAtEnd : (m_elapsed.isValid() ? m_elapsed.elapsed() : 0);
    if (msecs <= 0)
        return 0;
    return m_transferred * 1000 / msecs;
}

bool DccTransfer::isActive() const
{
    return m_status == Waiting || m_status == Connecting || m_status == Transferring;
}

bool DccTransfer::isFinished() const
{
    return m_status == Done || m_status == Failed || m_status == Cancelled || m_status == Declined;
}

void DccTransfer::setTimeouts(int connectSecs, int waitSecs, int stallSecs, int drainSecs)
{
    m_connectSecs = connectSecs;
    m_waitSecs = waitSecs;
    m_stallSecs = stallSecs;
    m_drainSecs = drainSecs;
}

QString DccTransfer::safeFileName(const QString& offered)
{
    QString name = offered;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = name.section(QLatin1Char('/'), -1);
    QString clean;
    foreach (const QChar& c, name) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            continue;
        clean += c;
    }
    clean = clean.trimmed();
    while (clean.startsWith(QLatin1Char('.')))
        clean.remove(0, 1);
    if (clean.length() > 200) {
        QFileInfo info(clean);
        QString suffix = info.suffix().left(20);
        clean = clean.left(200 - suffix.length() - 1);
        if (!suffix.isEmpty())
            clean += QLatin1Char('.') + suffix;
    }
    if (clean.isEmpty())
        clean = QStringLiteral("file");
    return clean;
}

QString DccTransfer::uniquePath(const QString& directory, const QString& fileName)
{
    QDir dir(directory);
    QString candidate = dir.filePath(fileName);
    if (!QFileInfo::exists(candidate) && !QFileInfo::exists(candidate + QStringLiteral(".part")))
        return candidate;

    QFileInfo info(fileName);
    QString base = info.completeBaseName();
    QString suffix = info.suffix();
    if (base.isEmpty()) {
        base = fileName;
        suffix.clear();
    }
    for (int n = 1; ; ++n) {
        QString name = QStringLiteral("%1 (%2)").arg(base).arg(n);
        if (!suffix.isEmpty())
            name += QLatin1Char('.') + suffix;
        candidate = dir.filePath(name);
        if (!QFileInfo::exists(candidate) && !QFileInfo::exists(candidate + QStringLiteral(".part")))
            return candidate;
    }
}

QString DccTransfer::offerFileName(const QString& path)
{
    // Most clients and bouncers split the offer on spaces, so avoid them.
    QString name = QFileInfo(path).fileName();
    name.remove(QLatin1Char('"'));
    QString clean;
    foreach (const QChar& c, name)
        clean += c.isSpace() ? QChar(QLatin1Char('_')) : c;
    return clean.isEmpty() ? QStringLiteral("file") : clean;
}

void DccTransfer::setOffer(const QString& nick, const QString& fileName,
                           const QHostAddress& address, quint16 port, qint64 size)
{
    m_nick = nick;
    m_fileName = fileName;
    m_address = address;
    m_port = port;
    m_size = size;
}

bool DccTransfer::accept(const QString& directory)
{
    if (m_direction != Incoming || m_status != Offered)
        return false;

    if (!QDir().mkpath(directory)) {
        fail(tr("Can't create the folder %1").arg(directory));
        return false;
    }
    m_filePath = uniquePath(directory, safeFileName(m_fileName));
    m_partPath = m_filePath + QStringLiteral(".part");
    m_file.setFileName(m_partPath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Can't save the file: %1").arg(m_file.errorString()));
        return false;
    }

    attachSocket(new QTcpSocket(this));
    setStatus(Connecting);
    m_timer.start(m_connectSecs * 1000);
    m_socket->connectToHost(m_address, m_port);
    return true;
}

void DccTransfer::decline()
{
    if (m_status != Offered)
        return;
    setStatus(Declined);
    emit finished();
}

bool DccTransfer::listen(const QString& nick, const QString& filePath,
                         quint16 firstPort, quint16 lastPort)
{
    if (m_direction != Outgoing)
        return false;

    m_nick = nick;
    m_filePath = filePath;
    m_fileName = offerFileName(filePath);

    QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        fail(tr("File not found: %1").arg(filePath));
        return false;
    }
    m_size = info.size();
    if (m_size <= 0) {
        fail(tr("The file is empty"));
        return false;
    }
    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        fail(tr("Can't read the file: %1").arg(m_file.errorString()));
        return false;
    }

    m_server = new QTcpServer(this);
    connect(m_server, SIGNAL(newConnection()), this, SLOT(onNewConnection()));
    bool listening = false;
    if (firstPort == 0 && lastPort == 0) {
        listening = m_server->listen(QHostAddress::Any, 0);
    } else {
        quint16 from = qMin(firstPort, lastPort);
        quint16 to = qMax(firstPort, lastPort);
        if (from == 0)
            from = 1;
        for (int p = from; p <= to && !listening; ++p)
            listening = m_server->listen(QHostAddress::Any, quint16(p));
    }
    if (!listening) {
        if (firstPort || lastPort)
            fail(tr("No free port between %1 and %2").arg(qMin(firstPort, lastPort)).arg(qMax(firstPort, lastPort)));
        else
            fail(tr("Can't listen for the receiver: %1").arg(m_server->errorString()));
        return false;
    }
    m_port = m_server->serverPort();
    setStatus(Waiting);
    m_timer.start(m_waitSecs * 1000);
    return true;
}

void DccTransfer::cancel()
{
    if (m_status == Offered) {
        decline();
        return;
    }
    if (!isActive())
        return;
    cleanup(true);
    setStatus(Cancelled);
    emit finished();
}

void DccTransfer::attachSocket(QTcpSocket* socket)
{
    m_socket = socket;
    m_socket->setParent(this);
    connect(m_socket, SIGNAL(connected()), this, SLOT(onConnected()));
    connect(m_socket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    connect(m_socket, SIGNAL(bytesWritten(qint64)), this, SLOT(onBytesWritten()));
    connect(m_socket, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
    connect(m_socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(onSocketError()));
}

void DccTransfer::onConnected()
{
    if (m_status != Connecting)
        return;
    m_elapsed.start();
    setStatus(Transferring);
    touch();
}

void DccTransfer::onNewConnection()
{
    QTcpSocket* socket = m_server->nextPendingConnection();
    if (!socket)
        return;
    if (m_socket || m_status != Waiting) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    m_server->close();
    m_address = socket->peerAddress();
    attachSocket(socket);
    m_elapsed.start();
    setStatus(Transferring);
    touch();
    writeMore();
}

void DccTransfer::onReadyRead()
{
    if (!m_socket || m_status != Transferring)
        return;

    const QByteArray data = m_socket->readAll();
    if (data.isEmpty())
        return;
    touch();

    if (m_direction == Incoming) {
        if (m_file.write(data) != data.size()) {
            fail(tr("Can't write the file: %1").arg(m_file.errorString()));
            return;
        }
        m_transferred += data.size();
        uchar ack[4];
        qToBigEndian<quint32>(quint32(m_transferred & 0xffffffff), ack);
        m_socket->write(reinterpret_cast<const char*>(ack), 4);
        scheduleProgress();
        if (m_size > 0 && m_transferred >= m_size)
            complete();
    } else {
        // Receivers acknowledge with the total byte count so far.
        m_ackBuffer += data;
        while (m_ackBuffer.size() >= 4) {
            m_acked = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(m_ackBuffer.constData()));
            m_ackBuffer.remove(0, 4);
        }
        checkSendComplete();
    }
}

void DccTransfer::onBytesWritten()
{
    if (m_direction != Outgoing || m_status != Transferring || !m_socket)
        return;
    touch();
    writeMore();
}

void DccTransfer::writeMore()
{
    while (m_sent < m_size && m_socket->bytesToWrite() < WriteBufferSize) {
        const QByteArray chunk = m_file.read(qMin(ChunkSize, m_size - m_sent));
        if (chunk.isEmpty()) {
            fail(tr("Can't read the file: %1").arg(m_file.errorString()));
            return;
        }
        m_socket->write(chunk);
        m_sent += chunk.size();
    }
    m_transferred = m_sent - m_socket->bytesToWrite();
    scheduleProgress();
    checkSendComplete();
}

void DccTransfer::checkSendComplete()
{
    if (m_status != Transferring || m_sent < m_size || m_socket->bytesToWrite() > 0)
        return;
    m_transferred = m_size;
    if (m_acked == quint32(m_size & 0xffffffff)) {
        complete();
    } else if (!m_draining) {
        // Everything is written; give the receiver a moment to confirm.
        m_draining = true;
        m_timer.start(m_drainSecs * 1000);
    }
}

void DccTransfer::onDisconnected()
{
    if (m_status != Transferring)
        return;
    if (m_direction == Incoming) {
        if (m_socket->bytesAvailable() > 0)
            onReadyRead();
        if (m_status != Transferring)
            return;
        if (m_size == 0 && m_transferred > 0)
            complete();
        else
            fail(tr("The sender closed the connection after %1 of %2 bytes").arg(m_transferred).arg(m_size));
    } else {
        if (m_sent >= m_size && m_socket->bytesToWrite() == 0)
            complete();
        else
            fail(tr("The receiver closed the connection after %1 of %2 bytes").arg(m_transferred).arg(m_size));
    }
}

void DccTransfer::onSocketError()
{
    if (!m_socket || !isActive())
        return;
    // A normal close also reports RemoteHostClosedError; onDisconnected()
    // decides whether that was the end of the file or a failure.
    if (m_socket->error() == QAbstractSocket::RemoteHostClosedError && m_status == Transferring)
        return;
    if (m_status == Connecting)
        fail(tr("Can't connect to %1 port %2: %3").arg(m_address.toString()).arg(m_port).arg(m_socket->errorString()));
    else
        fail(m_socket->errorString());
}

void DccTransfer::onTimeout()
{
    switch (m_status) {
    case Connecting:
        fail(tr("Can't connect to %1 port %2 (timed out)").arg(m_address.toString()).arg(m_port));
        break;
    case Waiting:
        fail(tr("Nobody connected to receive the file within %1 seconds").arg(m_waitSecs));
        break;
    case Transferring:
        if (m_draining)
            complete(); // all data was written; the receiver just never confirmed
        else
            fail(tr("The transfer stalled for %1 seconds").arg(m_stallSecs));
        break;
    default:
        break;
    }
}

void DccTransfer::touch()
{
    if (m_status == Transferring && !m_draining)
        m_timer.start(m_stallSecs * 1000);
}

void DccTransfer::scheduleProgress()
{
    if (!m_progressTimer.isActive())
        m_progressTimer.start();
}

void DccTransfer::emitProgress()
{
    emit changed();
}

void DccTransfer::complete()
{
    m_timer.stop();
    m_progressTimer.stop();
    m_elapsedAtEnd = m_elapsed.isValid() ? m_elapsed.elapsed() : 0;

    if (m_direction == Incoming) {
        m_file.close();
        QString target = m_filePath;
        if (QFileInfo::exists(target))
            target = uniquePath(QFileInfo(target).absolutePath(), QFileInfo(target).fileName());
        if (!QFile::rename(m_partPath, target)) {
            m_filePath = m_partPath;
        } else {
            m_filePath = target;
        }
        if (m_size == 0)
            m_size = m_transferred;
    } else {
        m_file.close();
        m_transferred = m_size;
    }
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->disconnectFromHost();
    }
    setStatus(Done);
    emit finished();
}

void DccTransfer::fail(const QString& error)
{
    m_error = error;
    cleanup(true);
    setStatus(Failed);
    emit finished();
}

void DccTransfer::cleanup(bool removePartial)
{
    m_timer.stop();
    m_progressTimer.stop();
    if (m_elapsed.isValid() && m_elapsedAtEnd < 0)
        m_elapsedAtEnd = m_elapsed.elapsed();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
    if (m_server)
        m_server->close();
    if (m_file.isOpen())
        m_file.close();
    if (removePartial && m_direction == Incoming && !m_partPath.isEmpty())
        QFile::remove(m_partPath);
}

void DccTransfer::setStatus(Status status)
{
    if (m_status != status) {
        m_status = status;
        emit changed();
    }
}

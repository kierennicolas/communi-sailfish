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

#include "dccmanager.h"
#include "messagestorage.h"
#include "messagemodel.h"

#include <QDir>
#include <QUrl>
#include <QDateTime>
#include <QFileInfo>
#include <QAbstractSocket>
#include <QStandardPaths>
#include <IrcBuffer>
#include <IrcCommand>
#include <IrcMessage>
#include <IrcConnection>
#include <IrcBufferModel>

IRC_USE_NAMESPACE

// Offers older than this (e.g. replayed by a bouncer) are shown but not accepted.
static const int OfferMaxAgeSecs = 300;

DccManager::DccManager(QObject* parent) : QAbstractListModel(parent),
    m_firstPort(0), m_lastPort(0), m_nextId(1), m_pending(0), m_active(0), m_finished(0)
{
    m_downloadDirectory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (m_downloadDirectory.isEmpty())
        m_downloadDirectory = QDir::homePath() + QStringLiteral("/Downloads");
}

DccManager::~DccManager()
{
    qDeleteAll(m_transfers);
}

void DccManager::setMessageStorage(MessageStorage* storage)
{
    m_storage = storage;
}

// Splits a CTCP line on spaces, keeping "quoted parts" together.
static QStringList splitCtcp(const QString& line)
{
    QStringList parts;
    QString current;
    bool quoted = false;
    bool pending = false;
    foreach (const QChar& c, line) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
            pending = true;
        } else if (c.isSpace() && !quoted) {
            if (pending)
                parts += current;
            current.clear();
            pending = false;
        } else {
            current += c;
            pending = true;
        }
    }
    if (pending)
        parts += current;
    return parts;
}

static bool isNumber(const QString& s)
{
    bool ok = false;
    s.toULongLong(&ok);
    return ok;
}

static bool isPort(const QString& s)
{
    bool ok = false;
    uint port = s.toUInt(&ok);
    return ok && port <= 65535;
}

static QHostAddress decodeAddress(const QString& s)
{
    bool ok = false;
    quint32 ip = s.toUInt(&ok);
    if (ok)
        return QHostAddress(ip);
    return QHostAddress(s); // dotted IPv4 or IPv6, used by some clients
}

static bool isAddress(const QString& s)
{
    return !decodeAddress(s).isNull();
}

DccManager::Offer DccManager::parseOffer(const QString& ctcp)
{
    Offer offer;
    const QStringList parts = splitCtcp(ctcp);
    if (parts.size() < 2 || parts.at(0).compare(QLatin1String("DCC"), Qt::CaseInsensitive))
        return offer;
    offer.type = parts.at(1).toUpper();
    if (offer.type != QLatin1String("SEND"))
        return offer;

    // DCC SEND <file> <address> <port> [<size> [<token>]]
    // The file name may be quoted; some clients send unquoted names with
    // spaces, so the numeric fields are taken from the end.
    const QStringList rest = parts.mid(2);
    const int n = rest.size();
    int nameEnd = -1;
    if (n >= 5 && rest.at(n - 3) == QLatin1String("0") && isNumber(rest.at(n - 2))
            && isNumber(rest.at(n - 1)) && isAddress(rest.at(n - 4))) {
        // Passive (reverse) DCC: port 0 and a token
        offer.passive = true;
        offer.address = decodeAddress(rest.at(n - 4));
        offer.size = rest.at(n - 2).toLongLong();
        offer.token = rest.at(n - 1);
        nameEnd = n - 4;
    } else if (n >= 4 && isAddress(rest.at(n - 3)) && isPort(rest.at(n - 2)) && isNumber(rest.at(n - 1))) {
        offer.address = decodeAddress(rest.at(n - 3));
        offer.port = rest.at(n - 2).toUShort();
        offer.size = rest.at(n - 1).toLongLong();
        nameEnd = n - 3;
    } else if (n >= 3 && isAddress(rest.at(n - 2)) && isPort(rest.at(n - 1))) {
        offer.address = decodeAddress(rest.at(n - 2));
        offer.port = rest.at(n - 1).toUShort();
        nameEnd = n - 2;
    }
    if (nameEnd < 1)
        return offer;
    offer.fileName = QStringList(rest.mid(0, nameEnd)).join(QStringLiteral(" "));
    offer.valid = !offer.fileName.isEmpty() && !offer.address.isNull() && (offer.passive || offer.port > 0);
    return offer;
}

QString DccManager::formatSize(qint64 bytes)
{
    if (bytes < 1024)
        return tr("%1 B").arg(bytes);
    double value = bytes / 1024.0;
    if (value < 1024)
        return tr("%1 kB").arg(value, 0, 'f', 1);
    value /= 1024.0;
    if (value < 1024)
        return tr("%1 MB").arg(value, 0, 'f', 1);
    value /= 1024.0;
    return tr("%1 GB").arg(value, 0, 'f', 2);
}

QString DccManager::encodeAddress(const QHostAddress& address)
{
    bool ok = false;
    quint32 ip = address.toIPv4Address(&ok);
    if (ok && ip)
        return QString::number(ip);
    if (address.protocol() == QAbstractSocket::IPv6Protocol)
        return address.toString();
    return QStringLiteral("0");
}

bool DccManager::messageFilter(IrcMessage* message)
{
    if (message->type() != IrcMessage::Private)
        return false;
    IrcPrivateMessage* msg = static_cast<IrcPrivateMessage*>(message);
    if (!msg->isRequest() || !msg->isPrivate() || msg->isOwn())
        return false;
    if (!msg->content().startsWith(QLatin1String("DCC "), Qt::CaseInsensitive))
        return false;

    const Offer offer = parseOffer(msg->content());
    if (offer.type != QLatin1String("SEND") || !offer.valid || offer.passive)
        return false; // shown as-is by the formatter

    const QDateTime sent = msg->timeStamp();
    if (sent.isValid() && sent.secsTo(QDateTime::currentDateTime()) > OfferMaxAgeSecs) {
        msg->setProperty("dccExpired", true);
        return false;
    }

    IrcConnection* connection = msg->connection();
    DccTransfer* transfer = addTransfer(DccTransfer::Incoming, connection);
    transfer->setOffer(msg->nick(), offer.fileName, offer.address, offer.port, offer.size);
    msg->setProperty("dccId", transfer->id());
    updateCounts();
    emit offerReceived(transfer->id(), transfer->nick(), transfer->fileName());
    return false; // let the offer show up in the chat, with an Accept link
}

DccTransfer* DccManager::addTransfer(DccTransfer::Direction direction, IrcConnection* connection)
{
    DccTransfer* transfer = new DccTransfer(m_nextId++, direction, this);
    if (connection)
        transfer->setNetwork(connection->displayName());
    connect(transfer, SIGNAL(changed()), this, SLOT(onTransferChanged()));
    connect(transfer, SIGNAL(finished()), this, SLOT(onTransferFinished()));

    // newest first
    beginInsertRows(QModelIndex(), 0, 0);
    m_transfers.prepend(transfer);
    m_connections.insert(transfer, connection);
    endInsertRows();
    emit countChanged();
    return transfer;
}

DccTransfer* DccManager::transfer(int id) const
{
    foreach (DccTransfer* transfer, m_transfers) {
        if (transfer->id() == id)
            return transfer;
    }
    return 0;
}

int DccManager::rowOf(DccTransfer* transfer) const
{
    return m_transfers.indexOf(transfer);
}

void DccManager::accept(int id)
{
    DccTransfer* t = transfer(id);
    if (t && t->status() == DccTransfer::Offered)
        t->accept(m_downloadDirectory);
}

void DccManager::decline(int id)
{
    DccTransfer* t = transfer(id);
    if (t)
        t->decline();
}

void DccManager::cancel(int id)
{
    DccTransfer* t = transfer(id);
    if (t)
        t->cancel();
}

void DccManager::remove(int id)
{
    DccTransfer* t = transfer(id);
    if (!t)
        return;
    if (!t->isFinished())
        t->cancel();
    int row = rowOf(t);
    if (row < 0)
        return;
    beginRemoveRows(QModelIndex(), row, row);
    m_transfers.removeAt(row);
    m_connections.remove(t);
    endRemoveRows();
    t->disconnect(this);
    t->deleteLater();
    emit countChanged();
    updateCounts();
}

void DccManager::clearFinished()
{
    QList<int> ids;
    foreach (DccTransfer* t, m_transfers) {
        if (t->isFinished())
            ids += t->id();
    }
    foreach (int id, ids)
        remove(id);
}

QString DccManager::fileUrl(int id) const
{
    DccTransfer* t = transfer(id);
    if (!t || t->filePath().isEmpty())
        return QString();
    return QUrl::fromLocalFile(t->filePath()).toString();
}

int DccManager::sendFile(IrcBuffer* buffer, const QString& path)
{
    if (!buffer)
        return -1;
    return sendFileTo(buffer->connection(), buffer->title(), path);
}

int DccManager::sendFileTo(IrcConnection* connection, const QString& nick, const QString& path)
{
    if (!connection || nick.isEmpty())
        return -1;

    QString localPath = path;
    if (localPath.startsWith(QLatin1String("file:")))
        localPath = QUrl(path).toLocalFile();

    if (!connection->isConnected()) {
        postInfo(connection, nick, tr("Can't send %1: not connected").arg(QFileInfo(localPath).fileName()));
        return -1;
    }

    DccTransfer* t = addTransfer(DccTransfer::Outgoing, connection);
    if (!t->listen(nick, localPath, quint16(m_firstPort), quint16(m_lastPort))) {
        updateCounts();
        return -1; // the failure is reported in the chat by onTransferFinished()
    }

    QHostAddress local;
    if (connection->socket())
        local = connection->socket()->localAddress();
    const QString ctcp = QStringLiteral("DCC SEND %1 %2 %3 %4")
            .arg(t->fileName(), encodeAddress(local))
            .arg(t->port())
            .arg(t->size());
    connection->sendCommand(IrcCommand::createCtcpRequest(nick, ctcp));
    postInfo(t, tr("Offering %1 (%2) to %3...").arg(t->fileName(), formatSize(t->size()), nick));
    updateCounts();
    return t->id();
}

void DccManager::addConnection(IrcConnection* connection)
{
    connection->installMessageFilter(this);
}

void DccManager::removeConnection(IrcConnection* connection)
{
    connection->removeMessageFilter(this);
}

int DccManager::count() const
{
    return m_transfers.count();
}

int DccManager::pendingOffers() const
{
    return m_pending;
}

int DccManager::activeTransfers() const
{
    return m_active;
}

int DccManager::finishedTransfers() const
{
    return m_finished;
}

QString DccManager::downloadDirectory() const
{
    return m_downloadDirectory;
}

void DccManager::setDownloadDirectory(const QString& directory)
{
    if (!directory.isEmpty() && m_downloadDirectory != directory) {
        m_downloadDirectory = directory;
        emit downloadDirectoryChanged();
    }
}

int DccManager::firstPort() const
{
    return m_firstPort;
}

void DccManager::setFirstPort(int port)
{
    port = qBound(0, port, 65535);
    if (m_firstPort != port) {
        m_firstPort = port;
        emit portsChanged();
    }
}

int DccManager::lastPort() const
{
    return m_lastPort;
}

void DccManager::setLastPort(int port)
{
    port = qBound(0, port, 65535);
    if (m_lastPort != port) {
        m_lastPort = port;
        emit portsChanged();
    }
}

int DccManager::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_transfers.count();
}

QHash<int, QByteArray> DccManager::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[IdRole] = "transferId";
    roles[IncomingRole] = "incoming";
    roles[StatusRole] = "status";
    roles[StatusTextRole] = "statusText";
    roles[NickRole] = "nick";
    roles[NetworkRole] = "network";
    roles[FileNameRole] = "fileName";
    roles[FilePathRole] = "filePath";
    roles[SizeRole] = "size";
    roles[SizeTextRole] = "sizeText";
    roles[TransferredRole] = "transferred";
    roles[ProgressRole] = "progress";
    roles[ErrorRole] = "error";
    roles[OfferedRole] = "offered";
    roles[ActiveRole] = "active";
    roles[FinishedRole] = "finished";
    roles[DoneRole] = "done";
    return roles;
}

QVariant DccManager::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_transfers.count())
        return QVariant();
    const DccTransfer* t = m_transfers.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case FileNameRole:
        return t->direction() == DccTransfer::Incoming && t->status() == DccTransfer::Done
                ? QFileInfo(t->filePath()).fileName() : t->fileName();
    case IdRole: return t->id();
    case IncomingRole: return t->direction() == DccTransfer::Incoming;
    case StatusRole: return int(t->status());
    case StatusTextRole: return statusText(t);
    case NickRole: return t->nick();
    case NetworkRole: return t->network();
    case FilePathRole: return t->filePath();
    case SizeRole: return t->size();
    case SizeTextRole: return t->size() > 0 ? formatSize(t->size()) : QString();
    case TransferredRole: return t->transferred();
    case ProgressRole: return t->size() > 0 ? qMin(1.0, double(t->transferred()) / t->size()) : 0.0;
    case ErrorRole: return t->errorString();
    case OfferedRole: return t->status() == DccTransfer::Offered;
    case ActiveRole: return t->isActive();
    case FinishedRole: return t->isFinished();
    case DoneRole: return t->status() == DccTransfer::Done;
    default: return QVariant();
    }
}

QString DccManager::statusText(const DccTransfer* t) const
{
    switch (t->status()) {
    case DccTransfer::Offered:
        return tr("Waiting for you to accept");
    case DccTransfer::Waiting:
        return tr("Waiting for %1 to accept").arg(t->nick());
    case DccTransfer::Connecting:
        return tr("Connecting...");
    case DccTransfer::Transferring:
        if (t->size() > 0)
            return tr("%1 of %2, %3/s").arg(formatSize(t->transferred()), formatSize(t->size()), formatSize(t->speed()));
        return tr("%1, %2/s").arg(formatSize(t->transferred()), formatSize(t->speed()));
    case DccTransfer::Done:
        if (t->direction() == DccTransfer::Incoming)
            return tr("Saved in %1").arg(QFileInfo(t->filePath()).absoluteDir().dirName());
        return tr("Sent");
    case DccTransfer::Failed:
        return tr("Failed: %1").arg(t->errorString());
    case DccTransfer::Cancelled:
        return tr("Cancelled");
    case DccTransfer::Declined:
        return tr("Declined");
    }
    return QString();
}

void DccManager::onTransferChanged()
{
    DccTransfer* t = qobject_cast<DccTransfer*>(sender());
    int row = rowOf(t);
    if (row >= 0) {
        QModelIndex idx = index(row);
        emit dataChanged(idx, idx);
    }
    updateCounts();
}

void DccManager::onTransferFinished()
{
    DccTransfer* t = qobject_cast<DccTransfer*>(sender());
    if (!t)
        return;
    const QString size = t->size() > 0 ? formatSize(t->size()) : QString();
    const QString name = t->direction() == DccTransfer::Incoming && t->status() == DccTransfer::Done
            ? QFileInfo(t->filePath()).fileName() : t->fileName();
    const QString file = size.isEmpty() ? name : tr("%1 (%2)").arg(name, size);
    switch (t->status()) {
    case DccTransfer::Done:
        if (t->direction() == DccTransfer::Incoming)
            postInfo(t, tr("Received %1, saved in %2").arg(file, QFileInfo(t->filePath()).absoluteDir().dirName()));
        else
            postInfo(t, tr("Sent %1 to %2").arg(file, t->nick()));
        break;
    case DccTransfer::Failed:
        postInfo(t, tr("File transfer of %1 failed: %2").arg(file, t->errorString()));
        break;
    case DccTransfer::Cancelled:
        postInfo(t, tr("File transfer of %1 cancelled").arg(file));
        break;
    case DccTransfer::Declined:
        postInfo(t, tr("Declined %1 from %2").arg(file, t->nick()));
        break;
    default:
        break;
    }
    updateCounts();
    emit transferFinished(t->id());
}

void DccManager::updateCounts()
{
    int pending = 0, active = 0, finished = 0;
    foreach (DccTransfer* t, m_transfers) {
        if (t->status() == DccTransfer::Offered)
            ++pending;
        else if (t->isActive())
            ++active;
        else if (t->isFinished())
            ++finished;
    }
    if (m_pending != pending) {
        m_pending = pending;
        emit pendingOffersChanged();
    }
    if (m_active != active) {
        m_active = active;
        emit activeTransfersChanged();
    }
    if (m_finished != finished) {
        m_finished = finished;
        emit finishedTransfersChanged();
    }
}

void DccManager::postInfo(DccTransfer* transfer, const QString& text)
{
    postInfo(m_connections.value(transfer), transfer->nick(), text);
}

void DccManager::postInfo(IrcConnection* connection, const QString& nick, const QString& text)
{
    if (!m_storage || !connection || nick.isEmpty())
        return;
    IrcBufferModel* model = connection->findChild<IrcBufferModel*>();
    if (!model)
        return;
    IrcBuffer* buffer = model->find(nick);
    if (!buffer)
        buffer = model->add(nick);
    MessageModel* messages = qobject_cast<MessageModel*>(m_storage->get(buffer));
    if (messages)
        messages->info(text);
}

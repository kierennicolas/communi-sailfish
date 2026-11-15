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

#ifndef DCCMANAGER_H
#define DCCMANAGER_H

#include <QAbstractListModel>
#include <QHostAddress>
#include <QPointer>
#include <QList>
#include <QHash>
#include <IrcGlobal>
#include <IrcMessageFilter>

#include "dcctransfer.h"

IRC_FORWARD_DECLARE_CLASS(IrcBuffer)
IRC_FORWARD_DECLARE_CLASS(IrcMessage)
IRC_FORWARD_DECLARE_CLASS(IrcConnection)

class MessageStorage;

// Keeps the list of DCC file transfers (a model for the Transfers page),
// notices incoming "DCC SEND" offers on every connection, and sends files.
class DccManager : public QAbstractListModel, public IrcMessageFilter
{
    Q_OBJECT
    Q_INTERFACES(IrcMessageFilter)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int pendingOffers READ pendingOffers NOTIFY pendingOffersChanged)
    Q_PROPERTY(int activeTransfers READ activeTransfers NOTIFY activeTransfersChanged)
    Q_PROPERTY(int finishedTransfers READ finishedTransfers NOTIFY finishedTransfersChanged)
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory WRITE setDownloadDirectory NOTIFY downloadDirectoryChanged)
    Q_PROPERTY(int firstPort READ firstPort WRITE setFirstPort NOTIFY portsChanged)
    Q_PROPERTY(int lastPort READ lastPort WRITE setLastPort NOTIFY portsChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        IncomingRole,
        StatusRole,
        StatusTextRole,
        NickRole,
        NetworkRole,
        FileNameRole,
        FilePathRole,
        SizeRole,
        SizeTextRole,
        TransferredRole,
        ProgressRole,
        ErrorRole,
        OfferedRole,
        ActiveRole,
        FinishedRole,
        DoneRole
    };

    struct Offer {
        Offer() : port(0), size(0), valid(false), passive(false) { }
        QString type;       // "SEND", "CHAT", ...
        QString fileName;
        QHostAddress address;
        quint16 port;
        qint64 size;
        QString token;      // passive DCC token, if any
        bool valid;
        bool passive;
    };

    explicit DccManager(QObject* parent = 0);
    ~DccManager();

    void setMessageStorage(MessageStorage* storage);

    // IrcMessageFilter
    bool messageFilter(IrcMessage* message);

    int count() const;
    int pendingOffers() const;
    int activeTransfers() const;
    int finishedTransfers() const;

    QString downloadDirectory() const;
    void setDownloadDirectory(const QString& directory);

    int firstPort() const;
    void setFirstPort(int port);
    int lastPort() const;
    void setLastPort(int port);

    int rowCount(const QModelIndex& parent = QModelIndex()) const;
    QVariant data(const QModelIndex& index, int role) const;
    QHash<int, QByteArray> roleNames() const;

    DccTransfer* transfer(int id) const;

    // Helpers, public for tests and the message formatter.
    static Offer parseOffer(const QString& ctcp);
    static QString formatSize(qint64 bytes);
    static QString encodeAddress(const QHostAddress& address);

    Q_INVOKABLE void accept(int id);
    Q_INVOKABLE void decline(int id);
    Q_INVOKABLE void cancel(int id);
    Q_INVOKABLE void remove(int id);
    Q_INVOKABLE void clearFinished();
    Q_INVOKABLE QString fileUrl(int id) const;

    // Offers a file to the other side of a private chat. Returns the
    // transfer id, or -1 if it could not be started (reported in the chat).
    Q_INVOKABLE int sendFile(IrcBuffer* buffer, const QString& path);
    int sendFileTo(IrcConnection* connection, const QString& nick, const QString& path);

public slots:
    void addConnection(IrcConnection* connection);
    void removeConnection(IrcConnection* connection);

signals:
    void countChanged();
    void pendingOffersChanged();
    void activeTransfersChanged();
    void finishedTransfersChanged();
    void downloadDirectoryChanged();
    void portsChanged();
    void offerReceived(int id, const QString& nick, const QString& fileName);
    void transferFinished(int id);

private slots:
    void onTransferChanged();
    void onTransferFinished();

private:
    DccTransfer* addTransfer(DccTransfer::Direction direction, IrcConnection* connection);
    int rowOf(DccTransfer* transfer) const;
    void updateCounts();
    void postInfo(DccTransfer* transfer, const QString& text);
    void postInfo(IrcConnection* connection, const QString& nick, const QString& text);
    QString statusText(const DccTransfer* transfer) const;

    QList<DccTransfer*> m_transfers;
    QHash<DccTransfer*, QPointer<IrcConnection> > m_connections;
    QPointer<MessageStorage> m_storage;
    QString m_downloadDirectory;
    int m_firstPort;
    int m_lastPort;
    int m_nextId;
    int m_pending;
    int m_active;
    int m_finished;
};

#endif // DCCMANAGER_H

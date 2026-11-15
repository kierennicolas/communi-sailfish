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

#ifndef DCCTRANSFER_H
#define DCCTRANSFER_H

#include <QObject>
#include <QFile>
#include <QTimer>
#include <QByteArray>
#include <QHostAddress>
#include <QElapsedTimer>

class QTcpServer;
class QTcpSocket;

// One DCC SEND file transfer, in either direction.
//
// Incoming: the other side offered a file and is listening; on accept() we
// connect to it, save the data to "<name>.part" while acknowledging every
// chunk (4-byte big-endian byte count), and rename it once complete.
//
// Outgoing: we listen on a port (optionally within a fixed range), the caller
// sends the offer, and once the receiver connects we stream the file.
//
// Only "active" DCC is implemented: it's the kind bouncers such as ZNC relay.
class DccTransfer : public QObject
{
    Q_OBJECT

public:
    enum Direction { Incoming, Outgoing };
    enum Status {
        Offered,        // incoming, waiting for the user to accept or decline
        Waiting,        // outgoing, waiting for the receiver to connect
        Connecting,     // incoming, connecting to the sender
        Transferring,
        Done,
        Failed,
        Cancelled,
        Declined
    };

    DccTransfer(int id, Direction direction, QObject* parent = 0);
    ~DccTransfer();

    int id() const { return m_id; }
    Direction direction() const { return m_direction; }
    Status status() const { return m_status; }
    QString nick() const { return m_nick; }
    QString network() const { return m_network; }
    void setNetwork(const QString& network) { m_network = network; }
    QString fileName() const { return m_fileName; }
    QString filePath() const { return m_filePath; }
    qint64 size() const { return m_size; }
    qint64 transferred() const { return m_transferred; }
    qint64 speed() const;
    QString errorString() const { return m_error; }
    QHostAddress address() const { return m_address; }
    quint16 port() const { return m_port; }

    bool isActive() const;
    bool isFinished() const;

    // Incoming
    void setOffer(const QString& nick, const QString& fileName,
                  const QHostAddress& address, quint16 port, qint64 size);
    bool accept(const QString& directory);
    void decline();

    // Outgoing. Listens on a free port in [firstPort, lastPort], or on any
    // free port when both are 0. Returns false (see errorString()) on error.
    bool listen(const QString& nick, const QString& filePath,
                quint16 firstPort, quint16 lastPort);

    void cancel();

    // Timeouts in seconds (defaults: 60, 300, 120, 30). The drain timeout is
    // how long to wait, once everything is sent, for the receiver to confirm.
    void setTimeouts(int connectSecs, int waitSecs, int stallSecs, int drainSecs = 30);

    // Helpers, public for tests.
    static QString safeFileName(const QString& offered);
    static QString uniquePath(const QString& directory, const QString& fileName);
    static QString offerFileName(const QString& path);

signals:
    void changed();
    void finished();

private slots:
    void onConnected();
    void onReadyRead();
    void onBytesWritten();
    void onDisconnected();
    void onSocketError();
    void onNewConnection();
    void onTimeout();
    void emitProgress();

private:
    void setStatus(Status status);
    void fail(const QString& error);
    void complete();
    void cleanup(bool removePartial);
    void attachSocket(QTcpSocket* socket);
    void writeMore();
    void checkSendComplete();
    void touch();
    void scheduleProgress();

    int m_id;
    Direction m_direction;
    Status m_status;
    QString m_nick;
    QString m_network;
    QString m_fileName;
    QString m_filePath;
    QString m_partPath;
    QString m_error;
    QHostAddress m_address;
    quint16 m_port;
    qint64 m_size;
    qint64 m_transferred;
    qint64 m_sent;
    quint32 m_acked;
    bool m_draining;
    QByteArray m_ackBuffer;
    QFile m_file;
    QTcpSocket* m_socket;
    QTcpServer* m_server;
    QTimer m_timer;
    QTimer m_progressTimer;
    QElapsedTimer m_elapsed;
    qint64 m_elapsedAtEnd;
    int m_connectSecs;
    int m_waitSecs;
    int m_stallSecs;
    int m_drainSecs;
};

#endif // DCCTRANSFER_H

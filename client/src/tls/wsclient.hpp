#ifndef WSCLIENT_HPP_
#define WSCLIENT_HPP_

// Device-only, same reason as tlsnetworkreply.hpp: mbedTLS is vendored for
// armv7 only. Simulator builds compile this down to nothing.
#ifdef BBPORT_HAVE_NATIVE_TLS

#include <QObject>
#include <QThread>
#include <QUrl>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QMutex>

#include "mbedtls/net_sockets.h"

// Internal to WsClient (mirrors TlsRequestThread/TlsNetworkReply in
// tlsnetworkreply.hpp: both the QThread worker and its façade declared in
// one header, defined in one .cpp -- same reason: this project's build
// invokes moc per HEADERS entry, not via a .moc-include in the .cpp).
// Owns the actual socket for the connection's whole lifetime: one TCP+TLS
// handshake, one HTTP Upgrade, then a read/write loop until stopped or the
// server closes. See wsclient.cpp for the frame-level protocol.
class WsWorker : public QThread
{
    Q_OBJECT
public:
    WsWorker(const QUrl &url, const QString &token, QObject *parent = 0);
    virtual ~WsWorker();

    // Callable from another thread (mirrors TlsRequestThread::requestAbort()):
    // unblocks a pending blocking read/handshake by shutdown()'ing the fd,
    // same reasoning as there for why shutdown() and not net_free() here.
    void requestStop();
    // Thread-safe: queues an already-built, already-masked frame (see
    // buildFrame() in the .cpp) for this thread to write out next.
    void enqueueRaw(const QByteArray &frameBytes);

signals:
    void workerOpened();
    void workerClosed(const QString &reason);
    void workerText(const QByteArray &utf8);
    void workerBinary(const QByteArray &data);

protected:
    virtual void run();

private:
    bool wasStopped();

    QUrl m_url;
    QString m_token;

    QMutex m_stopMutex;
    bool m_stopped;

    QMutex m_sendMutex;
    QList<QByteArray> m_sendQueue;

    mbedtls_net_context m_netCtx;
};

// Persistent WebSocket-over-TLS client (RFC6455) for Feather's live /ws
// protocol (see server/server.js). Built directly on mbedTLS, the same way
// tlsnetworkreply.hpp's TlsRequestThread is (BB10's system TLS can't be
// pointed at a modern server -- see that header for why) -- but where
// TlsRequestThread opens a brand-new TCP+TLS 1.2 connection for every single
// HTTP request ("Connection: close", confirmed by reading its run()), this
// pays that handshake exactly ONCE for the whole session and then keeps the
// socket open. That repeated per-request handshake -- not payload size, not
// anything about QML/JS logic -- was the actual reason Feather's
// scrolling and streaming never felt smooth however the request logic was
// tuned.
//
// All socket I/O (handshake, HTTP Upgrade, WS framing, both directions) runs
// on WsWorker's own QThread; this class is the UI-thread-facing façade that
// BrowserController talks to. Every signal here is delivered on the UI
// thread (a queued cross-thread connection to WsWorker, standard Qt).
class WsClient : public QObject
{
    Q_OBJECT
public:
    explicit WsClient(QObject *parent = 0);
    virtual ~WsClient();

    // wsUrl: ws://host:port/path?query or wss://host:port/path?query.
    // token: sent as "Authorization: Bearer <token>" on the Upgrade request
    // if non-empty (the same header applyAuth() puts on the old HTTP path).
    void connectTo(const QUrl &wsUrl, const QString &token);
    // Closes (if open) and stops the worker thread. Safe to call any time,
    // including when never connected.
    void disconnectFromHost();
    bool isConnected() const { return m_connected; }

    // Both are safe to call from the UI thread at any time, connected or
    // not: queued for the worker thread to actually write, silently dropped
    // if there is no live connection to queue them on.
    void sendText(const QByteArray &utf8);
    void sendBinary(const QByteArray &data);

signals:
    void opened();
    // reason is empty for a clean, locally-requested close (disconnectFromHost()).
    void closed(const QString &reason);
    void textMessage(const QByteArray &utf8);
    void binaryMessage(const QByteArray &data);

private slots:
    void onWorkerOpened();
    void onWorkerClosed(const QString &reason);
    void onWorkerText(const QByteArray &utf8);
    void onWorkerBinary(const QByteArray &data);
    void onWorkerFinished();

private:
    WsWorker *m_worker;
    bool m_connected;
};

#endif /* BBPORT_HAVE_NATIVE_TLS */
#endif /* WSCLIENT_HPP_ */

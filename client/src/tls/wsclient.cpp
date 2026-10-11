#include "wsclient.hpp"
#include "bbportlog.hpp"

#ifdef BBPORT_HAVE_NATIVE_TLS

#include <sys/socket.h>

#include <QCoreApplication>
#include <QMutexLocker>
#include <QUuid>
#include <QCryptographicHash>
#include <QElapsedTimer>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

#include <resolv.h>

namespace {

// How long a single blocking read waits before returning MBEDTLS_ERR_SSL_TIMEOUT
// so the loop can check the outgoing queue and the stop flag. Small enough
// that a queued outgoing message (a tap, a scroll delta, ...) never waits
// longer than this to actually go out; not busy-waiting since the wait
// itself is a real blocking select() inside mbedTLS (mbedtls_net_recv_timeout),
// not a spin loop -- see net_sockets.h.
const uint32_t kReadTimeoutMs = 50;
// Overall deadline for connect + TLS handshake + HTTP Upgrade; past this the
// worker gives up and reports a failure instead of hanging forever.
const int kConnectTimeoutMs = 15000;
const int kMaxFramePayload = 32 * 1024 * 1024; // guards against a garbled length field

// Unifies the plain-TCP (ws://, LAN dev server) and TLS (wss://, the public
// tunnel) cases behind one read/write call so the rest of this file doesn't
// need two code paths. Neither socket is ever set non-blocking: both
// mbedtls_net_recv_timeout and mbedtls_ssl_read (configured with
// mbedtls_ssl_conf_read_timeout) block for at most kReadTimeoutMs and return
// MBEDTLS_ERR_SSL_TIMEOUT on their own -- the exact same mechanism
// TlsRequestThread uses (see its run(), which explains why a raw
// SO_RCVTIMEO was tried and rejected on this platform).
struct WsIo {
    bool tls;
    mbedtls_net_context *net;
    mbedtls_ssl_context *ssl;
};

int ioRead(WsIo &io, unsigned char *buf, size_t len)
{
    if (io.tls) return mbedtls_ssl_read(io.ssl, buf, len);
    return mbedtls_net_recv_timeout(io.net, buf, len, kReadTimeoutMs);
}

int ioWrite(WsIo &io, const unsigned char *buf, size_t len)
{
    if (io.tls) return mbedtls_ssl_write(io.ssl, buf, len);
    return mbedtls_net_send(io.net, buf, len);
}

bool writeAll(WsIo &io, const QByteArray &data)
{
    int written = 0;
    while (written < data.size()) {
        int ret = ioWrite(io, reinterpret_cast<const unsigned char *>(data.constData()) + written,
                           data.size() - written);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret <= 0) return false;
        written += ret;
    }
    return true;
}

// Appends whatever is available (possibly nothing, on a timeout slice) to
// `buf`. Returns 1 on data read, 0 on a harmless timeout/retry slice
// (caller should just loop again), -1 on a real error or clean peer close.
int readMore(WsIo &io, QByteArray &buf)
{
    unsigned char chunk[8192];
    int n = ioRead(io, chunk, sizeof(chunk));
    if (n == MBEDTLS_ERR_SSL_TIMEOUT || n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) {
        return -1;
    }
    if (n < 0) {
        return -1;
    }
    buf.append(reinterpret_cast<const char *>(chunk), n);
    return 1;
}

// RFC6455 client->server frame: FIN=1 always (this protocol never
// fragments either direction -- see tryParseFrame()), always masked.
QByteArray buildFrame(quint8 opcode, const QByteArray &payload)
{
    QByteArray out;
    out.append(char(0x80 | (opcode & 0x0F)));
    quint64 len = static_cast<quint64>(payload.size());
    const quint8 maskBit = 0x80;
    if (len <= 125) {
        out.append(char(maskBit | len));
    } else if (len <= 0xFFFFu) {
        out.append(char(maskBit | 126));
        out.append(char((len >> 8) & 0xFF));
        out.append(char(len & 0xFF));
    } else {
        out.append(char(maskBit | 127));
        for (int i = 7; i >= 0; --i) out.append(char((len >> (8 * i)) & 0xFF));
    }
    // 4 random bytes for the mask key. QUuid::createUuid() is documented
    // reentrant/thread-safe and needs no shared RNG state (unlike qrand()),
    // which matters since this can run alongside other threads.
    QByteArray maskKey = QUuid::createUuid().toRfc4122().left(4);
    out.append(maskKey);
    QByteArray masked = payload;
    for (int i = 0; i < masked.size(); ++i) {
        masked[i] = char(masked[i] ^ maskKey.at(i % 4));
    }
    out.append(masked);
    return out;
}

// Parses one complete frame at the start of `buf` if present. Returns false
// if more bytes are needed (buf is left untouched). The server this talks to
// never masks (RFC6455 forbids it), but masked frames are still unmasked
// correctly here defensively; a garbled/oversized length is treated as a
// protocol error by the caller checking kMaxFramePayload separately.
bool tryParseFrame(const QByteArray &buf, int &consumed, quint8 &opcode, QByteArray &payload, bool &protocolError)
{
    protocolError = false;
    if (buf.size() < 2) return false;
    quint8 b0 = quint8(buf.at(0));
    quint8 b1 = quint8(buf.at(1));
    bool fin = (b0 & 0x80) != 0;
    opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    quint64 len = b1 & 0x7F;
    int pos = 2;

    if (len == 126) {
        if (buf.size() < pos + 2) return false;
        len = (quint64(quint8(buf.at(pos))) << 8) | quint8(buf.at(pos + 1));
        pos += 2;
    } else if (len == 127) {
        if (buf.size() < pos + 8) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | quint8(buf.at(pos + i));
        pos += 8;
    }
    if (len > quint64(kMaxFramePayload)) {
        protocolError = true;
        return false;
    }

    QByteArray maskKey;
    if (masked) {
        if (buf.size() < pos + 4) return false;
        maskKey = buf.mid(pos, 4);
        pos += 4;
    }
    if (buf.size() < pos + int(len)) return false; // payload not fully arrived yet

    payload = buf.mid(pos, int(len));
    if (masked) {
        for (int i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ maskKey.at(i % 4));
    }
    consumed = pos + int(len);
    if (!fin) {
        bbportLog(QString("[Feather:ws] unexpected fragmented frame opcode=%1 (treating payload as-is)").arg(opcode));
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// WsWorker
// ---------------------------------------------------------------------------

WsWorker::WsWorker(const QUrl &url, const QString &token, QObject *parent) :
        QThread(parent),
        m_url(url),
        m_token(token),
        m_stopped(false)
{
    mbedtls_net_init(&m_netCtx);
}

WsWorker::~WsWorker()
{
    mbedtls_net_free(&m_netCtx);
}

void WsWorker::requestStop()
{
    QMutexLocker locker(&m_stopMutex);
    m_stopped = true;
    // shutdown(), not mbedtls_net_free(): this can run concurrently with
    // run() blocked inside a read/write on m_netCtx.fd -- same reasoning as
    // TlsRequestThread::requestAbort() (see tlsnetworkreply.cpp): closing
    // the fd here instead would race whatever's blocked on it and could
    // hand the fd number to an unrelated new socket while this thread is
    // still using it.
    if (m_netCtx.fd >= 0) {
        shutdown(m_netCtx.fd, SHUT_RDWR);
    }
}

bool WsWorker::wasStopped()
{
    QMutexLocker locker(&m_stopMutex);
    return m_stopped;
}

void WsWorker::enqueueRaw(const QByteArray &frameBytes)
{
    QMutexLocker locker(&m_sendMutex);
    m_sendQueue.append(frameBytes);
}

void WsWorker::run()
{
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cacert);

    const bool useTls = (m_url.scheme() == "wss");
    WsIo io;
    io.tls = useTls;
    io.net = &m_netCtx;
    io.ssl = &ssl;

    bool haveSsl = false;
    QString closeReason = QCoreApplication::translate("WsClient", "Connection failed");
    char errbuf[256];
    QElapsedTimer deadline;
    deadline.start();

    for (;;) { // one pass; `break` jumps straight to cleanup from anywhere below
        if (useTls) {
            int ret = mbedtls_x509_crt_parse_file(&cacert, "app/native/assets/cacert.pem");
            if (ret < 0) { closeReason = QCoreApplication::translate("WsClient", "Couldn't load the CA certificates"); break; }
        }

        int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                         reinterpret_cast<const unsigned char *>("feather-ws"), 10);
        if (ret != 0) { closeReason = "Inizializzazione RNG fallita"; break; }

        QByteArray hostUtf8 = m_url.host().toUtf8();
        QByteArray portAscii = QByteArray::number(m_url.port(useTls ? 443 : 80));

        ret = mbedtls_net_connect(&m_netCtx, hostUtf8.constData(), portAscii.constData(), MBEDTLS_NET_PROTO_TCP);
        // The resolver reads the DNS servers once per process: after a move to
        // another Wi-Fi every lookup fails until it reloads them.
        if (ret == MBEDTLS_ERR_NET_UNKNOWN_HOST) res_init();
        if (ret != 0) {
            closeReason = wasStopped() ? QString() : "Connessione TCP fallita";
            break;
        }

        if (useTls) {
            ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                               MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
            if (ret != 0) { closeReason = "Configurazione TLS fallita"; break; }
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&conf, &cacert, 0);
            mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
            mbedtls_ssl_conf_read_timeout(&conf, kReadTimeoutMs);

            ret = mbedtls_ssl_setup(&ssl, &conf);
            if (ret == 0) ret = mbedtls_ssl_set_hostname(&ssl, hostUtf8.constData());
            if (ret != 0) { closeReason = "Setup TLS fallito"; break; }

            mbedtls_ssl_set_bio(&ssl, &m_netCtx, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

            bool handshakeFailed = false;
            while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
                if (wasStopped()) { closeReason.clear(); handshakeFailed = true; break; }
                if (deadline.elapsed() > kConnectTimeoutMs) { closeReason = QCoreApplication::translate("WsClient", "TLS handshake timeout"); handshakeFailed = true; break; }
                if (ret == MBEDTLS_ERR_SSL_TIMEOUT) continue;
                if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    mbedtls_strerror(ret, errbuf, sizeof(errbuf));
                    closeReason = QString("Handshake TLS fallito: %1").arg(QString::fromLatin1(errbuf));
                    handshakeFailed = true;
                    break;
                }
            }
            if (handshakeFailed) break;

            uint32_t verify = mbedtls_ssl_get_verify_result(&ssl);
            if (verify != 0) {
                char vbuf[256];
                mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "", verify);
                closeReason = QCoreApplication::translate("WsClient", "Invalid certificate: %1").arg(QString::fromLatin1(vbuf).trimmed());
                break;
            }
            haveSsl = true;
        }

        // --- HTTP Upgrade handshake ---
        QByteArray wsKeyRaw = QUuid::createUuid().toRfc4122();
        QByteArray wsKey = wsKeyRaw.toBase64();
        QByteArray path = m_url.encodedPath();
        if (path.isEmpty()) path = "/";
        if (!m_url.encodedQuery().isEmpty()) path += "?" + m_url.encodedQuery();

        QByteArray req;
        req += "GET " + path + " HTTP/1.1\r\n";
        req += "Host: " + hostUtf8 + "\r\n";
        req += "Upgrade: websocket\r\n";
        req += "Connection: Upgrade\r\n";
        req += "Sec-WebSocket-Key: " + wsKey + "\r\n";
        req += "Sec-WebSocket-Version: 13\r\n";
        if (!m_token.isEmpty()) req += "Authorization: Bearer " + m_token.toUtf8() + "\r\n";
        req += "\r\n";

        if (!writeAll(io, req)) { closeReason = wasStopped() ? QString() : QCoreApplication::translate("WsClient", "Sending the upgrade request failed"); break; }

        QByteArray pending;
        int headerEnd = -1;
        bool ioError = false;
        for (;;) {
            headerEnd = pending.indexOf("\r\n\r\n");
            if (headerEnd >= 0) break;
            if (pending.size() > 16384 || deadline.elapsed() > kConnectTimeoutMs) {
                closeReason = QCoreApplication::translate("WsClient", "Upgrade answer too large or timed out");
                ioError = true;
                break;
            }
            int n = readMore(io, pending);
            if (n < 0) {
                closeReason = wasStopped() ? QString() : QCoreApplication::translate("WsClient", "Connection closed during the upgrade");
                ioError = true;
                break;
            }
        }
        if (ioError) break;

        QByteArray headerBlock = pending.left(headerEnd);
        // Any bytes right after the blank line already belong to the first
        // WS frame(s) -- exactly like TlsRequestThread's HTTP body parsing,
        // must not be discarded.
        QByteArray recvBuf = pending.mid(headerEnd + 4);

        QList<QByteArray> lines = headerBlock.split('\n');
        QByteArray statusLine = lines.isEmpty() ? QByteArray() : lines.at(0).trimmed();
        if (!statusLine.contains(" 101 ")) {
            closeReason = QCoreApplication::translate("WsClient", "Upgrade refused: %1").arg(QString::fromLatin1(statusLine));
            break;
        }
        QByteArray acceptGot;
        for (int i = 1; i < lines.size(); ++i) {
            QByteArray line = lines.at(i).trimmed();
            int colon = line.indexOf(':');
            if (colon < 0) continue;
            QByteArray name = line.left(colon).trimmed();
            if (qstricmp(name.constData(), "Sec-WebSocket-Accept") == 0) {
                acceptGot = line.mid(colon + 1).trimmed();
            }
        }
        QByteArray acceptWant = QCryptographicHash::hash(
            wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
        if (acceptGot != acceptWant) {
            closeReason = QCoreApplication::translate("WsClient", "WebSocket handshake check failed");
            break;
        }

        // --- Connected: notify the UI thread, then pump frames until stopped/closed ---
        emit workerOpened();

        bool sawCloseFrame = false;
        for (;;) {
            {
                QMutexLocker locker(&m_sendMutex);
                QList<QByteArray> outgoing = m_sendQueue;
                m_sendQueue.clear();
                locker.unlock();
                for (int i = 0; i < outgoing.size(); ++i) {
                    if (!writeAll(io, outgoing.at(i))) { closeReason = QCoreApplication::translate("WsClient", "Write failed"); goto pumpDone; }
                }
            }
            if (wasStopped()) { closeReason.clear(); break; }

            {
                int n = readMore(io, recvBuf);
                if (n < 0) {
                    closeReason = wasStopped() ? QString() : QCoreApplication::translate("WsClient", "Connection closed by the server");
                    break;
                }
            }

            for (;;) {
                int consumed; quint8 opcode; QByteArray payload; bool protoErr;
                if (!tryParseFrame(recvBuf, consumed, opcode, payload, protoErr)) {
                    if (protoErr) { closeReason = QCoreApplication::translate("WsClient", "Invalid WebSocket frame"); goto pumpDone; }
                    break;
                }
                recvBuf.remove(0, consumed);
                if (opcode == 0x1) {
                    emit workerText(payload);
                } else if (opcode == 0x2) {
                    emit workerBinary(payload);
                } else if (opcode == 0x9) {
                    enqueueRaw(buildFrame(0xA, payload)); // ping -> pong, written on the next loop's drain
                } else if (opcode == 0x8) {
                    sawCloseFrame = true;
                    closeReason.clear();
                    goto pumpDone;
                }
                // 0xA (pong) and anything else: nothing to do.
            }
        }
        pumpDone:
        if (sawCloseFrame) {
            writeAll(io, buildFrame(0x8, QByteArray())); // best-effort close handshake reply
        }
        break;
    }

    if (haveSsl) {
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_net_free(&m_netCtx);

    emit workerClosed(closeReason);
}

// ---------------------------------------------------------------------------
// WsClient
// ---------------------------------------------------------------------------

WsClient::WsClient(QObject *parent) :
        QObject(parent),
        m_worker(0),
        m_connected(false)
{
}

WsClient::~WsClient()
{
    disconnectFromHost();
}

void WsClient::connectTo(const QUrl &wsUrl, const QString &token)
{
    disconnectFromHost();
    m_worker = new WsWorker(wsUrl, token, this);
    connect(m_worker, SIGNAL(workerOpened()), this, SLOT(onWorkerOpened()));
    connect(m_worker, SIGNAL(workerClosed(QString)), this, SLOT(onWorkerClosed(QString)));
    connect(m_worker, SIGNAL(workerText(QByteArray)), this, SLOT(onWorkerText(QByteArray)));
    connect(m_worker, SIGNAL(workerBinary(QByteArray)), this, SLOT(onWorkerBinary(QByteArray)));
    connect(m_worker, SIGNAL(finished()), this, SLOT(onWorkerFinished()));
    m_worker->start();
}

void WsClient::disconnectFromHost()
{
    if (!m_worker) return;
    m_worker->requestStop();
    m_worker->wait();
    m_worker->deleteLater();
    m_worker = 0;
    m_connected = false;
}

void WsClient::sendText(const QByteArray &utf8)
{
    if (!m_worker || !m_connected) return;
    m_worker->enqueueRaw(buildFrame(0x1, utf8));
}

void WsClient::sendBinary(const QByteArray &data)
{
    if (!m_worker || !m_connected) return;
    m_worker->enqueueRaw(buildFrame(0x2, data));
}

void WsClient::onWorkerOpened()
{
    m_connected = true;
    emit opened();
}

void WsClient::onWorkerClosed(const QString &reason)
{
    m_connected = false;
    if (!reason.isEmpty()) bbportLog("[Feather:ws] closed: " + reason);
    emit closed(reason);
}

void WsClient::onWorkerText(const QByteArray &utf8)
{
    emit textMessage(utf8);
}

void WsClient::onWorkerBinary(const QByteArray &data)
{
    emit binaryMessage(data);
}

void WsClient::onWorkerFinished()
{
    // The thread function has returned (workerClosed() already fired from
    // inside run(), delivered as a queued signal, so onWorkerClosed() above
    // either already ran or is about to -- this slot exists only so the
    // QThread object itself gets cleaned up; nothing else to do here.
}

#endif /* BBPORT_HAVE_NATIVE_TLS */

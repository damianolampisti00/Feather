#include "browsercontroller.hpp"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSettings>
#include <QUuid>
#include <QByteArray>
#include <QVariantMap>
#include <QVariantList>
#include <QDir>
#include <QFileInfo>
#include <QRegExp>
#include <QLocale>
#include <QStringList>
#include <QtConcurrentRun>

#include <bb/cascades/Application>
#include <bb/cascades/ThemeSupport>
#include <bb/cascades/Theme>
#include <bb/cascades/ColorTheme>
#include <bb/cascades/VisualStyle>
#include <bb/cascades/Image>
#include <bb/utility/ImageConverter>
#include <setjmp.h>
#include <string.h>
#include <stdio.h> // jpeglib.h uses FILE
#include <jpeglib.h>
#include <bb/ImageData>
#include <bb/cascades/ArrayDataModel>
#include <bb/data/JsonDataAccess>
#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>

#include "tls/bbportlog.hpp"
#include "audio/audiooutput.hpp"
#include <opus.h>
#ifdef BBPORT_HAVE_NATIVE_TLS
#include "tls/tlsnetworkaccessmanager.hpp"
#endif

namespace {
// Page area in pixels: the whole 720x720 Q5 screen. The address bar in
// main.qml slides over it (and the page moves down while it shows) instead of
// taking space, so the page never has to be laid out again for it.
const int kTileHeight = 720;
const double kZoomLevels[] = { 1.0, 1.25, 1.5, 2.0 };  // all divide 720 evenly
const int kZoomCount = 4;
const double kDefaultZoom = 1.5;
// Where finished downloads go (needs the access_shared permission).
const char *kDownloadDir = "/accounts/1000/shared/downloads";
// The server first completes the file on the PC, then sends it, so the first
// byte can take a while on a big file: only give up after this much silence.
const int kDownloadStallMs = 120000;
const char *kDefaultServer = "http://192.168.1.100:8080/api";
// App-level keepalive so an idle connection (browsing paused, phone locked)
// doesn't get silently dropped by a proxy/tunnel's own idle timeout.
const int kPingIntervalMs = 20000;
// How long to wait after an unexpected disconnect before trying again --
// mirrors the Android client's own reconnect backoff.
const int kReconnectDelayMs = 3000;
// In the background: frames paused and the page's sound muted at once, the
// connection closed after this (a quick look at another app doesn't cost a
// reconnect).
const int kBackgroundCloseMs = 15000;
// The server locks an address out for 15 minutes after 10 wrong tokens
// (server/guard.js); while it does, retrying every few seconds is pointless.
const int kLockedOutRetryMs = 5 * 60 * 1000;

// InnerTube /player clients tried in order for a YouTube video (playYoutube).
// ANDROID is what YouTubeNative uses on this phone; probed 2026-10-05 from
// the home IP it answered OK where ANDROID_VR got "Sign in to confirm you're
// not a bot". Both return direct, unciphered URLs; the only muxed (audio +
// video in one file, which BB10's player needs) format left is itag 18,
// 360p H.264/AAC -- 720p and up exist only as separate video-only streams.
struct YtClient {
    const char *headerId;   // X-YouTube-Client-Name
    const char *version;
    const char *userAgent;
    const char *clientJson; // context.client (hl/gl added from the phone's locale)
};
const YtClient kYtClients[] = {
    { "3", "21.26.364", "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip",
      "{\"clientName\":\"ANDROID\",\"clientVersion\":\"21.26.364\",\"androidSdkVersion\":30,"
      "\"osName\":\"Android\",\"osVersion\":\"11\"}" },
    { "28", "1.60.19", "com.google.android.apps.youtube.vr.oculus/1.60.19 (Linux; U; Android 12; en_US) gzip",
      "{\"clientName\":\"ANDROID_VR\",\"clientVersion\":\"1.60.19\",\"deviceMake\":\"Oculus\","
      "\"deviceModel\":\"Quest 3\",\"androidSdkVersion\":32}" },
};
const int kYtClientCount = sizeof(kYtClients) / sizeof(kYtClients[0]);
const int kYtMaxHeight = 720; // the screen is 720x720: nothing bigger is worth the bandwidth
// Frames decoded at once (onWsBinary): the Q10/Q5 CPU has two cores.
const int kMaxDecodes = 2;
}

BrowserController::BrowserController(QObject *parent) :
        QObject(parent),
        m_videoFromPage(false),
        m_webTapX(0),
        m_webTapY(0),
        m_ytNam(0),
        m_ytClient(0),
#ifdef BBPORT_HAVE_NATIVE_TLS
        m_ws(0),
#endif
        m_wsConnected(false),
        m_background(false),
        m_bgDisconnected(false),
        m_loading(false),
        m_zoom(kDefaultZoom),
        m_popup(false),
        m_darkTheme(false),
        m_extensionsEnabled(false),
        m_serverHasExtensions(false),
        m_histIndex(-1),
        m_decodesInFlight(0),
        m_frameSerial(0),
        m_applySerial(1),
        m_connGeneration(0),
        m_needKeyframe(false),
        m_statsShown(0),
        m_statsApplied(0),
        m_statsDeltas(0),
        m_statsBytes(0),
        m_statsDropped(0),
        m_statsDecodeMs(0),
        m_scrollY(0),
        m_pageHeight(0)
{
    // In the background the server holds the page's frames (see onAppBackground).
    connect(bb::cascades::Application::instance(), SIGNAL(thumbnail()), this, SLOT(onAppBackground()));
    connect(bb::cascades::Application::instance(), SIGNAL(invisible()), this, SLOT(onAppBackground()));
    connect(bb::cascades::Application::instance(), SIGNAL(fullscreen()), this, SLOT(onAppForeground()));
    m_bgTimer.setSingleShot(true);
    connect(&m_bgTimer, SIGNAL(timeout()), this, SLOT(onBackgroundIdle()));
#ifdef BBPORT_HAVE_NATIVE_TLS
    m_nam = new TlsNetworkAccessManager(this); // downloads only -- see header
#else
    m_nam = new QNetworkAccessManager(this);
#endif

    QSettings settings("Feather", "Feather");
    m_serverUrl = settings.value("serverUrl", kDefaultServer).toString();
    m_zoom = settings.value("zoom", kDefaultZoom).toDouble();
    m_token = settings.value("token").toString();
    m_extensionsEnabled = settings.value("extensions", false).toBool();
    // Until the user picks a theme in Impostazioni, follow the one the app
    // started with (the device's default).
    bb::cascades::ThemeSupport *themes = bb::cascades::Application::instance()->themeSupport();
    const QVariant savedTheme = settings.value("theme");
    if (savedTheme.isValid()) {
        m_darkTheme = savedTheme.toString() == "dark";
        themes->setVisualStyle(m_darkTheme ? bb::cascades::VisualStyle::Dark : bb::cascades::VisualStyle::Bright);
    } else {
        m_darkTheme = themes->theme()->colorTheme()->style() == bb::cascades::VisualStyle::Dark;
    }

    m_statsClock.invalidate(); // Qt 4's QElapsedTimer has no constructor: starts out garbage

    m_selectModel = new bb::cascades::ArrayDataModel(this);
    m_selectMultiple = false;
    m_bookmarksModel = new bb::cascades::ArrayDataModel(this);
    loadBookmarks();
    m_visitedModel = new bb::cascades::ArrayDataModel(this);
    {
        QByteArray json = QSettings("Feather", "Feather").value("visited").toByteArray();
        if (!json.isEmpty()) {
            bb::data::JsonDataAccess jda;
            m_visitedModel->append(jda.loadFromBuffer(json).toList());
        }
    }
    connect(this, SIGNAL(bookmarksChanged()), this, SIGNAL(currentBookmarkedChanged()));
    connect(this, SIGNAL(pageChanged()), this, SIGNAL(currentBookmarkedChanged()));
    connect(this, SIGNAL(historyChanged()), this, SIGNAL(currentBookmarkedChanged()));

    m_tabsModel = new bb::cascades::ArrayDataModel(this);
    m_tabCount = 0;
    m_sessionId = settings.value("sessionId").toString();
    if (m_sessionId.isEmpty()) {
        m_sessionId = QUuid::createUuid().toString().remove('{').remove('}');
        settings.setValue("sessionId", m_sessionId);
    }

    // Created here, on the UI thread, before the event loop starts: an invocation
    // that cold-starts the app is delivered to this connection once it runs.
    m_invoke = new bb::system::InvokeManager(this);
    connect(m_invoke, SIGNAL(invoked(const bb::system::InvokeRequest&)),
            this, SLOT(onInvoked(const bb::system::InvokeRequest&)));

    m_dlStall.setSingleShot(true);
    connect(&m_dlStall, SIGNAL(timeout()), this, SLOT(onDownloadStalled()));
    m_dlClear.setSingleShot(true);
    connect(&m_dlClear, SIGNAL(timeout()), this, SLOT(clearDownloadText()));

    m_pingTimer.setInterval(kPingIntervalMs);
    connect(&m_pingTimer, SIGNAL(timeout()), this, SLOT(sendPing()));
    m_pageAudioRing = new AudioRing(AudioOutput::kBytesPerMs * 2000);
    m_pageAudio = new AudioOutput(m_pageAudioRing);
    m_pageAudio->start(QThread::TimeCriticalPriority);
    int opusErr = 0;
    m_opus = opus_decoder_create(48000, 2, &opusErr);
    m_opusPcm.resize(5760 * 4);
    m_liveTimer.setSingleShot(true);
    connect(&m_liveTimer, SIGNAL(timeout()), this, SLOT(sendLive()));
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, SIGNAL(timeout()), this, SLOT(reconnectNow()));

    connectNow(); // eager: by the time the user finishes typing a URL, this is usually already up
}

QString BrowserController::currentUrl() const
{
    return (m_histIndex >= 0) ? m_history.at(m_histIndex).toString() : QString();
}

bb::cascades::DataModel* BrowserController::selectModel() const { return m_selectModel; }
bb::cascades::DataModel* BrowserController::bookmarksModel() const { return m_bookmarksModel; }
bb::cascades::DataModel* BrowserController::visitedModel() const { return m_visitedModel; }
bb::cascades::DataModel* BrowserController::tabsModel() const { return m_tabsModel; }

bool BrowserController::currentBookmarked() const
{
    const QString url = currentUrl();
    for (int i = 0; i < m_bookmarksModel->size(); ++i) {
        if (m_bookmarksModel->value(i).toMap().value("url").toString() == url) return true;
    }
    return false;
}

void BrowserController::loadBookmarks()
{
    QSettings settings("Feather", "Feather");
    bb::data::JsonDataAccess jda;
    const QByteArray json = settings.value("bookmarks").toByteArray();
    if (json.isEmpty()) return;
    QByteArray buf = json;
    const QVariantList list = jda.loadFromBuffer(buf).toList();
    m_bookmarksModel->clear();
    m_bookmarksModel->append(list);
}

void BrowserController::saveBookmarks()
{
    QVariantList list;
    for (int i = 0; i < m_bookmarksModel->size(); ++i) list.append(m_bookmarksModel->value(i));
    bb::data::JsonDataAccess jda;
    QByteArray buf;
    jda.saveToBuffer(QVariant(list), &buf);
    QSettings("Feather", "Feather").setValue("bookmarks", buf);
    emit bookmarksChanged();
}

void BrowserController::addBookmark()
{
    const QString url = currentUrl();
    if (url.isEmpty() || currentBookmarked()) return;
    QVariantMap row;
    row["title"] = m_title.isEmpty() ? url : m_title;
    row["url"] = url;
    m_bookmarksModel->append(row);
    saveBookmarks();
}

void BrowserController::removeBookmarkAt(int index)
{
    if (index < 0 || index >= m_bookmarksModel->size()) return;
    m_bookmarksModel->removeAt(index);
    saveBookmarks();
}

// A page was shown: it goes to the top of the history (an older visit of the
// same address moves up rather than repeating). Later results for the same
// page (taps, a title that arrives late) only refresh its title.
static const int kMaxVisited = 300;

void BrowserController::recordVisit(const QUrl &url, const QString &title)
{
    const QString scheme = url.scheme().toLower();
    if (scheme != "http" && scheme != "https") return;
    const QString u = QString::fromUtf8(url.toEncoded());
    const QString t = title.isEmpty() ? u : title;
    if (m_visitedModel->size() > 0) {
        const QVariantMap top = m_visitedModel->value(0).toMap();
        if (top.value("url").toString() == u) {
            if (top.value("title").toString() == t) return;
            QVariantMap row = top;
            row["title"] = t;
            m_visitedModel->replace(0, row);
            saveVisited();
            return;
        }
    }
    for (int i = m_visitedModel->size() - 1; i >= 0; --i) {
        if (m_visitedModel->value(i).toMap().value("url").toString() == u) m_visitedModel->removeAt(i);
    }
    QVariantMap row;
    row["title"] = t;
    row["url"] = u;
    m_visitedModel->insert(0, row);
    while (m_visitedModel->size() > kMaxVisited) m_visitedModel->removeAt(m_visitedModel->size() - 1);
    saveVisited();
}

void BrowserController::saveVisited()
{
    QVariantList list;
    for (int i = 0; i < m_visitedModel->size(); ++i) list.append(m_visitedModel->value(i));
    bb::data::JsonDataAccess jda;
    QByteArray buf;
    jda.saveToBuffer(QVariant(list), &buf);
    QSettings("Feather", "Feather").setValue("visited", buf);
}

void BrowserController::openVisitedAt(int index)
{
    if (index < 0 || index >= m_visitedModel->size()) return;
    const QUrl url = QUrl::fromEncoded(m_visitedModel->value(index).toMap().value("url").toString().toUtf8());
    if (url.isValid()) navigateTo(url, true);
}

void BrowserController::clearVisited()
{
    m_visitedModel->clear();
    saveVisited();
}

void BrowserController::openBookmarkAt(int index)
{
    if (index < 0 || index >= m_bookmarksModel->size()) return;
    const QUrl url(m_bookmarksModel->value(index).toMap().value("url").toString());
    if (url.isValid()) navigateTo(url, true);
}

void BrowserController::newTab()
{
    QVariantMap body;
    body["action"] = "newtab";
    sendTrackedAction(body, false);
}

void BrowserController::openExtensions()
{
    QVariantMap body;
    body["action"] = "newtab";
    body["url"] = "chrome://extensions";
    sendTrackedAction(body, true);
}

void BrowserController::switchTab(const QString &tabId)
{
    if (tabId == m_activeTabId) return;
    QVariantMap body;
    body["action"] = "switchtab";
    body["tabId"] = tabId;
    sendTrackedAction(body, false);
}

void BrowserController::closeTab(const QString &tabId)
{
    QVariantMap body;
    body["action"] = "closetab";
    body["tabId"] = tabId;
    sendTrackedAction(body, false);
}

// Applies the server's tab list to tabsModel, and if the ACTIVE tab changed
// (newtab/switchtab/closetab-of-the-active-one), swaps in that tab's own
// back/forward history instead of touching the current one. Returns true when
// the active tab changed, so the caller skips the normal per-navigation history
// update below (which only makes sense for staying within the same tab).
bool BrowserController::applyTabsUpdate(const QVariantList &tabsList, const QUrl &finalUrl)
{
    QString newActiveId = m_activeTabId;
    if (!tabsList.isEmpty()) {
        m_tabsModel->clear();
        for (int i = 0; i < tabsList.size(); ++i) {
            const QVariantMap t = tabsList.at(i).toMap();
            QVariantMap row;
            const QString id = t.value("id").toString();
            const bool active = t.value("active").toBool();
            QString title = t.value("title").toString();
            if (title.isEmpty()) title = t.value("url").toString();
            if (title.isEmpty()) title = tr("New tab");
            row["title"] = title;
            row["url"] = t.value("url").toString();
            row["zid"] = id;
            m_tabsModel->append(row);
            if (active) newActiveId = id;
        }
        m_tabCount = tabsList.size();
        emit tabsChanged();
    }
    const bool tabChanged = !newActiveId.isEmpty() && newActiveId != m_activeTabId;
    if (tabChanged) {
        m_tabHistoryCache[m_activeTabId] = TabHistory(); // ensure a slot exists even the first time
        m_tabHistoryCache[m_activeTabId].history = m_history;
        m_tabHistoryCache[m_activeTabId].histIndex = m_histIndex;
        if (m_tabHistoryCache.contains(newActiveId)) {
            const TabHistory st = m_tabHistoryCache.take(newActiveId);
            m_history = st.history;
            m_histIndex = st.histIndex;
        } else {
            m_history.clear();
            if (finalUrl.isValid()) m_history.append(finalUrl);
            m_histIndex = m_history.isEmpty() ? -1 : 0;
        }
        m_activeTabId = newActiveId;
        emit historyChanged();
    }
    return tabChanged;
}

void BrowserController::sendSelectValues(const QStringList &values)
{
    QVariantMap body;
    body["action"] = "select";
    QVariantList v; for (int i = 0; i < values.size(); ++i) v.append(values.at(i));
    body["values"] = v;
    sendTrackedAction(body, true); // an onchange handler may navigate
}

void BrowserController::toggleSelectOption(int index)
{
    if (index < 0 || index >= m_selectChecked.size()) return;
    if (!m_selectMultiple) {
        sendSelectValues(QStringList() << m_selectValues.at(index));
        return;
    }
    m_selectChecked[index] = !m_selectChecked[index];
    QVariantMap row = m_selectModel->value(index).toMap();
    row["zchecked"] = m_selectChecked[index];
    m_selectModel->replace(index, row);
}

void BrowserController::confirmSelect()
{
    QStringList values;
    for (int i = 0; i < m_selectChecked.size(); ++i) if (m_selectChecked.at(i)) values << m_selectValues.at(i);
    sendSelectValues(values);
}

QString BrowserController::zoomLabel() const
{
    return QString::number(m_zoom) + "x";
}

void BrowserController::cycleZoom()
{
    int next = 0;
    for (int i = 0; i < kZoomCount; ++i) {
        if (kZoomLevels[i] > m_zoom + 0.001) { next = i; break; }
    }
    m_zoom = kZoomLevels[next];
    QSettings("Feather", "Feather").setValue("zoom", m_zoom);
    emit zoomChanged();
    if (m_histIndex >= 0) {
        QVariantMap body;
        body["action"] = "refresh";
        sendUntracked(body); // view change only: no history entry, no loading flag
    }
}

void BrowserController::setDarkTheme(bool dark)
{
    if (dark == m_darkTheme) return;
    m_darkTheme = dark;
    QSettings("Feather", "Feather").setValue("theme", dark ? "dark" : "light");
    bb::cascades::Application::instance()->themeSupport()->setVisualStyle(
            dark ? bb::cascades::VisualStyle::Dark : bb::cascades::VisualStyle::Bright);
    emit themeChanged();
    if (m_histIndex >= 0) {
        QVariantMap body;
        body["action"] = "refresh";
        sendUntracked(body); // the page re-rendered in the new theme, like a zoom change
    }
}

void BrowserController::setExtensionsEnabled(bool on)
{
    if (on == m_extensionsEnabled) return;
    m_extensionsEnabled = on;
    QSettings("Feather", "Feather").setValue("extensions", on);
    emit extensionsChanged();
    // The server switches this session's browser right away (otherwise with
    // the next action, which carries the setting too); the open page comes
    // back in it. Tracked, so the result's tab list -- all new -- is applied.
    if (m_wsConnected) {
        QVariantMap body;
        body["action"] = "extensions";
        sendTrackedAction(body, false);
    }
}

void BrowserController::setToken(const QString &token)
{
    const QString t = token.trimmed();
    if (t == m_token) return;
    m_token = t;
    QSettings("Feather", "Feather").setValue("token", m_token);
    emit tokenChanged();
    connectNow(); // the new token only takes effect on a fresh Upgrade handshake
}

void BrowserController::applyAuth(QNetworkRequest &req) const
{
    if (!m_token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
}

void BrowserController::setServerUrl(const QString &url)
{
    QString u = url.trimmed();
    if (u.isEmpty() || u == m_serverUrl) return;
    m_serverUrl = u;
    QSettings("Feather", "Feather").setValue("serverUrl", m_serverUrl);
    emit serverUrlChanged();
    connectNow();
}

QUrl BrowserController::normalizeInput(const QString &raw)
{
    QString s = raw.trimmed();
    if (s.isEmpty()) return QUrl();
    if (s.startsWith("http://", Qt::CaseInsensitive) || s.startsWith("https://", Qt::CaseInsensitive))
        return QUrl::fromUserInput(s);
    // The server browser's extensions: chrome://extensions and their own
    // pages (the server refuses them when it has no extensions).
    if (!s.contains(' ') && (s.startsWith("chrome://extensions", Qt::CaseInsensitive)
                             || s.startsWith("chrome-extension://", Qt::CaseInsensitive)))
        return QUrl(s, QUrl::TolerantMode);
    // No scheme: something host-like is an address, anything else is a search.
    if (!s.contains(' ') && (s.contains('.') || s.startsWith("localhost")))
        return QUrl::fromUserInput("https://" + s);
    QUrl search("https://www.google.com/search");
    // Encoded by hand: Qt 4's addQueryItem leaves '+' as is, which Google
    // reads as a space ("c++" would become "c  ").
    search.addEncodedQueryItem("q", QUrl::toPercentEncoding(s));
    return search;
}

// http(s)://host:port/api -> ws(s)://host:port/ws?sessionId=... -- the path
// and query the user's saved serverUrl might carry (the old "/api" suffix)
// are replaced outright: the live endpoint is always exactly /ws.
QUrl BrowserController::wsUrlFor(const QString &serverUrl, const QString &sessionId)
{
    QUrl u(serverUrl);
    u.setScheme(u.scheme().compare("https", Qt::CaseInsensitive) == 0 ? "wss" : "ws");
    u.setPath("/ws");
    u.setEncodedQuery(QByteArray());
    u.addQueryItem("sessionId", sessionId);
    // Pace the server's frames by our acks (see onWsBinary / sendFrameAck).
    u.addQueryItem("flow", "ack");
    // Scroll-copy deltas: see applyDelta, and frames=delta in server.js.
    u.addQueryItem("frames", "delta");
    // The page's sound, as Opus packets (server/pageaudio.js, playPageAudio).
    u.addQueryItem("audio", "1");
    return u;
}

void BrowserController::connectNow()
{
#ifdef BBPORT_HAVE_NATIVE_TLS
    if (m_serverUrl.isEmpty()) return;
    if (!m_ws) {
        m_ws = new WsClient(this);
        connect(m_ws, SIGNAL(opened()), this, SLOT(onWsOpened()));
        connect(m_ws, SIGNAL(closed(QString)), this, SLOT(onWsClosed(QString)));
        connect(m_ws, SIGNAL(textMessage(QByteArray)), this, SLOT(onWsText(QByteArray)));
        connect(m_ws, SIGNAL(binaryMessage(QByteArray)), this, SLOT(onWsBinary(QByteArray)));
    }
    m_ws->connectTo(wsUrlFor(m_serverUrl, m_sessionId), m_token);
#endif
}

void BrowserController::reconnectNow()
{
    if (m_bgDisconnected) return; // closed on purpose: onAppForeground reconnects
    connectNow();
}

void BrowserController::sendPing()
{
#ifdef BBPORT_HAVE_NATIVE_TLS
    if (m_ws && m_wsConnected) m_ws->sendText("{\"type\":\"ping\"}");
#endif
}

void BrowserController::onWsOpened()
{
    m_wsConnected = true;
    m_authError.clear();
    ++m_connGeneration;
    emit connectedChanged();
    setError(QString());
    m_reconnectTimer.stop();
    m_pingTimer.start();
    if (m_background) m_ws->sendText("{\"type\":\"pause\"}"); // still in the background
    if (m_histIndex >= 0) {
        // Resync after a (re)connect: the server-side session/page is still
        // there (only this socket dropped), so ask for a fresh frame+meta of
        // whatever it's currently showing instead of leaving a stale tile up.
        QVariantMap body;
        body["action"] = "refresh";
        sendUntracked(body);
    }
    if (m_invokedUrl.isValid()) {
        const QUrl url = m_invokedUrl;
        m_invokedUrl = QUrl();
        openInvokedUrl(url);
    }
}

void BrowserController::onWsClosed(const QString &reason)
{
    m_wsConnected = false;
    emit connectedChanged();
    m_pingTimer.stop();
    // Acks belong to the connection that sent the frame: a new socket starts
    // with an empty window, so nothing still in hand here may be acked on it.
    // Frames still queued are dropped: a new connection always starts with a
    // whole frame (the server's delta state is per connection too).
    for (int i = 0; i < m_queuedFrames.size(); ++i) {
        DecodedFrame dropped;
        dropped.skip = true;
        m_decodedFrames.insert(m_queuedFrames.at(i).serial, dropped);
    }
    m_queuedFrames.clear();
    ++m_connGeneration; // frames still decoding came on the dead socket: no acks for them
    applyDecodedFrames();
    if (!m_pendingQueue.isEmpty()) {
        m_pendingQueue.clear();
        updateLoadingFlag();
    }
    if (reason.isEmpty()) return;
    // The reason carries the server's status line as is ("HTTP/1.1 401 ...").
    if (reason.contains(" 401 ")) {
        // Wrong token: retrying it would only get every device on this network
        // locked out (10 failures). Wait for a new token or server URL
        // (setToken/setServerUrl reconnect) or the next app start.
        m_authError = tr("Invalid token: enter it in Settings (top menu; the server shows it when it starts)");
        setError(m_authError);
        return;
    }
    if (reason.contains(" 429 ")) {
        m_authError = tr("Too many wrong tokens from this network: the server blocks it for 15 minutes");
        setError(m_authError);
        m_reconnectTimer.start(kLockedOutRetryMs);
        return;
    }
    m_authError.clear();
    setError(reason);
    m_reconnectTimer.start(kReconnectDelayMs);
}

void BrowserController::updateLoadingFlag()
{
    const bool loading = !m_pendingQueue.isEmpty();
    if (loading == m_loading) return;
    m_loading = loading;
    emit loadingChanged();
}

void BrowserController::dispatch(const QVariantMap &bodyIn)
{
    QVariantMap body = bodyIn;
    body["type"] = "action";
    body["zoom"] = m_zoom;
    body["theme"] = m_darkTheme ? "dark" : "light";
    body["extensions"] = m_extensionsEnabled;
    body["viewHeight"] = kTileHeight;
    // sessionId is not sent per-message: the server already bound this
    // connection to one via the ?sessionId=... on the /ws Upgrade request.
    bb::data::JsonDataAccess jda;
    QByteArray payload;
    jda.saveToBuffer(QVariant(body), &payload);
#ifdef BBPORT_HAVE_NATIVE_TLS
    if (m_ws) m_ws->sendText(payload);
#endif
}

void BrowserController::sendTrackedAction(QVariantMap body, bool pushHistory)
{
    if (!m_wsConnected) {
        setError(m_authError.isEmpty() ? tr("Server connection unavailable") : m_authError);
        return;
    }
    setError(QString());
    m_pendingQueue.append(pushHistory);
    updateLoadingFlag();
    dispatch(body);
}

void BrowserController::sendUntracked(QVariantMap body)
{
    if (!m_wsConnected) return; // e.g. a zoom cycle before the first connect lands: harmless no-op
    dispatch(body);
}

void BrowserController::go(const QString &input)
{
    QUrl url = normalizeInput(input);
    if (url.isValid() && !url.isEmpty()) navigateTo(url, true);
}

void BrowserController::navigateTo(const QUrl &url, bool pushHistory, bool interceptVideo, bool pauseMedia)
{
    // A new navigation to a video (tapped link, typed URL, bookmark, link
    // from another app) plays in the native player instead, and the page
    // underneath stays where it was. Back/forward (pushHistory false) still
    // go to the page itself.
    if (pushHistory && interceptVideo && playVideoAt(url, false, true)) return;
    QVariantMap body;
    body["action"] = "navigate";
    body["url"] = QString::fromUtf8(url.toEncoded()); // encoded: toString() would decode %xx
    // The video itself already plays natively: the server must not start a
    // second copy of it while showing the page.
    if (pauseMedia) body["pauseMedia"] = true;
    sendTrackedAction(body, pushHistory);
}

void BrowserController::openVideoPage()
{
    // Already on it (the video was spotted on the page itself, see
    // applyResult): closing the player is enough, that copy was paused then.
    if (m_videoFromPage) return;
    if (m_videoPageUrl.isValid()) navigateTo(m_videoPageUrl, true, false, true);
}

void BrowserController::back()
{
    if (!canGoBack()) return;
    --m_histIndex;
    emit historyChanged();
    navigateTo(m_history.at(m_histIndex), false);
}

void BrowserController::forward()
{
    if (!canGoForward()) return;
    ++m_histIndex;
    emit historyChanged();
    navigateTo(m_history.at(m_histIndex), false);
}

void BrowserController::reload()
{
    if (m_histIndex >= 0) navigateTo(m_history.at(m_histIndex), false);
}

void BrowserController::tapAt(int x, int y)
{
    bbportLog(QString("[Feather] tap %1,%2 links=%3").arg(x).arg(y).arg(m_links.size()));
    // Later links are painted on top of earlier ones, so test back to front.
    for (int i = m_links.size() - 1; i >= 0; --i) {
        if (m_links.at(i).rect.contains(x, y)) {
            bbportLog("[Feather] tap hit link " + m_links.at(i).url.toString());
            navigateTo(m_links.at(i).url, true);
            return;
        }
    }
    // Not on a known link: let the server click there (buttons, JS handlers).
    QVariantMap body;
    body["action"] = "tap";
    body["tapX"] = x;
    body["tapY"] = y;
    sendTrackedAction(body, true); // only actually pushed if the tap really changed the URL
}

void BrowserController::scrollAt(int x, int y, int dx, int dy)
{
    if (dx == 0 && dy == 0) return;
    QVariantMap body;
    body["action"] = "wheel";
    body["tapX"] = x;
    body["tapY"] = y;
    body["deltaX"] = dx;
    body["deltaY"] = dy;
    sendUntracked(body);
}

void BrowserController::typeLive(const QString &text)
{
    m_liveText = text;
    m_liveTimer.start(150);
}

void BrowserController::finishLive()
{
    if (m_liveTimer.isActive()) {
        m_liveTimer.stop();
        sendLive();
    }
}

void BrowserController::sendLive()
{
    // Untracked: no spinner, no history; the frames show the page reacting.
    QVariantMap body;
    body["action"] = "type";
    body["text"] = m_liveText;
    body["submit"] = false;
    sendUntracked(body);
}

void BrowserController::typeText(const QString &text, bool submit)
{
    m_liveTimer.stop();
    QVariantMap body;
    body["action"] = "type";
    body["text"] = text;
    body["submit"] = submit;
    sendTrackedAction(body, true); // a submit may navigate; only pushed if the URL changed
}

namespace {
// Keep the server-suggested name but make it safe as a file name.
QString safeFileName(const QString &name)
{
    QString n = QFileInfo(name).fileName();
    n.replace(QRegExp("[\\\\/:*?\"<>|\\x00-\\x1f]"), "_");
    n = n.trimmed();
    if (n.isEmpty() || n == "." || n == "..") n = "download";
    if (n.size() > 100) n = n.left(100);
    return n;
}

// dir/name, or dir/name (2).ext etc. if it already exists: never overwrite.
QString uniquePath(const QString &dir, const QString &name)
{
    QString path = dir + "/" + name;
    if (!QFile::exists(path)) return path;
    QFileInfo fi(name);
    const QString stem = fi.completeBaseName();
    const QString ext = fi.suffix().isEmpty() ? QString() : "." + fi.suffix();
    for (int i = 2; i < 1000; ++i) {
        path = QString("%1/%2 (%3)%4").arg(dir, stem).arg(i).arg(ext);
        if (!QFile::exists(path)) return path;
    }
    return dir + "/" + name;
}
}

void BrowserController::setDownloadText(const QString &t)
{
    if (t == m_dlText) return;
    m_dlText = t;
    emit downloadChanged();
}

void BrowserController::clearDownloadText()
{
    setDownloadText(QString());
}

void BrowserController::startDownload(const QString &id, const QString &name)
{
    if (m_dlReply) {
        cancelDownload(id); // one at a time: tell the server to drop this one
        return;
    }
    QDir().mkpath(kDownloadDir);
    m_dlName = safeFileName(name);
    m_dlFile.setFileName(uniquePath(kDownloadDir, m_dlName));
    if (!m_dlFile.open(QIODevice::WriteOnly)) {
        bbportLog("[Feather] cannot create " + m_dlFile.fileName() + ": " + m_dlFile.errorString());
        setDownloadText(tr("Can't save %1").arg(m_dlName));
        m_dlClear.start(8000);
        cancelDownload(id);
        return;
    }
    m_dlErrorBody.clear();
    m_dlClear.stop();

    QVariantMap body;
    body["sessionId"] = m_sessionId;
    body["id"] = id;
    bb::data::JsonDataAccess jda;
    QByteArray payload;
    jda.saveToBuffer(QVariant(body), &payload);

    QString url = m_serverUrl;
    while (url.endsWith('/')) url.chop(1);
    QNetworkRequest req(QUrl(url + "/download"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyAuth(req);

    setDownloadText(tr("Preparing %1...").arg(m_dlName));
    m_dlReply = m_nam->post(req, payload);
    connect(m_dlReply, SIGNAL(readyRead()), this, SLOT(onDownloadData()));
    connect(m_dlReply, SIGNAL(downloadProgress(qint64, qint64)), this, SLOT(onDownloadProgress(qint64, qint64)));
    connect(m_dlReply, SIGNAL(finished()), this, SLOT(onDownloadFinished()));
    m_dlStall.start(kDownloadStallMs);
}

void BrowserController::cancelDownload(const QString &id)
{
    QVariantMap body;
    body["sessionId"] = m_sessionId;
    body["id"] = id;
    body["cancel"] = true;
    bb::data::JsonDataAccess jda;
    QByteArray payload;
    jda.saveToBuffer(QVariant(body), &payload);

    QString url = m_serverUrl;
    while (url.endsWith('/')) url.chop(1);
    QNetworkRequest req(QUrl(url + "/download"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyAuth(req);
    QNetworkReply *r = m_nam->post(req, payload);
    connect(r, SIGNAL(finished()), r, SLOT(deleteLater())); // fire and forget
}

void BrowserController::onDownloadData()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != m_dlReply) return;
    m_dlStall.start(kDownloadStallMs);
    const QByteArray chunk = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) {
        m_dlErrorBody.append(chunk); // an error message, not file content
    } else if (m_dlFile.isOpen()) {
        m_dlFile.write(chunk);
    }
}

void BrowserController::onDownloadProgress(qint64 received, qint64 total)
{
    if (!m_dlReply) return;
    m_dlStall.start(kDownloadStallMs);
    const int status = m_dlReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) return;
    if (total > 0) {
        setDownloadText(tr("Downloading %1: %2%").arg(m_dlName).arg(int(received * 100 / total)));
    } else {
        setDownloadText(tr("Downloading %1: %2 KB").arg(m_dlName).arg(received / 1024));
    }
}

void BrowserController::onDownloadStalled()
{
    if (!m_dlReply) return;
    m_dlReply->disconnect(this);
    m_dlReply->abort();
    m_dlReply->deleteLater();
    m_dlReply = 0;
    finishDownload(tr("Download interrupted: the server doesn't answer"), false);
}

void BrowserController::onDownloadFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != m_dlReply) return;
    m_dlStall.stop();
    m_dlReply = 0;
    reply->deleteLater();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray rest = reply->readAll(); // TLS path delivers everything here
    if (status >= 400) {
        m_dlErrorBody.append(rest);
        bb::data::JsonDataAccess jda;
        QString msg = jda.loadFromBuffer(m_dlErrorBody).toMap().value("error").toString();
        if (msg.isEmpty()) msg = tr("Error %1").arg(status);
        finishDownload(msg, false);
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        finishDownload(tr("Download failed: %1").arg(reply->errorString()), false);
        return;
    }
    if (!rest.isEmpty() && m_dlFile.isOpen()) m_dlFile.write(rest);
    finishDownload(QString(), true);
}

// result empty + keepFile = success; otherwise `result` is the error to show.
void BrowserController::finishDownload(const QString &result, bool keepFile)
{
    const QString path = m_dlFile.fileName();
    const bool wrote = m_dlFile.isOpen() && m_dlFile.error() == QFile::NoError;
    if (m_dlFile.isOpen()) m_dlFile.close();
    if (keepFile && wrote) {
        const qint64 size = QFileInfo(path).size();
        bbportLog(QString("[Feather] download saved %1 (%2 bytes)").arg(path).arg(size));
        setDownloadText(tr("Saved in Downloads: %1 (%2 KB)").arg(QFileInfo(path).fileName()).arg(size / 1024));
    } else {
        QFile::remove(path);
        const QString msg = result.isEmpty() ? tr("Writing the file failed") : result;
        bbportLog("[Feather] download failed: " + msg);
        setDownloadText(msg);
    }
    m_dlClear.start(10000);
}

void BrowserController::onInvoked(const bb::system::InvokeRequest &request)
{
    // The link is normally in uri(); some senders put it in data() instead.
    // Keep the ENCODED form (toString() would decode %xx and alter the query), take
    // only the first line, and refuse absurd sizes before parsing anything.
    QByteArray raw = request.uri().toEncoded();
    if (raw.isEmpty()) raw = request.data();
    raw = raw.split('\n').value(0).trimmed();
    if (raw.isEmpty() || raw.size() > 8192) return;

    // Only web links: the server can't do anything with mailto:, file:, javascript:, ...
    const QByteArray lower = raw.left(8).toLower();
    if (!lower.startsWith("http://") && !lower.startsWith("https://")) return;

    const QUrl url = QUrl::fromEncoded(raw, QUrl::TolerantMode);
    if (!url.isValid()) return;

    // Links from emails can carry secrets (reset tokens...): log the host only.
    bbportLog("[Feather] invoked, opening a link on " + url.host());
    if (!m_wsConnected) {
        // The link itself launched the app: the connection isn't up yet, and
        // an action sent now would just be dropped. onWsOpened opens it.
        m_invokedUrl = url;
        return;
    }
    openInvokedUrl(url);
}

void BrowserController::openInvokedUrl(const QUrl &url)
{
    // A video goes to the native player (the tabs stay as they are); any
    // other link opens in a NEW tab, so the page the user was on isn't
    // replaced. The server puts it in the active tab instead if that one is
    // still blank (cold start), see 'newtab' in server.js.
    if (playVideoAt(url, false, true)) return;
    QVariantMap body;
    body["action"] = "newtab";
    body["url"] = QString::fromUtf8(url.toEncoded()); // encoded: toString() would decode %xx
    sendTrackedAction(body, true);
}

void BrowserController::log(const QString &msg)
{
    bbportLog("[Feather:ui] " + msg);
}

void BrowserController::setError(const QString &e)
{
    if (e == m_error) return;
    m_error = e;
    emit errorChanged();
}

// "MBA1" + seq u32 LE + one 20 ms Opus packet (48 kHz stereo) of the page's
// sound: decoded straight into the ring the playback thread drains. A pause
// in the sound just lets the ring run dry; the output then waits for a new
// start buffer (and gives the audio device back after a while).
void BrowserController::playPageAudio(const QByteArray &data)
{
    if (!m_opus || m_background) return; // muted in the background
    const int n = opus_decode(m_opus, reinterpret_cast<const unsigned char *>(data.constData()) + 8,
                              data.size() - 8, reinterpret_cast<opus_int16 *>(m_opusPcm.data()), 5760, 0);
    if (n > 0) m_pageAudioRing->write(m_opusPcm.constData(), n * 4);
}

BrowserController::~BrowserController()
{
    m_pageAudio->requestStop();
    m_pageAudio->wait(2000);
    delete m_pageAudio;
    delete m_pageAudioRing;
    if (m_opus) opus_decoder_destroy(m_opus);
}

void BrowserController::onWsBinary(const QByteArray &data)
{
    if (data.size() > 8 && data.startsWith("MBA1")) { // page sound, not a frame (no ack)
        playPageAudio(data);
        return;
    }
    // A playing video makes the server's screencast push ~20 frames/s of 40+
    // KB JPEGs. Decoding each one synchronously right here, on the UI thread,
    // fell behind: WsClient queues every frame to this thread with no limit,
    // so the backlog (and with it touch latency) just kept growing. Now the
    // decode runs on a pool thread, only the newest waiting frame is kept,
    // and each frame is acked once shown or superseded -- the server sends at
    // most FRAME_WINDOW unacked frames, so the phone sets the frame rate.
    //
    // Two decodes run side by side (the Q10/Q5 CPU is dual-core): decoding was
    // the frame-rate ceiling (~60 ms/frame with ImageConverter, 2026-10-06), so
    // a second decoder nearly doubles what the phone can take.
    //
    // With frames=delta most messages mid-scroll are deltas (see applyDelta),
    // which build on the frame before them: none may be skipped, so they are
    // queued, not superseded, and applied strictly in arrival order whatever
    // order their decodes finish in. Only a whole frame (a JPEG) makes
    // everything still waiting for a decoder obsolete. The server's
    // FRAME_WINDOW bounds the queue.
    const quint32 serial = ++m_frameSerial;
    const bool keyframe = data.size() >= 2 && uchar(data.at(0)) == 0xFF && uchar(data.at(1)) == 0xD8;
    if (keyframe) {
        for (int i = 0; i < m_queuedFrames.size(); ++i) {
            DecodedFrame dropped;
            dropped.skip = true;
            m_decodedFrames.insert(m_queuedFrames.at(i).serial, dropped);
            ++m_statsDropped; // superseded before it was ever decoded
            sendFrameAck(m_queuedFrames.at(i).generation);
        }
        m_queuedFrames.clear();
    }
    QueuedFrame q;
    q.data = data;
    q.serial = serial;
    q.generation = m_connGeneration;
    m_queuedFrames.append(q);
    startDecodes();
    applyDecodedFrames();
}

void BrowserController::startDecodes()
{
    while (m_decodesInFlight < kMaxDecodes && !m_queuedFrames.isEmpty()) {
        const QueuedFrame q = m_queuedFrames.takeFirst();
        ++m_decodesInFlight;
        QFutureWatcher<DecodedFrame> *watcher = new QFutureWatcher<DecodedFrame>(this);
        watcher->setProperty("serial", q.serial);
        watcher->setProperty("generation", q.generation);
        connect(watcher, SIGNAL(finished()), this, SLOT(onFrameDecoded()));
        watcher->setFuture(QtConcurrent::run(&BrowserController::decodeFrame, q.data));
    }
}

namespace {
// libjpeg reports errors by longjmp()ing out of its calls, which must never
// unwind past a C++ object with a destructor -- so every libjpeg call lives in
// this plain struct's methods, each arming its own setjmp, and the only C++
// object involved (the destination ImageData) is created by the caller,
// between the two calls.
struct TurboJpegDecoder {
    struct ErrorMgr {
        jpeg_error_mgr pub;
        jmp_buf jump;
    };
    jpeg_decompress_struct cinfo;
    ErrorMgr err;
    bool created;

    TurboJpegDecoder() : created(false) {}
    ~TurboJpegDecoder() { if (created) jpeg_destroy_decompress(&cinfo); }

    static void onError(j_common_ptr info) { longjmp(reinterpret_cast<ErrorMgr *>(info->err)->jump, 1); }
    static void onMessage(j_common_ptr, int) {} // warnings: not interesting here

    bool readHeader(const QByteArray &jpeg, int *width, int *height)
    {
        cinfo.err = jpeg_std_error(&err.pub);
        err.pub.error_exit = onError;
        err.pub.emit_message = onMessage;
        if (setjmp(err.jump)) return false;
        jpeg_create_decompress(&cinfo);
        created = true;
        jpeg_mem_src(&cinfo, reinterpret_cast<unsigned char *>(const_cast<char *>(jpeg.constData())),
                     static_cast<unsigned long>(jpeg.size()));
        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) return false;
        // Straight into the byte order of bb::PixelFormat::RGBX (R, G, B, pad),
        // which Cascades' Image takes without any conversion; the fast integer
        // IDCT (NEON) is visually identical at the server's JPEG quality.
        cinfo.out_color_space = JCS_EXT_RGBX;
        cinfo.dct_method = JDCT_IFAST;
        jpeg_calc_output_dimensions(&cinfo);
        *width = int(cinfo.output_width);
        *height = int(cinfo.output_height);
        return true;
    }

    bool decodeInto(unsigned char *pixels, int bytesPerLine)
    {
        if (setjmp(err.jump)) return false;
        jpeg_start_decompress(&cinfo);
        while (cinfo.output_scanline < cinfo.output_height) {
            JSAMPROW rows[8];
            int n = 0;
            for (; n < 8 && cinfo.output_scanline + n < cinfo.output_height; ++n)
                rows[n] = pixels + (cinfo.output_scanline + n) * bytesPerLine;
            jpeg_read_scanlines(&cinfo, rows, n);
        }
        jpeg_finish_decompress(&cinfo);
        return true;
    }
};

// One whole JPEG -> ImageData (invalid if the JPEG is broken), see decodeFrame.
bb::ImageData decodeJpeg(const QByteArray &jpeg)
{
    {
        TurboJpegDecoder decoder;
        int width = 0, height = 0;
        if (decoder.readHeader(jpeg, &width, &height) && width > 0 && height > 0) {
            bb::ImageData image(bb::PixelFormat::RGBX, width, height);
            if (decoder.decodeInto(image.pixels(), image.bytesPerLine())) return image;
        }
    }
    return bb::utility::ImageConverter::decode(jpeg);
}

// Little-endian fields of an "MBD1" header.
quint32 le16(const uchar *p) { return quint32(p[0]) | (quint32(p[1]) << 8); }
quint32 le32(const uchar *p) { return le16(p) | (le16(p + 2) << 16); }
}

DecodedFrame BrowserController::decodeFrame(const QByteArray &msg)
{
    // Pool thread: ImageConverter/ImageData are plain bb (not Cascades)
    // classes; only the bb::cascades::Image built from the result in
    // onFrameDecoded has to happen on the UI thread.
    //
    // Image(QByteArray) decodes the JPEG lazily, at paint time -- the SDK docs
    // note that QByteArray-based images "don't report state changes" the way
    // asset/tracked images do, i.e. there is no Loading->Loaded step before
    // the compositor needs pixels. On every incoming frame the ImageView had
    // nothing decoded yet to paint for that instant, so it fell through to the
    // container's background (white) for a frame -- the white flash on every
    // update. Decoding synchronously first, via ImageConverter, and handing
    // ImageView an already-materialized ImageData-backed Image means there is
    // no decode step left when the property changes: the compositor always has
    // real pixels ready to blit immediately.
    //
    // libjpeg-turbo (NEON, third_party/libjpeg-turbo, see
    // tools/build-libjpeg-turbo.sh) decodes straight into the ImageData's own
    // buffer; ImageConverter (~60 ms per 720x720 frame) only remains as the
    // fallback for anything libjpeg rejects.
    QElapsedTimer t;
    t.start();
    DecodedFrame f;
    f.bytes = msg.size();
    const uchar *p = reinterpret_cast<const uchar *>(msg.constData());
    if (msg.size() >= 16 && memcmp(p, "MBD1", 4) == 0) {
        // frames=delta (see server.js): "MBD1", u16 width, u16 height, i16 dx,
        // i16 dy, u16 count, u16 reserved; count x {u16 x, y, w, h, u32 length};
        // then the rectangles' JPEGs back to back. All little-endian.
        f.delta = true;
        f.width = int(le16(p + 4));
        f.height = int(le16(p + 6));
        f.dx = qint16(quint16(le16(p + 8)));
        f.dy = qint16(quint16(le16(p + 10)));
        const int count = int(le16(p + 12));
        int offset = 16 + count * 12;
        bool ok = msg.size() >= offset;
        for (int i = 0; ok && i < count; ++i) {
            const uchar *r = p + 16 + i * 12;
            const int length = int(le32(r + 8));
            if (length <= 0 || offset + length > msg.size()) {
                ok = false;
                break;
            }
            DecodedRect rect;
            rect.x = int(le16(r));
            rect.y = int(le16(r + 2));
            rect.pixels = decodeJpeg(QByteArray::fromRawData(msg.constData() + offset, length));
            ok = rect.pixels.isValid() && rect.pixels.width() == int(le16(r + 4))
                 && rect.pixels.height() == int(le16(r + 6));
            offset += length;
            f.rects.append(rect);
        }
        f.ok = ok;
    } else {
        f.image = decodeJpeg(msg);
    }
    f.decodeMs = int(t.elapsed());
    return f;
}

// frames=delta: while the page scrolls, most of a new frame is the previous
// one moved by the scroll (the server keeps every scroll step a multiple of
// the JPEG's 16 px blocks, so those parts are bit-identical, see server.js).
// So a delta carries that shift plus only the rectangles that really changed
// or just scrolled into view -- a fraction of a whole frame's bytes and
// decode time -- and here they are put back together. The previous frame is
// only read: Cascades may still be drawing it.
bb::ImageData BrowserController::applyDelta(const bb::ImageData &previous, const DecodedFrame &f)
{
    const int w = previous.width(), h = previous.height();
    bb::ImageData next(bb::PixelFormat::RGBX, w, h);
    const unsigned char *src = previous.pixels();
    unsigned char *dst = next.pixels();
    const int srcLine = previous.bytesPerLine(), dstLine = next.bytesPerLine();
    // Columns whose source is inside the previous frame; the rest (and rows
    // likewise) just scrolled into view and come as rectangles.
    const int x0 = qMax(0, -f.dx), x1 = qMin(w, w - f.dx);
    if (x1 > x0) {
        for (int y = 0; y < h; ++y) {
            const int sy = y + f.dy;
            if (sy < 0 || sy >= h) continue;
            memcpy(dst + y * dstLine + x0 * 4, src + sy * srcLine + (x0 + f.dx) * 4, (x1 - x0) * 4);
        }
    }
    for (int i = 0; i < f.rects.size(); ++i) {
        const DecodedRect &r = f.rects.at(i);
        const int rw = qMin(r.pixels.width(), w - r.x), rh = qMin(r.pixels.height(), h - r.y);
        if (r.x < 0 || r.y < 0 || rw <= 0 || rh <= 0) continue;
        const unsigned char *rs = r.pixels.pixels();
        const int rLine = r.pixels.bytesPerLine();
        for (int y = 0; y < rh; ++y)
            memcpy(dst + (r.y + y) * dstLine + r.x * 4, rs + y * rLine, rw * 4);
    }
    return next;
}

void BrowserController::onFrameDecoded()
{
    QFutureWatcher<DecodedFrame> *watcher = static_cast<QFutureWatcher<DecodedFrame> *>(sender());
    DecodedFrame f = watcher->result();
    const quint32 serial = watcher->property("serial").toUInt();
    f.generation = watcher->property("generation").toUInt();
    watcher->deleteLater();
    --m_decodesInFlight;
    m_decodedFrames.insert(serial, f);
    startDecodes();
    applyDecodedFrames();
}

// Parallel decodes can finish out of order: frames are applied strictly in
// arrival order, and whatever is ready in a row is shown as one update.
void BrowserController::applyDecodedFrames()
{
    bool changed = false;
    while (m_decodedFrames.contains(m_applySerial)) {
        const DecodedFrame f = m_decodedFrames.take(m_applySerial++);
        if (f.skip) continue; // acked when it was superseded
        if (!f.delta) {
            if (f.image.isValid()) {
                m_frame = f.image;
                m_needKeyframe = false;
                changed = true;
            } else {
                // Partial/corrupt: keep the last good one -- but the server's
                // next deltas build on this frame, so ask for a whole one
                // (again, if this was the answer to an earlier request).
                m_needKeyframe = false;
                requestKeyframe();
            }
        } else if (!m_needKeyframe && f.ok && m_frame.isValid()
                   && f.width == m_frame.width() && f.height == m_frame.height()) {
            m_frame = applyDelta(m_frame, f);
            changed = true;
            ++m_statsDeltas;
        } else {
            requestKeyframe();
        }
        if (!m_statsClock.isValid()) m_statsClock.start();
        ++m_statsApplied;
        m_statsBytes += f.bytes;
        m_statsDecodeMs += f.decodeMs;
        sendFrameAck(f.generation);
    }
    if (changed) {
        m_image = QVariant::fromValue(bb::cascades::Image(m_frame));
        emit imageChanged();
        ++m_statsShown;
    }

    if (m_statsClock.isValid() && m_statsClock.elapsed() >= 5000) {
        if (m_statsApplied >= 10) {
            bbportLog(QString("[Feather:frames] %1 fps shown, %2 frames (%3 deltas), avg %4 KB, decode avg %5 ms, %6 dropped")
                      .arg(m_statsShown * 1000.0 / m_statsClock.elapsed(), 0, 'f', 1)
                      .arg(m_statsApplied)
                      .arg(m_statsDeltas)
                      .arg(m_statsBytes / 1024.0 / m_statsApplied, 0, 'f', 1)
                      .arg(m_statsDecodeMs / m_statsApplied)
                      .arg(m_statsDropped));
        }
        m_statsShown = m_statsApplied = m_statsDeltas = m_statsBytes = m_statsDropped = m_statsDecodeMs = 0;
        m_statsClock.invalidate();
    }
}

// A delta that can't be applied (the frame it builds on is missing or broken):
// ignore deltas until the whole frame the server sends in answer arrives.
void BrowserController::requestKeyframe()
{
    if (m_needKeyframe) return;
    m_needKeyframe = true;
    bbportLog("[Feather:frames] delta could not be applied, asking for a whole frame");
#ifdef BBPORT_HAVE_NATIVE_TLS
    if (m_ws && m_wsConnected) m_ws->sendText("{\"type\":\"keyframe\"}");
#endif
}

// Minimized (Active Frame) or covered: the page's frames would only wake the
// phone -- a page with anything moving kept the radio and the CPU up ~12 times
// a second all night (standbymon, 2026-10-14). The server holds them (only the
// newest) at once, and the page's sound stops (the user didn't want a page
// playing on behind their back, 2026-10-28); after kBackgroundCloseMs the
// connection itself is closed (no frames, no pings: the radio can sleep).
// Back in full screen: resume, or reconnect (the server-side session and its
// tabs are still there; onWsOpened asks for a fresh frame).
void BrowserController::onAppBackground()
{
    if (m_background) return;
    m_background = true;
    if (m_pageAudio) m_pageAudio->setMuted(true);
#ifdef BBPORT_HAVE_NATIVE_TLS
    if (m_ws && m_wsConnected) m_ws->sendText("{\"type\":\"pause\"}");
#endif
    m_bgTimer.start(kBackgroundCloseMs);
}

void BrowserController::onBackgroundIdle()
{
    if (!m_background) return;
#ifdef BBPORT_HAVE_NATIVE_TLS
    bbportLog("[Feather:ws] in the background: closing the connection");
    m_bgDisconnected = true;
    m_reconnectTimer.stop();
    if (m_ws) m_ws->disconnectFromHost();
    onWsClosed(QString()); // a clean close: no error, no retry
#endif
}

void BrowserController::onAppForeground()
{
    if (!m_background) return;
    m_background = false;
    m_bgTimer.stop();
    if (m_pageAudio) m_pageAudio->setMuted(false);
#ifdef BBPORT_HAVE_NATIVE_TLS
    if (m_bgDisconnected) {
        m_bgDisconnected = false;
        bbportLog("[Feather:ws] back in full screen: reconnecting");
        connectNow();
        return;
    }
    if (m_ws && m_wsConnected) m_ws->sendText("{\"type\":\"resume\"}");
#endif
}

void BrowserController::sendFrameAck(quint32 generation)
{
    // A frame from a socket that has since closed: its server-side window
    // closed with it, and an ack now would wrongly widen the new one.
    if (generation != m_connGeneration) return;
#ifdef BBPORT_HAVE_NATIVE_TLS
    if (m_ws && m_wsConnected) m_ws->sendText("{\"type\":\"ack\"}");
#endif
}

// youtube.com/watch?v=ID (any subdomain: www, m, music), /shorts/ID,
// /embed/ID, /live/ID, youtu.be/ID -> ID; anything else -> empty.
QString BrowserController::youtubeVideoId(const QUrl &url)
{
    const QString host = url.host().toLower();
    const QString path = url.path();
    QString id;
    if (host == "youtu.be" || host.endsWith(".youtu.be")) {
        id = path.mid(1).section('/', 0, 0);
    } else if (host == "youtube.com" || host.endsWith(".youtube.com")
               || host == "youtube-nocookie.com" || host.endsWith(".youtube-nocookie.com")) {
        if (path == "/watch") {
            id = url.queryItemValue("v");
        } else {
            QRegExp re("^/(shorts|embed|live|v)/([^/]+)");
            if (re.indexIn(path) == 0) id = re.cap(2);
        }
    }
    const QRegExp valid("[A-Za-z0-9_-]{11}");
    return valid.exactMatch(id) ? id : QString();
}

// instagram.com/reel/CODE, /reels/CODE, /tv/CODE (and /p/CODE if
// includePosts), optionally after /<username> -> CODE; anything else -> empty.
QString BrowserController::instagramShortcode(const QUrl &url, bool includePosts)
{
    const QString host = url.host().toLower();
    if (host != "instagram.com" && host != "www.instagram.com" && host != "m.instagram.com") return QString();
    QRegExp re("^/(?:[A-Za-z0-9._]+/)?(reels?|tv|p)/([A-Za-z0-9_-]+)");
    if (re.indexIn(url.path()) != 0) return QString();
    if (re.cap(1) == "p" && !includePosts) return QString();
    return re.cap(2);
}

QString BrowserController::videoKeyFor(const QUrl &url, bool includePosts)
{
    const QString yt = youtubeVideoId(url);
    if (!yt.isEmpty()) return "yt:" + yt;
    const QString ig = instagramShortcode(url, includePosts);
    if (!ig.isEmpty()) return "ig:" + ig;
    return QString();
}

bool BrowserController::playVideoAt(const QUrl &pageUrl, bool fromPage, bool includePosts)
{
    const QString key = videoKeyFor(pageUrl, includePosts);
    if (key.isEmpty()) return false;
    bbportLog("[Feather:video] " + key);
    m_videoKey = key;
    m_videoPageUrl = pageUrl;
    m_videoFromPage = fromPage;
    m_videoFallback.clear();
    if (key.startsWith("yt:")) playYoutube(key.mid(3));
    else playInstagram(pageUrl);
    return true;
}

// A tap landed on a video player in the page (the server's videoAt, see
// server/webvideo.js) and was NOT clicked there: play it here instead. The
// server finds the stream (yt-dlp: ~1800 sites plus plain HTML5 <video>) and
// answers with "videoInfo"; if nothing playable turns up, the tap is sent
// again for the page's own player (onVideoInfo).
void BrowserController::playWebVideo(const QVariantMap &hit)
{
    const QUrl pageUrl(hit.value("pageUrl").toString());
    const QString frameUrl = hit.value("frameUrl").toString();
    // An embedded YouTube/Instagram player: those have their own way in.
    if (!frameUrl.isEmpty() && playVideoAt(QUrl(frameUrl), true, false)) return;
    dropYoutubeRequest(); // a YouTube lookup still in flight must not land on this sheet
    m_webTapX = hit.value("tapX").toInt();
    m_webTapY = hit.value("tapY").toInt();
    const QString target = frameUrl.isEmpty() ? hit.value("pageUrl").toString() : frameUrl;
    bbportLog("[Feather:video] web " + target);
    m_videoKey = "web:" + target;
    m_videoPageUrl = pageUrl;
    m_videoFromPage = true; // the page is on screen already: "Pagina" just closes the player
    m_videoFallback.clear();
    QString host = pageUrl.host();
    if (host.startsWith("www.")) host = host.mid(4);
    emit videoRequested(host);
    m_pendingVideoUrl = target;
#ifdef BBPORT_HAVE_NATIVE_TLS
    if (m_ws && m_wsConnected) {
        QVariantMap body;
        body["type"] = "resolveVideo";
        body["web"] = true;
        body["url"] = target;
        body["pageUrl"] = hit.value("pageUrl").toString();
        bb::data::JsonDataAccess jda;
        QByteArray payload;
        jda.saveToBuffer(QVariant(body), &payload);
        m_ws->sendText(payload);
        return;
    }
#endif
    m_pendingVideoUrl.clear();
    emit videoFailed(tr("Not connected to the server"));
}

QString BrowserController::serverRelative(const QString &url) const
{
    // Anything else (a CDN URL) is passed on exactly as received: a round
    // trip through QUrl could decode the %xx of a signed query string.
    if (!url.startsWith("/")) return url;
    return QString::fromUtf8(QUrl(m_serverUrl).resolved(QUrl(url)).toEncoded());
}

QString BrowserController::takeVideoFallback()
{
    const QString url = m_videoFallback;
    m_videoFallback.clear();
    if (!url.isEmpty()) bbportLog("[Feather:video] trying the other stream");
    return url;
}

void BrowserController::playYoutube(const QString &videoId)
{
    m_pendingVideoUrl.clear(); // an Instagram answer still on its way must not land on this sheet
    m_ytVideoId = videoId;
    m_ytClient = 0;
    emit videoRequested("YouTube");
    requestYoutubePlayer();
}

void BrowserController::playInstagram(const QUrl &pageUrl)
{
    // Resolved by the server (parth-dl, resolveInstagram in server.js); the
    // answer comes back as a "videoInfo" message, see onVideoInfo.
    dropYoutubeRequest(); // a YouTube lookup still in flight must not land on this sheet
    emit videoRequested("Instagram");
    m_pendingVideoUrl = QString::fromUtf8(pageUrl.toEncoded());
#ifdef BBPORT_HAVE_NATIVE_TLS
    if (m_ws && m_wsConnected) {
        QVariantMap body;
        body["type"] = "resolveVideo";
        body["url"] = m_pendingVideoUrl;
        bb::data::JsonDataAccess jda;
        QByteArray payload;
        jda.saveToBuffer(QVariant(body), &payload);
        m_ws->sendText(payload);
        return;
    }
#endif
    m_pendingVideoUrl.clear();
    emit videoFailed(tr("Not connected to the server"));
}

void BrowserController::onVideoInfo(const QVariantMap &map)
{
    if (m_pendingVideoUrl.isEmpty() || map.value("url").toString() != m_pendingVideoUrl) return; // stale or cancelled
    m_pendingVideoUrl.clear();
    if (m_videoKey.startsWith("web:")) {
        if (map.value("ok").toBool()) {
            // Direct from the site's CDN or through the server's relay,
            // whichever the server expects to work; the other one is kept
            // for the player to try if this one doesn't (takeVideoFallback).
            const int height = map.value("height").toInt();
            m_videoFallback = serverRelative(map.value("fallbackUrl").toString());
            const QString url = serverRelative(map.value("videoUrl").toString());
            bbportLog(QString("[Feather:video] web -> %1p %2")
                      .arg(height).arg(map.value("videoUrl").toString().startsWith("/") ? "relay" : "direct"));
            emit videoReady(url, height > 0 ? QString("%1p").arg(height) : QString());
            return;
        }
        // Nothing BB10's player can take (an unknown player, DRM, a VP9-only
        // site...): the tap goes to the page's own player after all.
        bbportLog("[Feather:video] web video not playable: " + map.value("reason").toString());
        emit videoDismissed();
        QVariantMap body;
        body["action"] = "tap";
        body["tapX"] = m_webTapX;
        body["tapY"] = m_webTapY;
        body["noVideo"] = true;
        sendTrackedAction(body, true);
        return;
    }
    if (map.value("ok").toBool()) {
        bbportLog(QString("[Feather:video] instagram -> %1x%2")
                  .arg(map.value("width").toInt()).arg(map.value("height").toInt()));
        // No quality label: parth-dl's width/height are only hints (a
        // "720x1280" format probed 480x854); QML shows the real size.
        emit videoReady(map.value("videoUrl").toString(), QString());
        return;
    }
    // A photo or carousel, a private post, a login wall...: show the page
    // itself instead (the server's own rendering of it).
    bbportLog("[Feather:video] instagram not playable: " + map.value("reason").toString());
    emit videoDismissed();
    if (!m_videoFromPage) navigateTo(m_videoPageUrl, true, false);
}

void BrowserController::dropYoutubeRequest()
{
    if (!m_ytReply) return;
    // Disconnect first: abort() emits finished() right away, which would
    // otherwise be taken for a failed answer and move on to the next client.
    m_ytReply->disconnect(this);
    m_ytReply->abort();
    m_ytReply->deleteLater();
    m_ytReply = 0;
}

void BrowserController::cancelVideo()
{
    dropYoutubeRequest();
    m_pendingVideoUrl.clear();
    m_videoFallback.clear();
}

void BrowserController::requestYoutubePlayer()
{
    dropYoutubeRequest();
    if (!m_ytNam) m_ytNam = new QNetworkAccessManager(this);
    const YtClient &c = kYtClients[m_ytClient];
    QNetworkRequest req(QUrl("https://www.youtube.com/youtubei/v1/player?prettyPrint=false"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("User-Agent", c.userAgent);
    req.setRawHeader("X-YouTube-Client-Name", c.headerId);
    req.setRawHeader("X-YouTube-Client-Version", c.version);
    // m_ytVideoId already passed youtubeVideoId()'s [A-Za-z0-9_-]{11} check:
    // safe to splice into the JSON as is.
    // The phone's language and region (e.g. "it_IT" -> hl "it", gl "IT"),
    // letters only so they splice safely into the JSON.
    const QStringList loc = QLocale().name().remove(QRegExp("[^A-Za-z_]")).split('_');
    QByteArray client(c.clientJson);
    client.insert(client.size() - 1, ",\"hl\":\"" + loc.value(0, "en").toLatin1()
                  + "\",\"gl\":\"" + (loc.value(1).isEmpty() ? QByteArray("US") : loc.value(1).toLatin1()) + "\"");
    const QByteArray body = QByteArray("{\"context\":{\"client\":") + client
            + "},\"videoId\":\"" + m_ytVideoId.toLatin1()
            + "\",\"contentCheckOk\":true,\"racyCheckOk\":true,"
              "\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":\"HTML5_PREF_WANTS\"}}}";
    m_ytReply = m_ytNam->post(req, body);
    connect(m_ytReply, SIGNAL(finished()), this, SLOT(onYoutubePlayerFinished()));
}

void BrowserController::onYoutubePlayerFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) return;
    reply->deleteLater();
    if (reply != m_ytReply) return; // superseded by a newer request
    m_ytReply = 0;

    QString why;
    if (reply->error() != QNetworkReply::NoError) {
        why = reply->errorString();
    } else {
        bb::data::JsonDataAccess jda;
        QByteArray buf = reply->readAll();
        const QVariantMap root = jda.loadFromBuffer(buf).toMap();
        const QVariantMap status = root.value("playabilityStatus").toMap();
        if (status.value("status").toString() != "OK") {
            why = status.value("reason").toString();
            if (why.isEmpty()) why = status.value("status").toString();
        } else {
            // Muxed formats only (audio + video in one MP4): the tallest that
            // fits the screen and comes with a direct URL.
            const QVariantList formats = root.value("streamingData").toMap().value("formats").toList();
            QVariantMap best;
            for (int i = 0; i < formats.size(); ++i) {
                const QVariantMap f = formats.at(i).toMap();
                const int h = f.value("height").toInt();
                if (f.value("url").toString().isEmpty() || h > kYtMaxHeight) continue;
                if (!f.value("mimeType").toString().startsWith("video/mp4")) continue;
                if (best.isEmpty() || h > best.value("height").toInt()) best = f;
            }
            if (!best.isEmpty()) {
                bbportLog(QString("[Feather:video] client %1 -> itag %2 %3")
                          .arg(m_ytClient).arg(best.value("itag").toInt())
                          .arg(best.value("qualityLabel").toString()));
                emit videoReady(best.value("url").toString(), best.value("qualityLabel").toString());
                return;
            }
            why = "nessun formato MP4 riproducibile";
        }
    }
    bbportLog(QString("[Feather:video] client %1 failed: %2").arg(m_ytClient).arg(why));
    if (++m_ytClient < kYtClientCount) {
        requestYoutubePlayer();
        return;
    }
    emit videoFailed(why.isEmpty() ? tr("Video not available") : why);
}

void BrowserController::onWsText(const QByteArray &utf8)
{
    bb::data::JsonDataAccess jda;
    QByteArray buf = utf8;
    QVariantMap map = jda.loadFromBuffer(buf).toMap();
    const QString type = map.value("type").toString();

    if (type == "pong") {
        return;
    }
    if (type == "error") {
        const QString action = map.value("action").toString();
        QString msg = map.value("message").toString();
        if (map.value("status").toInt() == 401) {
            msg = tr("Invalid token: enter it in Settings (top menu; the server shows it when it starts)");
        }
        bbportLog("[Feather] action failed: " + msg);
        setError(msg.isEmpty() ? tr("Network error") : msg);
        if (action != "wheel" && action != "refresh" && !m_pendingQueue.isEmpty()) {
            m_pendingQueue.removeFirst();
        }
        updateLoadingFlag();
        return;
    }
    if (type == "hello") {
        noteServerFeatures(map); // sent on connect, before any page has loaded
        return;
    }
    if (type == "result") {
        applyResult(map);
        return;
    }
    if (type == "videoInfo") {
        onVideoInfo(map);
        return;
    }
}

// What the server offers (its "hello" on connect, and every result).
void BrowserController::noteServerFeatures(const QVariantMap &map)
{
    if (!map.contains("extensionsAvailable")) return;
    const bool has = map.value("extensionsAvailable").toBool();
    if (has == m_serverHasExtensions) return;
    m_serverHasExtensions = has;
    emit extensionsChanged();
}

void BrowserController::applyResult(const QVariantMap &map)
{
    const QString action = map.value("action").toString();
    // wheel/refresh never touch history/tabs/editable state and were never
    // pushed onto m_pendingQueue in the first place (see sendUntracked).
    const bool tracked = (action != "wheel" && action != "refresh" && action != "pauseMedia");
    bool pendingPush = false;
    if (tracked) {
        if (!m_pendingQueue.isEmpty()) pendingPush = m_pendingQueue.takeFirst();
        updateLoadingFlag();
    }

    // --- hit regions ---
    m_links.clear();
    const QVariantList links = map.value("links").toList();
    for (int i = 0; i < links.size(); ++i) {
        QVariantMap l = links.at(i).toMap();
        Link link;
        link.url = QUrl(l.value("url").toString());
        link.rect = QRect(l.value("x").toInt(), l.value("y").toInt(),
                          l.value("w").toInt(), l.value("h").toInt());
        m_links.append(link);
    }

    noteServerFeatures(map);

    m_scrollY = map.value("scrollY").toInt();
    m_pageHeight = map.value("pageHeight").toInt();
    m_title = map.value("title").toString();
    QUrl finalUrl(map.value("url").toString());

    // A popup on top (OAuth sign-in...) is not part of the page history: its
    // URL must not end up behind the back button.
    const bool popup = map.value("depth").toInt() > 1;
    if (popup != m_popup) {
        m_popup = popup;
        emit popupChanged();
    }

    // --- tabs (may swap in a different tab's history instead of the block below) ---
    const bool tabChanged = tracked ? applyTabsUpdate(map.value("tabs").toList(), finalUrl) : false;

    // --- history ---
    if (!tabChanged && tracked && finalUrl.isValid() && !popup) {
        bool sameAsCurrent = (m_histIndex >= 0 && m_history.at(m_histIndex) == finalUrl);
        if (action == "navigate" && !pendingPush && m_histIndex >= 0) {
            // back/forward/reload: keep the entry, just record redirects.
            m_history[m_histIndex] = finalUrl;
        } else if (!sameAsCurrent) {
            // New navigation: cut the forward stack first, then append.
            m_history.resize(m_histIndex + 1);
            m_history.append(finalUrl);
            m_histIndex = m_history.size() - 1;
        }
        emit historyChanged();
    }
    if (tracked && !popup && finalUrl.isValid()) recordVisit(finalUrl, m_title);

    emit pageChanged();

    // A video reached some way navigateTo couldn't intercept (the site's own
    // in-page navigation, a redirect, ...): play it natively too, once per
    // video, and pause the copy the server's Chromium has started playing.
    // Instagram /p/ posts are left alone here: mostly photos.
    if (tracked && !popup) {
        const QString key = videoKeyFor(finalUrl, false);
        if (!key.isEmpty() && key != m_videoKey) {
            QVariantMap body;
            body["action"] = "pauseMedia";
            sendUntracked(body);
            playVideoAt(finalUrl, true, false);
        }
    }

    // A tap on a video player in the page, which the server did not click:
    // BB10's own player takes it (playWebVideo).
    const QVariantMap videoHit = map.value("video").toMap();
    if (action == "tap" && !videoHit.isEmpty()) playWebVideo(videoHit);

    // A file download started on the server (link / button / direct file URL).
    const QVariantMap dl = map.value("download").toMap();
    if (!dl.isEmpty()) {
        emit downloadRequested(dl.value("id").toString(), dl.value("name").toString());
    }

    // Only a tap can have focused a field (the server reports it for taps only).
    const QVariantMap ed = map.value("editable").toMap();
    if (action == "tap" && !ed.isEmpty()) {
        const QString t = ed.value("type").toString();
        if (t == "select-one" || t == "select-multiple") {
            const QVariantList options = ed.value("options").toList();
            m_selectMultiple = ed.value("multiple").toBool();
            m_selectValues.clear();
            m_selectChecked.clear();
            m_selectModel->clear();
            for (int i = 0; i < options.size(); ++i) {
                const QVariantMap o = options.at(i).toMap();
                m_selectValues << o.value("value").toString();
                const bool checked = o.value("selected").toBool();
                m_selectChecked << checked;
                QVariantMap row;
                QString title = o.value("label").toString();
                if (title.trimmed().isEmpty()) title = QString("(vuoto)");
                row["title"] = title;
                row["zchecked"] = checked;
                m_selectModel->append(row);
            }
            emit selectRequested(m_selectMultiple);
        } else if (t == "date" || t == "time" || t == "datetime-local") {
            emit dateRequested(ed.value("value").toString(), ed.value("min").toString(),
                               ed.value("max").toString(), t);
        } else {
            const QVariantMap r = ed.value("rect").toMap();
            const bool known = !r.isEmpty() && r.value("w").toInt() > 0;
            emit editRequested(ed.value("value").toString(), t,
                               ed.value("placeholder").toString(), ed.value("multiline").toBool(),
                               known ? qRound(r.value("x").toDouble() * m_zoom) : -1,
                               known ? qRound(r.value("y").toDouble() * m_zoom) : -1,
                               known ? qRound(r.value("w").toDouble() * m_zoom) : 0,
                               known ? qRound(r.value("h").toDouble() * m_zoom) : 0);
        }
    }
}

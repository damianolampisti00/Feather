#ifndef BROWSERCONTROLLER_HPP_
#define BROWSERCONTROLLER_HPP_

#include <QObject>
#include <QUrl>
#include <QVector>
#include <QVariant>
#include <QString>
#include <QRect>
#include <QPointer>
#include <QTimer>
#include <QNetworkReply>
#include <QFile>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QFutureWatcher>
#include <QElapsedTimer>

#include <bb/ImageData>

#include "tls/wsclient.hpp"

class QNetworkAccessManager;
class AudioOutput;
class AudioRing;
struct OpusDecoder;
namespace bb {
    namespace system { class InvokeManager; class InvokeRequest; }
    namespace cascades { class ArrayDataModel; class DataModel; }
}

// One rectangle of a delta frame, decoded: its pixels go at (x, y).
struct DecodedRect {
    DecodedRect() : x(0), y(0) {}
    int x, y;
    bb::ImageData pixels;
};

// One frame message decoded off the UI thread (see BrowserController::onWsBinary):
// a whole JPEG (a keyframe), or a frames=delta "MBD1" message -- the frame
// before it shifted by the page's scroll, plus the rectangles that changed.
struct DecodedFrame {
    DecodedFrame() : delta(false), ok(false), skip(false), width(0), height(0),
                     dx(0), dy(0), bytes(0), decodeMs(0), generation(0) {}
    bb::ImageData image; // keyframe
    bool delta;
    bool ok;             // delta: header and every rectangle decoded
    bool skip;           // superseded by a keyframe before it was decoded
    int width, height;   // delta: size of the frame it builds on
    int dx, dy;          // delta: new(x, y) = previous(x + dx, y + dy)
    QList<DecodedRect> rects;
    int bytes;
    int decodeMs;
    quint32 generation;
};

// Client side of the Feather protocol (see server/server.js): one
// persistent WebSocket connection per session (see tls/wsclient.hpp for why
// a WebSocket and not one HTTPS request per action -- the short version:
// BBport's mbedTLS transport pays a full TCP+TLS handshake on EVERY request,
// which was the actual reason scrolling/streaming never felt smooth,
// independent of anything about the request/response logic itself). Every
// user action becomes one small JSON text message; the server answers with
// a JSON "result" (or "error") message for it, but the JPEG frames
// themselves arrive as separate, independent binary messages -- some are a
// direct answer to an action, most are pushed automatically by the server's
// CDP screencast as the page repaints (mid-scroll, mid-load, ...), which is
// what gives this the feel of an actual live browser instead of one static
// screenshot per tap. There is no DOM here, only that image + link regions.
//
// All members live on the UI thread: WsClient's signals are always delivered
// there (its actual socket I/O runs on its own QThread but emits via a
// queued cross-thread connection, standard Qt), so bb::cascades::Image is
// only ever built on the UI thread. The one exception is the JPEG decode
// itself (decodeFrame), which runs on QtConcurrent pool threads -- two at a
// time, one per CPU core -- and only
// produces a plain bb::ImageData.
class BrowserController : public QObject
{
    Q_OBJECT
    // bb::cascades::Image wrapped in a QVariant so QML's ImageView can bind it.
    Q_PROPERTY(QVariant image READ image NOTIFY imageChanged)
    Q_PROPERTY(QString currentUrl READ currentUrl NOTIFY pageChanged)
    Q_PROPERTY(QString title READ title NOTIFY pageChanged)
    // Authoritative scroll position / page height from the latest server
    // result. QML reconciles its client-side predicted scrolling against these
    // (see main.qml tile.translationY / onServerScroll). Notified by pageChanged.
    Q_PROPERTY(int scrollY READ scrollY NOTIFY pageChanged)
    Q_PROPERTY(int pageHeight READ pageHeight NOTIFY pageChanged)
    // True while a "tracked" action (see sendTrackedAction) is awaiting its
    // result -- navigate/tap/type/select/tab switch. Scrolling and zoom
    // refreshes never set this: the live frames arriving on their own are
    // already the feedback for those.
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY historyChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY historyChanged)
    Q_PROPERTY(QString zoomLabel READ zoomLabel NOTIFY zoomChanged)
    // Light/dark (Impostazioni): the app's Cascades visual style and, through
    // the server, the pages themselves (see applyView in server.js).
    Q_PROPERTY(bool darkTheme READ darkTheme WRITE setDarkTheme NOTIFY themeChanged)
    // Extensions (Impostazioni): whether this phone's session on the server
    // runs with the server browser's extensions (sent with every action, see
    // setExtensionsMode in server.js), and whether the server has any at all
    // (from its results; false until the first one).
    Q_PROPERTY(bool extensionsEnabled READ extensionsEnabled WRITE setExtensionsEnabled NOTIFY extensionsChanged)
    Q_PROPERTY(bool serverHasExtensions READ serverHasExtensions NOTIFY extensionsChanged)
    // True while the server's front tab is a popup (e.g. Google sign-in).
    Q_PROPERTY(bool popup READ popup NOTIFY popupChanged)
    // Progress / result line of the current file download (empty = nothing to show).
    Q_PROPERTY(QString downloadText READ downloadText NOTIFY downloadChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl WRITE setServerUrl NOTIFY serverUrlChanged)
    // Shared secret shown when the server starts; sent as "Authorization: Bearer" on connect.
    Q_PROPERTY(QString token READ token WRITE setToken NOTIFY tokenChanged)
    // This phone's session on the server (Settings shows it, for server/login.js).
    Q_PROPERTY(QString sessionId READ sessionId CONSTANT)
    // Options of the <select> currently being edited (see selectRequested); each
    // row is {title, checked}. Bind a ListView.dataModel to this directly.
    Q_PROPERTY(bb::cascades::DataModel* selectModel READ selectModel CONSTANT)
    // Saved bookmarks, each row {title, url}. Bind a ListView.dataModel to this.
    Q_PROPERTY(bb::cascades::DataModel* bookmarksModel READ bookmarksModel CONSTANT)
    // Browsing history (the "History" sheet), newest first, each row
    // {title, url}: every web page shown, kept on the phone only. Not the
    // back/forward list (m_history), which is per tab and not saved.
    Q_PROPERTY(bb::cascades::DataModel* visitedModel READ visitedModel CONSTANT)
    // Whether the current page's URL is already bookmarked.
    // Depends on BOTH the bookmark list and the current page: it used to notify
    // only on list changes, so after saving one page every other page still
    // read "Gia salvata" (stale) and nothing more could be added.
    Q_PROPERTY(bool currentBookmarked READ currentBookmarked NOTIFY currentBookmarkedChanged)
    // Open tabs, each row {id, title, url}. Bind a ListView.dataModel to this;
    // compare a row's id to activeTabId (below) to know if it's the active one.
    Q_PROPERTY(bb::cascades::DataModel* tabsModel READ tabsModel CONSTANT)
    Q_PROPERTY(int tabCount READ tabCount NOTIFY tabsChanged)
    // Compare ListItemData.id to this in QML to mark the active row, instead of
    // carrying a per-row bool in tabsModel (a QVariantMap row mixing a bool with
    // string fields made every field of that row read back as "true"/"false" in
    // QML on device -- an ArrayDataModel/QML bridge quirk in this older Qt,
    // confirmed by removing the bool).
    Q_PROPERTY(QString activeTabId READ activeTabId NOTIFY tabsChanged)
    // Whether the live connection to the server is currently up. QML can use
    // this to show a small "reconnecting..." indicator; errorText already
    // carries the reason when it drops.
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
public:
    explicit BrowserController(QObject *parent = 0);
    ~BrowserController();

    QVariant image() const { return m_image; }
    QString currentUrl() const;
    QString title() const { return m_title; }
    int scrollY() const { return m_scrollY; }
    int pageHeight() const { return m_pageHeight; }
    bool loading() const { return m_loading; }
    QString errorText() const { return m_error; }
    bool canGoBack() const { return m_histIndex > 0; }
    bool canGoForward() const { return m_histIndex + 1 < m_history.size(); }
    QString zoomLabel() const;
    bool darkTheme() const { return m_darkTheme; }
    void setDarkTheme(bool dark);
    bool extensionsEnabled() const { return m_extensionsEnabled; }
    void setExtensionsEnabled(bool on);
    bool serverHasExtensions() const { return m_serverHasExtensions; }
    bool popup() const { return m_popup; }
    QString downloadText() const { return m_dlText; }
    QString serverUrl() const { return m_serverUrl; }
    void setServerUrl(const QString &url);
    QString token() const { return m_token; }
    QString sessionId() const { return m_sessionId; }
    void setToken(const QString &token);
    bb::cascades::DataModel* selectModel() const;
    bb::cascades::DataModel* bookmarksModel() const;
    bb::cascades::DataModel* visitedModel() const;
    bool currentBookmarked() const;
    bb::cascades::DataModel* tabsModel() const;
    int tabCount() const { return m_tabCount; }
    QString activeTabId() const { return m_activeTabId; }
    bool connected() const { return m_wsConnected; }

    // Address-bar input: full URL, bare host ("example.com") or a search.
    Q_INVOKABLE void go(const QString &input);
    Q_INVOKABLE void back();
    Q_INVOKABLE void forward();
    Q_INVOKABLE void reload();
    // Steps through the zoom levels and re-renders the current page.
    Q_INVOKABLE void cycleZoom();
    // Touch at (x, y) in tile pixel coordinates.
    Q_INVOKABLE void tapAt(int x, int y);
    // Wheel scroll at tile point (x, y): moves whatever scrollable element is
    // under it (inner containers or the page). Positive dy = content further
    // down. Fire-and-forget over the open connection -- QML can call this on
    // every touch-move sample during a drag with no coalescing of its own;
    // the server debounces a burst of these into one actual scroll action
    // (see scheduleScroll/flushScroll in server.js) while live screencast
    // frames keep arriving throughout, which is what makes a drag track the
    // finger instead of only moving on release.
    Q_INVOKABLE void scrollAt(int x, int y, int dx, int dy);
    // Types text into the field focused on the server (see editRequested).
    // submit=true also presses Enter (search boxes, login forms).
    Q_INVOKABLE void typeText(const QString &text, bool submit);
    // Typing in place (QML's field laid over the page's): the text so far,
    // sent to the page's field a moment after the last key (m_liveTimer), so
    // the site sees it while typing (suggestions); finishLive sends what is
    // still pending at once.
    Q_INVOKABLE void typeLive(const QString &text);
    Q_INVOKABLE void finishLive();
    // The user confirmed / refused the file offered by downloadRequested().
    Q_INVOKABLE void startDownload(const QString &id, const QString &name);
    Q_INVOKABLE void cancelDownload(const QString &id);
    // The user tapped option `index` of the picker opened by selectRequested().
    // Single-choice: applies and submits immediately. Multiple-choice: only
    // toggles that row; confirmSelect() sends the final set.
    Q_INVOKABLE void toggleSelectOption(int index);
    Q_INVOKABLE void confirmSelect();
    // Bookmarks: add/remove the current page, open a saved one (by row index
    // in bookmarksModel), reorder-free simple list.
    Q_INVOKABLE void addBookmark();
    Q_INVOKABLE void removeBookmarkAt(int index);
    Q_INVOKABLE void openBookmarkAt(int index);
    // History: open a row of visitedModel, or forget all of it.
    Q_INVOKABLE void openVisitedAt(int index);
    Q_INVOKABLE void clearVisited();
    // Tabs: open a new blank one, switch to / close the one with this server-side id
    // (see tabsModel's "id" field -- these ids come from the server, unlike bookmarks'
    // plain row indices, since tabs can be reordered/closed server-side too).
    Q_INVOKABLE void newTab();
    // A new tab with chrome://extensions, the server browser's extension list.
    Q_INVOKABLE void openExtensions();
    Q_INVOKABLE void switchTab(const QString &tabId);
    Q_INVOKABLE void closeTab(const QString &tabId);
    // Diagnostics from QML into the on-device log file.
    Q_INVOKABLE void log(const QString &msg);
    // The video sheet was closed before the stream URL arrived: drop the request.
    Q_INVOKABLE void cancelVideo();
    // The player couldn't play the stream it was given: the other way to the
    // same video (direct <-> through the server, see playWebVideo), once, or empty.
    Q_INVOKABLE QString takeVideoFallback();
    // "Pagina" in the video sheet: show the video's own web page too (with
    // the server's copy of the video kept paused).
    Q_INVOKABLE void openVideoPage();

signals:
    void imageChanged();
    void pageChanged();
    void loadingChanged();
    void errorChanged();
    void historyChanged();
    void serverUrlChanged();
    void tokenChanged();
    void zoomChanged();
    void themeChanged();
    void extensionsChanged();
    void popupChanged();
    void downloadChanged();
    void connectedChanged();
    // A click/link turned into a file download on the server: ask the user.
    void downloadRequested(const QString &id, const QString &name);
    // A tap focused a text field on the page: QML types in place over it.
    // x/y/w/h: the field in tile pixels (x < 0: unknown, e.g. a field in
    // another site's frame -- QML asks with a prompt instead).
    void editRequested(const QString &value, const QString &type,
                       const QString &placeholder, bool multiline,
                       int x, int y, int w, int h);
    // A tap focused a <select>: QML should open the option-picker sheet.
    void selectRequested(bool multiple);
    // A tap focused a date/time field: QML should open the matching picker.
    // mode is "date", "time" or "datetime-local"; min/max may be empty.
    void dateRequested(const QString &value, const QString &min, const QString &max, const QString &mode);
    void bookmarksChanged();
    void currentBookmarkedChanged();
    void tabsChanged();
    // A video (YouTube, Instagram) is played in BB10's native player instead
    // of the server-rendered page (see playVideoAt): QML opens the player
    // sheet on videoRequested ("YouTube"/"Instagram"), then gets either the
    // stream URL, an error, or videoDismissed (not a video after all: the
    // page is being shown instead).
    void videoRequested(const QString &source);
    void videoReady(const QString &url, const QString &quality);
    void videoFailed(const QString &message);
    void videoDismissed();

private slots:
    void sendLive();
    // The system asked us to open a link (tapped in an email, another app...).
    void onInvoked(const bb::system::InvokeRequest &request);
    void onDownloadData();
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished();
    void onDownloadStalled();
    void clearDownloadText();

    // Live connection (see tls/wsclient.hpp).
    void onWsOpened();
    void onWsClosed(const QString &reason);
    void onWsText(const QByteArray &utf8);
    void onWsBinary(const QByteArray &data);
    void onFrameDecoded();
    void sendPing();
    void reconnectNow();
    void onYoutubePlayerFinished();

private:
    struct Link {
        QUrl url;
        QRect rect;
    };

    void connectNow();
    // Queued (tracked) actions -- navigate/tap/type/select/tab changes: the
    // ones whose result updates history/tabs/loading/editable state. Sent in
    // order, and the connection is itself ordered+reliable, so a plain FIFO
    // of "did this one ask to push a new history entry" is enough to match
    // each arriving result to the request that caused it -- no id needed.
    void sendTrackedAction(QVariantMap body, bool pushHistory);
    // Frame pipeline (see onWsBinary): up to kMaxDecodes messages decoding at
    // once, the rest queued; applied strictly in arrival order (a delta builds
    // on the frame before it), and each acked to the server once applied or
    // superseded, which is what paces the server's sending.
    static DecodedFrame decodeFrame(const QByteArray &msg);
    static bb::ImageData applyDelta(const bb::ImageData &previous, const DecodedFrame &f);
    void startDecodes();
    void applyDecodedFrames();
    void requestKeyframe();
    void sendFrameAck(quint32 generation);
    // Untracked -- wheel/refresh: fire-and-forget, no history/loading effect,
    // but the resulting image/links/scrollY/title are still applied.
    void sendUntracked(QVariantMap body);
    void playPageAudio(const QByteArray &data);
    void dispatch(const QVariantMap &body);
    void applyResult(const QVariantMap &map);
    void noteServerFeatures(const QVariantMap &map);
    void updateLoadingFlag();

    // interceptVideo false: load even a video URL as a page (openVideoPage);
    // pauseMedia: ...with the server's own copy of the video kept paused.
    void navigateTo(const QUrl &url, bool pushHistory, bool interceptVideo = true, bool pauseMedia = false);
    // A link from another app (onInvoked): new tab, or the native video player.
    void openInvokedUrl(const QUrl &url);
    QUrl m_invokedUrl; // ...one that arrived before the connection was up (cold start)
    void setError(const QString &e);
    static QUrl normalizeInput(const QString &input);
    static QUrl wsUrlFor(const QString &serverUrl, const QString &sessionId);

    // Videos played in BB10's native player instead of the server-rendered
    // page. YouTube: the phone asks InnerTube's /player itself (like
    // YouTubeNative does on this phone), because those stream URLs only work
    // from the IP that requested them -- the server's would break on mobile
    // data. Instagram: the server resolves it (parth-dl, Python), its CDN URLs
    // aren't IP-bound. Any other site: a tap on a video player in the page
    // (playWebVideo), resolved by the server (yt-dlp, server/webvideo.js).
    static QString youtubeVideoId(const QUrl &url);
    // includePosts: /p/ links too, which are often photos (fine for a link
    // the user tapped -- it falls back to the page -- not for every URL the
    // page itself navigates to).
    static QString instagramShortcode(const QUrl &url, bool includePosts);
    // "yt:<id>" / "ig:<shortcode>", or empty if the native player can't take it.
    static QString videoKeyFor(const QUrl &url, bool includePosts);
    // fromPage: the server already shows pageUrl (applyResult found it there).
    bool playVideoAt(const QUrl &pageUrl, bool fromPage, bool includePosts);
    void playYoutube(const QString &videoId);
    void playInstagram(const QUrl &pageUrl);
    void playWebVideo(const QVariantMap &hit);
    void onVideoInfo(const QVariantMap &map);
    void requestYoutubePlayer();
    void dropYoutubeRequest();
    // "/media/..." (the server's relay) -> absolute, on the server we talk to.
    QString serverRelative(const QString &url) const;
    QString m_videoKey;      // last video handed to the native player
    QUrl m_videoPageUrl;     // ...the page it came from (openVideoPage)
    bool m_videoFromPage;    // ...and whether the server already shows that page
    QString m_pendingVideoUrl;  // URL whose "videoInfo" answer we're waiting for (Instagram, web)
    QString m_videoFallback; // see takeVideoFallback
    int m_webTapX;           // the tap that hit a web video, replayed if it can't play here
    int m_webTapY;
    QNetworkAccessManager *m_ytNam; // BB10's own TLS stack: proven against youtube.com by YouTubeNative
    QPointer<QNetworkReply> m_ytReply;
    QString m_ytVideoId;
    int m_ytClient;      // index into the InnerTube client list being tried

    QNetworkAccessManager *m_nam; // downloads only now (see startDownload) -- one-shot, infrequent, HTTP is fine for those
#ifdef BBPORT_HAVE_NATIVE_TLS
    WsClient *m_ws;
#endif
    bool m_wsConnected;
    bool m_loading;
    QList<bool> m_pendingQueue; // one entry per in-flight tracked action, its pushHistory flag
    QTimer m_pingTimer;
    // Page sound (server/pageaudio.js): Opus packets -> m_pageAudioRing ->
    // m_pageAudio, a playback thread (src/audio, shared with Jukeberry).
    AudioRing *m_pageAudioRing;
    AudioOutput *m_pageAudio;
    OpusDecoder *m_opus;
    QByteArray m_opusPcm;
    QTimer m_liveTimer;
    QString m_liveText;
    QTimer m_reconnectTimer;
    // Why the server refused the last handshake (token/lockout), until one succeeds.
    QString m_authError;
    bb::system::InvokeManager *m_invoke;

    void applyAuth(QNetworkRequest &req) const;
    void setDownloadText(const QString &t);
    void finishDownload(const QString &result, bool keepFile);

    QPointer<QNetworkReply> m_dlReply;
    QFile m_dlFile;
    QString m_dlName;
    QString m_dlText;
    QByteArray m_dlErrorBody; // body of a non-2xx answer (JSON {error})
    QTimer m_dlStall;
    QTimer m_dlClear;

    double m_zoom;
    bool m_popup;
    bool m_darkTheme;
    bool m_extensionsEnabled;
    bool m_serverHasExtensions;
    QString m_serverUrl;
    QString m_token;
    QString m_sessionId;

    // Single list + index. A new navigation drops everything after the
    // index *before* appending, so the forward stack never survives (or
    // leaks) a fresh navigation; back/forward only move the index.
    QVector<QUrl> m_history;
    int m_histIndex;

    QVariant m_image;
    bb::ImageData m_frame;    // what m_image shows: the base the next delta builds on
    // Frame pipeline (see onWsBinary): up to kMaxDecodes messages decoding at
    // once, the rest queued, results applied in arrival order.
    struct QueuedFrame {
        QByteArray data;
        quint32 serial;
        quint32 generation;
    };
    int m_decodesInFlight;
    quint32 m_frameSerial;    // arrival order of frames
    quint32 m_applySerial;    // the next one to apply
    quint32 m_connGeneration; // per WebSocket: acks only go back to the socket a frame came on
    QList<QueuedFrame> m_queuedFrames;           // waiting for a free decoder
    QMap<quint32, DecodedFrame> m_decodedFrames; // decoded, waiting for their turn
    bool m_needKeyframe;      // a delta couldn't be applied: ignore deltas until a whole frame
    // Frame-rate diagnostics, logged every few seconds while frames flow.
    QElapsedTimer m_statsClock;
    int m_statsShown;
    int m_statsApplied;
    int m_statsDeltas;
    int m_statsBytes;
    int m_statsDropped;
    int m_statsDecodeMs;
    QString m_title;
    QString m_error;
    QVector<Link> m_links;
    bb::cascades::ArrayDataModel *m_selectModel;
    QStringList m_selectValues;   // option values, same order as m_selectModel rows
    QVector<bool> m_selectChecked;
    bool m_selectMultiple;
    void sendSelectValues(const QStringList &values);

    bb::cascades::ArrayDataModel *m_bookmarksModel;
    void loadBookmarks();
    void saveBookmarks();
    bb::cascades::ArrayDataModel *m_visitedModel;
    void recordVisit(const QUrl &url, const QString &title);
    void saveVisited();

    // Each tab has its own back/forward history; m_history/m_histIndex above are
    // always the ACTIVE tab's. Switching tabs saves the outgoing tab's pair in
    // here and restores the incoming one's (or starts fresh if never seen before).
    struct TabHistory { QVector<QUrl> history; int histIndex; };
    QString m_activeTabId;
    QMap<QString, TabHistory> m_tabHistoryCache;
    bb::cascades::ArrayDataModel *m_tabsModel;
    int m_tabCount;
    bool applyTabsUpdate(const QVariantList &tabsList, const QUrl &finalUrl);
    int m_scrollY;
    int m_pageHeight;
};

#endif /* BROWSERCONTROLLER_HPP_ */

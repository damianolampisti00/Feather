import bb.cascades 1.4
import bb.system 1.2
import bb.multimedia 1.0

Page {
    id: root
    // Address bar (see addressBar): moved only by swipes -- away after
    // swiping up (scrolling down), back after swiping down. Taps, loads and
    // page changes leave it where it is.
    property int barHeight: 90
    property bool barShown: true
    // Finger travel (px) since the scroll direction last changed.
    property real barTravel: 0
    function barAfterScroll(dy) {
        if (urlField.focused) return
        if ((dy > 0) != (barTravel > 0)) barTravel = 0
        barTravel += dy
        if (barTravel > 40) barShown = false
        else if (barTravel < -40) barShown = true
    }
    // History/zoom/settings/bookmarks/tabs live in BB10's own native app menu here
    // (swipe DOWN from the top bezel to open it -- no button needed, every
    // BB10 app's menu works this way): "A Menu is positioned at the top of
    // the screen and can be accessed by swiping downwards from the top of the
    // screen." (bb/cascades/core/menu.h). Confirmed real syntax via the
    // BlackBerry Cascades Cookbook sample app (Menu.qml/CookbookMenu.qml),
    // not guessed.
    Menu.definition: MenuDefinition {
        actions: [
            // Icons: Material Symbols (Apache 2.0, see images/README.txt), see root.icon().
            ActionItem {
                title: qsTr("History")
                imageSource: root.icon("ic_history")
                onTriggered: historySheet.open()
            },
            ActionItem {
                title: qsTr("Zoom: %1").arg(controller.zoomLabel)
                imageSource: root.icon("ic_zoom")
                onTriggered: controller.cycleZoom()
            },
            ActionItem {
                title: qsTr("Settings")
                imageSource: root.icon("ic_settings")
                onTriggered: root.showSettings()
            },
            ActionItem {
                title: controller.currentBookmarked ? qsTr("Bookmarks (saved)") : qsTr("Bookmarks")
                imageSource: root.icon("ic_bookmarks")
                onTriggered: bookmarksSheet.open()
            },
            ActionItem {
                title: qsTr("Tabs (%1)").arg(controller.tabCount)
                imageSource: root.icon("ic_tabs")
                onTriggered: tabsSheet.open()
            }
        ]
    }

    // A tap focused a text field on the page: ask for its text. (Connected in
    // onCreationCompleted -- Cascades has no QtQuick Connections element, and
    // using one here made the whole file fail to load.)
    function showEdit(value, type, placeholder, multiline, x, y, w, h) {
        if (x >= 0 && w > 0) {
            root.startInline(value, type, multiline, x, y, w, h)
            return
        }
        editPrompt.multi = multiline
        editPrompt.title = placeholder.length > 0 ? placeholder : qsTr("Text")
        editPrompt.inputField.defaultText = value
        var mode = SystemUiInputMode.Default
        if (type == "password") mode = SystemUiInputMode.Password
        else if (type == "email") mode = SystemUiInputMode.Email
        else if (type == "url") mode = SystemUiInputMode.Url
        else if (type == "tel" || type == "number") mode = SystemUiInputMode.NumericKeypad
        editPrompt.inputField.inputMode = mode
        editPrompt.confirmButton.label = multiline ? qsTr("OK") : qsTr("Send")
        editPrompt.show()
    }

    // Typing in place: a native field laid exactly over the page's own one
    // (x/y/w/h in tile px). Every change goes to the page while typing
    // (controller.typeLive); Enter submits single-line fields; tapping or
    // scrolling elsewhere ends it.
    function startInline(value, type, multiline, x, y, w, h) {
        inlineEdit.multi = multiline
        var fw = Math.min(720, Math.max(w, 240))
        var fx = Math.max(0, Math.min(x, 720 - fw))
        if (multiline) {
            var ah = Math.min(400, Math.max(h, 140))
            inlineArea.preferredWidth = fw
            inlineArea.preferredHeight = ah
            inlineAreaPos.positionX = fx
            inlineAreaPos.positionY = Math.max(0, Math.min(y, 720 - ah))
            inlineArea.text = value
        } else {
            var mode = TextFieldInputMode.Text
            if (type == "password") mode = TextFieldInputMode.Password
            else if (type == "email") mode = TextFieldInputMode.EmailAddress
            else if (type == "url") mode = TextFieldInputMode.Url
            else if (type == "tel") mode = TextFieldInputMode.PhoneNumber
            else if (type == "number") mode = TextFieldInputMode.NumbersAndPunctuation
            inlineField.inputMode = mode
            inlineField.preferredWidth = fw
            inlineFieldPos.positionX = fx
            inlineFieldPos.positionY = Math.max(0, Math.min(y + h / 2 - 42, 720 - 84))
            inlineField.text = value
        }
        inlineEdit.visible = true
        if (multiline) inlineArea.requestFocus()
        else inlineField.requestFocus()
    }
    function finishInline(submit) {
        if (!inlineEdit.visible) return
        inlineEdit.visible = false
        var text = inlineEdit.multi ? inlineArea.text : inlineField.text
        if (submit) controller.typeText(text, true)
        else controller.finishLive()
    }

    // A file is being offered by the server: ask before pulling it onto the phone.
    function showDownload(id, name) {
        downloadDialog.dlId = id
        downloadDialog.dlName = name
        downloadDialog.show()
    }

    // A tap focused a <select>: the model is already filled by the controller
    // (selectModel); just open the sheet with the right mode.
    function showSelect(multiple) {
        selectSheet.multi = multiple
        selectSheet.open()
    }

    // A tap focused a date/time field: open the matching picker, seeded with
    // the field's current value (and min/max for "date").
    function showDate(value, min, max, mode) {
        dateSheet.mode = mode
        dateSheet.minStr = min
        dateSheet.maxStr = max
        if (mode == "time") {
            // <input type=time> value is just "HH:mm"; DateTimePicker still
            // wants a full date, today's serves only to carry the time part.
            var iso = new Date().toISOString().substring(0, 10) + "T" + (value.length ? value : "00:00")
            picker.value = iso
        } else if (value.length) {
            picker.value = value
        } else {
            picker.value = new Date()
        }
        if (min.length && mode != "time") picker.minimum = min
        if (max.length && mode != "time") picker.maximum = max
        dateSheet.open()
    }

    // Material Symbols icons (images/README.txt): light glyphs for the dark
    // theme, a dark grey version ("_d") for the light one.
    function icon(name) {
        return "asset:///images/" + name + (controller.darkTheme ? "" : "_d") + ".png"
    }

    // ---- Impostazioni (settingsSheet below): every app option lives there ----
    function showSettings() {
        setServerField.text = controller.serverUrl
        setTokenField.text = controller.token
        setThemeDrop.selectedIndex = controller.darkTheme ? 1 : 0
        setExtensionsToggle.checked = controller.extensionsEnabled
        settingsSheet.open()
    }
    function saveSettings() {
        controller.darkTheme = (setThemeDrop.selectedIndex == 1)
        controller.extensionsEnabled = setExtensionsToggle.checked
        // Both setters ignore an unchanged value, so saving without touching
        // the server fields doesn't drop and re-open the connection.
        controller.serverUrl = setServerField.text
        controller.token = setTokenField.text
    }

    // ---- Videos in BB10's own player (videoSheet below) ----
    // Opened as soon as a video is recognized (YouTube, Instagram, or a tap on
    // a page's video player); the stream URL follows once the controller has
    // found it (videoReady) -- or an error does.
    function showVideo(source) {
        videoSheet.source = source
        videoSheet.status = qsTr("Loading…")
        videoSheet.quality = ""
        videoSheet.controlsShown = true
        videoSheet.open()
    }
    function playVideo(url, quality) {
        videoSheet.quality = quality
        videoSheet.status = qsTr("Buffering…")
        startVideo(url)
    }
    function startVideo(url) {
        videoPlayer.sourceUrl = url
        if (!videoNowPlaying.acquired) videoNowPlaying.acquire()
        var err = videoPlayer.play()
        if (err != 0) videoPlayFailed(err) // 0 = MediaError.None
    }
    // A web video comes with a second way to the same stream (straight from
    // the site, or through the server): tried once before giving up.
    function videoPlayFailed(err) {
        controller.log("video player error " + err)
        var fallback = controller.takeVideoFallback()
        if (fallback.length > 0) {
            videoSheet.status = qsTr("Buffering…")
            videoPlayer.stop()
            startVideo(fallback)
            return
        }
        videoSheet.status = qsTr("Playback error (%1)").arg(err)
    }
    function videoError(message) {
        videoSheet.status = qsTr("Can't play the video:\n%1").arg(message)
    }
    // Not a video after all (an Instagram photo post...): the controller
    // is loading the page instead.
    function dismissVideo() {
        videoSheet.close()
    }
    function toggleVideo() {
        if (videoSheet.playing) videoPlayer.pause()
        else videoPlayer.play()
    }
    function seekVideo(deltaMs) {
        var d = Number(videoPlayer.duration)
        var p = Math.max(0, Number(videoPlayer.position) + deltaMs)
        if (d > 0) p = Math.min(p, d - 1000)
        videoPlayer.seekTime(p)
    }
    // Size the video window to the video's own aspect ratio, as large as the
    // 720x720 screen allows, so nothing gets stretched.
    function fitVideo() {
        var w = videoPlayer.videoDimensions.width
        var h = videoPlayer.videoDimensions.height
        if (!(w > 0 && h > 0)) return
        if (h <= w) {
            videoSheet.videoW = 720
            videoSheet.videoH = Math.round(720 * h / w)
        } else {
            videoSheet.videoH = 720
            videoSheet.videoW = Math.round(720 * w / h)
        }
        videoSheet.quality = Math.min(w, h) + "p" // the real size, whatever the site claimed
    }
    function fmtTime(ms) {
        var s = Math.floor(Number(ms) / 1000)
        if (!(s > 0)) s = 0
        var m = Math.floor(s / 60)
        s = s % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }

    onCreationCompleted: {
        controller.editRequested.connect(root.showEdit)
        controller.downloadRequested.connect(root.showDownload)
        controller.selectRequested.connect(root.showSelect)
        controller.dateRequested.connect(root.showDate)
        controller.videoRequested.connect(root.showVideo)
        controller.videoReady.connect(root.playVideo)
        controller.videoFailed.connect(root.videoError)
        controller.videoDismissed.connect(root.dismissVideo)
    }

    Container {
        layout: DockLayout {}
        background: controller.darkTheme ? Color.create("#121212") : Color.White

        // ---- page tile ----
        // Always the whole 720x720 screen (the server renders exactly
        // that, see kTileHeight); while the address bar shows, the page
        // is pushed down under it instead of being re-laid out.
        Container {
            id: pageArea
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            translationY: root.barShown ? root.barHeight : 0
            clipContentToBounds: true
            layout: DockLayout {}

            ImageView {
                id: tile
                image: controller.image
                // Fill: a frame always covers the whole tile. Full-size frames
                // (720x720) are 1:1; the CSS-pixel-sized ones the server sends
                // mid-scroll at zoom != 1 (e.g. 480x420, same aspect) are
                // stretched -- soft while moving, sharp again when it stops.
                scalingMethod: ScalingMethod.Fill
                // Every frame is a NON-asset image (built from WebSocket
                // bytes), and for those the default ImageView loadEffect
                // animates each one as it "loads in" -- the full-screen
                // animation seen on every update, which at frame rate is the
                // strobe. None shows each frame instantly, with no effect.
                loadEffect: ImageViewLoadEffect.None
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Fill
                // Cascades plays an implicit animation whenever a control's
                // properties change -- including the image "appearing" on
                // every new frame. At frame rate those stacked fade/scale
                // animations ARE the strobing. Disable all implicit
                // animations so each frame is applied instantly, like a
                // video surface.
                implicitLayoutAnimationsEnabled: false
                attachedObjects: [
                    ImplicitAnimationController {
                        propertyName: "ALL"
                        enabled: false
                    }
                ]
                // The tile is NEVER shifted locally: it only ever shows the
                // newest server frame, as-is. An earlier build translated it
                // (translationY) by "finger travel minus the server's last
                // confirmed scrollY" to hide latency, but frames and the
                // scrollY number arrive as separate messages, so a frame that
                // already contained the new scroll was shifted AGAIN by the
                // stale gap -- the image overshot the finger and snapped back
                // (the "dragged" look), and scrollY (CSS px) vs the tile (px
                // at zoom z) don't even match units unless z == 1. Build
                // 0.1.1.61 retried it properly (each frame tagged with the
                // scroll it shows, from Page.screencastFrame metadata) and it
                // still looked bad: the leading edge stays blank for a round
                // trip (~170 ms via Cloudflare). Removed for good (2026-10-06).
                property real startX: 0
                property real startY: 0
                property real startWinX: 0
                property real startWinY: 0
                // Position (in window coords) as of the last Move sample that
                // actually sent a scroll delta -- deltas sent to
                // controller.scrollAt() are incremental since THIS point, not
                // since touch-down (see onTouch's Move branch).
                property real lastMoveWinX: 0
                property real lastMoveWinY: 0
                // Finger travel (px) above which a touch is a swipe, not a
                // tap. The Q5 is ~330 dpi, so 20px was under normal finger
                // jitter and every tap was read as a swipe.
                property int swipeThreshold: 50
                // Page scrolls this many px per px of finger travel.
                property real swipeGain: 2.0
                // True once the current drag has crossed swipeThreshold.
                property bool scrolling: false

                // No fade/cross-dissolve here (an earlier version had one): with
                // the server pushing real frames continuously over the open
                // WebSocket connection (see BrowserController/wsclient.cpp) as
                // the page actually repaints, an instant swap on every arriving
                // frame IS what a smooth scroll/video looks like -- a fade would
                // just soften/blur it. Matches how a real frame-streaming client
                // renders (see the Android reference client, BrowserView.java:
                // updateFrame() also just swaps the bitmap and invalidates).

                onTouch: {
                    if (event.touchType == TouchType.Down) {
                        root.finishInline(false) // a touch on the page ends typing in place
                        startX = event.localX
                        startY = event.localY
                        startWinX = event.windowX
                        startWinY = event.windowY
                        scrolling = false
                    } else if (event.touchType == TouchType.Move) {
                        var mx = event.windowX - startWinX
                        var my = event.windowY - startWinY
                        if (!scrolling && (Math.abs(mx) > swipeThreshold || Math.abs(my) > swipeThreshold)) {
                            scrolling = true
                            // Baseline at the CURRENT event, not touch-down:
                            // a real swipe's first reported Move sample can
                            // already be well past swipeThreshold (a fast
                            // flick covers many px before the first touch
                            // event lands), so basing the next delta on
                            // startWinX/Y would dump that entire since-down
                            // distance -- amplified by swipeGain -- as one
                            // lump jump the instant the swipe is recognized.
                            // Basing it on the current position instead
                            // absorbs the threshold crossing as dead zone,
                            // so the image starts tracking smoothly from here.
                            lastMoveWinX = event.windowX
                            lastMoveWinY = event.windowY
                        }
                        if (scrolling) {
                            // Incremental delta since the last sample: the server
                            // coalesces a burst of these into one actual scroll
                            // (see scheduleScroll/flushScroll in server.js), so
                            // there is nothing to batch or throttle here -- every
                            // Move sample can just be sent straight through.
                            var dx = lastMoveWinX - event.windowX
                            var dy = lastMoveWinY - event.windowY
                            lastMoveWinX = event.windowX
                            lastMoveWinY = event.windowY
                            root.barAfterScroll(dy)
                            if (dx !== 0 || dy !== 0) {
                                controller.scrollAt(Math.round(startX), Math.round(startY),
                                                    Math.round(dx * swipeGain), Math.round(dy * swipeGain))
                            }
                        }
                    } else if (event.touchType == TouchType.Up) {
                        if (!scrolling) {
                            var dx = startWinX - event.windowX
                            var dy = startWinY - event.windowY
                            controller.log("tap at " + startX + "," + startY + " travel=" + dx + "," + dy)
                            controller.tapAt(Math.round(startX), Math.round(startY))
                        }
                    }
                }
            }

            Container {
                visible: controller.popup
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Top
                background: Color.Black
                opacity: 0.8
                Label {
                    text: qsTr("Pop-up window: finish signing in, it closes by itself")
                    multiline: true
                    textStyle.color: Color.White
                    textStyle.fontSize: FontSize.XSmall
                }
            }

            // Typing in place (root.startInline): only the field itself takes
            // touches, everything else reaches the page underneath.
            Container {
                id: inlineEdit
                property bool multi: false
                visible: false
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Fill
                layout: AbsoluteLayout {}
                touchPropagationMode: TouchPropagationMode.PassThrough
                overlapTouchPolicy: OverlapTouchPolicy.Allow
                TextField {
                    id: inlineField
                    visible: !inlineEdit.multi
                    layoutProperties: AbsoluteLayoutProperties { id: inlineFieldPos }
                    input.submitKey: SubmitKey.Submit
                    input.onSubmitted: root.finishInline(true)
                    onTextChanging: if (inlineEdit.visible) controller.typeLive(text)
                    onFocusedChanged: if (!focused) root.finishInline(false)
                }
                TextArea {
                    id: inlineArea
                    visible: inlineEdit.multi
                    layoutProperties: AbsoluteLayoutProperties { id: inlineAreaPos }
                    onTextChanging: if (inlineEdit.visible) controller.typeLive(text)
                    onFocusedChanged: if (!focused) root.finishInline(false)
                }
            }

            ActivityIndicator {
                horizontalAlignment: HorizontalAlignment.Center
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: 100
                // controller.loading only covers tracked actions (navigate/tap/
                // type/select/tab changes) -- scrolling and zoom refreshes never
                // set it, since the live frames arriving on their own already
                // are the feedback for those (see browsercontroller.hpp).
                running: controller.loading
                visible: controller.loading
            }


        }

        // ---- address bar ----
        // Slides away on a swipe up, back on a swipe down (root.barShown).
        Container {
            id: addressBar
            preferredHeight: root.barHeight
            maxHeight: root.barHeight
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Top
            translationY: root.barShown ? 0 : -root.barHeight
            rightPadding: 10
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
            background: controller.darkTheme ? Color.create("#222222") : Color.create("#E6E6E6")

            // Bare glyphs (no button frame), dimmed when there is nowhere to go.
            ImageButton {
                defaultImageSource: root.icon("ic_back")
                pressedImageSource: root.icon("ic_back")
                disabledImageSource: controller.darkTheme ? "asset:///images/ic_back_off.png" : "asset:///images/ic_back_d_off.png"
                preferredWidth: 60
                preferredHeight: 60
                leftMargin: 10
                verticalAlignment: VerticalAlignment.Center
                enabled: controller.canGoBack
                onClicked: controller.back()
            }
            ImageButton {
                defaultImageSource: root.icon("ic_forward")
                pressedImageSource: root.icon("ic_forward")
                disabledImageSource: controller.darkTheme ? "asset:///images/ic_forward_off.png" : "asset:///images/ic_forward_d_off.png"
                preferredWidth: 60
                preferredHeight: 60
                leftMargin: 10
                verticalAlignment: VerticalAlignment.Center
                enabled: controller.canGoForward
                onClicked: controller.forward()
            }
            TextField {
                id: urlField
                hintText: controller.connected ? qsTr("URL or search") : qsTr("Connecting to the server...")
                text: controller.currentUrl
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                // Centred in the 90px bar like the arrows (it sat at the top).
                verticalAlignment: VerticalAlignment.Center
                leftMargin: 10
                input.submitKey: SubmitKey.Go
                input.onSubmitted: controller.go(urlField.text)
            }
            ImageButton {
                defaultImageSource: root.icon("ic_reload")
                pressedImageSource: root.icon("ic_reload")
                preferredWidth: 60
                preferredHeight: 60
                leftMargin: 10
                verticalAlignment: VerticalAlignment.Center
                onClicked: controller.reload()
            }
        }

        Container {
            visible: controller.downloadText.length > 0
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Bottom
            background: Color.Black
            opacity: 0.85
            Label {
                text: controller.downloadText
                multiline: true
                textStyle.color: Color.White
                textStyle.fontSize: FontSize.XSmall
            }
        }

        Label {
            visible: controller.errorText.length > 0
            text: controller.errorText
            multiline: true
            textStyle.color: Color.Red
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Bottom
        }
    }

    attachedObjects: [
        // A video (YouTube, Instagram, a page's own player), played by BB10's own player
        // (controller.playVideoAt): hardware H.264 with sound, instead of the
        // server's silent JPEG frames.
        Sheet {
            id: videoSheet
            property string source: ""
            property string status: ""
            property string quality: ""
            property bool playing: false
            property bool controlsShown: true
            property int videoW: 720
            property int videoH: 405
            onClosed: {
                controller.cancelVideo()
                videoPlayer.stop()
                videoPlayer.sourceUrl = ""
                if (videoNowPlaying.acquired) videoNowPlaying.revoke()
                videoSheet.playing = false
            }
            content: Page {
                // Shown/hidden together with the bottom controls (tap on the picture).
                titleBar: TitleBar {
                    title: videoSheet.quality.length > 0 ? videoSheet.source + " " + videoSheet.quality : videoSheet.source
                    visibility: videoSheet.controlsShown ? ChromeVisibility.Visible : ChromeVisibility.Hidden
                    dismissAction: ActionItem {
                        title: qsTr("Close")
                        onTriggered: videoSheet.close()
                    }
                    // The video's own web page (description, comments...), with
                    // the server's copy of the video kept paused.
                    acceptAction: ActionItem {
                        title: qsTr("Page")
                        onTriggered: {
                            controller.openVideoPage()
                            videoSheet.close()
                        }
                    }
                }
                shortcuts: [
                    Shortcut {
                        key: "Space"
                        onTriggered: root.toggleVideo()
                    }
                ]
                Container {
                    background: Color.Black
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    layout: DockLayout {}

                    ForeignWindowControl {
                        id: videoSurface
                        windowId: "FeatherVideo"
                        preferredWidth: videoSheet.videoW
                        preferredHeight: videoSheet.videoH
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        updatedProperties: WindowProperty.Size | WindowProperty.Position | WindowProperty.Visible
                    }
                    Label {
                        text: videoSheet.status
                        visible: videoSheet.status.length > 0
                        multiline: true
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        textStyle.color: Color.White
                        textStyle.textAlign: TextAlign.Center
                    }
                    // A tap on the picture shows/hides the controls.
                    Container {
                        horizontalAlignment: HorizontalAlignment.Fill
                        verticalAlignment: VerticalAlignment.Fill
                        onTouch: {
                            if (event.isUp()) videoSheet.controlsShown = !videoSheet.controlsShown
                        }
                    }
                    Container {
                        visible: videoSheet.controlsShown
                        horizontalAlignment: HorizontalAlignment.Fill
                        verticalAlignment: VerticalAlignment.Bottom
                        background: Color.create("#CC000000")
                        topPadding: 8
                        bottomPadding: 8
                        leftPadding: 8
                        rightPadding: 8
                        layout: StackLayout { orientation: LayoutOrientation.TopToBottom }

                        ProgressIndicator {
                            fromValue: 0
                            toValue: videoPlayer.duration > 0 ? videoPlayer.duration : 1
                            value: videoPlayer.position
                            horizontalAlignment: HorizontalAlignment.Fill
                        }
                        Label {
                            text: root.fmtTime(videoPlayer.position) + " / " + root.fmtTime(videoPlayer.duration)
                            horizontalAlignment: HorizontalAlignment.Center
                            textStyle.color: Color.White
                            textStyle.fontSize: FontSize.Small
                        }
                        Container {
                            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                            horizontalAlignment: HorizontalAlignment.Fill
                            Button {
                                text: "-10s"
                                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                onClicked: root.seekVideo(-10000)
                            }
                            Button {
                                text: videoSheet.playing ? qsTr("Pause") : qsTr("Play")
                                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                onClicked: root.toggleVideo()
                            }
                            Button {
                                text: "+10s"
                                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                                onClicked: root.seekVideo(10000)
                            }
                        }
                    }
                }
                attachedObjects: [
                    MediaPlayer {
                        id: videoPlayer
                        windowId: "FeatherVideo"
                        videoOutput: VideoOutput.PrimaryDisplay
                        onMediaStateChanged: {
                            videoSheet.playing = (mediaState == MediaState.Started)
                            if (videoSheet.playing) videoSheet.status = ""
                            videoNowPlaying.mediaState = mediaState
                        }
                        onVideoDimensionsChanged: root.fitVideo()
                        onPlaybackCompleted: videoSheet.controlsShown = true
                        onError: root.videoPlayFailed(mediaError)
                    },
                    // Audio focus + the system's media keys / incoming-call pause.
                    NowPlayingConnection {
                        id: videoNowPlaying
                        connectionName: "FeatherVideo"
                        onPause: videoPlayer.pause()
                        onPlay: videoPlayer.play()
                    }
                ]
            }
        },
        SystemDialog {
            id: clearHistoryDialog
            title: qsTr("Clear the history?")
            body: qsTr("Every page in the list is forgotten. Bookmarks and logins stay.")
            confirmButton.label: qsTr("Clear")
            cancelButton.label: qsTr("Cancel")
            onFinished: {
                if (value == SystemUiResult.ConfirmButtonSelection) controller.clearVisited()
            }
        },
        // Every page shown, newest first (controller.visitedModel, phone only).
        Sheet {
            id: historySheet
            content: Page {
                titleBar: TitleBar {
                    title: qsTr("History")
                    visibility: ChromeVisibility.Visible
                    dismissAction: ActionItem {
                        title: qsTr("Close")
                        onTriggered: historySheet.close()
                    }
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.TopToBottom }
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill

                    Container {
                        topPadding: ui.du(1)
                        leftPadding: ui.du(2)
                        rightPadding: ui.du(2)
                        bottomPadding: ui.du(1)
                        horizontalAlignment: HorizontalAlignment.Fill
                        Button {
                            text: qsTr("Clear history")
                            horizontalAlignment: HorizontalAlignment.Fill
                            onClicked: clearHistoryDialog.show()
                        }
                    }
                    ListView {
                        dataModel: controller.visitedModel
                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        listItemComponents: [
                            ListItemComponent {
                                type: "item"
                                StandardListItem {
                                    preferredHeight: ui.du(11)
                                    title: ListItemData.title
                                    description: ListItemData.url
                                }
                            }
                        ]
                        onTriggered: {
                            controller.openVisitedAt(indexPath[0])
                            historySheet.close()
                        }
                    }
                }
            }
        },
        SystemDialog {
            id: downloadDialog
            property string dlId: ""
            property string dlName: ""
            title: qsTr("Download the file?")
            body: dlName
            confirmButton.label: qsTr("Download")
            cancelButton.label: qsTr("Cancel")
            onFinished: {
                if (value == SystemUiResult.ConfirmButtonSelection)
                    controller.startDownload(downloadDialog.dlId, downloadDialog.dlName)
                else
                    controller.cancelDownload(downloadDialog.dlId)
            }
        },
        SystemPrompt {
            id: editPrompt
            property bool multi: false
            confirmButton.label: qsTr("Send")
            customButton.label: qsTr("Text only")
            cancelButton.label: qsTr("Cancel")
            onFinished: {
                if (value == SystemUiResult.ConfirmButtonSelection)
                    controller.typeText(editPrompt.inputFieldTextEntry(), !editPrompt.multi)
                else if (value == SystemUiResult.CustomButtonSelection)
                    controller.typeText(editPrompt.inputFieldTextEntry(), false)
            }
        },
        // Every app option (server, token, theme, and whatever comes next),
        // filled in from the controller by root.showSettings().
        Sheet {
            id: settingsSheet
            content: Page {
                titleBar: TitleBar {
                    title: qsTr("Settings")
                    dismissAction: ActionItem {
                        title: qsTr("Cancel")
                        onTriggered: settingsSheet.close()
                    }
                    acceptAction: ActionItem {
                        title: qsTr("Save")
                        onTriggered: {
                            root.saveSettings()
                            settingsSheet.close()
                        }
                    }
                }
                ScrollView {
                    horizontalAlignment: HorizontalAlignment.Fill
                    Container {
                        horizontalAlignment: HorizontalAlignment.Fill
                        bottomPadding: 30

                        Header {
                            title: qsTr("Appearance")
                        }
                        Container {
                            leftPadding: 20
                            rightPadding: 20
                            topPadding: 20
                            DropDown {
                                id: setThemeDrop
                                title: qsTr("Theme")
                                Option {
                                    text: qsTr("Light")
                                }
                                Option {
                                    text: qsTr("Dark")
                                }
                            }
                            Label {
                                text: qsTr("Web pages follow it too: sites with a dark theme of their own use it, the others are darkened automatically.")
                                multiline: true
                                textStyle.fontSize: FontSize.Small
                            }
                        }

                        Header {
                            title: qsTr("Extensions")
                        }
                        Container {
                            leftPadding: 20
                            rightPadding: 20
                            topPadding: 20
                            Container {
                                layout: StackLayout {
                                    orientation: LayoutOrientation.LeftToRight
                                }
                                Label {
                                    text: qsTr("Use the server's extensions")
                                    verticalAlignment: VerticalAlignment.Center
                                    multiline: true
                                    layoutProperties: StackLayoutProperties {
                                        spaceQuota: 1
                                    }
                                }
                                ToggleButton {
                                    id: setExtensionsToggle
                                    enabled: controller.serverHasExtensions
                                    verticalAlignment: VerticalAlignment.Center
                                }
                            }
                            Label {
                                text: controller.serverHasExtensions
                                      ? qsTr("Pages open in a browser of your own on the server, with its Chrome extensions (it uses more of the server's memory). Switching reloads the open page; the other tabs are closed.")
                                      : qsTr("This server has no extensions installed (see server/extensions.js).")
                                multiline: true
                                textStyle.fontSize: FontSize.Small
                            }
                            Button {
                                text: qsTr("Open extensions")
                                visible: setExtensionsToggle.checked && controller.serverHasExtensions
                                horizontalAlignment: HorizontalAlignment.Fill
                                onClicked: {
                                    root.saveSettings()
                                    settingsSheet.close()
                                    controller.openExtensions()
                                }
                            }
                        }

                        Header {
                            title: "Server"
                        }
                        Container {
                            leftPadding: 20
                            rightPadding: 20
                            topPadding: 20
                            Label {
                                text: qsTr("Server URL")
                            }
                            TextField {
                                id: setServerField
                                hintText: qsTr("e.g. https://feather.example.com/api")
                                inputMode: TextFieldInputMode.Url
                            }
                            Label {
                                text: qsTr("Access token (the server shows it when it starts, server/token.txt)")
                                multiline: true
                            }
                            TextField {
                                id: setTokenField
                                hintText: qsTr("Token")
                                inputMode: TextFieldInputMode.Password
                            }
                            Label {
                                text: qsTr("Session ID (for server/login.js): %1").arg(controller.sessionId)
                                multiline: true
                                textStyle.fontSize: FontSize.XSmall
                            }
                        }
                    }
                }
            }
        },
        Sheet {
            id: selectSheet
            property bool multi: false
            content: Page {
                titleBar: TitleBar {
                    title: selectSheet.multi ? qsTr("Choose one or more options") : qsTr("Choose an option")
                    visibility: ChromeVisibility.Visible
                    dismissAction: ActionItem {
                        title: qsTr("Cancel")
                        onTriggered: selectSheet.close()
                    }
                    acceptAction: ActionItem {
                        title: qsTr("Confirm")
                        enabled: selectSheet.multi
                        onTriggered: {
                            controller.confirmSelect()
                            selectSheet.close()
                        }
                    }
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.TopToBottom }
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill

                    ListView {
                        dataModel: controller.selectModel
                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        listItemComponents: [
                            ListItemComponent {
                                type: "item"
                                StandardListItem {
                                    preferredHeight: ui.du(9)
                                    title: ListItemData.title
                                    status: ListItemData.zchecked ? "✓" : ""
                                }
                            }
                        ]
                        onTriggered: {
                            controller.toggleSelectOption(indexPath[0])
                            if (!selectSheet.multi) selectSheet.close()
                        }
                    }
                }
            }
        },
        Sheet {
            id: dateSheet
            property string mode: "date"
            property string minStr: ""
            property string maxStr: ""
            content: Page {
                titleBar: TitleBar {
                    title: dateSheet.mode == "time" ? qsTr("Time") : (dateSheet.mode == "datetime-local" ? qsTr("Date and time") : qsTr("Date"))
                    visibility: ChromeVisibility.Visible
                    dismissAction: ActionItem {
                        title: qsTr("Cancel")
                        onTriggered: dateSheet.close()
                    }
                    acceptAction: ActionItem {
                        title: qsTr("Confirm")
                        onTriggered: {
                            var fmt = dateSheet.mode == "time" ? "hh:mm"
                                     : dateSheet.mode == "datetime-local" ? "yyyy-MM-ddThh:mm"
                                     : "yyyy-MM-dd"
                            controller.typeText(Qt.formatDateTime(picker.value, fmt), false)
                            dateSheet.close()
                        }
                    }
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.TopToBottom }
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill

                    DateTimePicker {
                        id: picker
                        // kind: PickerKind.List still left unset (renders "Expandable"
                        // instead, cosmetic only): "kind" is a BB10 10.3.1 (QML revision 4)
                        // property, and setting it crashed the whole QML document's load
                        // when this file only imported bb.cascades 1.2 (real device,
                        // confirmed by bisection). Now on 1.4, it may well be safe to add
                        // back -- untried, so left alone rather than risking another
                        // black-screen round on something purely cosmetic.
                        mode: dateSheet.mode == "time" ? DateTimePickerMode.Time
                            : dateSheet.mode == "datetime-local" ? DateTimePickerMode.DateTime
                            : DateTimePickerMode.Date
                    }
                }
            }
        },
        Sheet {
            id: bookmarksSheet
            property bool editMode: false
            content: Page {
                titleBar: TitleBar {
                    title: qsTr("Bookmarks")
                    visibility: ChromeVisibility.Visible
                    dismissAction: ActionItem {
                        title: qsTr("Close")
                        onTriggered: bookmarksSheet.close()
                    }
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.TopToBottom }
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill

                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        topPadding: ui.du(1)
                        leftPadding: ui.du(2)
                        rightPadding: ui.du(2)
                        bottomPadding: ui.du(1)
                        Button {
                            text: controller.currentBookmarked ? qsTr("Already saved") : qsTr("Add the current page")
                            imageSource: root.icon("ic_add_bookmark")
                            enabled: !controller.currentBookmarked
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            onClicked: controller.addBookmark()
                        }
                        Button {
                            text: bookmarksSheet.editMode ? qsTr("Done") : qsTr("Remove...")
                            onClicked: bookmarksSheet.editMode = !bookmarksSheet.editMode
                        }
                    }
                    ListView {
                        dataModel: controller.bookmarksModel
                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        listItemComponents: [
                            ListItemComponent {
                                type: "item"
                                StandardListItem {
                                    preferredHeight: ui.du(11)
                                    title: ListItemData.title
                                    description: ListItemData.url
                                    status: bookmarksSheet.editMode ? "✖" : ""
                                }
                            }
                        ]
                        onTriggered: {
                            if (bookmarksSheet.editMode) {
                                controller.removeBookmarkAt(indexPath[0])
                            } else {
                                controller.openBookmarkAt(indexPath[0])
                                bookmarksSheet.close()
                            }
                        }
                    }
                }
            }
        },
        Sheet {
            id: tabsSheet
            property bool editMode: false
            content: Page {
                titleBar: TitleBar {
                    title: qsTr("Tabs")
                    visibility: ChromeVisibility.Visible
                    dismissAction: ActionItem {
                        title: qsTr("Close")
                        onTriggered: tabsSheet.close()
                    }
                }
                Container {
                    layout: StackLayout { orientation: LayoutOrientation.TopToBottom }
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill

                    Container {
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        topPadding: ui.du(1)
                        leftPadding: ui.du(2)
                        rightPadding: ui.du(2)
                        bottomPadding: ui.du(1)
                        Button {
                            text: qsTr("+ New tab")
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            onClicked: { controller.newTab(); tabsSheet.close() }
                        }
                        Button {
                            text: tabsSheet.editMode ? qsTr("Done") : qsTr("Close...")
                            onClicked: tabsSheet.editMode = !tabsSheet.editMode
                        }
                    }
                    ListView {
                        dataModel: controller.tabsModel
                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        listItemComponents: [
                            ListItemComponent {
                                type: "item"
                                StandardListItem {
                                    preferredHeight: ui.du(11)
                                    // No per-row "active" bool in the model data (see
                                    // BrowserController::activeTabId's comment): compare id to
                                    // the controller's own activeTabId property instead.
                                    title: ListItemData.title
                                    description: ListItemData.url
                                    status: tabsSheet.editMode ? "✖" : (ListItemData.zid == controller.activeTabId ? qsTr("● Active") : "")
                                    // Temporary: confirms on-device what each field really
                                    // resolves to, so a third wrong guess is not needed if this
                                    // fix is also off. Remove once tab titles are confirmed correct.
                                    onCreationCompleted: controller.log("tab row: title=[" + ListItemData.title +
                                        "] url=[" + ListItemData.url + "] zid=[" + ListItemData.zid + "]")
                                }
                            }
                        ]
                        onTriggered: {
                            var t = dataModel.data(indexPath)
                            if (tabsSheet.editMode) {
                                controller.closeTab(t.zid)
                            } else {
                                controller.switchTab(t.zid)
                                tabsSheet.close()
                            }
                        }
                    }
                }
            }
        }
    ]
}

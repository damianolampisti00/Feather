import bb.cascades 1.4

// Active Frame: the open tabs (controller.tabsModel), the active one marked
// by a blue bar and bold text. The text size follows the number of tabs so
// they fill the space above the system's bar: title + site for 1-2 tabs,
// the title alone for 3-6 (too long for the line: elided).
// Rebuilt whenever the tabs change and when the app is minimized.
Container {
    id: cover
    background: controller.darkTheme ? Color.create("#121212") : Color.create("#FAFAFA")
    leftPadding: 16
    rightPadding: 12
    topPadding: 12
    // The system's own bar (app name + close) covers the bottom of the frame.
    bottomPadding: 72

    function host(url) {
        var u = String(url).replace(/^[a-z-]+:\/\//i, "");
        var h = u.split("/")[0];
        return h.length > 0 ? h : url;
    }

    function refresh() {
        while (rows.count() > 0) {
            var old = rows.at(0);
            rows.remove(old);
            old.destroy();
        }
        var model = controller.tabsModel;
        var n = model.size();
        // Sizes in pt, measured on the Q5's frame (~312 px above the bar,
        // a line ~9.3 px per pt): 6 lines at 6 pt did not fit, at 5.6 pt do.
        var titlePt = [9, 9, 8.5, 9, 7.5, 6.3, 5.6][Math.min(n, 6)];
        for (var i = 0; i < n; i++) {
            var t = model.value(i);
            var row = rowDef.createObject();
            row.title = t.title;
            row.site = host(t.url);
            row.active = (t.zid == controller.activeTabId);
            row.titlePt = titlePt;
            row.sitePt = Math.max(5, titlePt * 0.8);
            row.withSite = n <= 2;
            row.wrap = n == 1;
            rows.add(row);
        }
    }

    Container {
        id: rows
    }

    attachedObjects: [
        ComponentDefinition {
            id: rowDef
            Container {
                property string title
                property string site
                property bool active
                property real titlePt: 6
                property real sitePt: 5
                property bool withSite: false
                property bool wrap: false
                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                topPadding: withSite ? 4 : 0
                bottomPadding: withSite ? 4 : 0
                // The active tab: a bar on the left.
                Container {
                    preferredWidth: 5
                    minWidth: 5
                    verticalAlignment: VerticalAlignment.Fill
                    background: active ? Color.create("#2E9AFE") : Color.Transparent
                }
                Container {
                    leftPadding: 10
                    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                    Label {
                        text: title
                        topMargin: 0
                        bottomMargin: 0
                        multiline: wrap
                        textStyle.fontSize: FontSize.PointValue
                        textStyle.fontSizeValue: titlePt
                        textStyle.fontWeight: active ? FontWeight.Bold : FontWeight.Normal
                        textStyle.color: controller.darkTheme ? Color.White : Color.Black
                    }
                    Label {
                        text: site
                        visible: withSite && site != title
                        topMargin: 0
                        bottomMargin: 0
                        textStyle.fontSize: FontSize.PointValue
                        textStyle.fontSizeValue: sitePt
                        textStyle.color: Color.create("#8A8A8A")
                    }
                }
            }
        }
    ]

    onCreationCompleted: {
        refresh();
        controller.tabsChanged.connect(cover.refresh);
        controller.themeChanged.connect(cover.refresh);
        Application.thumbnail.connect(cover.refresh);
    }
}

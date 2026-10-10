import bb.cascades 1.4

// Active Frame: the open tabs (controller.tabsModel), the active one marked.
// Rebuilt whenever the tabs change and when the app is minimized.
Container {
    id: cover
    background: controller.darkTheme ? Color.create("#121212") : Color.create("#FAFAFA")
    leftPadding: 16
    rightPadding: 16
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
        for (var i = 0; i < model.size(); i++) {
            var t = model.value(i);
            var row = rowDef.createObject();
            row.title = t.title;
            row.site = host(t.url);
            row.active = (t.zid == controller.activeTabId);
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
                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                topPadding: 4
                bottomPadding: 4
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
                        textStyle.fontSize: FontSize.XSmall
                        textStyle.fontWeight: active ? FontWeight.Bold : FontWeight.Normal
                        textStyle.color: controller.darkTheme ? Color.White : Color.Black
                    }
                    Label {
                        text: site
                        visible: site != title
                        textStyle.fontSize: FontSize.XXSmall
                        textStyle.color: Color.create("#8A8A8A")
                    }
                }
            }
        }
    ]

    onCreationCompleted: {
        refresh();
        controller.tabsChanged.connect(cover.refresh);
        Application.thumbnail.connect(cover.refresh);
    }
}

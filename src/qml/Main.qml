import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import UqNotes

ApplicationWindow {
    id: root
    width: 1400
    height: 900
    visible: true
    color: "#1a1a20"
    title: document.ready ? qsTr("uq notes — %1").arg(document.source) : qsTr("uq notes")

    property bool sidebarVisible: true
    property string lastExportPath: ""

    // Палитра быстрых цветов; произвольный выбирается отдельной кнопкой.
    readonly property var penPalette: [
        "#ff1c2a5a", // тёмно-синие чернила
        "#ff141414", // чёрный
        "#ffc02828", // красный
        "#ff1f7a3d", // зелёный
        "#66f2d024"  // жёлтый маркер, полупрозрачный
    ]
    // Готовые толщины: от волосяной для мелких пометок до маркерной.
    readonly property var penWidths: [0.4, 0.7, 1.0, 1.6, 2.6, 4.5]

    PdfDocumentModel {
        id: document
    }

    InkDocument {
        id: ink
    }

    NoteStore {
        id: store
    }

    NoteExporter {
        id: exporter
        onFinished: function (ok, message) {
            toast.show(ok ? qsTr("Экспортировано: %1").arg(root.lastExportPath) : message)
        }
    }

    OutlineNavigator {
        id: outline
        document: document
        currentPage: canvas.currentPage
    }

    // Инструментация кадров включена всегда: бюджеты плавности - проверяемое
    // требование, а не режим отладки.
    FrameStats {
        id: frameStats
        window: root
    }

    Component.onCompleted: if (initialFile.length > 0) openPath(initialFile)

    // Состояние просмотра запоминается на закрытии, а несохранённые чернила
    // дописываются: закрытие окна не должно стоить работы.
    onClosing: root.persist()

    function persist() {
        if (!store.open)
            return
        store.rememberView(canvas.currentPage, canvas.zoom, canvas.pageMode,
                           canvas.originX, canvas.originY)
        store.rememberTool(canvas.penWidth, canvas.penColor)
        store.flush()
    }

    // Настройки инструмента запоминаются сразу при смене, а не только на
    // закрытии: иначе аварийное завершение вернуло бы старое перо.
    function rememberTool() {
        if (store.open)
            store.rememberTool(canvas.penWidth, canvas.penColor)
    }

    function openPath(path) {
        if (!document.open(path)) {
            errorBanner.show(document.errorString,
                             document.lastOpenResult === PdfDocumentModel.PasswordRequired)
            return
        }
        errorBanner.visible = false
        attachNote(path)
    }

    // Заметка живёт рядом с PDF: открывается, если уже есть, и создаётся при
    // первом штрихе - но создать её сразу дешевле, чем ловить первый штрих.
    function attachNote(pdfPath) {
        const notePath = store.notePathFor(pdfPath)
        if (!store.openOrCreate(notePath, pdfPath)) {
            toast.show(qsTr("Не удалось открыть заметку: %1").arg(store.errorString))
            return
        }
        store.attach(ink)
        store.load(ink)

        if (store.sourceState === NoteStore.Changed) {
            toast.show(qsTr("Исходный PDF изменился с прошлого раза — чернила могут "
                            + "не совпадать с содержимым страниц."))
        } else if (store.sourceState === NoteStore.Missing) {
            toast.show(qsTr("Исходный PDF не найден по сохранённому пути."))
        }

        if (store.hasToolState) {
            canvas.penWidth = store.toolWidth
            canvas.penColor = store.toolColor
        }

        canvas.restoreViewState(store.viewPage, store.viewZoom, store.viewPageMode,
                                store.viewOriginX, store.viewOriginY, store.hasViewState)
    }

    // Сценарий «Битая ссылка»: вид не меняется, пользователь получает
    // уведомление о том, что переход невозможен.
    function goToOutlineEntry(index) {
        if (!outline.isReachable(index)) {
            toast.show(qsTr("Пункт «%1» указывает на страницу, которой нет в документе.")
                       .arg(outline.titleOf(index)))
            return
        }
        canvas.goToPage(outline.pageOf(index))
    }

    header: ToolBar {
        background: Rectangle { color: "#24242c" }
        RowLayout {
            anchors.fill: parent
            spacing: 4

            ToolButton {
                text: qsTr("☰")
                checkable: true
                checked: root.sidebarVisible
                onClicked: root.sidebarVisible = !root.sidebarVisible
            }
            ToolButton {
                text: qsTr("Открыть…")
                onClicked: fileDialog.open()
            }
            ToolSeparator {}
            ToolButton {
                text: qsTr("−")
                enabled: document.ready
                onClicked: canvas.zoomBy(Qt.point(canvas.width / 2, canvas.height / 2), 1 / 1.25)
            }
            Label {
                text: Math.round(canvas.zoom * 100) + "%"
                color: "#d0d0da"
                Layout.minimumWidth: 52
                horizontalAlignment: Text.AlignHCenter
            }
            ToolButton {
                text: qsTr("+")
                enabled: document.ready
                onClicked: canvas.zoomBy(Qt.point(canvas.width / 2, canvas.height / 2), 1.25)
            }
            ToolButton {
                text: qsTr("По странице")
                enabled: document.ready
                onClicked: canvas.fitPage(canvas.currentPage)
            }
            ToolSeparator {}
            ToolButton {
                text: qsTr("Перо")
                checkable: true
                checked: canvas.tool === DocumentCanvas.Pen
                onClicked: canvas.tool = DocumentCanvas.Pen
            }
            ToolButton {
                text: qsTr("Ластик")
                checkable: true
                checked: canvas.tool === DocumentCanvas.Eraser
                onClicked: canvas.tool = DocumentCanvas.Eraser
            }
            // Инструмент на одно касание: ставит вкладыш и сам возвращает
            // прежний инструмент.
            ToolButton {
                text: qsTr("Вкладыш")
                checkable: true
                checked: canvas.tool === DocumentCanvas.Insert
                enabled: document.ready
                ToolTip.text: qsTr("Коснитесь страницы, чтобы вложить листок для записей")
                ToolTip.visible: hovered
                onClicked: canvas.tool = DocumentCanvas.Insert
            }
            ToolButton {
                id: penSettingsButton
                text: qsTr("▾")
                ToolTip.text: qsTr("Толщина и цвет")
                ToolTip.visible: hovered
                onClicked: penPopup.open()

                // Текущее перо видно прямо на кнопке.
                Rectangle {
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: 9
                    width: 22
                    height: Math.max(2, canvas.penWidth * 2)
                    radius: height / 2
                    color: canvas.penColor
                }

                Popup {
                    id: penPopup
                    objectName: "penPopup"
                    y: parent.height
                    width: 260
                    padding: 12
                    background: Rectangle { color: "#2a2a34"; radius: 6; border.color: "#42424f" }

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 10

                        Label { text: qsTr("Толщина"); color: "#9a9aa8"; font.pixelSize: 12 }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Repeater {
                                model: root.penWidths
                                delegate: Rectangle {
                                    required property real modelData
                                    Layout.fillWidth: true
                                    height: 34
                                    radius: 4
                                    color: Math.abs(canvas.penWidth - modelData) < 0.01
                                           ? "#3c5a86" : "#33333f"

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: parent.width - 10
                                        height: Math.max(1, modelData * 2)
                                        radius: height / 2
                                        color: "#e2e2ea"
                                    }
                                    TapHandler {
                                        onTapped: {
                                            canvas.penWidth = modelData
                                            root.rememberTool()
                                        }
                                    }
                                }
                            }
                        }

                        Slider {
                            Layout.fillWidth: true
                            from: canvas.minPenWidthValue
                            to: canvas.maxPenWidthValue
                            value: canvas.penWidth
                            onMoved: {
                                canvas.penWidth = value
                                root.rememberTool()
                            }
                        }

                        Label { text: qsTr("Цвет"); color: "#9a9aa8"; font.pixelSize: 12 }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Repeater {
                                model: root.penPalette
                                // Образец рисуется поверх белого: полупрозрачный
                                // маркер должен выглядеть так, как ляжет на
                                // страницу, а не так, как смотрится на панели.
                                delegate: Rectangle {
                                    required property string modelData
                                    Layout.fillWidth: true
                                    height: 30
                                    radius: 4
                                    color: "#f4f4f6"
                                    readonly property bool chosen:
                                        Qt.colorEqual(canvas.penColor, modelData)
                                    border.width: chosen ? 2 : 1
                                    border.color: chosen ? "#ffffff" : "#55555f"

                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 3
                                        radius: 2
                                        color: modelData
                                    }
                                    TapHandler {
                                        onTapped: {
                                            canvas.penColor = modelData
                                            root.rememberTool()
                                        }
                                    }
                                }
                            }
                        }

                        Button {
                            Layout.fillWidth: true
                            text: qsTr("Другой цвет…")
                            onClicked: colorDialog.open()
                        }
                    }
                }
            }
            ToolButton {
                text: qsTr("Отменить")
                enabled: ink.canUndo
                onClicked: ink.undo()
            }
            ToolButton {
                text: qsTr("Вернуть")
                enabled: ink.canRedo
                onClicked: ink.redo()
            }
            ToolSeparator {}
            ToolButton {
                text: qsTr("Экспорт в PDF…")
                enabled: document.ready && !exporter.busy
                onClicked: {
                    exportDialog.currentFolder = document.folderUrl
                    exportDialog.selectedFile = document.suggestedExportUrl
                    exportDialog.open()
                }
            }
            ToolSeparator {}
            ToolButton {
                text: canvas.pageMode === DocumentCanvas.Single ? qsTr("Разворот")
                                                                : qsTr("Одна страница")
                enabled: document.ready
                onClicked: canvas.pageMode = canvas.pageMode === DocumentCanvas.Single
                    ? DocumentCanvas.Spread : DocumentCanvas.Single
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: document.ready
                color: "#d0d0da"
                text: qsTr("стр. %1 из %2").arg(canvas.currentPage + 1).arg(document.pageCount)
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 300
            visible: root.sidebarVisible && document.ready
            color: "#1f1f27"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6

                Label {
                    text: qsTr("Оглавление")
                    color: "#9a9aa8"
                    font.pixelSize: 12
                }

                // Сценарий «Документ без закладок».
                Label {
                    visible: !outline.hasOutline
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#6e6e80"
                    text: qsTr("В этом документе нет оглавления.")
                }

                TreeView {
                    id: outlineView
                    visible: outline.hasOutline
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: outline.model

                    // Подсветка текущего раздела и раскрытие его родителей при
                    // прокрутке документа.
                    Connections {
                        target: outline
                        function onCurrentChanged() {
                            if (!outline.currentIndex.valid)
                                return
                            outlineView.expandToIndex(outline.currentIndex)
                        }
                    }

                    delegate: Item {
                        id: entry

                        required property int depth
                        required property bool expanded
                        required property bool hasChildren
                        required property int row
                        required property int column
                        required property string title

                        // В делегате TreeView `index` - это порядковый номер,
                        // а не QModelIndex; настоящий индекс берётся у вида.
                        readonly property var modelIndex: outlineView.index(row, column)

                        implicitWidth: outlineView.width
                        implicitHeight: 26

                        Rectangle {
                            anchors.fill: parent
                            radius: 3
                            // Подсветка текущего раздела: пункт, в который
                            // попадает страница, которую сейчас читают.
                            color: outline.currentIndex, outline.isCurrent(entry.modelIndex)
                                   ? "#33507a" : "transparent"
                        }

                        Label {
                            id: twisty
                            x: entry.depth * 16
                            width: 16
                            anchors.verticalCenter: parent.verticalCenter
                            horizontalAlignment: Text.AlignHCenter
                            visible: entry.hasChildren
                            color: "#8a8a9a"
                            text: entry.expanded ? "▾" : "▸"

                            TapHandler {
                                onTapped: outlineView.toggleExpanded(entry.row)
                            }
                        }

                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            x: twisty.x + 18
                            width: outlineView.width - x - 6
                            elide: Text.ElideRight
                            text: entry.title
                            // Пункт, ведущий в никуда, видно сразу.
                            color: outline.isReachable(entry.modelIndex) ? "#d8d8e2" : "#8a6a6a"

                            TapHandler {
                                onTapped: root.goToOutlineEntry(entry.modelIndex)
                            }
                        }
                    }
                }
            }
        }

        DocumentCanvas {
            id: canvas
            objectName: "canvas"
            Layout.fillWidth: true
            Layout.fillHeight: true
            document: document
            ink: ink
            frameStats: frameStats

            // Колесо: зум вокруг курсора либо прокрутка.
            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function (event) {
                    if (event.modifiers & Qt.ControlModifier)
                        canvas.zoomBy(event.position, Math.pow(1.0012, event.angleDelta.y))
                    else
                        canvas.panBy(Qt.point(event.angleDelta.x, event.angleDelta.y))
                }
            }

            // Мышь: левая кнопка рисует, средняя тянет холст.
            DragHandler {
                acceptedDevices: PointerDevice.Mouse
                acceptedButtons: Qt.MiddleButton
                target: null
                property point last
                onActiveChanged: {
                    if (active) {
                        canvas.stopFlick()
                        last = centroid.position
                    } else {
                        canvas.flick(centroid.velocity)
                    }
                }
                onCentroidChanged: {
                    if (!active)
                        return
                    canvas.panBy(Qt.point(centroid.position.x - last.x,
                                          centroid.position.y - last.y))
                    last = centroid.position
                }
            }

            // Пальцевый ввод - только навигация, никогда не чернила.
            // Масштаб и сдвиг идут одним жестом: точка документа под центром
            // жеста остаётся под ним, пока пальцы и сводятся, и смещаются.
            PinchHandler {
                id: pinch
                target: null
                // Пока стилус в контакте, жест не начинается: приоритет у
                // штриха пера - требование «жест во время касания пером».
                enabled: !canvas.stylusActive
                acceptedDevices: PointerDevice.TouchScreen
                minimumPointCount: 2
                maximumPointCount: 2

                property real startZoom: 1
                property point lastCentroid

                onActiveChanged: {
                    if (active) {
                        canvas.stopFlick()
                        startZoom = canvas.zoom
                        lastCentroid = centroid.position
                    } else {
                        canvas.flick(centroid.velocity)
                    }
                }
                onActiveScaleChanged: {
                    if (active)
                        canvas.zoomAt(centroid.position, startZoom * activeScale)
                }
                onCentroidChanged: {
                    if (!active)
                        return
                    canvas.panBy(Qt.point(centroid.position.x - lastCentroid.x,
                                          centroid.position.y - lastCentroid.y))
                    lastCentroid = centroid.position
                }
            }

            // Элементы управления вкладышами: значок свёрнутого, кнопки
            // «свернуть» и «удалить» и ручка размера раскрытого (design.md, D5).
            //
            // Сам слой ввод не принимает: вне элементов перо и мышь проходят к
            // холсту, пальцы - к жестам. Элементы - MouseArea: они забирают
            // нажатие целиком, и касание значка пером не ставит под ним точку.
            // Положение считается привязками к масштабу и сдвигу холста, в том
            // же кадре, что и граф сцены; модель на зуме не меняется.
            Item {
                id: insertOverlay
                objectName: "insertOverlay"
                anchors.fill: parent
                // Ладонь или рукав во время штриха не свернёт вкладыш.
                enabled: !canvas.stylusActive

                component InsertButton: Rectangle {
                    id: button
                    property alias label: glyph.text
                    property string hint
                    signal activated()

                    width: 28
                    height: 28
                    radius: 5
                    color: area.pressed ? "#3c5a86" : "#2a2a34"
                    border.color: "#55555f"

                    Text {
                        id: glyph
                        anchors.centerIn: parent
                        color: "#e2e2ea"
                        font.pixelSize: 15
                    }
                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: button.activated()
                    }
                    ToolTip.text: hint
                    ToolTip.visible: area.containsMouse && hint.length > 0
                }

                Repeater {
                    model: canvas.insertModel

                    delegate: Item {
                        id: insertControls
                        objectName: "insertControls"

                        required property var insertId
                        required property rect docRect
                        required property bool open
                        required property int stack

                        z: stack

                        // Рамка вкладыша в пикселях холста.
                        readonly property real frameX: docRect.x * canvas.zoom + canvas.originX
                        readonly property real frameY: docRect.y * canvas.zoom + canvas.originY
                        readonly property real frameWidth: docRect.width * canvas.zoom
                        readonly property real frameHeight: docRect.height * canvas.zoom

                        // Свёрнутый: значок по центру точки установки. Размер
                        // идёт за масштабом страницы, но в пределах: мельче
                        // по нему не попасть, крупнее он закрывает текст.
                        Rectangle {
                            objectName: "insertAnchor"
                            readonly property real size:
                                Math.max(28, Math.min(112, 56 * canvas.zoom))

                            visible: !insertControls.open
                            x: insertControls.frameX - width / 2
                            y: insertControls.frameY - height / 2
                            width: size
                            height: size
                            radius: size / 2
                            color: anchorArea.pressed ? "#5aaee6" : "#7cc4f2"
                            border.color: "#2f6f9a"
                            border.width: Math.max(1, size / 28)

                            Text {
                                anchors.centerIn: parent
                                text: "✎"
                                color: "#0f3450"
                                font.pixelSize: parent.size * 0.54
                            }
                            MouseArea {
                                id: anchorArea
                                anchors.fill: parent
                                onClicked: canvas.openInsert(insertControls.insertId)
                            }
                        }

                        // Раскрытый: кнопки над правым верхним краем рамки, чтобы
                        // не отнимать места для письма.
                        Row {
                            visible: insertControls.open
                            x: insertControls.frameX + insertControls.frameWidth - width
                            y: insertControls.frameY - height - 3
                            spacing: 3

                            InsertButton {
                                objectName: "insertClose"
                                label: "▁"
                                hint: qsTr("Свернуть")
                                onActivated: canvas.closeInsert(insertControls.insertId)
                            }
                            InsertButton {
                                objectName: "insertDelete"
                                label: "✕"
                                hint: qsTr("Удалить вкладыш")
                                onActivated: canvas.deleteInsert(insertControls.insertId)
                            }
                        }

                        // Ручка размера в правом нижнем углу. Левый верхний угол
                        // стоит на месте, минимальный размер зажимает холст.
                        Rectangle {
                            objectName: "insertResize"
                            visible: insertControls.open
                            width: 22
                            height: 22
                            radius: 4
                            x: insertControls.frameX + insertControls.frameWidth - width / 2
                            y: insertControls.frameY + insertControls.frameHeight - height / 2
                            color: resizeArea.pressed ? "#3c5a86" : "#2a2a34"
                            border.color: "#55555f"

                            Text {
                                anchors.centerIn: parent
                                text: "⤡"
                                color: "#e2e2ea"
                                font.pixelSize: 13
                            }
                            MouseArea {
                                id: resizeArea
                                anchors.fill: parent
                                preventStealing: true
                                onPositionChanged: function (mouse) {
                                    if (!pressed)
                                        return
                                    canvas.resizeInsert(insertControls.insertId,
                                                        mapToItem(canvas, mouse.x, mouse.y))
                                }
                            }
                        }
                    }
                }
            }

            // Панорама одним пальцем с инерцией по завершении жеста.
            DragHandler {
                id: touchPan
                target: null
                enabled: !canvas.stylusActive
                acceptedDevices: PointerDevice.TouchScreen
                minimumPointCount: 1
                maximumPointCount: 1

                property point last

                onActiveChanged: {
                    if (active) {
                        canvas.stopFlick()
                        last = centroid.position
                    } else {
                        canvas.flick(centroid.velocity)
                    }
                }
                onCentroidChanged: {
                    if (!active)
                        return
                    canvas.panBy(Qt.point(centroid.position.x - last.x,
                                          centroid.position.y - last.y))
                    last = centroid.position
                }
            }
        }
    }

    // Сообщение об ошибке открытия. Приложение при этом остаётся
    // работоспособным: ранее открытый документ никуда не девается.
    Rectangle {
        id: errorBanner
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: visible ? bannerRow.implicitHeight + 20 : 0
        visible: false
        color: "#5a2330"

        property bool passwordCase: false

        function show(message, needsPassword) {
            bannerLabel.text = message
            passwordCase = needsPassword === true
            visible = true
        }

        RowLayout {
            id: bannerRow
            anchors { fill: parent; margins: 10 }
            spacing: 10

            Label {
                id: bannerLabel
                Layout.fillWidth: true
                color: "#ffd9df"
                wrapMode: Text.WordWrap
            }
            TextField {
                id: passwordField
                visible: errorBanner.passwordCase
                placeholderText: qsTr("Пароль")
                echoMode: TextInput.Password
                onAccepted: {
                    if (document.open(document.attemptedSource, text)) {
                        errorBanner.visible = false
                        text = ""
                    } else {
                        bannerLabel.text = document.errorString
                    }
                }
            }
            ToolButton {
                text: qsTr("✕")
                onClicked: errorBanner.visible = false
            }
        }
    }

    // Короткие уведомления - например, о непроходимом пункте оглавления.
    Rectangle {
        id: toast
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom
                  bottomMargin: 40 }
        width: toastLabel.implicitWidth + 32
        height: toastLabel.implicitHeight + 20
        radius: 6
        color: "#2c2c38"
        border.color: "#454556"
        visible: false

        function show(message) {
            toastLabel.text = message
            visible = true
            toastTimer.restart()
        }

        Timer {
            id: toastTimer
            interval: 4000
            onTriggered: toast.visible = false
        }

        Label {
            id: toastLabel
            anchors.centerIn: parent
            color: "#e2e2ea"
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !document.ready
        color: "#5a5a6a"
        font.pixelSize: 18
        text: qsTr("Откройте PDF, чтобы начать")
    }

    Shortcut {
        sequences: [StandardKey.Undo]
        onActivated: ink.undo()
    }
    Shortcut {
        sequences: [StandardKey.Redo]
        onActivated: ink.redo()
    }

    // Экспорт: выбор пути и индикация выполнения.
    ColorDialog {
        id: colorDialog
        selectedColor: canvas.penColor
        onAccepted: {
            canvas.penColor = selectedColor
            root.rememberTool()
        }
    }

    FileDialog {
        id: exportDialog
        title: qsTr("Экспортировать в PDF")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "pdf"
        nameFilters: [qsTr("PDF (*.pdf)")]
        onAccepted: {
            root.lastExportPath = exporter.withPdfSuffix(document.toLocalPath(selectedFile))
            root.persist()
            if (!exporter.exportToPdf(document, ink, root.lastExportPath))
                toast.show(qsTr("Не удалось запустить экспорт."))
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: 360
        height: 110
        radius: 8
        color: "#242430"
        border.color: "#3d3d4e"
        visible: exporter.busy

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 12

            Label {
                Layout.fillWidth: true
                color: "#e2e2ea"
                elide: Text.ElideMiddle
                text: exporter.statusText
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: exporter.progress
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Открыть PDF")
        nameFilters: [qsTr("PDF (*.pdf)"), qsTr("Все файлы (*)")]
        onAccepted: root.openPath(document.toLocalPath(selectedFile))
    }

    // Сводка по кадрам показывается только когда замер запрошен явно
    // переменной окружения UQ_FRAME_LOG.
    Rectangle {
        visible: frameStats.logging
        anchors { left: parent.left; bottom: parent.bottom; margins: 10 }
        width: hudText.implicitWidth + 20
        height: hudText.implicitHeight + 14
        radius: 4
        color: "#cc000000"

        Text {
            id: hudText
            anchors.centerIn: parent
            color: frameStats.longFrameCount > 0 ? "#ff9f6a" : "#9de5a0"
            font.family: "monospace"
            font.pixelSize: 11
            text: frameStats.summary + "\ntextures: " + canvas.liveTextureCount
                  + " / retained: " + canvas.retainedPageCount
                  + " / placeholders: " + canvas.placeholderPageCount
                  + " / ink pages: " + canvas.inkGeometryPageCount
        }
    }
}

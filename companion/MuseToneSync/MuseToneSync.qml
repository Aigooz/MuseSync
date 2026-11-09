import QtQuick 2.7
import QtQuick.Window 2.2
import MuseScore 3.0
import "ScoreSnapshot.js" as Snapshot

MuseScore {
    id: root
    title: "MuseSync Bridge"
    description: "Live tempo and time-signature bridge for MuseSync."
    version: "1.4.0"
    requiresScore: false
    property var lastScore: null
    property string scoreSession: ""
    property int sessionCounter: 0
    property string lastCommand: ""
    property var bridge: null
    property var commands: null
    property var metadata: null

    function openSettings(path) {
        var settings;
        // Qt 6 hosts provide MuseScore.Settings; early Qt 5 hosts use Qt.labs.
        // Create lazily so an unavailable module cannot prevent plugin loading.
        try {
            settings = Qt.createQmlObject('import MuseScore 3.0; Settings {}', root);
        } catch (error) {
            settings = Qt.createQmlObject('import QtQuick 2.7; import Qt.labs.settings 1.0; Settings {}', root);
        }
        settings.fileName = path;
        return settings;
    }

    function initializeSettings() {
        if (!bridge) bridge = openSettings(Snapshot.localFilePath(Qt.resolvedUrl("MuseToneSync.ini")));
        if (!commands) commands = openSettings(Snapshot.localFilePath(Qt.resolvedUrl("MuseToneCommands.ini")));
    }

    function updateIdentity() {
        var score = curScore;
        // curScore returns a new wrapper each time; compare the underlying score.
        var sameScore = score && lastScore && score.is(lastScore);
        if (!sameScore && (score || lastScore)) {
            lastScore = score;
            ++sessionCounter;
            scoreSession = score ? Date.now() + "-" + sessionCounter : "";
        }
        return score;
    }

    function publish() {
        if (!bridge) return false;
        var hex = "";
        try {
            // Read curScore on every pass: a cached binding misses tab switches.
            metadata = Snapshot.readScore(updateIdentity(), division);
            hex = Snapshot.encodeSnapshot(metadata, Date.now(), scoreSession);
        } catch (error) {
            console.log("MuseSync Bridge:", String(error));
        }
        bridge.setValue("Snapshot/hex", hex);
        bridge.sync();
        return hex.length > 0;
    }

    Window {
        id: started
        objectName: "startupNotice"
        width: 360
        height: 164
        visible: false
        title: "MuseSync Bridge"
        flags: Qt.Dialog | Qt.WindowStaysOnTopHint
        color: "#f4f5f6"
        property string status: ""
        Text {
            x: 24; y: 22
            text: "MuseSync Bridge 已开启"
            color: "#232527"
            font.pixelSize: 18
        }
        Text {
            x: 24; y: 58; width: parent.width - 48
            text: started.status
            color: "#60666b"
            font.pixelSize: 13
            wrapMode: Text.WordWrap
        }
        Rectangle {
            x: parent.width - 104; y: parent.height - 48
            width: 80; height: 28; radius: 4
            color: "#292d31"
            Text { anchors.centerIn: parent; text: "确定"; color: "white"; font.pixelSize: 13 }
            MouseArea { anchors.fill: parent; onClicked: started.close() }
        }
    }

    function receiveCommand() {
        if (!commands) return;
        try {
            var score = updateIdentity();
            commands.sync();
            var text = String(commands.value("Command/seek", ""));
            if (!text || text === lastCommand) return;
            lastCommand = text; // Consume even rejected requests; never replay an old click.
            var request = Snapshot.parseSeekCommand(text, scoreSession, Date.now());
            if (!request) return;
            var selected = Snapshot.selectPlaybackPosition(score, request.seconds, division);
            if (selected) cmd("play-from-selection");
            commands.setValue("Result/ack", request.id + (selected ? "|ok" : "|no-selection"));
            commands.sync();
        } catch (error) {
            console.log("MuseSync Bridge seek:", String(error));
        }
    }

    function publishSelection() {
        if (!bridge) return;
        try {
            var score = curScore;
            var sameScore = score && lastScore && score.is(lastScore);
            var seconds = sameScore ? Snapshot.selectionSeconds(score, division, metadata) : null;
            bridge.setValue("Position/value", seconds === null ? "" :
                "MP1|" + Date.now() + "|" + scoreSession + "|" + seconds.toFixed(9));
            bridge.sync();
        } catch (error) {
            bridge.setValue("Position/value", "");
            bridge.sync();
        }
    }

    Timer {
        id: selectionPoll
        interval: 150
        repeat: true
        onTriggered: root.publishSelection()
    }

    Timer {
        id: commandPoll
        interval: 80
        repeat: true
        onTriggered: root.receiveCommand()
    }

    Timer {
        id: heartbeat
        interval: 1000
        repeat: true
        onTriggered: root.publish()
    }

    Timer {
        id: changed
        interval: 150
        onTriggered: root.publish()
    }

    onRun: {
        try {
            initializeSettings();
        } catch (error) {
            started.status = "无法启动同步设置组件，请使用官方 MuseScore 4 安装包。";
            started.show();
            return;
        }
        var connected = publish();
        heartbeat.start();
        commandPoll.start();
        selectionPoll.start();
        started.status = connected ? "已连接当前谱面，速度与拍号将自动同步。" :
                curScore ? "同步服务已启动，但未能读取当前谱面，请检查谱面后重试。" :
                "同步服务已启动，打开谱面后将自动连接。";
        started.show();
        started.raise();
        started.requestActivate();
    }
    onScoreStateChanged: changed.restart()
    Component.onDestruction: {
        if (bridge) {
            bridge.setValue("Snapshot/hex", "");
            bridge.setValue("Position/value", "");
            bridge.sync();
        }
    }
}

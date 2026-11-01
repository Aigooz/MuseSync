import QtQuick
import QtQuick.Window
import MuseScore 3.0
import "ScoreSnapshot.js" as Snapshot

MuseScore {
    id: root
    title: "MuseTone Sync"
    description: "Live tempo and time-signature bridge for MuseTone."
    version: "1.2.0"
    requiresScore: false
    property var lastScore: null
    property string scoreSession: ""
    property int sessionCounter: 0
    property string lastCommand: ""

    Settings {
        id: bridge
        fileName: Snapshot.localFilePath(Qt.resolvedUrl("MuseToneSync.ini"))
    }

    Settings {
        id: commands
        fileName: Snapshot.localFilePath(Qt.resolvedUrl("MuseToneCommands.ini"))
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
        var hex = "";
        try {
            // Read curScore on every pass: a cached binding misses tab switches.
            hex = Snapshot.encodeSnapshot(Snapshot.readScore(updateIdentity(), division), Date.now(), scoreSession);
        } catch (error) {
            console.log("MuseTone Sync:", String(error));
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
        title: "MuseTone Sync"
        flags: Qt.Dialog | Qt.WindowStaysOnTopHint
        color: "#f4f5f6"
        property string status: ""
        Text {
            x: 24; y: 22
            text: "MuseTone Sync 已开启"
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
            console.log("MuseTone Sync seek:", String(error));
        }
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
        var connected = publish();
        heartbeat.start();
        commandPoll.start();
        started.status = connected ? "已连接当前谱面，速度与拍号将自动同步。" :
                curScore ? "同步服务已启动，但未能读取当前谱面，请检查谱面后重试。" :
                "同步服务已启动，打开谱面后将自动连接。";
        started.show();
        started.raise();
        started.requestActivate();
    }
    onScoreStateChanged: changed.restart()
    Component.onDestruction: {
        bridge.setValue("Snapshot/hex", "");
        bridge.sync();
    }
}

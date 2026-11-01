"""Headless QML contract test. Mocks score API; never launches MuseScore."""
import ctypes
import os
from pathlib import Path
import tempfile
import time

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PyQt5 import sip
from PyQt5.QtCore import QCoreApplication, QEvent, QObject, QSettings, QUrl, pyqtProperty, pyqtSignal, pyqtSlot
from PyQt5.QtGui import QGuiApplication
from PyQt5.QtQml import QQmlComponent, QQmlEngine, qmlRegisterType
from PyQt5.QtQuick import QQuickItem


class Fraction(QObject):
    numerator = pyqtProperty(int, fget=lambda self: 7, constant=True)
    denominator = pyqtProperty(int, fget=lambda self: 8, constant=True)


class Measure(QObject):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.fraction = Fraction(self)

    timesigNominal = pyqtProperty(QObject, fget=lambda self: self.fraction, constant=True)
    timesigActual = pyqtProperty(QObject, fget=lambda self: self.fraction, constant=True)
    no = pyqtProperty(int, fget=lambda self: 9, constant=True)
    noOffset = pyqtProperty(int, fget=lambda self: 9, constant=True)
    nextMeasure = pyqtProperty(QObject, fget=lambda self: None, constant=True)


class Cursor(QObject):
    tempo = pyqtProperty(float, fget=lambda self: 1.5, constant=True)
    filter = pyqtProperty(int, fget=lambda self: -1, fset=lambda self, value: None)
    track = pyqtProperty(int, fget=lambda self: 0, fset=lambda self, value: None)

    @pyqtSlot(int)
    def rewind(self, mode):
        assert mode == 0

    tick = pyqtProperty(int, fget=lambda self: 0)
    segment = pyqtProperty(QObject, fget=lambda self: self)
    element = pyqtProperty(QObject, fget=lambda self: self)

    @pyqtSlot(result=bool)
    def next(self):
        return False

    @pyqtSlot(bool, result=float)
    def time(self, repeats):
        assert not repeats
        return 0.0


class Selection(QObject):
    @pyqtSlot(QObject, bool, result=bool)
    def select(self, element, add):
        return element is not None and not add


class Score(QObject):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.measure = Measure(self)
        self.cursor = Cursor(self)
        self.selected = Selection(self)

    firstMeasure = pyqtProperty(QObject, fget=lambda self: self.measure, constant=True)
    ntracks = pyqtProperty(int, fget=lambda self: 1, constant=True)
    selection = pyqtProperty(QObject, fget=lambda self: self.selected, constant=True)

    @pyqtSlot(QObject, result=bool, name="is")
    def is_(self, other):
        return other is self

    @pyqtSlot(result=QObject)
    def newCursor(self):
        return self.cursor


class MuseScore(QQuickItem):
    run = pyqtSignal()
    scoreStateChanged = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.score = Score(self)
        self.commands = []

    curScore = pyqtProperty(QObject, fget=lambda self: self.score)
    division = pyqtProperty(int, fget=lambda self: 480, constant=True)

    @pyqtSlot(str)
    def cmd(self, action):
        self.commands.append(action)


class Settings(QObject):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.path = ""
        self.settings = None

    def set_file(self, path):
        self.path = path
        self.settings = QSettings(path, QSettings.IniFormat)

    fileName = pyqtProperty(str, fget=lambda self: self.path, fset=set_file)

    @pyqtSlot(str, "QVariant")
    def setValue(self, key, value):
        self.settings.setValue(key, value)

    @pyqtSlot()
    def sync(self):
        self.settings.sync()

    @pyqtSlot(str, "QVariant", result="QVariant")
    def value(self, key, default):
        return self.settings.value(key, default)


app = QGuiApplication([])
qmlRegisterType(MuseScore, "MuseScore", 3, 0, "MuseScore")
qmlRegisterType(Settings, "MuseScore", 3, 0, "Settings")
engine = QQmlEngine()
source_dir = Path(__file__).resolve().parents[1] / "companion" / "MuseToneSync"
source = (source_dir / "MuseToneSync.qml").read_text(encoding="utf-8")
# Qt 5 needs a version; the installed MuseScore uses Qt 6.
source = source.replace("import QtQuick\n", "import QtQuick 2.7\n")
source = source.replace("import QtQuick.Window\n", "import QtQuick.Window 2.2\n")
for name, kind in [("title", "string"), ("description", "string"),
                   ("version", "string"), ("requiresScore", "bool")]:
    source = source.replace("    " + name + ":", "    property " + kind + " " + name + ":")
with tempfile.TemporaryDirectory(prefix="MuseTone-QML-") as directory:
    target = Path(directory) / "MuseToneSync.ini"
    command_target = Path(directory) / "MuseToneCommands.ini"
    source = source.replace('Snapshot.localFilePath(Qt.resolvedUrl("MuseToneSync.ini"))',
                            '"' + target.as_posix() + '"')
    source = source.replace('Snapshot.localFilePath(Qt.resolvedUrl("MuseToneCommands.ini"))',
                            '"' + command_target.as_posix() + '"')
    component = QQmlComponent(engine)
    component.setData(source.encode("utf-8"), QUrl.fromLocalFile(str(source_dir / "MuseToneSync.qml")))
    assert component.isReady(), "\n".join(e.toString() for e in component.errors())
    plugin = component.create()
    assert plugin is not None, "\n".join(e.toString() for e in component.errors())
    assert plugin.property("title") == "MuseSync Bridge"
    assert plugin.property("version") == "1.3.0"
    plugin.run.emit()
    get_profile = ctypes.windll.kernel32.GetPrivateProfileStringW
    get_profile.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p,
                           ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_wchar_p]

    def read_snapshot():
        output = ctypes.create_unicode_buffer(16384)
        count = get_profile("Snapshot", "hex", "", output, len(output), str(target))
        assert count > 0, "Win32 reader could not read QSettings output"
        return bytes.fromhex(output.value).decode("ascii")

    text = read_snapshot()
    fields = text.split("|")
    assert fields[0] == "MT3" and fields[2:7] == ["90.000000", "7", "8", "", "1,3.500000000,10"]
    notice = plugin.findChild(QObject, "startupNotice")
    assert notice and notice.property("visible"), "successful startup did not show confirmation"
    assert notice.property("title") == "MuseSync Bridge", "startup notice uses an old brand"
    assert "已连接" in notice.property("status")
    notice.setProperty("visible", False)
    assert abs(int(time.time() * 1000) - int(fields[1])) < 5000
    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        app.processEvents()
        time.sleep(0.01)
    assert read_snapshot() != text, "QML heartbeat did not run after onRun returned"
    session = fields[7]
    assert read_snapshot().split("|")[7] == session, "session changed without switching score"
    write_profile = ctypes.windll.kernel32.WritePrivateProfileStringW
    write_profile.argtypes = [ctypes.c_wchar_p] * 4

    def spin(seconds=0.25):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            app.processEvents()
            time.sleep(0.01)

    request = f"MC1|123-1|{int(time.time() * 1000)}|{session}|0.25"
    assert write_profile("Command", "seek", request, str(command_target))
    spin()
    assert plugin.commands == ["play-from-selection"], "command did not start score playback"
    spin()
    assert len(plugin.commands) == 1, "command replayed on timer"
    assert QSettings(str(command_target), QSettings.IniFormat).value("Result/ack") == "123-1|ok"
    request = f"MC1|123-2|{int(time.time() * 1000)}|wrong-tab|0.5"
    assert write_profile("Command", "seek", request, str(command_target))
    spin()
    assert len(plugin.commands) == 1, "accepted wrong project"
    plugin.score = Score(plugin)
    plugin.scoreStateChanged.emit()
    spin()
    assert read_snapshot().split("|")[7] != session, "switching score did not rotate session"
    request = f"MC1|123-3|{int(time.time() * 1000)}|{session}|0.25"
    assert write_profile("Command", "seek", request, str(command_target))
    spin()
    assert len(plugin.commands) == 1, "accepted an old project's click after switching score"
    plugin.score = None
    plugin.scoreStateChanged.emit()
    deadline = time.monotonic() + 0.4
    while time.monotonic() < deadline:
        app.processEvents()
        time.sleep(0.01)
    assert QSettings(str(target), QSettings.IniFormat).value("Snapshot/hex") == "", "closed score retained metadata"
    plugin.run.emit()
    assert notice.property("visible") and "打开谱面" in notice.property("status"), "no-score startup status"
    notice.setProperty("visible", False)
    plugin.score = Score(plugin)
    plugin.scoreStateChanged.emit()
    spin()
    assert read_snapshot().startswith("MT3|"), "dismissing startup notice stopped background sync"
    plugin.deleteLater()
    # Destroy QML windows before the engine/application, including deferred deletes.
    QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
    sip.delete(engine)
    app.processEvents()
print("PASS: headless QML loading, score API contract, heartbeat, score close and QSettings/Win32 interoperability")

"""A desktop's notification server, faked on a private session bus for test_bus.py.

Each call it takes is a JSON line on stdout (after a first line, "ready", once it owns the
name); each JSON line on stdin emits a signal: {"action": [id, key]} or {"closed": [id, reason]}.
"""

import json
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

NAME = "org.freedesktop.Notifications"
PATH = "/org/freedesktop/Notifications"


def report(call):
    print(json.dumps(call), flush=True)


def plain(value):
    if isinstance(value, dbus.Boolean):
        return bool(value)
    if isinstance(value, (dbus.Byte, dbus.Int32, dbus.UInt32, dbus.Int64, dbus.UInt64)):
        return int(value)
    return str(value)


class Server(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, PATH)
        self.next_id = 1

    @dbus.service.method(NAME, in_signature="susssasa{sv}i", out_signature="u")
    def Notify(self, app_name, replaces, icon, summary, body, actions, hints, expire):
        given = int(replaces)
        if not given:
            given = self.next_id
            self.next_id += 1
        report(
            {
                "call": "Notify",
                "app_name": str(app_name),
                "replaces": int(replaces),
                "icon": str(icon),
                "summary": str(summary),
                "body": str(body),
                "actions": [str(each) for each in actions],
                "hints": {str(key): plain(value) for key, value in hints.items()},
                "expire": int(expire),
                "id": given,
            }
        )
        return given

    @dbus.service.method(NAME, in_signature="u")
    def CloseNotification(self, given):
        report({"call": "CloseNotification", "id": int(given)})
        # 3: closed by a call to CloseNotification.
        self.NotificationClosed(given, 3)

    @dbus.service.method(NAME, out_signature="as")
    def GetCapabilities(self):
        return ["actions", "body"]

    @dbus.service.method(NAME, out_signature="ssss")
    def GetServerInformation(self):
        return ["fake", "egraph", "1", "1.2"]

    @dbus.service.signal(NAME, signature="us")
    def ActionInvoked(self, given, key):
        pass

    @dbus.service.signal(NAME, signature="uu")
    def NotificationClosed(self, given, reason):
        pass


def main():
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    server = Server(bus)
    owned = dbus.service.BusName(NAME, bus, do_not_queue=True)
    loop = GLib.MainLoop()

    def command(source, condition):
        line = sys.stdin.readline()
        if not line:
            loop.quit()
            return False
        request = json.loads(line)
        if "action" in request:
            given, key = request["action"]
            server.ActionInvoked(dbus.UInt32(given), key)
        else:
            given, reason = request["closed"]
            server.NotificationClosed(dbus.UInt32(given), dbus.UInt32(reason))
        report({"emitted": request})
        return True

    GLib.io_add_watch(sys.stdin, GLib.IO_IN | GLib.IO_HUP, command)
    print("ready", flush=True)
    loop.run()
    del owned


if __name__ == "__main__":
    main()

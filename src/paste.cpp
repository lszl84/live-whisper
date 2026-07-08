#include "paste.h"

#include <dbus/dbus.h>

#include <cstdio>
#include <cstring>

namespace paste {

bool try_enable_extension()
{
    DBusError error;
    dbus_error_init(&error);

    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!conn) {
        dbus_error_free(&error);
        return false;
    }

    DBusMessage* msg = dbus_message_new_method_call(
        "org.gnome.Shell",
        "/org/gnome/Shell",
        "org.gnome.Shell.Extensions",
        "EnableExtension");

    if (!msg) {
        dbus_connection_unref(conn);
        return false;
    }

    const char* uuid = "live-whisper@local";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &uuid, DBUS_TYPE_INVALID);

    DBusPendingCall* pending = nullptr;
    bool ok = false;
    if (dbus_connection_send_with_reply(conn, msg, &pending, 2000) && pending) {
        dbus_pending_call_block(pending);
        DBusMessage* reply = dbus_pending_call_steal_reply(pending);
        if (reply) {
            dbus_bool_t success = false;
            if (dbus_message_get_args(reply, &error,
                                      DBUS_TYPE_BOOLEAN, &success,
                                      DBUS_TYPE_INVALID)) {
                ok = success;
            }
            dbus_message_unref(reply);
        }
        dbus_pending_call_unref(pending);
    }

    dbus_message_unref(msg);
    dbus_connection_unref(conn);
    return ok;
}

bool is_available()
{
    DBusError error;
    dbus_error_init(&error);

    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!conn) {
        dbus_error_free(&error);
        return false;
    }

    // Check if the extension's bus name is owned
    DBusMessage* msg = dbus_message_new_method_call(
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "NameHasOwner");

    const char* name = "com.livewhisper.Extension";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);

    DBusPendingCall* pending = nullptr;
    bool available = false;

    if (dbus_connection_send_with_reply(conn, msg, &pending, 1000) && pending) {
        dbus_pending_call_block(pending);
        DBusMessage* reply = dbus_pending_call_steal_reply(pending);
        if (reply) {
            dbus_bool_t has_owner = false;
            if (dbus_message_get_args(reply, &error, DBUS_TYPE_BOOLEAN, &has_owner, DBUS_TYPE_INVALID))
                available = has_owner;
            dbus_message_unref(reply);
        }
        dbus_pending_call_unref(pending);
    }

    dbus_message_unref(msg);
    dbus_connection_unref(conn);
    return available;
}

bool type_text(const std::string& text)
{
    if (text.empty()) return true;

    DBusError error;
    dbus_error_init(&error);

    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!conn) {
        std::fprintf(stderr, "paste: dbus_bus_get failed: %s\n", error.message);
        dbus_error_free(&error);
        return false;
    }

    DBusMessage* msg = dbus_message_new_method_call(
        "com.livewhisper.Extension",
        "/com/livewhisper/Extension",
        "com.livewhisper.Extension",
        "TypeText");

    if (!msg) {
        dbus_connection_unref(conn);
        return false;
    }

    const char* text_str = text.c_str();
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &text_str, DBUS_TYPE_INVALID);

    DBusPendingCall* pending = nullptr;
    if (!dbus_connection_send_with_reply(conn, msg, &pending, 5000)) {
        dbus_message_unref(msg);
        dbus_connection_unref(conn);
        return false;
    }

    dbus_message_unref(msg);

    if (!pending) {
        dbus_connection_unref(conn);
        return false;
    }

    dbus_pending_call_block(pending);
    DBusMessage* reply = dbus_pending_call_steal_reply(pending);
    dbus_pending_call_unref(pending);

    bool ok = false;
    if (reply) {
        ok = (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN);
        dbus_message_unref(reply);
    }

    dbus_connection_unref(conn);
    return ok;
}

} // namespace paste

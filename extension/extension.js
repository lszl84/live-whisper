// Live Whisper GNOME Shell Extension
//
// Exposes a D-Bus method so the live-whisper binary can type transcribed
// text into the focused window. Uses Clutter's virtual keyboard device
// to inject keystrokes directly through the compositor.
//
// D-Bus interface:
//   Bus:      session
//   Name:     com.livewhisper.Extension
//   Path:     /com/livewhisper/Extension
//   Method:   TypeText(s: text) -> nothing
//
// Usage from shell:
//   gdbus call --session --dest com.livewhisper.Extension \
//     --object-path /com/livewhisper/Extension \
//     --method com.livewhisper.Extension.TypeText "hello world"

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const DBUS_IFACE_XML = `
<node>
  <interface name="com.livewhisper.Extension">
    <method name="TypeText">
      <arg type="s" name="text" direction="in"/>
    </method>
  </interface>
</node>`;

// Map basic ASCII characters to Clutter keysyms.
// For characters not in this map, we fall back to Unicode hex input
// (Ctrl+Shift+U, hex, space).
function char_to_keysym(c) {
    const code = c.charCodeAt(0);

    // ASCII letters, digits, and common symbols map directly to XKB keysyms
    if (code >= 0x20 && code <= 0x7e) {
        return code; // ASCII printable: keysym == codepoint
    }

    // Newline
    if (c === '\n') return Clutter.KEY_Return;
    if (c === '\t') return Clutter.KEY_Tab;

    return null; // needs Unicode input method
}

// Type a single character using Clutter virtual keyboard
function type_char(vk, c) {
    const keysym = char_to_keysym(c);

    if (keysym !== null) {
        // Direct keysym injection
        vk.notify_keyval(Clutter.CURRENT_TIME, keysym, Clutter.KeyState.PRESSED);
        vk.notify_keyval(Clutter.CURRENT_TIME, keysym, Clutter.KeyState.RELEASED);
    } else {
        // Unicode character: use Ctrl+Shift+U + hex + space
        const code = c.charCodeAt(0);
        const hex = code.toString(16);

        // Press Ctrl+Shift+U
        vk.notify_keyval(Clutter.CURRENT_TIME, Clutter.KEY_Control_L, Clutter.KeyState.PRESSED);
        vk.notify_keyval(Clutter.CURRENT_TIME, Clutter.KEY_Shift_L, Clutter.KeyState.PRESSED);
        vk.notify_keyval(Clutter.CURRENT_TIME, Clutter.KEY_U, Clutter.KeyState.PRESSED);
        vk.notify_keyval(Clutter.CURRENT_TIME, Clutter.KEY_U, Clutter.KeyState.RELEASED);
        vk.notify_keyval(Clutter.CURRENT_TIME, Clutter.KEY_Shift_L, Clutter.KeyState.RELEASED);
        vk.notify_keyval(Clutter.CURRENT_TIME, Clutter.KEY_Control_L, Clutter.KeyState.RELEASED);

        // Type hex digits
        for (const ch of hex) {
            const ks = ch.toUpperCase().charCodeAt(0);
            vk.notify_keyval(Clutter.CURRENT_TIME, ks, Clutter.KeyState.PRESSED);
            vk.notify_keyval(Clutter.CURRENT_TIME, ks, Clutter.KeyState.RELEASED);
        }

        // Press space to commit
        vk.notify_keyval(Clutter.CURRENT_TIME, Clutter.KEY_space, Clutter.KeyState.PRESSED);
        vk.notify_keyval(Clutter.CURRENT_TIME, Clutter.KEY_space, Clutter.KeyState.RELEASED);
    }
}

export default class LiveWhisperExtension extends Extension {
    enable() {
        // Own the D-Bus name
        this._busNameId = Gio.bus_own_name_on_connection(
            Gio.DBus.session,
            'com.livewhisper.Extension',
            Gio.BusNameOwnerFlags.NONE,
            null, null
        );

        // Export the D-Bus interface
        this._dbusImpl = Gio.DBusExportedObject.wrapJSObject(
            DBUS_IFACE_XML, this
        );
        this._dbusImpl.export(Gio.DBus.session, '/com/livewhisper/Extension');
    }

    disable() {
        if (this._dbusImpl) {
            this._dbusImpl.unexport();
            this._dbusImpl = null;
        }
        if (this._busNameId) {
            Gio.bus_unown_name(this._busNameId);
            this._busNameId = null;
        }
    }

    // Called via D-Bus when live-whisper wants to type text
    TypeText(text) {
        try {
            const seat = Clutter.get_default_backend().get_default_seat();
            const vk = seat.create_virtual_device(
                Clutter.InputDeviceType.KEYBOARD_DEVICE
            );

            for (const c of text) {
                type_char(vk, c);
            }
        } catch (e) {
            log(`live-whisper extension: TypeText failed: ${e}`);
        }
    }
}

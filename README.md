# fpkbd

fpkbd is a keyboard driver for ThinkPad T25 keyboard installed into ThinkPad T480.

## Broken Keys

Using the standard atkbd + thinkpad_acpi drivers, the keyboard will work as follows:

| Key             | Expected Behavior  | Actual Behavior    |
| :-------------: | :----------------: | :----------------: |
| Microphone mute | Microphone mute    | *Nothing*          |
| Fn + F1         | *Nothing*          | Mute               |
| Fn + F2         | Screen lock        | Volume down        |
| Fn + F3         | Battery status     | Volume up          |
| Fn + F4         | Suspend            | Microphone mute    |
| Fn + F5         | Touchpad settings  | Brightness down    |
| Fn + F6         | Camera settings    | Brightness up      |
| Fn + Home       | Brightness up      | *Nothing*          |
| Fn + End        | Brightness down    | Insert             |
| Fn + PgUp       | Keyboard backlight | *Nothing*          |
| Fn + Space      | Zoom               | Keyboard backlight |
| Fn + Left       | Previous track     | Home               |
| Fn + Right      | Next track         | End                |
| Fn + Up         | Stop playback      | *Nothing*          |
| Fn + Down       | Play/pause         | *Nothing*          |

Unfortunately, it's impossible to do anything with keys which don't produce any event,
which means that Microphone mute, Fn + Home, Fn + PgUp, Fn + Up, and Fn + Down are
lost forever without patching EC.

## Fixed Keyboard Layout

With fpkbd, the keyboard will work as follows:

| Key             | Expected Behavior  | Actual Behavior    |
| :-------------: | :----------------: | :----------------: |
| Microphone mute | Microphone mute    | *Nothing*          |
| Fn + F1         | *Nothing*          | Microphone mute    |
| Fn + F2         | Screen lock        | Screen lock        |
| Fn + F3         | Battery status     | Battery status     |
| Fn + F4         | Suspend            | Suspend            |
| Fn + F5         | Touchpad settings  | Touchpad settings  |
| Fn + F6         | Camera settings    | Camera settings    |
| Fn + Home       | Brightness up      | *Nothing*          |
| Fn + End        | Brightness down    | *Nothing*          |
| Fn + PgUp       | Keyboard backlight | *Nothing*          |
| Fn + Space      | Zoom               | Keyboard backlight |
| Fn + Left       | Previous track     | Brightness down    |
| Fn + Right      | Next track         | Brightness up      |
| Fn + Up         | Stop playback      | *Nothing*          |
| Fn + Down       | Play/pause         | *Nothing*          |

Since Fn + F1 is unused on T25 keyboard, I decided to remap it to microphone mute button.
I also decided that it will be a good idea to remap brightness control keys to
Fn + Left and Fn + Right. Keyboard backlight is toggled by Fn + Space which is good as well.
The rest keys work as expected.

## Building and Installing

Run `make` to build the kernel module and then run `insmod fpkbd.ko` to install the module.
You can also use DKMS and provided `dkms.conf`.

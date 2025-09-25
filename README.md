Santroller xinput Emulator
====================

This is an xinput dll reimplementation compatible with DirectInput controllers. Think of x360ce without the configuration. For supported instruments, it makes sure to set the correct subtype.

It currently supports santroller instruments, CRKD instruments, and any wired XInput guitars.

##### Usage
- download and extract the [latest release](https://github.com/sanjay900/xinputemu/releases/latest)
- copy all xinputXYZ.dll's next to the game executable and start the game
- on Wine, dumbxinputemu uses evdev and ignores jsdev devices by default. That should work with almost everything,
  but you can control this behavior using `XINPUT_NO_IGNORE_JS` and `XINPUT_IGNORE_EVDEV` environment variables.

##### Building
- grab `mingw-w64-gcc` package or your distro equivalent containing an `i686-w64-mingw32-gcc` binary
- navigate to the directory with `Makefile`
- run `make`, or `make 64bit` for the 64-bit version

##### Credits
Based on https://github.com/kozec/dumbxinputemu

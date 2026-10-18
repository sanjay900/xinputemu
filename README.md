Santroller xinput Emulator
====================

This is an xinput dll reimplementation compatible with DirectInput controllers. Think of x360ce without the configuration. For supported instruments, it makes sure to set the correct subtype.

Supported instruments:
- Santroller Guitars and adapters (works on windows and linux)
- PS3 RB / GH (Works on windows and linux)
- PS4 / PS5 RB (requires a recent linux kernel for linux, just works on windows)
- XInput guitars (works on windows and linux)
- Xbox One Guitars
- CRKD Guitars (use PC mode 8 / legacy pc mode)
- Raphnet wii adapters (just works on windows, requires [hidraw](#raphnet-adapters-on-wine) on wine)
- Raphnet PSX adapters (untested, should just work on windows, requires [hidraw](#raphnet-adapters-on-wine) on wine)

##### Usage
- download and extract the [latest release](https://github.com/sanjay900/xinputemu/releases/latest)
- copy all xinputXYZ.dll's next to the game executable and start the game

##### Raphnet adapters on Wine
Wine's SDL backend maps raphnet adapters as classic controllers, which drops strum up, whammy and the slider.
Give Wine raw HID access to them instead:
- allow your user to read the adapter's hidraw device by installing raphnet's udev rules from [gcn64tools](https://github.com/raphnet/gcn64tools):
  ```
  sudo curl -o /etc/udev/rules.d/70-raphnet.rules https://raw.githubusercontent.com/raphnet/gcn64tools/master/scripts/10-raphnet.rules
  sudo udevadm control --reload && sudo udevadm trigger
  ```
  then replug the adapter
- enable hidraw for raphnet devices in the Wine prefix:
  ```
  wine reg add 'HKLM\System\CurrentControlSet\Services\WineBus\Devices\289b' /v HidRaw /t REG_DWORD /d 1 /f
  ```

##### Building
- grab `mingw-w64-gcc` package or your distro equivalent containing an `i686-w64-mingw32-gcc` binary
- navigate to the directory with `Makefile`
- run `make`, or `make 64bit` for the 64-bit version

##### Credits
Based on https://github.com/kozec/dumbxinputemu

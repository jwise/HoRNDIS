# HoRNDIS(the USB tethering driver for Mac OS X)

**HoRNDIS** (pronounce: *"horrendous"*) is a driver for Mac OS X that allows you to use your Android phone's native [USB tethering](http://en.wikipedia.org/wiki/Tethering) mode to get Internet access.

For more information, [visit the home page for HoRNDIS on my site](http://www.joshuawise.com/horndis).

## Installation

### From Source/Binary

* Get the installation package ([Download](http://www.joshuawise.com/horndis) or [Build](#building-the-source) the installation package from source yourself)
* Run the installation package

### From Homebrew

```sh
brew cask install horndis
sudo kextload /Library/Extensions/HoRNDIS.kext
```

## Configuration

* Assuming that the installation proceeds without errors, after it completes, connect your phone to your Mac by USB.
* Enter the settings menu on your phone.
* In the connections section, below Wi-Fi and Bluetooth:
  * Select "More..."
  * Select "Tethering & portable hotspot"
* Check the "USB tethering" box. It should flash once, and then become solidly checked.

## Uninstallation

* Delete the `HoRNDIS.kext` under `/System/Library/Extensions` and `/Library/Extensions` folder
* Restart your computer

## Building the source

* `git clone` the repository
* Simply running xcodebuild in the checkout directory should be sufficient to build the kext.
* If you wish to package it up, you can run `make` to assemble the package in the build/ directory

## Debugging and Development Notes

This sections contains tips and tricks for developing and debugging the driver.

### USB Device Information

*Mac OS System Menu* -> *About This Mac* -> *System Report* --> *Hardware*/*USB* <br>
Lists all USB devices that OS recognizes. Unfortunately, it does not give USB descriptors.

`lsusb -v`<br>
It prints USB configuration, such as interface and endpoint descriptors. You can print it for all devices or limit the output to specific ones. In order to run this command, you need to install *usbutils*.
* Homebrew users: `brew install mikhailai/misc/usbutils`<br>
  Please *do not* install *lsusb* package from Homebrew Core, it's a different utility with the same name.
* Macports users: `sudo port install usbutils`

### IO Registry

`ioreg -l -r -c IOUSBHostDevice`<br>
This command lists all the Mac OS IO Registry information under all USB devices. Unlike *lsusb*, ioreg tells how Mac OS recognized USB devices and interfaces, and how it matched drivers to these interfaces. The `-r -c IOUSBHostDevice` limits the output to USB devices; to get complete OS registry, please run `ioreg -l`.

### OS Logging

The `LOG(....)` statements, sprinkled throughout the HoRNDIS code, call the `IOLog` functions. On Mac OS *El Capitan* (10.11) and earlier, the log messages go into `/var/log/system.log`. Starting from *Sierra* (10.12), these messages are no longer written to `system.log` and instead can be viewed via:
* *GUI*, using *Console* application, located in *Utilities* folder. You need to enter `process:kernel` in the search box in order to filter the relevant messages.
* *Command Line*, using the `log` command. For example:<br>
  `log show --predicate process==\"kernel\" --start "2018-12-11 22:54:00"`<br>
  The start value needs to be several minutes in the past, so it would not flood the console. Here is a convenient command that prints the messages from the past 3 mintes:<br>
  `log show --predicate process==\"kernel\" --start "$(date -v-3M +'%F %T')"`

I've observed that Mac OS logging is unreliable (especially in *Sierra*). In some cases, the messages may come out garbled (looking like bad multi-threaded code). In other cases, either GUI or Command Line may be missing messages that were emitted. Sometimes, reloading the driver may fix the problem.

### Fork of HoRNDIS(the USB tethering driver for macOS Monterey 12 and Big Sur 11)
- Working on Mac (Apple M1 and Apple Intel)

![Icon](https://user-images.githubusercontent.com/6248794/146649603-cfc7f97a-83eb-4deb-bd64-01b3645fed2f.png)

[Discussion ➤ for HoRNDIS on my Page](https://github.com/chris1111/HoRNDIS/discussions)

[Open Issue ➤ for HoRNDIS](https://github.com/chris1111/HoRNDIS/issues)

** HoRNDIS ** (pronounce: *"horrendous"*) is a driver for Mac OS X that allows you to use your Android phone's native [USB tethering](http://en.wikipedia.org/wiki/Tethering) mode to get Internet access.

For more information, [visit the home page for HoRNDIS on my site](http://www.joshuawise.com/horndis).

## Installation

### From Source/Binary

* Build All Source ✅  You need Xcode installed ([➤ Build](#Building-the-source-and-the-Package) Source and package from CommandLine)

* Signing PKGs ➤ [Signing Package](https://github.com/chris1111/HoRNDIS/blob/master/Signing%20PKGs.pdf)

* Get the installation package [Download Release](https://github.com/chris1111/HoRNDIS/releases/tag/rel9.2)

* Run the installation package
- See  ➤ [Package Installation for Apple Intel](https://user-images.githubusercontent.com/6248794/147510275-100e705a-5471-4550-a393-d76c78e6d8ad.png)
- See  ➤ [Package Installation for Apple M1](https://user-images.githubusercontent.com/6248794/147512939-5bc5ef65-9652-4089-ac98-7f35bf46fbe1.png)



![Screen Shot 1](https://user-images.githubusercontent.com/6248794/147509998-e071e5da-ddf3-46fa-9837-807394396ed8.png)

![Screen Shot 2](https://user-images.githubusercontent.com/6248794/147510000-4dd26464-9d0f-4905-b036-46552b2b25f8.png)


* `The Package works only for macOS12 and macOS11 Do not use the Package(.pkg) in another version of macOS`

* Configuration: `SIP security and Gatekeeper must be Disable`

* Assuming SIP Security is Disable; that the installation proceeds without errors. 
* Go to System Preferences → Security & Privacy and approve the HoRNDIS kernel extension. then restart macos.
* Connect your phone to your Mac by USB.
* Enter the settings menu on your phone.
* In the connections section, below Wi-Fi and Bluetooth:
  * Select "More..."
  * Select "Tethering & portable hotspot"
* Check the "USB tethering" box. It should flash once, and then become solidly checked.

## HoRNDIS with SIP Enable [Download Release](https://github.com/chris1111/HoRNDIS/releases/tag/SIP-Enable)

## Uninstallation

* Delete the `HoRNDIS.kext` under `/Library/Extensions` folder
* Restart your computer

## Building the source and the Package

```bash
git clone --recursive https://github.com/chris1111/HoRNDIS.git
```

```bash
cd $HOME/HoRNDIS
```

```bash
make
```
### Result: Building the source and the Package ⇩
![Screen Shot](https://user-images.githubusercontent.com/6248794/147509856-88702279-7f4f-45ef-a613-3ad55335b948.png)

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

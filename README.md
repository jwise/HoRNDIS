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
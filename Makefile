# For the building the driver for MacOS 10.11+, the preferred
# XCode version is 7.3.1.
#
# You can do this by downloading Xcode 7.3.1 as a dmg from Apple, then
# copying the contents into /Applications as Xcode-7.3.1.
# If you also want to build from "release_pre_10_11", you may want to
# also download "Xcode-4.3.3", and copy "MacOSX10.6.sdk" from it to
# SDK directory under:
# /Applications/Xcode-7.3.1.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/

XCODE_VER = 7.3.1
XCODEBUILD ?= $(wildcard /Applications/Xcode*$(XCODE_VER).app/Contents/Developer/usr/bin/xcodebuild)

ifeq (,$(XCODEBUILD))
    $(error Cannot find Xcode $(XCODE_VER). Please download it from: \
        "https://developer.apple.com/download" and install as \
        /Applications/Xcode-$(XCODE_VER)/)
endif

all: build/Release/HoRNDIS.kext build/_complete

clean:
	rm -rf build

# We now sign as part of the xcodebuild process.
build/Release/HoRNDIS.kext: $(wildcard *.cpp *.h *.plist HoRNDIS.xcodeproj/* *.lproj/*)
	$(XCODEBUILD) -project HoRNDIS.xcodeproj

build/root: build/Release/HoRNDIS.kext
	rm -rf build/root
	mkdir -p build/root/Library/Extensions
	cp -R build/Release/HoRNDIS.kext build/root/Library/Extensions/

build/HoRNDIS-kext.pkg: build/root
	pkgbuild --identifier com.joshuawise.kexts.HoRNDIS --root $< $@

# The variable is to be resolved first time it's used:
VERSION = $(shell defaults read $(PWD)/build/Release/HoRNDIS.kext/Contents/Info.plist CFBundleVersion)

build/_complete: build/HoRNDIS-kext.pkg $(wildcard package/*)
	productbuild --distribution package/Distribution.xml --package-path build --resources package --version $(VERSION) $(if $(CODESIGN_INST),--sign $(CODESIGN_INST)) build/HoRNDIS-$(VERSION).pkg && touch build/_complete

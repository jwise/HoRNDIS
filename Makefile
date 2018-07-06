# For the building the driver for MacOS 10.11+, the preferred
# XCode version is 7.3.1.
#
# You can do this by downloading Xcode 7.3.1 as a dmg from Apple, then
# copying the contents into /Applications as Xcode-7.3.1.
# If you also want to build from "release_pre_10_11", you may want to
# also download "Xcode-4.3.3", and copy "MacOSX10.6.sdk" from it to
# SDK directory under:
# /Applications/Xcode-7.3.1.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/

# Can be set from the environment:
HORNDIS_XCODE ?= /Applications/Xcode*$(XCODE_VER).app

XCODE_VER = 7.3.1
XCODEBUILD ?= $(wildcard $(HORNDIS_XCODE)/Contents/Developer/usr/bin/xcodebuild)

ifeq (,$(XCODEBUILD))
    $(error Cannot find xcodebuild under $(HORNDIS_XCODE). Please either \
    	download Xcode $(XCODE_VER) from: "https://developer.apple.com/download" \
    	and install as /Applications/Xcode-$(XCODE_VER)/ or point HORNDIS_XCODE \
    	to your preferred Xcode app path)
endif

# The package signing certificate must either be set or explicitly disabled:
ifeq (,$(CODESIGN_INST))
    $(error Please set CODESIGN_INST variable to your Mac Installer \
      certificate or 'none' if you don't have any. \
      E.g. "export CODESIGN_INST=G3H8VBSL7A")
else ifeq (none,$(CODESIGN_INST))
    # Clear the 'none' vaulue: easier to test in 'if' condition.
    CODESIGN_INST :=
endif

all: build/Release/HoRNDIS.kext build/pkg/_complete

clean:
	rm -rf build

# We now sign as part of the xcodebuild process.
build/Release/HoRNDIS.kext: $(wildcard *.cpp *.h *.plist HoRNDIS.xcodeproj/* *.lproj/*)
	$(XCODEBUILD) -project HoRNDIS.xcodeproj

build/pkg/root: build/Release/HoRNDIS.kext
	rm -rf build/pkg/
	mkdir -p build/pkg/root/Library/Extensions
	cp -R build/Release/HoRNDIS.kext build/pkg/root/Library/Extensions/

build/pkg/HoRNDIS-kext.pkg: build/pkg/root
	pkgbuild --identifier com.joshuawise.kexts.HoRNDIS --root $< $@

# The variable is to be resolved first time it's used:
VERSION = $(shell defaults read $(PWD)/build/Release/HoRNDIS.kext/Contents/Info.plist CFBundleVersion)

build/pkg/_complete: build/pkg/HoRNDIS-kext.pkg $(wildcard package/*)
	productbuild --distribution package/Distribution.xml --package-path build/pkg --resources package --version $(VERSION) $(if $(CODESIGN_INST),--sign $(CODESIGN_INST)) build/HoRNDIS-$(VERSION).pkg && touch build/pkg/_complete

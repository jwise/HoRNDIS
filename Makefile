# We need to use Xcode 4.6.3 in order to build HoRNDIS from this branch
# for 10.6 (Snow Leopard) - 10.10 (Yosemite), since building 32-bit kexts has
# been removed in Xcode 5.x and later.
#
# You can do this by downloading Xcode 4.6.3 as a dmg from Apple, then
# copying the contents into /Applications as Xcode-4.6.3.app, then downloading
# Xcode 4.3.3, and copying its MacOSX10.6.sdk into
# /Applications/Xcode-4.6.3.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/.
# Don't you love backwards compatibility?
#
# If building on Mavericks or Yosemite, you also need to have a working
# "lipo" binary; otherwise, the Makefile will hang on "CreateUniversalBinary"
# step. In order to fix, take an Xcode build 5.x or later (but not too late),
# find the "lipo" binary and copy under:
#   /Applications/Xcode-4.6.3.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/
# For reference:
#    https://stackoverflow.com/questions/18667916/xcrun-lipo-freezes-with-os-x-mavericks-and-xcode-4-x

XCODEBUILD ?= /Applications/Xcode-4.6.3.app/Contents/Developer/usr/bin/xcodebuild

# Ok, no joy there?
ifeq (,$(wildcard $(XCODEBUILD)))
	XCODEBUILD = xcodebuild
endif

# Should we sign?  Hack hack.  Oh well.
ifeq (joshua,$(USER))
	CODESIGN_KEXT ?= "Developer ID Application: Joshua Wise (54GTJ2AU36)"
	CODESIGN_INST ?= "Developer ID Installer: Joshua Wise (54GTJ2AU36)"
endif

all: build/Release/HoRNDIS.kext build/signed/HoRNDIS.kext build/_complete

clean:
	rm -rf build

build/Release/HoRNDIS.kext: HoRNDIS.cpp HoRNDIS.h HoRNDIS-Info.plist HoRNDIS.xcodeproj HoRNDIS.xcodeproj/project.pbxproj
	$(XCODEBUILD) -project HoRNDIS.xcodeproj

build/root: build/Release/HoRNDIS.kext build/signed/HoRNDIS.kext
	rm -rf build/root
	mkdir -p build/root/System/Library/Extensions/
	cp -R build/Release/HoRNDIS.kext build/root/System/Library/Extensions/
	mkdir -p build/root/Library/Extensions
	cp -R build/signed/HoRNDIS.kext build/root/Library/Extensions/

build/HoRNDIS-kext.pkg: build/root
	pkgbuild --identifier com.joshuawise.kexts.HoRNDIS --root $< $@

# The variable is to be resolved first time it's used:
VERSION = $(shell defaults read $(PWD)/build/Release/HoRNDIS.kext/Contents/Info.plist CFBundleVersion)

build/_complete: build/HoRNDIS-kext.pkg package/Distribution.xml
	productbuild --distribution package/Distribution.xml --package-path build --resources package $(if $(CODESIGN_INST),--sign $(CODESIGN_INST)) build/HoRNDIS-$(VERSION).pkg && touch $@

ifeq (,$(CODESIGN_KEXT))

build/signed/%: build/Release/%
	@echo not building $@ because we have no key to sign with
	@echo ...but, you can still use $<, if you want
	@exit 1

else

build/signed/%: build/Release/%
	rm -rf $@
	mkdir -p build/signed
	cp -R $< $@
	codesign --force -s $(CODESIGN_KEXT) $@

endif

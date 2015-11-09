# You may not be able to build with a modern Xcode, since we still support 10.6.
# So, if we have Xcode 4.6 around, we should use it.
#
# You can do this by downloading Xcode 4.6.3 as a dmg from Apple, then
# copying the contents into /Applications, then taking your ancient
# MacOSX10.6.sdk that you keep kicking around, and copying it into
# /Applications/Xcode-4.6.3.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/. 
# Don't you love backwards compatibility?

XCODEBUILD_ANCIENT ?= /Applications/Xcode-4.6.3.app/Contents/Developer/usr/bin/xcodebuild
XCODEBUILD_MODERN ?= xcodebuild

# Ok, no joy there?
ifeq (,$(wildcard $(XCODEBUILD_ANCIENT)))
	XCODEBUILD_ANCIENT = xcodebuild
endif


all: build/Release/MicroDriver.kext build/Release-unsigned/MicroDriver.kext build/MicroDriver.pkg

# We now sign as part of the xcodebuild process.  Also, 'release' is now
# signed, as opposed to 'signed', which used to be signed.
build/Release/MicroDriver.kext: MicroDriver.cpp MicroDriver.h MicroDriver-Info.plist MicroDriver.xcodeproj MicroDriver.xcodeproj/project.pbxproj
	$(XCODEBUILD_MODERN) -project MicroDriver.xcodeproj

build/Release-unsigned/MicroDriver.kext: MicroDriver.cpp MicroDriver.h MicroDriver-Info.plist MicroDriver.xcodeproj MicroDriver.xcodeproj/project.pbxproj
	$(XCODEBUILD_ANCIENT) -configuration Release-unsigned -project MicroDriver.xcodeproj

build/root: build/Release/MicroDriver.kext build/Release-unsigned/MicroDriver.kext
	rm -rf build/root
	mkdir -p build/root/System/Library/Extensions/
	cp -R build/Release-unsigned/MicroDriver.kext build/root/System/Library/Extensions/
	mkdir -p build/root/Library/Extensions
	cp -R build/Release/MicroDriver.kext build/root/Library/Extensions/

build/MicroDriver-kext.pkg: build/root
	pkgbuild --identifier com.joshuawise.kexts.MicroDriver --root $< $@

build/MicroDriver.pkg: build/MicroDriver-kext.pkg package/Distribution.xml Package/intro-text.rtf
	productbuild --distribution package/Distribution.xml --package-path build --resources package/resources $(if $(CODESIGN_INST),--sign $(CODESIGN_INST)) build/MicroDriver.pkg

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


all: build/Release/HoRNDIS.kext build/Release-unsigned/HoRNDIS.kext build/HoRNDIS.pkg

# We now sign as part of the xcodebuild process.  Also, 'release' is now
# signed, as opposed to 'signed', which used to be signed.
build/Release/HoRNDIS.kext: HoRNDIS.cpp HoRNDIS.h HoRNDIS-Info.plist HoRNDIS.xcodeproj HoRNDIS.xcodeproj/project.pbxproj
	$(XCODEBUILD_MODERN) -project HoRNDIS.xcodeproj

build/Release-unsigned/HoRNDIS.kext: HoRNDIS.cpp HoRNDIS.h HoRNDIS-Info.plist HoRNDIS.xcodeproj HoRNDIS.xcodeproj/project.pbxproj
	$(XCODEBUILD_ANCIENT) -configuration Release-unsigned -project HoRNDIS.xcodeproj

build/root: build/Release/HoRNDIS.kext build/Release-unsigned/HoRNDIS.kext
	rm -rf build/root
	mkdir -p build/root/System/Library/Extensions/
	cp -R build/Release-unsigned/HoRNDIS.kext build/root/System/Library/Extensions/
	mkdir -p build/root/Library/Extensions
	cp -R build/Release/HoRNDIS.kext build/root/Library/Extensions/

build/HoRNDIS-kext.pkg: build/root
	pkgbuild --identifier com.joshuawise.kexts.HoRNDIS --root $< $@

build/HoRNDIS.pkg: build/HoRNDIS-kext.pkg package/Distribution.xml Package/intro-text.rtf
	productbuild --distribution package/Distribution.xml --package-path build --resources package/resources $(if $(CODESIGN_INST),--sign $(CODESIGN_INST)) build/HoRNDIS.pkg

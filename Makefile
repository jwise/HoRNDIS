# You may not be able to build with a modern Xcode, since we still support 10.6.
# So, if we have Xcode 4.6 around, we should use it.
#
# You can do this by downloading Xcode 4.6.3 as a dmg from Apple, then
# copying the contents into /Applications, then taking your ancient
# MacOSX10.6.sdk that you keep kicking around, and copying it into
# /Applications/Xcode-4.6.3.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/. 
# Don't you love backwards compatibility?

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

all: build/Release/HoRNDIS.kext build/signed/HoRNDIS.kext build/HoRNDIS.pkg

build/Release/HoRNDIS.kext: HoRNDIS.cpp HoRNDIS.h HoRNDIS-Info.plist HoRNDIS.xcodeproj HoRNDIS.xcodeproj/project.pbxproj
	$(XCODEBUILD)

build/root: build/Release/HoRNDIS.kext build/signed/HoRNDIS.kext
	rm -rf build/root
	mkdir -p build/root/System/Library/Extensions/
	cp -R build/Release/HoRNDIS.kext build/root/System/Library/Extensions/
	mkdir -p build/root/Library/Extensions
	cp -R build/signed/HoRNDIS.kext build/root/Library/Extensions/

build/HoRNDIS-kext.pkg: build/root
	pkgbuild --identifier com.joshuawise.kexts.HoRNDIS --root $< $@

build/HoRNDIS.pkg: build/HoRNDIS-kext.pkg package/Distribution.xml
	productbuild --distribution package/Distribution.xml --package-path build --resources package/resources $(if $(CODESIGN_INST),--sign $(CODESIGN_INST)) build/HoRNDIS.pkg

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
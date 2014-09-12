# You may not be able to build with a modern Xcode, since we still support 10.6.
# So, if we have Xcode 4.6 around, we should use it.

XCODEBUILD ?= /Applications/Xcode46-DP4.app/Contents/Developer/usr/bin/xcodebuild

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

build/HoRNDIS-kext.pkg: build/Release/HoRNDIS.kext
	pkgbuild --component $< --install-location /System/Library/Extensions/ $@

build/HoRNDIS-signed-kext.pkg: build/signed/HoRNDIS.kext
	pkgbuild --component $< --install-location /Library/Extensions/ $@

build/HoRNDIS.pkg: build/HoRNDIS-kext.pkg build/HoRNDIS-signed-kext.pkg package/Distribution.xml
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
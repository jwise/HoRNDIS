#!/bin/sh
# (c) Copyright 2021 chris1111
PARENTDIR=$(dirname "$0")
cd "$PARENTDIR"
echo "Build the Project"
Sleep 2
xcodebuild -sdk macosx -configuration Release
# Create the Packages with pkgbuild/productbuild
echo "Build the Package"
mkdir -p ./Package/HoRNDIS
Sleep 2
cp -r ./build/Release/HoRNDIS.kext ./Package/HoRNDIS
sleep 1
pkgbuild --root ./Package/HoRNDIS --scripts ./Package/scripts --identifier com.horndis.horndis --version 1 --install-location /Library/Extensions HoRNDIS.pkg
sleep 1
productbuild --distribution  ./Package/Distribution.xml --resources ./Package/Resources --package-path ./HoRNDIS.pkg ./Build/HoRNDIS-Package.pkg
rm -rf ./HoRNDIS.pkg
Open ./build
echo "** PACKAGE BUILD SUCCEEDED **"


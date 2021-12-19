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
# Create the Packages with pkgbuild
echo "More info under SIGNED PACKAGES.
https://github.com/chris1111/HoRNDIS/blob/master/Signing%20PKGs.pdf"
pkgbuild --identifier "com.joshuawise.kexts.HoRNDIS" \
    --root "./Package/HoRNDIS" \
    --scripts "./Package/scripts" \
    --version "1" \
    --install-location "/Library/Extensions" \
    "./HoRNDIS.pkg"
# Create the final Packages with Productbuild
echo "
Create the final Packages with Productbuild "
Sleep 2
productbuild --distribution "./Package/Distribution.xml"  \
--package-path "./HoRNDIS.pkg" \
--resources "./Package/Resources" \
"./build//HoRNDIS-Package.pkg"
# Remove the first pkg of pkgbuild
rm -rf ./HoRNDIS.pkg
Open ./build
echo "** PACKAGE BUILD SUCCEEDED **"

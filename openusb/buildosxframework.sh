#!/bin/sh
#
#	buildosxframework.sh
#
#		This script will run ./autogen.sh and build OpenUSB using make. Finally,
#		it will construct a Mac OS/X Framework from the result.
#
set -e
echo
echo 'buildosxframework.sh'
echo '     Building OpenUSB as a Mac OS/X framework...'
echo

# Parse out the OpenUSB version from configure.in
majorVer=`cat configure.in | grep LIBOPENUSB_MAJOR_VERSION= | cut -f2 -d=`
minorVer=`cat configure.in | grep LIBOPENUSB_MINOR_VERSION= | cut -f2 -d=`
microVer=`cat configure.in | grep LIBOPENUSB_MICRO_VERSION= | cut -f2 -d=`
if [ -z "$majorVer" ] || [ -z "$minorVer" ] || [ -z "$microVer" ]
then
	echo 'Could not get version number from configure.in'
	exit -1
fi

# Construct the install directory so we can pass it to autogen.sh
currentDir=`pwd`
versionStr=$majorVer.$minorVer.$microVer
frameworkDir="$currentDir"/openusb.framework
versionsDir="$frameworkDir"/Versions
installDir="$versionsDir"/"$versionStr"
docDir="$installDir"/Resources/English.lproj/Documentation
mkdir -p "$installDir"
if [ "$?" -ne 0 ]; then echo "failed to create framework directory"; exit -1; fi


# Run autogen.sh
./autogen.sh --disable-dependency-tracking --prefix="$installDir"
if [ "$?" -ne 0 ]; then echo "autogen.sh failed"; exit -1; fi

# make/make install
make
if [ "$?" -ne 0 ]; then echo "make failed"; exit -1; fi

make install
if [ "$?" -ne 0 ]; then echo "make install failed"; exit -1; fi

# Construct the rest of the framework directories
mkdir -p "$installDir"/Resources
cd "$frameworkDir"/Versions
ln -sf "$versionStr" Current
cd ..
ln -s Versions/Current/Resources Resources
ln -s Versions/Current/include Headers
ln -s Versions/Current/lib/libopenusb.dylib OpenUSB
cd "$currentDir"

# Copy the documentation
if [ -e doc/html ]; then
	mkdir -p "$docDir"
	if [ "$?" -ne 0 ]; then echo "failed to create framework directory"; exit -1; fi
	cp doc/html/* "$docDir"
fi

# Construct Info.plist -- Most of the data here is constant and only a few things
# are subsituted in. Namely, CFBundleGetInfoString, CFBundleShortVersionString,
# and CFBundleVersion
infoxmlPath="$installDir"/Resources/Info.plist
echo '<?xml version="1.0" encoding="UTF-8"?>' > "$infoxmlPath"
echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' >> "$infoxmlPath"
echo '<plist version="1.0">' >> "$infoxmlPath"
echo '<dict>' >> "$infoxmlPath"
echo '    <key>CFBundleDevelopmentRegion</key>' >> "$infoxmlPath"
echo '    <string>English</string>' >> "$infoxmlPath"
echo '    <key>CFBundleExecutable</key>' >> "$infoxmlPath"
echo '    <string>OpenUSB</string>' >> "$infoxmlPath"
echo '    <key>CFBundleIdentifier</key>' >> "$infoxmlPath"
echo '    <string>com.openusb.openusb</string>' >> "$infoxmlPath"
echo '    <key>CFBundleGetInfoString</key>' >> "$infoxmlPath"
printf "    <string>OpenUSB version %s</string>\n" $versionStr >> "$infoxmlPath"
echo '	  <key>CFBundleInfoDictionaryVersion</key>' >> "$infoxmlPath"
echo '    <string>6.0</string>' >> "$infoxmlPath"
echo '    <key>CFBundleName</key>'  >> "$infoxmlPath"
echo '	  <string>openusb</string>' >> "$infoxmlPath"
echo '    <key>CFBundlePackageType</key>' >> "$infoxmlPath"
echo '    <string>FMWK</string>' >> "$infoxmlPath"
echo '    <key>CFBundleShortVersionString</key>' >> "$infoxmlPath"
printf "    <string>%s</string>\n" $versionStr >> "$infoxmlPath"
echo '    <key>CFBundleVersion</key>' >> "$infoxmlPath"
printf "    <string>%s</string>\n" $versionStr >> "$infoxmlPath"
echo '    <key>NSHumanReadableCopyright</key>' >> "$infoxmlPath"
echo '    <string>Copyright (c) 2007-2009 OpenUSB Project. All Rights Reserved. OpenUSB is licensed under the LGPL</string>' >> "$infoxmlPath"
echo '</dict>' >> "$infoxmlPath"
echo '</plist>' >> "$infoxmlPath"

# DONE!
echo 
echo 'DONE! Copy openusb.framework to /Library/Frameworks for use by all users'
echo







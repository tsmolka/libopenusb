#!/bin/bash
#
# This script manually performs the openusb.spec steps for Mac, because getting
# it all to work properly with fink is dicy at best.


#
# Validate...
if [ "${EUID}" != "0" ]; then
	echo "This script must be run as root..."
	exit 1
fi
if [ "$(uname -a | grep Darwin)" == "" ]; then
	echo "This script can only be used on Mac OS X..."
	exit 1
fi


#
# Initialize stuff...
NAME=openusb-package
TOPDIR=~/${NAME}
TMPDIR=/tmp/${NAME}
TMPPKG=/tmp/${NAME}/tmppkg
TMPDMG=/tmp/${NAME}/tmpdmg
TARGET=i586
PKGCONFIG=/usr/local/lib/pkgconfig
BASENAME=$(basename `pwd`)
ECHO=/bin/echo
BOLD="[31;1m"
NORMAL="[0m"
${ECHO}
${ECHO}
${ECHO}
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
${ECHO} "${BOLD}Initialization...${NORMAL}"
${ECHO} "${BOLD}**********************************************************************${NORMAL}"


#
# Talk to the user...
${ECHO} "This script builds openusb for Mac OS X.  It is assumed that you have already modified"
${ECHO} "the configure file and this directory name to the desired version and release."
${ECHO}
${ECHO} "When completed the finished PKG file will be located in this directory:"
${ECHO} "${TOPDIR}"
${ECHO} "It will also update /Library/Frameworks/openusb.framework"
${ECHO}
${ECHO} "Please confirm that the variables and version numbers are set up the way you want:"
${ECHO} "Directory name (there's no release number in this part): ${BOLD}${BASENAME}${NORMAL}"
${ECHO} "configure file major number:    ${BOLD}$(grep LIBOPENUSB_MAJOR_VERSION= configure)${NORMAL}"
${ECHO} "configure file minor number:    ${BOLD}$(grep LIBOPENUSB_MINOR_VERSION= configure)${NORMAL}"
${ECHO} "configure file micro number:    ${BOLD}$(grep LIBOPENUSB_MICRO_VERSION= configure)${NORMAL}"
${ECHO} "openusb.spec.in release number: ${BOLD}$(grep ' RELEASE' openusb.spec.in)${NORMAL}"
${ECHO}


#
# Confirmation...
read -n 1 -p "Are these correct (y/N)? " ANSWER
if ! /bin/echo "${ANSWER}" | grep -i "Y" &> /dev/null ;then
        ${ECHO} ""
        ${ECHO} "buildmacosx.sh aborted..."
        exit 1
fi


#
# Remove the old tree and create the SOURCES directory...
rm -rf ${TOPDIR}
mkdir -p ${TOPDIR}
if [ "$?" != 0 ]; then
        ${ECHO} "mkdir failed for ${TOPDIR}"
        exit 1
fi


#
# Move the whole thing to tmp, so we don't pollute our original...
${ECHO}
${ECHO}
${ECHO}
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
${ECHO} "${BOLD}Copy to ${TMPDIR}${NORMAL}"
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
rm -rf ${TMPDIR}
mkdir -p ${TMPDIR}
if [ "$?" != 0 ]; then
        ${ECHO} "mkdir failed for ${TMPDIR}"
        exit 1
fi
cp -R * ${TMPDIR}
if [ "$?" != 0 ]; then
        ${ECHO} "cp failed for ${TMPDIR}"
        exit 1
fi
pushd ${TMPDIR}
if [ "$?" != 0 ]; then
        ${ECHO} "pushd failed for ${TMPDIR}"
        exit 1
fi


#
# Parse out the OpenUSB version from configure.in
majorVer=`cat configure.in | grep LIBOPENUSB_MAJOR_VERSION= | cut -f2 -d=`
minorVer=`cat configure.in | grep LIBOPENUSB_MINOR_VERSION= | cut -f2 -d=`
microVer=`cat configure.in | grep LIBOPENUSB_MICRO_VERSION= | cut -f2 -d=`
if [ -z "$majorVer" ] || [ -z "$minorVer" ] || [ -z "$microVer" ]; then
	${ECHO} "Could not get version number from configure.in"
	release 1
fi


#
# Parse out the OpenUSB release version from openusb.spec.in
releaseVer=`cat openusb.spec.in | grep '%define RELEASE' | sed 's/%define//' | sed 's/RELEASE//'`
releaseVer="${releaseVer//[[:space:]]/}"
if [ -z "$releaseVer" ]; then
	${ECHO} "Could not get release version number from openusb.spec.in"
	release 1
fi


#
# Construct the install directory so we can pass it to autogen.sh
versionStr=$majorVer.$minorVer.$microVer
frameworkDir=/Library/Frameworks/openusb.framework
versionsDir="$frameworkDir"/Versions
installDir="$versionsDir"/"$versionStr"
docDir="$installDir"/Resources/English.lproj/Documentation


#
# Remove any existing framework and create the directory for the new one
rm -rf "$frameworkDir"
mkdir -p "$installDir"
if [ "$?" -ne 0 ]; then
	popd
	${ECHO} "failed to create framework directory"
	exit 1
fi


#
# Run configure
PKG_CONFIG_PATH=${PKGCONFIG} ./configure --disable-dependency-tracking --prefix="$installDir"
if [ "$?" -ne 0 ]; then
	popd
	${ECHO} "configure failed..."
	exit 1
fi


#
# make...
make
if [ "$?" -ne 0 ]; then
	popd
	${ECHO} "make failed..."
	exit 1
fi


#
# make install...
make install
if [ "$?" -ne 0 ]; then
	popd
	${ECHO} "make install failed..."
	exit 1
fi


#
# Construct the rest of the framework directories
mkdir -p "$installDir"/Resources
cd "$frameworkDir"/Versions
ln -sf "$versionStr" Current
cd ..
ln -s Versions/Current/Resources Resources
ln -s Versions/Current/include Headers
ln -s Versions/Current/lib/libopenusb.dylib OpenUSB
cd "$currentDir"


#
# Copy the documentation
if [ -e doc/html ]; then
	mkdir -p "$docDir"
	if [ "$?" -ne 0 ]; then
		${ECHO} "failed to create framework directory"
		exit 1
	fi
	cp doc/html/* "$docDir"
fi


#
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
echo '    <string>Copyright (c) 2007-2012 OpenUSB Project. All Rights Reserved. OpenUSB is licensed under the LGPL</string>' >> "$infoxmlPath"
echo '</dict>' >> "$infoxmlPath"
echo '</plist>' >> "$infoxmlPath"


#
# Construct our package from the framework...
${ECHO}
${ECHO}
${ECHO}
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
${ECHO} "${BOLD}Make the component package...${NORMAL}"
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
rm -rf "${TMPPKG}"
mkdir -p "${TMPPKG}/Library/Frameworks"
rm -rf ${TMPDMG}
mkdir -p "${TMPDMG}"
pushd /Library/Frameworks
tar cf - openusb.framework | (cd "${TMPPKG}/Library/Frameworks" && tar xBf -)
popd
chown -R root "${TMPPKG}"
chgrp -R wheel "${TMPPKG}"
FULLNAME="OpenUSB-${versionStr}-${releaseVer}"
pkgbuild \
	--identifier net.sourceforge.openusb.openusbanMtsafeLibusb.openusb.pkg \
	--version ${versionStr} \
	--root "${TMPPKG}" \
	"${TMPDMG}/${FULLNAME}.pkg"
if [ "$?" != "0" ]; then
	${ECHO} "pkgbuild failed..."
	exit 1
fi


#
# Create a disk image from the package...
${ECHO}
${ECHO}
${ECHO}
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
${ECHO} "${BOLD}Create disk image...${NORMAL}"
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
hdiutil \
	create \
	-srcfolder "${TMPDMG}" \
	-fs HFS+ \
	-volname "${FULLNAME}.pkg" \
	"${TOPDIR}/tmp.dmg"
if [ "$?" != "0" ]; then
	${ECHO} "hdiutil create disk image failed..."
	exit 1
fi


#
# Make the disk image readonly...
${ECHO}
${ECHO}
${ECHO}
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
${ECHO} "${BOLD}Make disk image readonly...${NORMAL}"
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
hdiutil \
	convert \
	-format UDRO \
	-o "${TOPDIR}/${FULLNAME}.dmg" \
	"${TOPDIR}/tmp.dmg"
if [ "$?" != "0" ]; then
	${ECHO} "hdiutil make image readonly failed..."
	exit 1
fi
rm "${TOPDIR}/tmp.dmg"


#
# Turn on the internet enable bit that pops open the disk image
# if it's downloaded from the web...
${ECHO}
${ECHO}
${ECHO}
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
${ECHO} "${BOLD}Internet enable...${NORMAL}"
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
hdiutil \
	internet-enable \
	-yes \
	"${TOPDIR}/${FULLNAME}.dmg"
if [ "$?" != "0" ]; then
	${ECHO} "hdiutil internet enable failed..."
	exit 1
fi


#
# Cleanup...
${ECHO}
${ECHO}
${ECHO}
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
${ECHO} "${BOLD}Cleanup...${NORMAL}"
${ECHO} "${BOLD}**********************************************************************${NORMAL}"
#rm -rf "${TMPDIR}"


#
# All done...
popd > /dev/null 2>&1
exit 0

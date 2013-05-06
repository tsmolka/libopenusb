#!/bin/bash


#
# Validation...
if [ "${EUID}" != "0" ]; then
	${ECHO} "buildlinux.sh must be run as root..."
	exit 1
fi
if [ "$(uname -a | grep Linux)" == "" ]; then
        echo "This script can only be used on Linux..."
        exit 1
fi


#
# Initialize stuff...
NAME=rpmbuild
TOPDIR=~/${NAME}
TMPDIR=/tmp/${NAME}
TARGET=i586
PKGCONFIG=/usr/bin/pkg-config
BASENAME=$(basename `pwd`)
ECHO=/bin/echo
BOLD="\033[31;1m"
NORMAL="\033[0m"
${ECHO}
${ECHO}
${ECHO}
${ECHO} -e "${BOLD}**********************************************************************${NORMAL}"
${ECHO} -e "${BOLD}Initialization...${NORMAL}"
${ECHO} -e "${BOLD}**********************************************************************${NORMAL}"


#
# Talk to the user...
${ECHO} "This script builds openusb for Linux.  It is assumed that you have already modified"
${ECHO} "the configure file and this directory name to the desired version and release."
${ECHO}
${ECHO} "When completed the finished RPM file will be located in this directory:"
${ECHO} "${TOPDIR}/RPMS/${TARGET}"
${ECHO}
${ECHO} "Please confirm that the variables and version numbers are set up the way you want:"
${ECHO} -e "Directory name (there's no release number in this part): ${BOLD}${BASENAME}${NORMAL}"
${ECHO} -e "configure file major number:    ${BOLD}$(grep LIBOPENUSB_MAJOR_VERSION= configure)${NORMAL}"
${ECHO} -e "configure file minor number:    ${BOLD}$(grep LIBOPENUSB_MINOR_VERSION= configure)${NORMAL}"
${ECHO} -e "configure file micro number:    ${BOLD}$(grep LIBOPENUSB_MICRO_VERSION= configure)${NORMAL}"
${ECHO} -e "openusb.spec.in release number: ${BOLD}$(grep ' RELEASE' openusb.spec.in)${NORMAL}"
${ECHO}


#
# Confirmation...
read -n 1 -p "Are these correct (y/N)? " ANSWER
if ! /bin/echo "${ANSWER}" | grep -i "Y" &> /dev/null ;then
	${ECHO} ""
	${ECHO} "buildlinux.sh aborted..."
	exit 1
fi


#
# Remove the old tree and create the SOURCES directory...
rm -rf ${TOPDIR}
mkdir -p ${TOPDIR}/SOURCES
if [ "$?" != 0 ]; then
	${ECHO} "mkdir failed for ${TOPDIR}/SOURCES"
	exit 1
fi


#
# Extract the spec file...
${ECHO}
${ECHO}
${ECHO}
${ECHO} -e "${BOLD}**********************************************************************${NORMAL}"
${ECHO} -e "${BOLD}Extract the spec file...${NORMAL}"
${ECHO} -e "${BOLD}**********************************************************************${NORMAL}"
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
PKG_CONFIG_PATH=${PKGCONFIG} ./configure
if [ "$?" != 0 ]; then
	${ECHO} "configure failed..."
	exit 1
fi
popd
cp ${TMPDIR}/openusb.spec .
if [ "$?" != 0 ]; then
	${ECHO} "cp failed for openusb.spec"
	exit 1
fi


#
# Create a tarfile for us in the rpmbuild tree...
${ECHO}
${ECHO}
${ECHO}
${ECHO} -e "${BOLD}**********************************************************************${NORMAL}"
${ECHO} -e "${BOLD}Create a tarball of us in the build directory...${NORMAL}"
${ECHO} -e "${BOLD}**********************************************************************${NORMAL}"
pushd ..
tar cfz ${TOPDIR}/SOURCES/${BASENAME}.tar.gz ${BASENAME}
if [ "$?" != 0 ]; then
	${ECHO} "tar failed..."
	exit 1
fi
popd
rm openusb.spec


#
# Build everthing from the tarfile...
${ECHO}
${ECHO}
${ECHO}
${ECHO} -e "${BOLD}**********************************************************************${NORMAL}"
${ECHO} -e "${BOLD}Build openusb...${NORMAL}"
${ECHO} -e "${BOLD}**********************************************************************${NORMAL}"
PKG_CONFIG_PATH=${PKGCONFIG} rpmbuild -ta --target ${TARGET} ${TOPDIR}/SOURCES/${BASENAME}.tar.gz
if [ "$?" != 0 ]; then
	${ECHO} "rpmbuild failed..."
	exit 1
fi


#
# Cleanup...
${ECHO}
${ECHO}
${ECHO}
${ECHO} -e "${BOLD}**********************************************************************${NORMAL}"
${ECHO} -e "${BOLD}Cleanup...${NORMAL}"
${ECHO} -e "${BOLD}**********************************************************************${NORMAL}"
rm -rf ${TMPDIR}


#
# All done...
exit 0

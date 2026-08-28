# How to Vendor jack2

This document uses ASD-STE100 Simplified Technical English.

## 1. Purpose

This document tells you how to put jack2 in this project.
The project keeps jack2 as a git submodule.
The build makes one installer.
The installer installs jack2 and PiStomp Companion together.

## 2. Description

PiStomp Companion needs the TreefallSound fork of jack2.
The stock jack2 does not have the multicast pin.
Discovery stops if the multicast pin is not in the build.

Today the user installs two packages.
The user installs the jack2 package first.
Then the user installs the PiStomp Companion package.
Nothing makes sure that the sequence is correct.

A submodule records the correct commit.
A combined installer removes the sequence problem.

## 3. Warnings

WARNING: The jack2 component writes files to /usr/local.
It replaces a jack2 installation that is already on the disk.
Tell the user about this before the installation starts.

CAUTION: jackd shows the same version for the fork and for the stock build.
You cannot find the difference at run time.

## 4. Add the Submodule

1. Add the submodule.

       git submodule add https://github.com/TreefallSound/jack2.git vendor/jack2

2. Set the submodule to the correct commit.
3. Commit the submodule and the .gitmodules file.

## 5. Build jack2

1. Get the submodule.

       git submodule update --init --recursive

2. Build the jack2 package.

       ./vendor/jack2/build-macos-pkg.sh 1.9.22-tfs.5

3. Find the package in vendor/jack2/build/.

Do not build jack2 for each build of the installer.
A build of jack2 takes much time.
Keep the result.
Build jack2 again only when the submodule commit changes.

## 6. Make the Combined Installer

1. Build the PiStomp Companion component with pkgbuild.
2. Put the jack2 package and the component package in the same directory.
3. Add one line for each package in distribution.xml.
4. Put the jack2 line first.
5. Build the product with productbuild.

The installer installs jack2 first.
Then it installs PiStomp Companion.

Remove the installation_check function.
The combined installer supplies jack2.
A check for jack2 is not necessary.

## 7. Obey the License

jackd and libjackserver use the GPL version 2 license.
libjack uses the LGPL version 2.1 license.
PiStomp Companion uses the MIT license.

You can distribute these licenses together.
But you must give the source code of jack2 to the user.
Keep the fork public.
Put a link to the fork and to the commit in the README file.

## 8. Limits

The combined installer does not prevent the replacement of the user's jack2.
A private directory for jack2 prevents this problem.
This document does not tell you how to make that change.

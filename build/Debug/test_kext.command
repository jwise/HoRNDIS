#!/bin/bash

# Variables
SOURCE="${BASH_SOURCE[0]}"
while [ -h "$SOURCE" ]; do DIR="$( cd -P "$( dirname "$SOURCE" )" && pwd )";SOURCE="$(readlink "$SOURCE")";[[ $SOURCE != /* ]] && SOURCE="$DIR/$SOURCE"; done
CURDIR="$( cd -P "$( dirname "$SOURCE" )" && pwd )"

DESTDIR="/tmp/HoRNDIS";


# Get sudo
if [ -z $(sudo echo "gotsudo") ]; then echo "Could not get super-user permissions"; exit -1; fi


# Copy KEXT to tmp
echo "Copying...";

if [ -d $DESTDIR ]; then sudo rm -r $DESTDIR; fi
sudo mkdir $DESTDIR;
sudo cp -r $CURDIR/HoRNDIS* $DESTDIR/; #Copies all files beginning with HoRNDIS…
sudo chmod -R 0700 $DESTDIR;
sudo chown -R root:wheel $DESTDIR;

# Load KEXT

KEXTFILES=();
while read -r -d $'\0'; do KEXTFILES+=("$REPLY"); done < <(sudo find $DESTDIR -iname "*.kext" -maxdepth 1 -print0);
if [ ${#KEXTFILES[@]} -gt 1 ]; then
	# Ask user which kext to take
	NUM_KEXTS=${#KEXTFILES[@]};
	
	for (( i=0; i < $NUM_KEXTS; i++ )); do
		echo "$i: ${KEXTFILES[$i]}";
	done

	KEXTNUM=-1;
	while [ $KEXTNUM -lt 0 ] || [ $KEXTNUM -ge $NUM_KEXTS ]; do read -p "Which file?: " KEXTNUM; done
	
	# Got file
	KEXTFILE=${KEXTFILES[$KEXTNUM]};
else
	KEXTFILE=${KEXTFILES[0]};
fi

if [ -n "$KEXTFILE" ]; then
	# Load KEXT if found
	echo "Loading \"$KEXTFILE\"...";
	sudo kextload -t "$KEXTFILE";
	
	echo "Loaded:";
	echo $(kextstat | grep HoRNDIS);
	
	exit 0;
else
	# No KEXT found
	echo 'No KEXTFILE found.';
	exit 1;
fi

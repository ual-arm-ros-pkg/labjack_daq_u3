#! /bin/bash

RULES=90-labjack.rules
RULES_DEST_PRIMARY=/lib/udev/rules.d

RULES_DEST=$RULES_DEST_PRIMARY
echo "Adding $RULES to $RULES_DEST.."
cp -f $RULES $RULES_DEST

echo -n "Restarting the rules.."
udevadm control --reload-rules 2> /dev/null
ret=$?
if [ $ret -ne 0 ]; then
	udevadm control --reload_rules 2> /dev/null
	ret=$?
fi
if [ $ret -ne 0 ]; then
	/etc/init.d/udev-post reload 2> /dev/null
	ret=$?
fi
if [ $ret -ne 0 ]; then
	udevstart 2> /dev/null
	ret=$?
fi
if [ $ret -ne 0 ]; then
	NEED_RESTART=$TRUE
	echo " could not restart the rules."
else
	echo # Finishes previous echo -n
fi



#!/bin/rc
# gather /dev/kmesg contents since boot into /sys/log/kmesg

if(! test -d /sys/log/kmesg)
	mkdir /sys/log/kmesg

cd /sys/log/kmesg
tr -d '\0' </dev/kmesg >$sysname.temp
{
	echo; echo; date
	if (grep -s '^Plan 9($| )' $sysname.temp) {
		echo '?^Plan 9($| )?,$p' | ed - $sysname.temp
	}
	if not
		tail -150 $sysname.temp
	echo
	ls -l /srv/boot
	cat /dev/swap
} >>$sysname
rm -f $sysname.temp

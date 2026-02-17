#!/boot/rc -m /boot/rcmain
# /boot/boot script for file servers, including standalone ones
#
# any variable set with "search" may be overriden in plan9.ini.

rfork e
path=(/boot /$cputype/bin /rc/bin .)

fn search {		# var file...
	name = $1
	curr = $$1
	if (~ $#$1 0 || ! test -e $$1) {
		curr = ()
		shift
		for (f)
			if (~ $#curr 0 && test -e $f)
				curr = $f
		while (~ $#curr 0 || ! test -e $curr) {
			echo -n $name^'? ' >/dev/cons
			curr = `{read; if (~ $status eof) echo /}
		}
	}
	echo $curr
}

cd /boot
echo -n boot...

# set up initial namespace.
# initcode (startboot) has bound #c to /dev, #e & #ec to /env & #s to /srv.
bind -a '#I0' /net
bind -a '#l0' /net
for (dev in '#¤' '#S' '#k' '#æ' '#u')	# auth, sd, fs, aoe, usb
	bind -a $dev /dev >[2]/dev/null
bind '#p' /proc
bind '#d' /fd
# bind -a /boot /

cp '#r/rtc' /dev/time

# start usb for keyboard, disks, etc.
if (test -e /dev/usb/ctl) {
	echo -n usb...
	usbd
}

# make local disk partitions visible
fn diskparts {			# set up any /dev/sd partitions
	# avoid touching sdF1 (no drive), it may hang
	for(disk in `{grep -l '^inquiry..' '#S'/sd*/ctl | sed 's;/ctl$;;'}) @ {
		echo -n $disk...
		cd $disk
		if (test -f data)
			{ fdisk -p data | grep -v '^delpart ' >ctl } \
				>[2]/dev/null
		if (test -f plan9)
			parts=(plan9*)
		if not
			parts=data
		for (part in $parts)
			if (test -f $part) {
				prep -p $part | grep -v '^delpart ' >ctl
			} >[2]/dev/null
	}
	echo
}
# local hackery for AoE: make visible extra sr luns of shelf 1.
# doesn't use the ip stack, just ethernet.
if (test -e /dev/aoe) {
	echo -n aoe...
	if (test -e /dev/aoe/1.1 && ! test -e /dev/sdf0)
		echo config switch on spec f type aoe//dev/aoe/1.1 >/dev/sdctl
	if (test -e /dev/aoe/1.2 && ! test -e /dev/sdg0)
		echo config switch on spec g type aoe//dev/aoe/1.2 >/dev/sdctl
}
echo -n partitions...
diskparts
fn diskparts

# set up any fs(3) partitions
# don't match AoE disks for config, as those may be shared.
fscfg = `{search fscfg /dev/sd[~e-h]?/fscfg}
echo reading /dev/fs definitions from $fscfg...
zerotrunc <$fscfg | sed -n '/^exit/q; p' | read -m >/dev/fs/ctl

#
# set up the network.
#
echo -n ip...
if (~ $#ip 1 && ! ~ $ip '') {
	# need to supply ip, ipmask and ipgw in plan9.ini to use this
	ipconfig -g $ipgw ether /net/ether0 $ip $ipmask
	echo 'add 0 0 '^$ipgw >>/net/iproute
}
if not
	ipconfig
ipconfig loopback /dev/null 127.1
# routing example: if outside, add explicit vfw routing to the inside
# switch (`{sed '/\.(0|255)[	 ]/d' /net/ipselftab}) {
# case 135.104.24.*				# new outside
# 	echo 'add 135.104.9.0 255.255.255.0 135.104.24.13' >>/net/iproute
# }

#
# set up auth via factotum, load from secstore & mount network root, if named.
#
# so far we're using the default key from $nvram,
# and have mounted our root over the net, if running diskless.
# factotum always mounts itself (on /mnt by default).
echo -n factotum...
if(~ $#auth 1){
	echo start factotum on $auth
	factotum -sfactotum -S -a $auth
}
if not
	factotum -sfactotum -S
mount -b /srv/factotum /mnt

# if a keys partition exists, add its aes-enctypted contents to factotum's
keys = `{search keys /dev/sd*/keys /dev/null}
echo -n add keys...
zerotrunc <$keys | aescbc -n -d | read -m >/mnt/factotum/ctl

# get root from network if fs addr set in plan9.ini.  bail out on error.
if (test -e /env/fs) {
	echo -n $fs on /root...
	if(! srv tcp!$fs!564 boot || ! mount -c /srv/boot /root)
		exec ./rc -m/boot/rcmain -i
}

#
# start venti store if we have arenas (don't search AoE).
# otherwise point to the local venti machine.
#
arena0 = `{search arena0 /dev/fs/arena0 /dev/sd[~e-h]?/arena0}
if (test -r $arena0) {
	if (! test -e /env/vmpc)
		vmpc=23			# % of free memory for venti
	venti=tcp!127.0.0.1!17034
	venticfg=`{search venticfg /dev/sd*/venticfg $arena0}
	echo start venti from $venticfg...
	venti -m $vmpc -c $venticfg
	echo -n waiting...
	sleep 10			# wait for venti to start serving
}
if not if (! test -e /env/venti)
	venti=tcp!96.78.174.33!17034	# local default: fs

#
# figure out which root fossil partition to use, if any (don't search AoE),
# and start the root fossil, which must serve /srv/boot.
#
fossil = `{search fossil /dev/fs/stand /dev/fs/fossil /dev/sd[~e-h]?/fossil}
echo -n root fossil on $fossil...
fossil -m 10 -f $fossil
echo -n waiting...
sleep 3				# wait for fossil to start serving

#
# mount new root.
# factotum is now mounted in /mnt; keep it visible.
# newns() needs it, among others.
#
# used by /lib/namespace
rootdir=/root
rootspec=main/active
# rootsrv really needs to be /srv/boot, as /lib/namespace assumes /srv/boot.
rootsrv = `{search rootsrv /srv/boot /srv/fossil /srv/fossil.stand /srv/fsmain \
	/srv/*stand*}
echo -n mount -cC $rootsrv $rootdir...
	mount -cC $rootsrv $rootdir
bind -a $rootdir /

if (test -d $rootdir/mnt)
	bind -ac $rootdir/mnt /mnt
mount -b /srv/factotum /mnt

#
# now that our normal root is mounted,
# bind a standard /bin and run init, which will run cpurc,
# then give the local user a bootes console shell.
#
if (! test -d /$cputype) {
	echo /$cputype missing!
	exec ./rc -m/boot/rcmain -i
}
bind /$cputype/bin /bin
bind -a /rc/bin /bin
path=(/bin . /boot)
# cpurc will have to start other fossils
echo
echo
echo init, cpurc...
/$cputype/init -c

# init died: let the local user repair what he can.
exec ./rc -m/boot/rcmain -i

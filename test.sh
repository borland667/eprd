#logrotate -f /etc/logrotate.conf
insmod ./eprd.ko
sleep 1
#./eprd_setup -f /data/eprddta.img -m 3 
#./eprd_setup -f /data/eprddta
#./eprd_setup -f /data/eprddta -p 1000M -m 5 -b
#./eprd_setup -f /data/eprddta.img -c -s 8192M -p 512M -m 3 -b 
#./eprd_setup -f /dev/mapper/vg_dynamic11-lv_data -m3 -p 4096M
#sleep 2
#mkfs.xfs -f /dev/eprda -b size=4096
#mount /dev/eprda /mnt -o nobarrier 
#mount /dev/eprda /mnt 
#mkfs.ext4 /dev/eprda
#mount /dev/eprda /mnt -o barrier=0
#mount /dev/eprda /mnt -o barrier=1

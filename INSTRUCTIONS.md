The kernel is based on 5.18-rc6. Patches have been added to enable ZNS
devices with non power of 2 zone sizes and without zone append support.

## Compiling the kernel
- Make sure to enable CONFIG_BLK_DEV_ZONED by `make menuconfig` or
  simply executing `scripts/config -e BLK_DEV_ZONED`

## Running FIO:
- Enable the mq-deadline scheduler by doing the following:
`echo "mq-deadline" > /sys/block/<block-device>/queue/scheduler`

- Now FIO workload can be run on the device. A simple workload:
`fio --size=1G --rw=write --bs=4k --direct=1 --zonemode=zbd --name=zns --filename=/dev/nvme0n2`
Note: it is important to have --direct=1 and --zonemode=zbd for fio to
run correctly on ZNS devices.


echo "=== Debix Model AB — RT bootargs (isolcpus=2,3 + PREEMPT_RT threadirqs) ==="
setenv bootargs "Debix_Model_AB V1.0.3 console=ttymxc1,115200 root=/dev/mmcblk1p2 rootwait rw isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 threadirqs"
echo "bootargs = ${bootargs}"
load mmc ${mmcdev}:${mmcpart} ${loadaddr} Image
load mmc ${mmcdev}:${mmcpart} ${fdt_addr_r} imx8mp-evk.dtb
booti ${loadaddr} - ${fdt_addr_r}

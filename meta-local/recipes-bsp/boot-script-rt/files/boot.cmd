echo "=== A.L.A. (Audio Live Assistant) by Electrosens R&D — RT bootargs (isolcpus=2,3 + threadirqs) ==="
setenv bootargs "ALA_Electrosens V1.0 console=ttymxc1,115200 root=/dev/mmcblk1p2 rootwait rw isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 threadirqs vt.global_cursor_default=0"
echo "bootargs = ${bootargs}"
load mmc ${mmcdev}:${mmcpart} ${loadaddr} Image
load mmc ${mmcdev}:${mmcpart} ${fdt_addr_r} imx8mp-evk.dtb
booti ${loadaddr} - ${fdt_addr_r}

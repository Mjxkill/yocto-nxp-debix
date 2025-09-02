#DISTRO=fsl-imx-fb MACHINE=imx6ulevk source imx-setup-release.sh -b imx6ul

#DISTRO=fsl-imx-xwayland MACHINE=imx8mp-lpddr4-evk source imx-setup-release.sh -b Model_A

EULA=1 DISTRO=fsl-imx-xwayland MACHINE=imx8mpevk source imx-setup-release.sh -b Model_AB_Infinity

# bitbake imx-image-full

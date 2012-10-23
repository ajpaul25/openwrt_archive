#!/bin/sh
#
# Copyright (C) 2007 OpenWrt.org
#
#

. /lib/brcm63xx.sh

do_fixcrc() {
	mtd fixtrx linux
}

brcm63xx_detect

case "$board_name" in
	96328avng |\
	96328A-1241N |\
	96328A-1441N1 |\
	963281TAN |\
	963281T_TEF |\
	"CPVA502+" |\
	AW4339U |\
	CPVA642 |\
	CT6373-1 |\
	MAGIC |\
	V2110 |\
	V2500V_BB)
		do_fixcrc
		;;
esac


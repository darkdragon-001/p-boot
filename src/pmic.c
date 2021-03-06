/*
 * Copyright (c) 2017-2018 ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This code was taken and adapted from ATF by:
 * Copyright (C) 2020  Ondřej Jirman <megi@xff.cz>
 */

#include <common.h>
#include <asm/io.h>
#include "pmic.h"

#define AXP803_HW_ADDR 0x3a3
#define AXP803_RT_ADDR	0x2d

#define SUNXI_R_RSB_BASE		0x01f03400
#define SUNXI_R_PIO_BASE		0x01f02c00
#define SUNXI_R_PRCM_BASE		0x01f01400

#define RSB_CTRL	0x00
#define RSB_CCR		0x04
#define RSB_INTE	0x08
#define RSB_STAT	0x0c
#define RSB_DADDR0	0x10
#define RSB_DLEN	0x18
#define RSB_DATA0	0x1c
#define RSB_LCR		0x24
#define RSB_PMCR	0x28
#define RSB_CMD		0x2c
#define RSB_SADDR	0x30

#define RSBCMD_SRTA	0xE8
#define RSBCMD_RD8	0x8B
#define RSBCMD_RD16	0x9C
#define RSBCMD_RD32	0xA6
#define RSBCMD_WR8	0x4E
#define RSBCMD_WR16	0x59
#define RSBCMD_WR32	0x63

#define MAX_TRIES	100000

//XXX: PL0,1 pins s_rsb, clock, reset on r_ccu

static inline uint32_t mmio_read_32(uint32_t a)
{
	return readl((void*)(uintptr_t)a);
}

static inline void mmio_write_32(uint32_t a, uint32_t v)
{
	writel(v, (void*)(uintptr_t)a);
}

static inline void mmio_clrbits_32(uintptr_t addr, uint32_t clear)
{
        mmio_write_32(addr, mmio_read_32(addr) & ~clear);
}

static inline void mmio_setbits_32(uintptr_t addr, uint32_t set)
{
        mmio_write_32(addr, mmio_read_32(addr) | set);
}

static inline void mmio_clrsetbits_32(uintptr_t addr,
                                uint32_t clear,
                                uint32_t set)
{
        mmio_write_32(addr, (mmio_read_32(addr) & ~clear) | set);
}

static int rsb_wait_bit(const char *desc, unsigned int offset, uint32_t mask)
{
	uint32_t reg, tries = MAX_TRIES;

	do
		reg = mmio_read_32(SUNXI_R_RSB_BASE + offset);
	while ((reg & mask) && --tries);	/* transaction in progress */
	if (reg & mask) {
		printf("%s: timed out\n", desc);
		return -ETIMEDOUT;
	}

	return 0;
}

static int rsb_wait_stat(const char *desc)
{
	uint32_t reg;
	int ret = rsb_wait_bit(desc, RSB_CTRL, BIT(7));

	if (ret)
		return ret;

	reg = mmio_read_32(SUNXI_R_RSB_BASE + RSB_STAT);
	if (reg == 0x01)
		return 0;

	printf("%s: 0x%x\n", desc, reg);
	return -reg;
}

/* Initialize the RSB controller. */
int rsb_init_controller(void)
{
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_CTRL, 0x01); /* soft reset */

	return rsb_wait_bit("RSB: reset controller", RSB_CTRL, BIT(0));
}

int rsb_read(uint8_t rt_addr, uint8_t reg_addr)
{
	int ret;

	mmio_write_32(SUNXI_R_RSB_BASE + RSB_CMD, RSBCMD_RD8); /* read a byte */
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_SADDR, rt_addr << 16);
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_DADDR0, reg_addr);
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_CTRL, 0x80);/* start transaction */

	ret = rsb_wait_stat("RSB: read command");
	if (ret)
		return ret;

	return mmio_read_32(SUNXI_R_RSB_BASE + RSB_DATA0) & 0xff; /* result */
}

int rsb_write(uint8_t rt_addr, uint8_t reg_addr, uint8_t value)
{
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_CMD, RSBCMD_WR8);	/* byte write */
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_SADDR, rt_addr << 16);
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_DADDR0, reg_addr);
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_DATA0, value);
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_CTRL, 0x80);/* start transaction */

	return rsb_wait_stat("RSB: write command");
}

int rsb_set_device_mode(uint32_t device_mode)
{
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_PMCR,
		      (device_mode & 0x00ffffff) | BIT(31));

	return rsb_wait_bit("RSB: set device to RSB", RSB_PMCR, BIT(31));
}

int rsb_set_bus_speed(uint32_t source_freq, uint32_t bus_freq)
{
	uint32_t reg;

	if (bus_freq == 0)
		return -EINVAL;

	reg = source_freq / bus_freq;
	if (reg < 2)
		return -EINVAL;

	reg = reg / 2 - 1;
	reg |= (1U << 8);		/* one cycle of CD output delay */

	mmio_write_32(SUNXI_R_RSB_BASE + RSB_CCR, reg);

	return 0;
}

/* Initialize the RSB PMIC connection. */
int rsb_assign_runtime_address(uint16_t hw_addr, uint8_t rt_addr)
{
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_SADDR, hw_addr | (rt_addr << 16));
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_CMD, RSBCMD_SRTA);
	mmio_write_32(SUNXI_R_RSB_BASE + RSB_CTRL, 0x80);

	return rsb_wait_stat("RSB: set run-time address");
}

int rsb_init(void)
{
	int ret;

	/* un-gate R_PIO clock */
	mmio_setbits_32(SUNXI_R_PRCM_BASE + 0x28, BIT(0));

	/* switch pins PL0 and PL1 to the desired function */
	mmio_clrsetbits_32(SUNXI_R_PIO_BASE + 0x00, 0xffU, 0x22);
	/* level 2 drive strength */
	mmio_clrsetbits_32(SUNXI_R_PIO_BASE + 0x14, 0x0fU, 0xaU);
	/* set both pins to pull-up */
	mmio_clrsetbits_32(SUNXI_R_PIO_BASE + 0x1c, 0x0fU, 0x5U);

	/* assert, then de-assert reset of I2C/RSB controller */
	mmio_clrbits_32(SUNXI_R_PRCM_BASE + 0xb0, BIT(3));
	mmio_setbits_32(SUNXI_R_PRCM_BASE + 0xb0, BIT(3));

	/* un-gate clock */
	mmio_setbits_32(SUNXI_R_PRCM_BASE + 0x28, BIT(3));

	ret = rsb_init_controller();
	if (ret)
		return ret;

	/* Start with 400 KHz to issue the I2C->RSB switch command. */
	ret = rsb_set_bus_speed(24000000, 400000);
	if (ret)
		return ret;

	/*
	 * Initiate an I2C transaction to write 0x7c into register 0x3e,
	 * switching the PMIC to RSB mode.
	 */
	ret = rsb_set_device_mode(0x7c3e00);
	if (ret)
		return ret;

	/* Now in RSB mode, switch to the recommended 3 MHz. */
	ret = rsb_set_bus_speed(24000000, 3000000);
	if (ret)
		return ret;

	/* Associate the 8-bit runtime address with the 12-bit bus address. */
	return rsb_assign_runtime_address(AXP803_HW_ADDR,
					  AXP803_RT_ADDR);
}

int pmic_write(uint8_t reg, uint8_t val)
{
	return rsb_write(AXP803_RT_ADDR, reg, val);
}

int pmic_read(uint8_t reg_addr)
{
	return rsb_read(AXP803_RT_ADDR, reg_addr);
}

int pmic_clrsetbits(uint8_t reg, uint8_t clr_mask, uint8_t set_mask)
{
	uint8_t regval;
	int ret;

	ret = rsb_read(AXP803_RT_ADDR, reg);
	if (ret < 0)
		return ret;

	regval = (ret & ~clr_mask) | set_mask;

	return rsb_write(AXP803_RT_ADDR, reg, regval);
}

void pmic_poweroff(void)
{
	// power off via PMIC
	pmic_setbits(0x32, BIT(7));
	hang();
}

void pmic_reboot(void)
{
	// soft power restart via PMIC
	pmic_setbits(0x31, BIT(6));
	hang();
}

void pmic_write_data(unsigned off, uint8_t data)
{
	if (off > 11)
		return;

	// data registers inside PMIC
	pmic_write(0x04 + off, data);
}

int pmic_read_data(unsigned off)
{
	if (off > 11)
		return -1;

	// data registers inside PMIC
	return pmic_read(0x04 + off);
}

void pmic_init(void)
{
        // enable DCDC/PWM chg freq spread
	pmic_write(0x3b, 0x88);

        // up the DCDC2 voltage to 1.3V (CPUX)
        // default is 0.9V, and rampup speed is 2.5mV/us
        // so we need 400mV/2.5mV = 160us before being able to ramp up
        // CPU frequency
        pmic_write(0x21, 0x4b);

	// disable temp sensor charger effect
	//pmic_setbits(0x84, BIT(2));

	// when SDP not detected set 2A VBUS current limit (my charger can do that)
	// set VBUS Vhold to 4.5V
	pmic_write(0x30, 0x02 | (5 << 3));

	// enable charger detection
	pmic_write(0x2c, 0x95);

	// short POK reaction times
	pmic_write(0x36, 0x08);

        // start battery max capacity calibration
        //pmic_setbits(0xb8, BIT(5));
}

void pmic_dump_registers(void)
{
	printf("Dumping PMIC registers:");
	for (int i = 0; i < 0x80; i++)
		printf("%x: %x\n", i, pmic_read(i));
}

void pmic_dump_status(void)
{
        int status0, status1, status2;

	// read status registers
	status0 = pmic_read(0x00);
	status1 = pmic_read(0x01);
	status2 = pmic_read(0x02);

	// clear power up status
	pmic_write(0x02, 0xff);

	// print PMIC status
	if (status0 >= 0 && status1 >= 0 && status2 >= 0) {
		if (status2 & BIT(0))
			printf("  PMIC power up by POK\n");
		if (status2 & BIT(1))
			printf("  PMIC power up by USB power\n");
		if (status2 & BIT(5))
			printf("  PMIC UVLO!\n");

		printf("  VBUS %s\n", status0 & BIT(5) ? "present" : "absent");

		if (status1 & BIT(5) && status1 & BIT(4)) {
			printf("  Battery %s3.5V\n", status0 & BIT(3) ? ">" : "<");
			printf("  Battery %s\n", status0 & BIT(2) ? "charging" : "discharging");
			if (status1 & BIT(3))
				printf("  Battery in SAFE mode\n");
		} else {
			printf("  Battery absent\n");
		}
	}
}

#if 0
void pmic_reboot_with_timer(void)
{
	// enable power up by IRQ source
	// detect irq wakeup by 8f[7]
	//pmic_setbits(0x31, BIT(3));
	//pmic_write(0x8a, 0x83); // start timer for 3 units
	// 0x4c BIT(7) - event timer interrupt flag, wr 1 to clear
	// 0x44 BIT(7) - event timer int en
}
#endif

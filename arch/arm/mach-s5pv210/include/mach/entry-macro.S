/* linux/arch/arm/mach-s5pv210/include/mach/entry-macro.S
 *
 * Copyright (c) 2010 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * Low-level IRQ helper macros for the Samsung S5PV210
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/irqchip/arm-vic.h>
#include <mach/map.h>
#include <plat/irqs.h>

	.macro	disable_fiq
	.endm

	.macro	get_irqnr_preamble, base, tmp
	ldr	\base, =VA_VIC0
	.endm

	.macro	arch_ret_to_user, tmp1, tmp2
	.endm

	.macro	get_irqnr_and_base, irqnr, irqstat, base, tmp

	@ check the vic0
	mov	\irqnr, # S5P_IRQ_OFFSET + 31
	ldr	\irqstat, [ \base, # VIC_IRQ_STATUS ]
	teq	\irqstat, #0

	@ otherwise try vic1
	addeq	\tmp, \base, #(VA_VIC1 - VA_VIC0)
	addeq	\irqnr, \irqnr, #32
	ldreq	\irqstat, [ \tmp, # VIC_IRQ_STATUS ]
	teqeq	\irqstat, #0

	@ otherwise try vic2
	addeq	\tmp, \base, #(VA_VIC2 - VA_VIC0)
	addeq	\irqnr, \irqnr, #32
	ldreq	\irqstat, [ \tmp, # VIC_IRQ_STATUS ]
	teqeq	\irqstat, #0

	@ otherwise try vic3
	addeq	\tmp, \base, #(VA_VIC3 - VA_VIC0)
	addeq	\irqnr, \irqnr, #32
	ldreq	\irqstat, [ \tmp, # VIC_IRQ_STATUS ]
	teqeq	\irqstat, #0

	clzne	\irqstat, \irqstat
	subne	\irqnr, \irqnr, \irqstat
	.endm

/*
 * Copyright 2015 Pedro Pinto, pemdpinto@gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#ifndef __PLATFORM_EXYNOS5250_H
#define __PLATFORM_EXYNOS5250_H

#define SDRAM_BASE 0x40000000

#define VECT_BASE 0x40200000
#define VECT_SIZE 0x10000

#define DEVICE_BASE 0x48000000
#define DEVICE_SIZE 0x2000000

/* framebuffer */
#define FB_BASE 0x88000000
#define FB_SIZE 0x200000

/* UART */
#define UART0_BASE    0x12C00000
#define UART1_BASE    0x12C10000
#define UART2_BASE    0x12C20000
#define UART3_BASE    0x12C30000

#endif /* __PLATFORM_EXYNOS5250_H */

/*
 * Copyright 2015 Pedro Pinto, pemdpinto@gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#ifndef _BOARD_ARM_CHROMEBOOK_CONFIG_H
#define _BOARD_ARM_CHROMEBOOK_CONFIG_H


#define BOARD_NAME_PRETTY "Samsung Series 3 Chromebook"

#define BOARD_CPU_TYPE_EXYNOS 1
#define BOARD_CPU_EXYNOS5250 1

#include <arch/arm/exynos5250.h>

// UART Settings
#define BOARD_UART_EXYNOS 1

//#define BOARD_UART1_BASE OMAP_UART1_BASE
//#define BOARD_UART2_BASE OMAP_UART2_BASE
//#define BOARD_UART3_BASE OMAP_UART3_BASE
//#define BOARD_UART4_BASE OMAP_UART3_BASE

#define BOARD_UART_DEBUG UART3_BASE

#define BOARD_UART_CLOCK 40000000
	// 40MHz


#endif /* _BOARD_ARM_CHROMEBOOK_CONFIG_H */

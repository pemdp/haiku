/*
 * Copyright 2015 Pedro Pinto, pemdpinto@gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#ifndef _KERNEL_ARCH_UART_EXYNOS_H
#define _KERNEL_ARCH_UART_EXYNOS_H


#include <sys/types.h>

#include <SupportDefs.h>

#include <arch/generic/debug_uart.h>


class ArchUARTExynos : public DebugUART {
public:
						ArchUARTExynos(addr_t base, int64 clock);
						~ArchUARTExynos();

			void			InitEarly();
			void			Init();
			void			InitPort(uint32 baud);

			int			PutChar(char c);
			int			GetChar(bool wait);

			void			FlushTx();
			void			FlushRx();
};


extern ArchUARTExynos *arch_get_uart_exynos(addr_t base, int64 clock);


#endif /* _KERNEL_ARCH_UART_EXYNOS_H */

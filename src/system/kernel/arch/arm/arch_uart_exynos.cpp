/*
 * Copyright 2015 Pedro Pinto, pemdpinto@gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include <arch/arm/reg.h>
#include <arch/arm/arch_uart_exynos.h>
#include <board_config.h>
#include <debug.h>
#include <new>


ArchUARTExynos::ArchUARTExynos(addr_t base, int64 clock)
	: DebugUART(base, clock)
{
}


ArchUARTExynos::~ArchUARTExynos()
{
}


void
ArchUARTExynos::InitEarly()
{
	// Perform special hardware UART configuration
}

void
ArchUARTExynos::Init()
{}

void
ArchUARTExynos::InitPort(uint32 baud)
{}

int
ArchUARTExynos::PutChar(char c)
{
	return -1;
}

int
ArchUARTExynos::GetChar(bool wait)
{
	return -1;
}

void
ArchUARTExynos::FlushTx()
{}

void
ArchUARTExynos::FlushRx()
{}

ArchUARTExynos *arch_get_uart_exynos(addr_t base, int64 clock)
{
	static char buffer[sizeof(ArchUARTExynos)];
	ArchUARTExynos *uart = new(buffer) ArchUARTExynos(base, clock);
	return uart;
}

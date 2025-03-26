/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <zephyr/net/socket.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define BIND_PORT 4242

/* Check overlay exists for CDC UART console */
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart),
	    "Console device is not ACM CDC UART device");

int main(void)
{
	/*
	 * This is a simple TCP echo server for the Wiznet W5500-EVB-Pico.
	 * It is a single-threaded server that listens for incoming connections on port 4242.
	 * When a connection is established, it echoes back any data sent to it.
	 * It's designed around Zephyr's `net/socket/echo` [example](https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/net/sockets/echo), with some modifications to output serial logging on the Pico's USB port.
	 */
    int opt;
	socklen_t optlen = sizeof(int);
	int serv, ret;
	struct sockaddr_in6 bind_addr = {
		.sin6_family = AF_INET6,
		.sin6_addr = IN6ADDR_ANY_INIT,
		.sin6_port = htons(BIND_PORT),
	};
	static int counter;

	/* Configure to set Console output to USB Serial */
	const struct device *usb_device = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

    /* Check if USB can be initialised, bails out if fail is returned */
	if (usb_enable(NULL) != 0) {
		return 1;
	}

	/* Wait for a console connection, if the DTR flag was set to activate USB.
     * If you wish to start generating serial data immediately, you can simply
     * remove the while loop, to not wait until the control line is set.
     */
	while (!dtr) {
		uart_line_ctrl_get(usb_device, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}

    printk("Hello from the Zephyr Console on the RPi Pico...\n");

	serv = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (serv < 0) {
		printk("error: socket: %d\n", errno);
		exit(1);
	}

	ret = getsockopt(serv, IPPROTO_IPV6, IPV6_V6ONLY, &opt, &optlen);
	if (ret == 0) {
		if (opt) {
			printk("IPV6_V6ONLY option is on, turning it off.\n");

			opt = 0;
			ret = setsockopt(serv, IPPROTO_IPV6, IPV6_V6ONLY,
					 &opt, optlen);
			if (ret < 0) {
				printk("Cannot turn off IPV6_V6ONLY option\n");
			} else {
				printk("Sharing same socket between IPv6 and IPv4\n");
			}
		}
	}

	if (bind(serv, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		printk("error: bind: %d\n", errno);
		exit(1);
	}

	if (listen(serv, 5) < 0) {
		printk("error: listen: %d\n", errno);
		exit(1);
	}

	printk("Single-threaded TCP echo server waits for a connection on "
	       "port %d...\n", BIND_PORT);

	while (1) {
		struct sockaddr_in6 client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		char addr_str[32];
		int client = accept(serv, (struct sockaddr *)&client_addr,
				    &client_addr_len);

		if (client < 0) {
			printk("error: accept: %d\n", errno);
			continue;
		}

		inet_ntop(client_addr.sin6_family, &client_addr.sin6_addr,
			  addr_str, sizeof(addr_str));
		printk("Connection #%d from %s\n", counter++, addr_str);

		while (1) {
			char buf[128], *p;
			int len = recv(client, buf, sizeof(buf), 0);
			int out_len;

			if (len <= 0) {
				if (len < 0) {
					printk("error: recv: %d\n", errno);
				}
				break;
			}

			p = buf;
            printk("Received message: %s\n", p);
			do {
				out_len = send(client, p, len, 0);
				if (out_len < 0) {
					printk("error: send: %d\n", errno);
					goto error;
				}
				p += out_len;
				len -= out_len;
			} while (len);
		}

error:
		close(client);
		printk("Connection from %s closed\n", addr_str);
	}
	return 0;
}
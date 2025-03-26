# Zephyr TCP Echo Server for Wiznet W5500-EVB-Pico

This is a simple TCP echo server for the Wiznet W5500-EVB-Pico. It is a single-threaded server that listens for incoming connections on port 4242. When a connection is established, it echoes back any data sent to it. It's designed around Zephyr's `net/socket/echo` [example](https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/net/sockets/echo), with some modifications to output serial logging on the Pico's USB port.

## Building

You'll need a working Zephyr environment on your machine. See the [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/getting_started/index.html) for more information.

To prepare the environment, run `west init .` and `west update` from the root of this repository.

To build the project, run `west build -b w5500_evb_pico PROJECT_SELECTION`.

## TCP Echo Client

To connect to the server, you can use the following command:

```bash
# MacOS
nc -v 192.0.2.1 4242

# Linux
telnet 192.0.2.1 4242

# Windows
nc -v 192.0.2.1 4242
```

## Notes

Under MacOS, you'll likely need to navigate into your settings to configure your network connection to manual for the Pico to get an IP address (rather than your Mac trying to get an address via DHCP). You can do this by going to `System Preferences > Network > Your Ethernet Adapter > Details... > TCP/IP` and then selecting `Manually` from the `Configure IPv4` dropdown. You'll also need to set the `IPv4 Address` to `192.0.2.2` (your Mac's IP address), the `Subnet Mask` to `255.255.255.0`, and the `Router` to `192.0.2.1` (your Pico's IP address).

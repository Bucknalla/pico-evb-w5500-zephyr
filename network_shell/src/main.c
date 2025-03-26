#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/shell/shell.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/net/socket.h>

/* Check overlay exists for CDC UART console */
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart),
	    "Console device is not ACM CDC UART device");

static const struct device *bme280_dev = DEVICE_DT_GET_ANY(bosch_bme280);

#define MAX_SERVER_ADDR_LEN 40
#define MAX_MSG_SIZE 128

static int send_sensor_data_tcp(const char *server_addr, uint16_t port)
{
    int sock, ret;
    struct sockaddr_in addr;
    char message[MAX_MSG_SIZE];
    struct sensor_value temp, press, humidity;

    /* Create socket */
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return -errno;
    }

    /* Set up server address */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ret = inet_pton(AF_INET, server_addr, &addr.sin_addr);
    if (ret <= 0) {
        close(sock);
        return -EINVAL;
    }

    /* Connect to server */
    ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        close(sock);
        return -errno;
    }

    /* Read sensor data */
    ret = sensor_sample_fetch(bme280_dev);
    if (ret < 0) {
        close(sock);
        return ret;
    }

    sensor_channel_get(bme280_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
    sensor_channel_get(bme280_dev, SENSOR_CHAN_PRESS, &press);
    sensor_channel_get(bme280_dev, SENSOR_CHAN_HUMIDITY, &humidity);

    /* Format message */
    snprintf(message, sizeof(message),
             "Temperature: %.2f C, Pressure: %.2f kPa, Humidity: %.2f %%\n",
             sensor_value_to_double(&temp),
             sensor_value_to_double(&press),
             sensor_value_to_double(&humidity));

    /* Send data */
    ret = send(sock, message, strlen(message), 0);
    close(sock);

    return (ret < 0) ? -errno : 0;
}

static int cmd_bme280_temp(const struct shell *shell, size_t argc, char *argv[])
{
    struct sensor_value temp;
    int rc = sensor_sample_fetch(bme280_dev);
    if (rc != 0) {
        shell_fprintf(shell, SHELL_ERROR, "Failed to fetch sample (%d)\n", rc);
        return -1;
    }

    rc = sensor_channel_get(bme280_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
    if (rc != 0) {
        shell_fprintf(shell, SHELL_ERROR, "Failed to get temperature (%d)\n", rc);
        return -1;
    }

    shell_fprintf(shell, SHELL_NORMAL, "Temperature: %.2f °C\n",
                 sensor_value_to_double(&temp));
    return 0;
}

static int cmd_bme280_press(const struct shell *shell, size_t argc, char *argv[])
{
    struct sensor_value press;
    int rc = sensor_sample_fetch(bme280_dev);
    if (rc != 0) {
        shell_fprintf(shell, SHELL_ERROR, "Failed to fetch sample (%d)\n", rc);
        return -1;
    }

    rc = sensor_channel_get(bme280_dev, SENSOR_CHAN_PRESS, &press);
    if (rc != 0) {
        shell_fprintf(shell, SHELL_ERROR, "Failed to get pressure (%d)\n", rc);
        return -1;
    }

    shell_fprintf(shell, SHELL_NORMAL, "Pressure: %.2f kPa\n",
                 sensor_value_to_double(&press));
    return 0;
}

static int cmd_bme280_humidity(const struct shell *shell, size_t argc, char *argv[])
{
    struct sensor_value humidity;
    int rc = sensor_sample_fetch(bme280_dev);
    if (rc != 0) {
        shell_fprintf(shell, SHELL_ERROR, "Failed to fetch sample (%d)\n", rc);
        return -1;
    }

    rc = sensor_channel_get(bme280_dev, SENSOR_CHAN_HUMIDITY, &humidity);
    if (rc != 0) {
        shell_fprintf(shell, SHELL_ERROR, "Failed to get humidity (%d)\n", rc);
        return -1;
    }

    shell_fprintf(shell, SHELL_NORMAL, "Humidity: %.2f %%\n",
                 sensor_value_to_double(&humidity));
    return 0;
}

static int cmd_bme280_send(const struct shell *shell, size_t argc, char *argv[])
{
    if (argc != 3) {
        shell_fprintf(shell, SHELL_ERROR,
                     "Usage: bme280 send <server_ip> <port>\n"
                     "Example: bme280 send 192.168.1.100 4242\n");
        return -1;
    }

    const char *server_addr = argv[1];
    char *endptr;
    long port = strtol(argv[2], &endptr, 10);

    if (*endptr != '\0' || port <= 0 || port > 65535) {
        shell_fprintf(shell, SHELL_ERROR, "Invalid port number\n");
        return -1;
    }

    int ret = send_sensor_data_tcp(server_addr, (uint16_t)port);
    if (ret < 0) {
        shell_fprintf(shell, SHELL_ERROR, "Failed to send data: %d\n", ret);
        return -1;
    }

    shell_fprintf(shell, SHELL_NORMAL, "Data sent successfully\n");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    bme280_cmds,
    SHELL_CMD(temp, NULL, "Read BME280 temperature", cmd_bme280_temp),
    SHELL_CMD(press, NULL, "Read BME280 pressure", cmd_bme280_press),
    SHELL_CMD(humidity, NULL, "Read BME280 humidity", cmd_bme280_humidity),
    SHELL_CMD_ARG(send, NULL,
        "Send BME280 data to TCP server\n"
        "Usage: send <server_ip> <port>",
        cmd_bme280_send, 3, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bme280, &bme280_cmds, "BME280 sensor commands", NULL);

void main(void)
{
	const struct device *usb_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;
	struct net_if *iface;

	if (usb_enable(NULL)) {
		return;
	}

	/* Wait for DTR to be set before starting shell */
	while (!dtr) {
		uart_line_ctrl_get(usb_dev, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}

	/* Check if BME280 is ready */
	if (!device_is_ready(bme280_dev)) {
		printk("BME280 device not ready\n");
		return;
	}

	/* Initialize networking */
	iface = net_if_get_default();
	if (!iface) {
		printk("No network interface available\n");
		return;
	}

	/* Wait for network interface to be up */
	if (!net_if_is_up(iface)) {
		net_if_up(iface);
		k_msleep(500); /* Give some time for the network to initialize */
	}

	/* The shell is now ready for input */
	k_sleep(K_FOREVER);
}
#include <zephyr/kernel.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/icmp.h>
#include <zephyr/random/random.h>

LOG_MODULE_REGISTER(w5500_dhcp_monitor, LOG_LEVEL_DBG);

#define INTERNET_CHECK_INTERVAL K_SECONDS(10)
#define INTERNET_CHECK_HOST "8.8.8.8"  // Google's DNS server
#define PING_PAYLOAD_SIZE 32           // Size of ping payload
#define PING_TIMEOUT K_SECONDS(2)      // Timeout for ping response
#define DHCP_TIMEOUT K_SECONDS(30)     // Timeout for DHCP address acquisition
#define DHCP_MAX_RETRIES 3            // Maximum number of DHCP retries
#define DHCP_RETRY_DELAY K_SECONDS(5)  // Initial retry delay
#define ALERT_STACK_SIZE 1024         // Stack size for alert thread
#define ALERT_THREAD_PRIORITY 5       // Priority for alert thread

// Add connection state tracking
static bool was_previously_connected = false;

static void check_internet_connectivity(struct k_work *work);
static struct k_work_delayable internet_check_work;
static uint8_t ping_data[PING_PAYLOAD_SIZE];  // Ping payload buffer
static K_SEM_DEFINE(dhcp_bound_sem, 0, 1);    // Semaphore for DHCP completion

struct ping_context {
    struct net_icmp_ctx icmp;
    struct sockaddr_in addr;
    struct net_if *iface;
    bool is_connected;
    struct k_work_delayable timeout_work;  // For handling timeouts
};

static struct ping_context ping_ctx;

static K_THREAD_STACK_DEFINE(alert_stack_area, ALERT_STACK_SIZE);
static struct k_thread alert_thread_data;
static k_tid_t alert_tid;
static K_SEM_DEFINE(alert_sem, 0, 1);

// Structure to hold alert information
struct connectivity_alert {
    uint32_t timestamp;
    const char *reason;
};

static struct connectivity_alert current_alert;

static void connectivity_alert_handler(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        // Wait for alert trigger
        k_sem_take(&alert_sem, K_FOREVER);
        
        LOG_ERR("ALERT: Internet connectivity lost at timestamp %u", current_alert.timestamp);
        LOG_ERR("Reason: %s", current_alert.reason);
        
        // TODO: Implement your alert actions here
        // For example:
        // - Update status LEDs
        // - Send notification to backup system
        // - Log to persistent storage
        // - Trigger external alert mechanism
    }
}

static void trigger_connectivity_alert(const char *reason)
{
    // Only trigger alert if we're transitioning from connected to disconnected
    if (was_previously_connected) {
        // Update alert information
        current_alert.timestamp = k_uptime_get_32();
        current_alert.reason = reason;
        
        // Signal the alert thread
        k_sem_give(&alert_sem);
        
        // Update state
        was_previously_connected = false;
    }
}

static void ping_timeout_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct ping_context *ctx = CONTAINER_OF(dwork, struct ping_context, timeout_work);

    if (!ctx->is_connected) {
        trigger_connectivity_alert("Ping timeout - no response from remote host");
    }

    net_icmp_cleanup_ctx(&ctx->icmp);
}

static int echo_reply_handler(struct net_icmp_ctx *ctx,
                             struct net_pkt *pkt,
                             struct net_icmp_ip_hdr *ip_hdr,
                             struct net_icmp_hdr *icmp_hdr,
                             void *user_data)
{
    struct ping_context *pctx = user_data;
    uint32_t send_time = *(uint32_t *)ping_data;
    uint32_t rtt = k_uptime_get() - send_time;

    // Cancel the timeout since we got a response
    k_work_cancel_delayable(&pctx->timeout_work);

    pctx->is_connected = true;
    was_previously_connected = true;  // Update state on successful connection
    LOG_INF("Internet connectivity is ACTIVE (RTT: %u ms)", rtt);

    net_icmp_cleanup_ctx(&pctx->icmp);
    return 0;
}

static void internet_connectivity_monitor(void)
{
    int ret;
    struct net_icmp_ping_params params = {
        .identifier = sys_rand32_get(),
        .sequence = 1,
        .tc_tos = 0,
        .priority = 0,
        .data = ping_data,
        .data_size = sizeof(uint32_t)  // Just send timestamp
    };

    LOG_INF("Checking internet connectivity...");

    // Initialize ICMP context
    ret = net_icmp_init_ctx(&ping_ctx.icmp, NET_ICMPV4_ECHO_REPLY, 0, echo_reply_handler);
    if (ret < 0) {
        LOG_ERR("Failed to init ICMP context: %d", ret);
        return;
    }

    // Setup destination address
    memset(&ping_ctx.addr, 0, sizeof(ping_ctx.addr));
    ping_ctx.addr.sin_family = AF_INET;
    ret = zsock_inet_pton(AF_INET, INTERNET_CHECK_HOST, &ping_ctx.addr.sin_addr);
    if (ret != 1) {
        LOG_ERR("Invalid address format");
        goto cleanup;
    }

    // Get interface
    ping_ctx.iface = net_if_get_default();
    if (!ping_ctx.iface) {
        LOG_ERR("No default interface");
        goto cleanup;
    }

    ping_ctx.is_connected = false;

    // Store current timestamp in ping data for RTT calculation
    *(uint32_t *)ping_data = k_uptime_get();

    LOG_INF("Sending ICMP echo request...");

    ret = net_icmp_send_echo_request_no_wait(&ping_ctx.icmp,
                                            ping_ctx.iface,
                                            (struct sockaddr *)&ping_ctx.addr,
                                            &params,
                                            &ping_ctx);
    if (ret < 0) {
        LOG_ERR("Failed to send echo request: %d", ret);
        goto cleanup;
    }

    // Schedule timeout handler
    k_work_schedule(&ping_ctx.timeout_work, PING_TIMEOUT);
    return;

cleanup:
    net_icmp_cleanup_ctx(&ping_ctx.icmp);
    trigger_connectivity_alert("Failed to establish ICMP connection");
    LOG_ERR("Internet is UNREACHABLE");
}

static void check_internet_connectivity(struct k_work *work)
{
    internet_connectivity_monitor();

    // Reschedule the work
    k_work_schedule(&internet_check_work, INTERNET_CHECK_INTERVAL);
}

static void net_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                   uint32_t mgmt_event,
                                   struct net_if *iface)
{
    if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND) {
        char ip[INET_ADDRSTRLEN];
        struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

        // Log the acquired IP address
        zsock_inet_ntop(AF_INET,
                       &ipv4->unicast[0].ipv4.address.in_addr,
                       ip, sizeof(ip));
        LOG_INF("DHCP bound - IP address: %s", ip);

        // Initialize the timeout work
        k_work_init_delayable(&ping_ctx.timeout_work, ping_timeout_handler);

        // Start internet connectivity monitoring
        k_work_init_delayable(&internet_check_work, check_internet_connectivity);
        k_work_schedule(&internet_check_work, K_SECONDS(2)); // Start sooner for first check

        // Signal DHCP completion
        k_sem_give(&dhcp_bound_sem);
    }
}

static struct net_mgmt_event_callback net_mgmt_cb;

static int start_dhcp_with_retries(struct net_if *iface)
{
    int ret;
    int retry_count = 0;
    uint32_t delay_ms = k_ticks_to_ms_floor32(DHCP_RETRY_DELAY.ticks);

    while (retry_count < DHCP_MAX_RETRIES) {
        if (retry_count > 0) {
            LOG_INF("DHCP retry %d of %d (delay: %d seconds)...", 
                   retry_count, DHCP_MAX_RETRIES, 
                   delay_ms / 1000);
            k_msleep(delay_ms);
            // Double the delay for next retry (exponential backoff)
            delay_ms *= 2;
        }

        LOG_INF("Starting DHCP...");
        net_dhcpv4_start(iface);

        // Wait for DHCP with timeout
        ret = k_sem_take(&dhcp_bound_sem, DHCP_TIMEOUT);
        if (ret == 0) {
            // Success
            return 0;
        }

        LOG_WRN("DHCP attempt %d failed (timeout)", retry_count + 1);
        retry_count++;
    }

    LOG_ERR("Failed to get IP address after %d attempts", DHCP_MAX_RETRIES);
    return -1;
}

int main(void)
{
    struct net_if *iface;
    const struct device *dev;
    uint32_t dtr = 0;
    int ret;

    /* Initialize USB */
    if (usb_enable(NULL)) {
        LOG_ERR("Failed to enable USB");
        return -1;
    }

    /* Wait for the USB device to be ready */
    dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);
    if (!device_is_ready(dev)) {
        LOG_ERR("CDC ACM device not ready");
        return -1;
    }

    /* Wait for DTR to be set before starting shell */
    while (!dtr) {
        uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100));
    }

    LOG_INF("W5500 DHCP and Internet Connectivity Monitor");

    // Create alert handler thread
    alert_tid = k_thread_create(&alert_thread_data, alert_stack_area,
                               K_THREAD_STACK_SIZEOF(alert_stack_area),
                               connectivity_alert_handler,
                               NULL, NULL, NULL,
                               ALERT_THREAD_PRIORITY, 0, K_NO_WAIT);
    if (!alert_tid) {
        LOG_ERR("Failed to create alert handler thread");
        return -1;
    }
    k_thread_name_set(alert_tid, "net_alert");

    // Get the network interface
    iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("No default network interface found");
        return -1;
    }

    // Setup network management event callback for DHCP
    net_mgmt_init_event_callback(&net_mgmt_cb, net_mgmt_event_handler,
                                NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&net_mgmt_cb);

    // Start DHCP with retries
    ret = start_dhcp_with_retries(iface);
    if (ret < 0) {
        return -1;
    }

    return 0;
}
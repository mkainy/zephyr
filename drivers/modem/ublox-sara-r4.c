/*
 * Copyright (c) 2019-2020 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_REGISTER(modem_ublox_sara_r4, CONFIG_MODEM_LOG_LEVEL);

#include <kernel.h>
#include <ctype.h>
#include <errno.h>
#include <zephyr.h>
#include <drivers/gpio.h>
#include <device.h>
#include <init.h>

#include <net/net_if.h>
#include <net/net_offload.h>
#include <net/socket_offload.h>

#include "modem_context.h"
#include "modem_socket.h"
#include "modem_cmd_handler.h"
#include "modem_iface_uart.h"

#if !defined(CONFIG_MODEM_UBLOX_SARA_R4_MANUAL_MCCMNO)
#define CONFIG_MODEM_UBLOX_SARA_R4_MANUAL_MCCMNO ""
#endif

/* pin settings */
enum mdm_control_pins {
	MDM_POWER = 0,
	MDM_RESET,
#if defined(DT_UBLOX_SARA_R4_0_MDM_VINT_GPIOS_CONTROLLER)
	MDM_VINT,
#endif
};

static struct modem_pin modem_pins[] = {
	/* MDM_POWER */
	MODEM_PIN(DT_INST_0_UBLOX_SARA_R4_MDM_POWER_GPIOS_CONTROLLER,
		  DT_INST_0_UBLOX_SARA_R4_MDM_POWER_GPIOS_PIN,
		  DT_INST_0_UBLOX_SARA_R4_MDM_POWER_GPIOS_FLAGS | GPIO_OUTPUT),

	/* MDM_RESET */
	MODEM_PIN(DT_INST_0_UBLOX_SARA_R4_MDM_RESET_GPIOS_CONTROLLER,
		  DT_INST_0_UBLOX_SARA_R4_MDM_RESET_GPIOS_PIN,
		  DT_INST_0_UBLOX_SARA_R4_MDM_RESET_GPIOS_FLAGS | GPIO_OUTPUT),

#if defined(DT_UBLOX_SARA_R4_0_MDM_VINT_GPIOS_CONTROLLER)
	/* MDM_VINT */
	MODEM_PIN(DT_INST_0_UBLOX_SARA_R4_MDM_VINT_GPIOS_CONTROLLER,
		  DT_INST_0_UBLOX_SARA_R4_MDM_VINT_GPIOS_PIN,
		  DT_INST_0_UBLOX_SARA_R4_MDM_VINT_GPIOS_FLAGS | GPIO_INPUT),
#endif
};

#define MDM_UART_DEV_NAME		DT_INST_0_UBLOX_SARA_R4_BUS_NAME

#define MDM_POWER_ENABLE		1
#define MDM_POWER_DISABLE		0
#define MDM_RESET_NOT_ASSERTED		1
#define MDM_RESET_ASSERTED		0
#if defined(DT_UBLOX_SARA_R4_0_MDM_VINT_GPIOS_CONTROLLER)
#define MDM_VINT_ENABLE			1
#define MDM_VINT_DISABLE		0
#endif

#define MDM_CMD_TIMEOUT			K_SECONDS(10)
#define MDM_DNS_TIMEOUT			K_SECONDS(70)
#define MDM_CMD_CONN_TIMEOUT		K_SECONDS(120)
#define MDM_REGISTRATION_TIMEOUT	K_SECONDS(180)
#define MDM_PROMPT_CMD_DELAY		K_MSEC(75)

#define MDM_MAX_DATA_LENGTH		1024
#define MDM_RECV_MAX_BUF		30
#define MDM_RECV_BUF_SIZE		128

#define MDM_MAX_SOCKETS			6
#define MDM_BASE_SOCKET_NUM		0

#define MDM_NETWORK_RETRY_COUNT		3
#define MDM_WAIT_FOR_RSSI_COUNT		10
#define MDM_WAIT_FOR_RSSI_DELAY		K_SECONDS(2)

#define BUF_ALLOC_TIMEOUT		K_SECONDS(1)

#define MDM_MANUFACTURER_LENGTH		10
#define MDM_MODEL_LENGTH		16
#define MDM_REVISION_LENGTH		64
#define MDM_IMEI_LENGTH			16

#define RSSI_TIMEOUT_SECS		30

NET_BUF_POOL_DEFINE(mdm_recv_pool, MDM_RECV_MAX_BUF, MDM_RECV_BUF_SIZE,
		    0, NULL);

/* RX thread structures */
K_THREAD_STACK_DEFINE(modem_rx_stack,
		      CONFIG_MODEM_UBLOX_SARA_R4_RX_STACK_SIZE);
struct k_thread modem_rx_thread;

/* RX thread work queue */
K_THREAD_STACK_DEFINE(modem_workq_stack,
		      CONFIG_MODEM_UBLOX_SARA_R4_RX_WORKQ_STACK_SIZE);
static struct k_work_q modem_workq;

/* socket read callback data */
struct socket_read_data {
	char *recv_buf;
	size_t recv_buf_len;
	struct sockaddr *recv_addr;
	u16_t recv_read_len;
};

/* driver data */
struct modem_data {
	struct net_if *net_iface;
	u8_t mac_addr[6];

	/* modem interface */
	struct modem_iface_uart_data iface_data;
	u8_t iface_isr_buf[MDM_RECV_BUF_SIZE];
	u8_t iface_rb_buf[MDM_MAX_DATA_LENGTH];

	/* modem cmds */
	struct modem_cmd_handler_data cmd_handler_data;
	u8_t cmd_read_buf[MDM_RECV_BUF_SIZE];
	u8_t cmd_match_buf[MDM_RECV_BUF_SIZE + 1];

	/* socket data */
	struct modem_socket_config socket_config;
	struct modem_socket sockets[MDM_MAX_SOCKETS];

	/* RSSI work */
	struct k_delayed_work rssi_query_work;

	/* modem data */
	char mdm_manufacturer[MDM_MANUFACTURER_LENGTH];
	char mdm_model[MDM_MODEL_LENGTH];
	char mdm_revision[MDM_REVISION_LENGTH];
	char mdm_imei[MDM_IMEI_LENGTH];

	/* modem state */
	int ev_creg;

	/* bytes written to socket in last transaction */
	int sock_written;

	/* response semaphore */
	struct k_sem sem_response;
};

static struct modem_data mdata;
static struct modem_context mctx;

#if defined(CONFIG_DNS_RESOLVER)
static struct addrinfo result;
static struct sockaddr result_addr;
static char result_canonname[DNS_MAX_NAME_SIZE + 1];
#endif

/* helper macro to keep readability */
#define ATOI(s_, value_, desc_) modem_atoi(s_, value_, desc_, __func__)

/**
 * @brief  Convert string to long integer, but handle errors
 *
 * @param  s: string with representation of integer number
 * @param  err_value: on error return this value instead
 * @param  desc: name the string being converted
 * @param  func: function where this is called (typically __func__)
 *
 * @retval return integer conversion on success, or err_value on error
 */
static int modem_atoi(const char *s, const int err_value,
		      const char *desc, const char *func)
{
	int ret;
	char *endptr;

	ret = (int)strtol(s, &endptr, 10);
	if (!endptr || *endptr != '\0') {
		LOG_ERR("bad %s '%s' in %s", log_strdup(s), log_strdup(desc),
			log_strdup(func));
		return err_value;
	}

	return ret;
}

/* send binary data via the +USO[ST/WR] commands */
static ssize_t send_socket_data(struct modem_socket *sock,
				const struct sockaddr *dst_addr,
				struct modem_cmd *handler_cmds,
				size_t handler_cmds_len,
				const char *buf, size_t buf_len, int timeout)
{
	int ret;
	char send_buf[sizeof("AT+USO**=#,!###.###.###.###!,#####,####\r\n")];
	u16_t dst_port = 0U;

	if (!sock) {
		return -EINVAL;
	}

	/*
	 * Binary and ASCII mode allows sending MDM_MAX_DATA_LENGTH bytes to
	 * the socket in one command
	 */
	if (buf_len > MDM_MAX_DATA_LENGTH) {
		buf_len = MDM_MAX_DATA_LENGTH;
	}

	/* The number of bytes written will be reported by the modem */
	mdata.sock_written = 0;

	if (sock->ip_proto == IPPROTO_UDP) {
		ret = modem_context_get_addr_port(dst_addr, &dst_port);
		snprintk(send_buf, sizeof(send_buf),
			 "AT+USOST=%d,\"%s\",%u,%zu", sock->id,
			 modem_context_sprint_ip_addr(dst_addr),
			 dst_port, buf_len);
	} else {
		snprintk(send_buf, sizeof(send_buf), "AT+USOWR=%d,%zu",
			 sock->id, buf_len);
	}

	k_sem_take(&mdata.cmd_handler_data.sem_tx_lock, K_FOREVER);

	ret = modem_cmd_send_nolock(&mctx.iface, &mctx.cmd_handler,
				    NULL, 0U, send_buf, NULL, K_NO_WAIT);
	if (ret < 0) {
		goto exit;
	}

	/* set command handlers */
	ret = modem_cmd_handler_update_cmds(&mdata.cmd_handler_data,
					    handler_cmds, handler_cmds_len,
					    true);
	if (ret < 0) {
		goto exit;
	}

	/* slight pause per spec so that @ prompt is received */
	k_sleep(MDM_PROMPT_CMD_DELAY);
	mctx.iface.write(&mctx.iface, buf, buf_len);

	if (timeout == K_NO_WAIT) {
		ret = 0;
		goto exit;
	}

	k_sem_reset(&mdata.sem_response);
	ret = k_sem_take(&mdata.sem_response, timeout);

	if (ret == 0) {
		ret = modem_cmd_handler_get_error(&mdata.cmd_handler_data);
	} else if (ret == -EAGAIN) {
		ret = -ETIMEDOUT;
	}

exit:
	/* unset handler commands and ignore any errors */
	(void)modem_cmd_handler_update_cmds(&mdata.cmd_handler_data,
					    NULL, 0U, false);
	k_sem_give(&mdata.cmd_handler_data.sem_tx_lock);

	if (ret < 0) {
		return ret;
	}

	return mdata.sock_written;
}

/*
 * Modem Response Command Handlers
 */

/* Handler: OK */
MODEM_CMD_DEFINE(on_cmd_ok)
{
	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&mdata.sem_response);
	return 0;
}

/* Handler: ERROR */
MODEM_CMD_DEFINE(on_cmd_error)
{
	modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&mdata.sem_response);
	return 0;
}

/* Handler: +CME Error: <err>[0] */
MODEM_CMD_DEFINE(on_cmd_exterror)
{
	/* TODO: map extended error codes to values */
	modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&mdata.sem_response);
	return 0;
}

/*
 * Modem Info Command Handlers
 */

/* Handler: <manufacturer> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_manufacturer)
{
	size_t out_len;

	out_len = net_buf_linearize(mdata.mdm_manufacturer,
				    sizeof(mdata.mdm_manufacturer) - 1,
				    data->rx_buf, 0, len);
	mdata.mdm_manufacturer[out_len] = '\0';
	LOG_INF("Manufacturer: %s", log_strdup(mdata.mdm_manufacturer));
	return 0;
}

/* Handler: <model> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_model)
{
	size_t out_len;

	out_len = net_buf_linearize(mdata.mdm_model,
				    sizeof(mdata.mdm_model) - 1,
				    data->rx_buf, 0, len);
	mdata.mdm_model[out_len] = '\0';
	LOG_INF("Model: %s", log_strdup(mdata.mdm_model));
	return 0;
}

/* Handler: <rev> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_revision)
{
	size_t out_len;

	out_len = net_buf_linearize(mdata.mdm_revision,
				    sizeof(mdata.mdm_revision) - 1,
				    data->rx_buf, 0, len);
	mdata.mdm_revision[out_len] = '\0';
	LOG_INF("Revision: %s", log_strdup(mdata.mdm_revision));
	return 0;
}

/* Handler: <IMEI> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_imei)
{
	size_t out_len;

	out_len = net_buf_linearize(mdata.mdm_imei, sizeof(mdata.mdm_imei) - 1,
				    data->rx_buf, 0, len);
	mdata.mdm_imei[out_len] = '\0';
	LOG_INF("IMEI: %s", log_strdup(mdata.mdm_imei));
	return 0;
}

#if !defined(CONFIG_MODEM_UBLOX_SARA_U2)
/*
 * Handler: +CESQ: <rxlev>[0],<ber>[1],<rscp>[2],<ecn0>[3],<rsrq>[4],<rsrp>[5]
 */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_rssi_cesq)
{
	int rsrp, rxlev;

	rsrp = ATOI(argv[5], 0, "rsrp");
	rxlev = ATOI(argv[0], 0, "rxlev");
	if (rsrp >= 0 && rsrp <= 97) {
		mctx.data_rssi = -140 + (rsrp - 1);
		LOG_INF("RSRP: %d", mctx.data_rssi);
	} else if (rxlev >= 0 && rxlev <= 63) {
		mctx.data_rssi = -110 + (rxlev - 1);
		LOG_INF("RSSI: %d", mctx.data_rssi);
	} else {
		mctx.data_rssi = -1000;
		LOG_INF("RSRP/RSSI not known");
	}

	return 0;
}
#endif

#if defined(CONFIG_MODEM_UBLOX_SARA_U2)
/* Handler: +CSQ: <signal_power>[0],<qual>[1] */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_rssi_csq)
{
	int rssi;

	rssi = ATOI(argv[1], 0, "qual");
	if (rssi == 31) {
		mctx.data_rssi = -46;
	} else if (rssi >= 0 && rssi <= 31) {
		/* FIXME: This value depends on the RAT */
		mctx.data_rssi = -110 + ((rssi * 2) + 1);
	} else {
		mctx.data_rssi = -1000;
	}

	LOG_INF("QUAL: %d", mctx.data_rssi);
	return 0;
}
#endif

/*
 * Modem Socket Command Handlers
 */

/* Handler: +USOCR: <socket_id>[0] */
MODEM_CMD_DEFINE(on_cmd_sockcreate)
{
	struct modem_socket *sock = NULL;

	/* look up new socket by special id */
	sock = modem_socket_from_newid(&mdata.socket_config);
	if (sock) {
		sock->id = ATOI(argv[0],
				mdata.socket_config.base_socket_num - 1,
				"socket_id");
		/* on error give up modem socket */
		if (sock->id == mdata.socket_config.base_socket_num - 1) {
			modem_socket_put(&mdata.socket_config, sock->sock_fd);
		}
	}

	/* don't give back semaphore -- OK to follow */
	return 0;
}

/* Handler: +USO[WR|ST]: <socket_id>[0],<length>[1] */
MODEM_CMD_DEFINE(on_cmd_sockwrite)
{
	mdata.sock_written = ATOI(argv[1], 0, "length");
	LOG_DBG("bytes written: %d", mdata.sock_written);
	return 0;
}

/* Common code for +USOR[D|F]: "<data>" */
static int on_cmd_sockread_common(int socket_id,
				  struct modem_cmd_handler_data *data,
				  int socket_data_length, u16_t len)
{
	struct modem_socket *sock = NULL;
	struct socket_read_data *sock_data;
	int ret;

	if (!len) {
		LOG_ERR("Short +USOR[D|F] value.  Aborting!");
		return -EAGAIN;
	}

	/*
	 * make sure we still have buf data and next char in the buffer is a
	 * quote.
	 */
	if (!data->rx_buf || *data->rx_buf->data != '\"') {
		LOG_ERR("Incorrect format! Ignoring data!");
		return -EINVAL;
	}

	/* zero length */
	if (socket_data_length <= 0) {
		LOG_ERR("Length problem (%d).  Aborting!", socket_data_length);
		return -EAGAIN;
	}

	/* check to make sure we have all of the data (minus quotes) */
	if ((net_buf_frags_len(data->rx_buf) - 2) < socket_data_length) {
		LOG_DBG("Not enough data -- wait!");
		return -EAGAIN;
	}

	/* skip quote */
	len--;
	net_buf_pull_u8(data->rx_buf);
	if (!data->rx_buf->len) {
		data->rx_buf = net_buf_frag_del(NULL, data->rx_buf);
	}

	sock = modem_socket_from_id(&mdata.socket_config, socket_id);
	if (!sock) {
		LOG_ERR("Socket not found! (%d)", socket_id);
		ret = -EINVAL;
		goto exit;
	}

	sock_data = (struct socket_read_data *)sock->data;
	if (!sock_data) {
		LOG_ERR("Socket data not found! Skip handling (%d)", socket_id);
		ret = -EINVAL;
		goto exit;
	}

	ret = net_buf_linearize(sock_data->recv_buf, sock_data->recv_buf_len,
				data->rx_buf, 0, (u16_t)socket_data_length);
	data->rx_buf = net_buf_skip(data->rx_buf, ret);
	sock_data->recv_read_len = ret;
	if (ret != socket_data_length) {
		LOG_ERR("Total copied data is different then received data!"
			" copied:%d vs. received:%d", ret, socket_data_length);
		ret = -EINVAL;
	}

exit:
	/* remove packet from list (ignore errors) */
	(void)modem_socket_packet_size_update(&mdata.socket_config, sock,
					      -socket_data_length);

	/* don't give back semaphore -- OK to follow */
	return ret;
}

/*
 * Handler: +USORF: <socket_id>[0],<remote_ip_addr>[1],<remote_port>[2],
 *          <length>[3],"<data>"
*/
MODEM_CMD_DEFINE(on_cmd_sockreadfrom)
{
	/* TODO: handle remote_ip_addr */

	return on_cmd_sockread_common(ATOI(argv[0], 0, "socket_id"), data,
				      ATOI(argv[3], 0, "length"), len);
}

/* Handler: +USORD: <socket_id>[0],<length>[1],"<data>" */
MODEM_CMD_DEFINE(on_cmd_sockread)
{
	return on_cmd_sockread_common(ATOI(argv[0], 0, "socket_id"), data,
				      ATOI(argv[1], 0, "length"), len);
}

#if defined(CONFIG_DNS_RESOLVER)
/* Handler: +UDNSRN: "<resolved_ip_address>"[0], "<resolved_ip_address>"[1] */
MODEM_CMD_DEFINE(on_cmd_dns)
{
	/* chop off end quote */
	argv[0][strlen(argv[0]) - 1] = '\0';

	/* FIXME: Hard-code DNS on SARA-R4 to return IPv4 */
	result_addr.sa_family = AF_INET;
	/* skip beginning quote when parsing */
	(void)net_addr_pton(result.ai_family, &argv[0][1],
			    &((struct sockaddr_in *)&result_addr)->sin_addr);
	return 0;
}
#endif

/*
 * MODEM UNSOLICITED NOTIFICATION HANDLERS
 */

/* Handler: +UUSOCL: <socket_id>[0] */
MODEM_CMD_DEFINE(on_cmd_socknotifyclose)
{
	struct modem_socket *sock;

	sock = modem_socket_from_id(&mdata.socket_config,
				    ATOI(argv[0], 0, "socket_id"));
	if (sock) {
		sock->is_connected = false;
	}

	return 0;
}

/* Handler: +UUSOR[D|F]: <socket_id>[0],<length>[1] */
MODEM_CMD_DEFINE(on_cmd_socknotifydata)
{
	int ret, socket_id, new_total;
	struct modem_socket *sock;

	socket_id = ATOI(argv[0], 0, "socket_id");
	new_total = ATOI(argv[1], 0, "length");
	sock = modem_socket_from_id(&mdata.socket_config, socket_id);
	if (!sock) {
		return 0;
	}

	ret = modem_socket_packet_size_update(&mdata.socket_config, sock,
					      new_total);
	if (ret < 0) {
		LOG_ERR("socket_id:%d left_bytes:%d err: %d", socket_id,
			new_total, ret);
	}

	if (new_total > 0) {
		modem_socket_data_ready(&mdata.socket_config, sock);
	}

	return 0;
}

/* Handler: +CREG: <stat>[0] */
MODEM_CMD_DEFINE(on_cmd_socknotifycreg)
{
	mdata.ev_creg = ATOI(argv[0], 0, "stat");
	LOG_DBG("CREG:%d", mdata.ev_creg);
	return 0;
}

/* RX thread */
static void modem_rx(void)
{
	while (true) {
		/* wait for incoming data */
		k_sem_take(&mdata.iface_data.rx_sem, K_FOREVER);

		mctx.cmd_handler.process(&mctx.cmd_handler, &mctx.iface);

		/* give up time if we have a solid stream of data */
		k_yield();
	}
}

static int pin_init(void)
{
	LOG_INF("Setting Modem Pins");

	LOG_DBG("MDM_RESET_PIN -> NOT_ASSERTED");
	modem_pin_write(&mctx, MDM_RESET, MDM_RESET_NOT_ASSERTED);

	LOG_DBG("MDM_POWER_PIN -> ENABLE");
	modem_pin_write(&mctx, MDM_POWER, MDM_POWER_ENABLE);
	k_sleep(K_SECONDS(4));

	LOG_DBG("MDM_POWER_PIN -> DISABLE");
	modem_pin_write(&mctx, MDM_POWER, MDM_POWER_DISABLE);
#if defined(CONFIG_MODEM_UBLOX_SARA_U2)
	k_sleep(K_SECONDS(1));
#else
	k_sleep(K_SECONDS(4));
#endif
	LOG_DBG("MDM_POWER_PIN -> ENABLE");
	modem_pin_write(&mctx, MDM_POWER, MDM_POWER_ENABLE);
	k_sleep(K_SECONDS(1));

	/* make sure module is powered off */
#if defined(DT_UBLOX_SARA_R4_0_MDM_VINT_GPIOS_CONTROLLER)
	LOG_DBG("Waiting for MDM_VINT_PIN = 0");

	while (modem_pin_read(&mctx, MDM_VINT) != MDM_VINT_DISABLE) {
#if defined(CONFIG_MODEM_UBLOX_SARA_U2)
		/* try to power off again */
		LOG_DBG("MDM_POWER_PIN -> DISABLE");
		modem_pin_write(&mctx, MDM_POWER, MDM_POWER_DISABLE);
		k_sleep(K_SECONDS(1));
		LOG_DBG("MDM_POWER_PIN -> ENABLE");
		modem_pin_write(&mctx, MDM_POWER, MDM_POWER_ENABLE);
#endif
		k_sleep(K_MSEC(100));
	}
#else
	k_sleep(K_SECONDS(8));
#endif

	LOG_DBG("MDM_POWER_PIN -> DISABLE");

	unsigned int irq_lock_key = irq_lock();

	modem_pin_write(&mctx, MDM_POWER, MDM_POWER_DISABLE);
#if defined(CONFIG_MODEM_UBLOX_SARA_U2)
	k_usleep(50);		/* 50-80 microseconds */
#else
	k_sleep(K_SECONDS(1));
#endif
	modem_pin_write(&mctx, MDM_POWER, MDM_POWER_ENABLE);

	irq_unlock(irq_lock_key);

	LOG_DBG("MDM_POWER_PIN -> ENABLE");

#if defined(DT_UBLOX_SARA_R4_0_MDM_VINT_GPIOS_CONTROLLER)
	LOG_DBG("Waiting for MDM_VINT_PIN = 1");
	do {
		k_sleep(K_MSEC(100));
	} while (modem_pin_read(&mctx, MDM_VINT) != MDM_VINT_ENABLE);
#else
	k_sleep(K_SECONDS(10));
#endif

	modem_pin_config(&mctx, MDM_POWER, false);

	LOG_INF("... Done!");

	return 0;
}

static void modem_rssi_query_work(struct k_work *work)
{
	struct modem_cmd cmd =
#if defined(CONFIG_MODEM_UBLOX_SARA_U2)
		MODEM_CMD("+CSQ: ", on_cmd_atcmdinfo_rssi_csq, 2U, ",");
	static char *send_cmd = "AT+CSQ";
#else
		MODEM_CMD("+CESQ: ", on_cmd_atcmdinfo_rssi_cesq, 6U, ",");
	static char *send_cmd = "AT+CESQ";
#endif
	int ret;

	/* query modem RSSI */
	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
			     &cmd, 1U, send_cmd, &mdata.sem_response,
			     MDM_CMD_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("AT+C[E]SQ ret:%d", ret);
	}

	/* re-start RSSI query work */
	if (work) {
		k_delayed_work_submit_to_queue(&modem_workq,
					       &mdata.rssi_query_work,
					       K_SECONDS(RSSI_TIMEOUT_SECS));
	}
}

static void modem_reset(void)
{
	int ret = 0, retry_count = 0, counter = 0;
	static struct setup_cmd setup_cmds[] = {
		/* turn off echo */
		SETUP_CMD_NOHANDLE("ATE0"),
		/* stop functionality */
		SETUP_CMD_NOHANDLE("AT+CFUN=0"),
		/* extended error numbers */
		SETUP_CMD_NOHANDLE("AT+CMEE=1"),
#if defined(CONFIG_BOARD_PARTICLE_BORON)
		/* use external SIM */
		SETUP_CMD_NOHANDLE("AT+UGPIOC=23,0,0"),
#endif
#if defined(CONFIG_MODEM_UBLOX_SARA_R4_NET_STATUS_PIN)
		/* enable the network status indication */
		SETUP_CMD_NOHANDLE("AT+UGPIOC="
			STRINGIFY(CONFIG_MODEM_UBLOX_SARA_R4_NET_STATUS_PIN)
			",2"),
#endif
		/* UNC messages for registration */
		SETUP_CMD_NOHANDLE("AT+CREG=1"),
		/* query modem info */
		SETUP_CMD("AT+CGMI", "", on_cmd_atcmdinfo_manufacturer, 0U, ""),
		SETUP_CMD("AT+CGMM", "", on_cmd_atcmdinfo_model, 0U, ""),
		SETUP_CMD("AT+CGMR", "", on_cmd_atcmdinfo_revision, 0U, ""),
		SETUP_CMD("AT+CGSN", "", on_cmd_atcmdinfo_imei, 0U, ""),
		/* setup PDP context definition */
		SETUP_CMD_NOHANDLE("AT+CGDCONT=1,\"IP\",\""
					CONFIG_MODEM_UBLOX_SARA_R4_APN "\""),
		/* start functionality */
		SETUP_CMD_NOHANDLE("AT+CFUN=1"),
	};

	static struct setup_cmd post_setup_cmds[] = {
#if defined(CONFIG_MODEM_UBLOX_SARA_U2)
		/* set the APN */
		SETUP_CMD_NOHANDLE("AT+UPSD=0,1,\""
				CONFIG_MODEM_UBLOX_SARA_R4_APN "\""),
		/* set dynamic IP */
		SETUP_CMD_NOHANDLE("AT+UPSD=0,7,\"0.0.0.0\""),
		/* activate the GPRS connection */
		SETUP_CMD_NOHANDLE("AT+UPSDA=0,3"),
#else
		/* activate the PDP context */
		SETUP_CMD_NOHANDLE("AT+CGACT=1,1"),
#endif
	};

restart:
	/* stop RSSI delay work */
	k_delayed_work_cancel(&mdata.rssi_query_work);

	pin_init();

	LOG_INF("Waiting for modem to respond");

	/* Give the modem a while to start responding to simple 'AT' commands.
	 * Also wait for CSPS=1 or RRCSTATE=1 notification
	 */
	ret = -1;
	while (counter++ < 50 && ret < 0) {
		k_sleep(K_SECONDS(2));
		ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
				     NULL, 0, "AT", &mdata.sem_response,
				     MDM_CMD_TIMEOUT);
		if (ret < 0 && ret != -ETIMEDOUT) {
			break;
		}
	}

	if (ret < 0) {
		LOG_ERR("MODEM WAIT LOOP ERROR: %d", ret);
		goto error;
	}

	ret = modem_cmd_handler_setup_cmds(&mctx.iface, &mctx.cmd_handler,
					   setup_cmds, ARRAY_SIZE(setup_cmds),
					   &mdata.sem_response,
					   MDM_REGISTRATION_TIMEOUT);
	if (ret < 0) {
		goto error;
	}


	if (strlen(CONFIG_MODEM_UBLOX_SARA_R4_MANUAL_MCCMNO) > 0) {
		/* use manual MCC/MNO entry */
		ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
				     NULL, 0,
				     "AT+COPS=1,2,\""
					CONFIG_MODEM_UBLOX_SARA_R4_MANUAL_MCCMNO
					"\"",
				     &mdata.sem_response,
				     MDM_REGISTRATION_TIMEOUT);
	} else {
		/* register operator automatically */
		ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
				     NULL, 0, "AT+COPS=0,0",
				     &mdata.sem_response,
				     MDM_REGISTRATION_TIMEOUT);
	}

	if (ret < 0) {
		LOG_ERR("AT+COPS ret:%d", ret);
		goto error;
	}

	LOG_INF("Waiting for network");

	/*
	 * TODO: A lot of this should be setup as a 3GPP module to handle
	 * basic connection to the network commands / polling
	 */

	/* wait for +CREG: 1(normal) or 5(roaming) */
	counter = 0;
	while (counter++ < 40 && mdata.ev_creg != 1 && mdata.ev_creg != 5) {
		if (counter == 20) {
			LOG_WRN("Force restart of RF functionality");

			/* Disable RF temporarily */
			ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
				NULL, 0, "AT+CFUN=0", &mdata.sem_response,
				MDM_CMD_TIMEOUT);

			k_sleep(K_SECONDS(1));

			/* Enable RF */
			ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
				NULL, 0, "AT+CFUN=1", &mdata.sem_response,
				MDM_CMD_TIMEOUT);
		}

		k_sleep(K_SECONDS(1));
	}

	/* query modem RSSI */
	modem_rssi_query_work(NULL);
	k_sleep(MDM_WAIT_FOR_RSSI_DELAY);

	counter = 0;
	/* wait for RSSI < 0 and > -1000 */
	while (counter++ < MDM_WAIT_FOR_RSSI_COUNT &&
	       (mctx.data_rssi >= 0 ||
		mctx.data_rssi <= -1000)) {
		modem_rssi_query_work(NULL);
		k_sleep(MDM_WAIT_FOR_RSSI_DELAY);
	}

	if (mctx.data_rssi >= 0 || mctx.data_rssi <= -1000) {
		retry_count++;
		if (retry_count >= MDM_NETWORK_RETRY_COUNT) {
			LOG_ERR("Failed network init.  Too many attempts!");
			ret = -ENETUNREACH;
			goto error;
		}

		LOG_ERR("Failed network init.  Restarting process.");
		goto restart;
	}

	ret = modem_cmd_handler_setup_cmds(&mctx.iface, &mctx.cmd_handler,
					   post_setup_cmds,
					   ARRAY_SIZE(post_setup_cmds),
					   &mdata.sem_response,
					   MDM_REGISTRATION_TIMEOUT);
	if (ret < 0) {
		goto error;
	}

	LOG_INF("Network is ready.");

	/* start RSSI query */
	k_delayed_work_submit_to_queue(&modem_workq,
				       &mdata.rssi_query_work,
				       K_SECONDS(RSSI_TIMEOUT_SECS));

error:
	return;
}

/*
 * generic socket creation function
 * which can be called in bind() or connect()
 */
static int create_socket(struct modem_socket *sock, const struct sockaddr *addr)
{
	int ret;
	struct modem_cmd cmd = MODEM_CMD("+USOCR: ", on_cmd_sockcreate, 1U, "");
	char buf[sizeof("AT+USOCR=#,#####\r")];
	u16_t local_port = 0U, proto = 6U;

	if (addr) {
		if (addr->sa_family == AF_INET6) {
			local_port = ntohs(net_sin6(addr)->sin6_port);
		} else if (addr->sa_family == AF_INET) {
			local_port = ntohs(net_sin(addr)->sin_port);
		}
	}

	if (sock->ip_proto == IPPROTO_UDP) {
		proto = 17U;
	}

	if (local_port > 0U) {
		snprintk(buf, sizeof(buf), "AT+USOCR=%d,%u", proto, local_port);
	} else {
		snprintk(buf, sizeof(buf), "AT+USOCR=%d", proto);
	}

	/* create socket */
	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
			     &cmd, 1U, buf,
			     &mdata.sem_response, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("%s ret:%d", log_strdup(buf), ret);
		modem_socket_put(&mdata.socket_config, sock->sock_fd);
		errno = -ret;
		return -1;
	}

	errno = 0;
	return 0;
}

/*
 * Socket Offload OPS
 */

static const struct socket_op_vtable offload_socket_fd_op_vtable;

static int offload_socket(int family, int type, int proto)
{
	int ret;

	/* defer modem's socket create call to bind() */
	ret = modem_socket_get(&mdata.socket_config, family, type, proto);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	errno = 0;
	return ret;
}

static int offload_close(struct modem_socket *sock)
{
	char buf[sizeof("AT+USOCL=#\r")];
	int ret;

	/* make sure we assigned an id */
	if (sock->id < mdata.socket_config.base_socket_num) {
		return 0;
	}

	if (sock->is_connected) {
		snprintk(buf, sizeof(buf), "AT+USOCL=%d", sock->id);

		ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
				     NULL, 0U, buf,
				     &mdata.sem_response, MDM_CMD_TIMEOUT);
		if (ret < 0) {
			LOG_ERR("%s ret:%d", log_strdup(buf), ret);
		}
	}

	modem_socket_put(&mdata.socket_config, sock->sock_fd);
	return 0;
}

static int offload_bind(void *obj, const struct sockaddr *addr,
			socklen_t addrlen)
{
	struct modem_socket *sock = (struct modem_socket *)obj;

	/* save bind address information */
	memcpy(&sock->src, addr, sizeof(*addr));

	/* make sure we've created the socket */
	if (sock->id == mdata.socket_config.sockets_len + 1) {
		if (create_socket(sock, addr) < 0) {
			return -1;
		}
	}

	return 0;
}

static int offload_connect(void *obj, const struct sockaddr *addr,
			   socklen_t addrlen)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	int ret;
	char buf[sizeof("AT+USOCO=#,!###.###.###.###!,#####,#\r")];
	u16_t dst_port = 0U;

	if (!addr) {
		errno = EINVAL;
		return -1;
	}

	if (sock->id < mdata.socket_config.base_socket_num - 1) {
		LOG_ERR("Invalid socket_id(%d) from fd:%d",
			sock->id, sock->sock_fd);
		errno = EINVAL;
		return -1;
	}

	/* make sure we've created the socket */
	if (sock->id == mdata.socket_config.sockets_len + 1) {
		if (create_socket(sock, NULL) < 0) {
			return -1;
		}
	}

	memcpy(&sock->dst, addr, sizeof(*addr));
	if (addr->sa_family == AF_INET6) {
		dst_port = ntohs(net_sin6(addr)->sin6_port);
	} else if (addr->sa_family == AF_INET) {
		dst_port = ntohs(net_sin(addr)->sin_port);
	} else {
		errno = EAFNOSUPPORT;
		return -1;
	}

	/* skip socket connect if UDP */
	if (sock->ip_proto == IPPROTO_UDP) {
		errno = 0;
		return 0;
	}

	snprintk(buf, sizeof(buf), "AT+USOCO=%d,\"%s\",%d", sock->id,
		 modem_context_sprint_ip_addr(addr), dst_port);
	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
			     NULL, 0U, buf,
			     &mdata.sem_response, MDM_CMD_CONN_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("%s ret:%d", log_strdup(buf), ret);
		errno = -ret;
		return -1;
	}

	sock->is_connected = true;
	errno = 0;
	return 0;
}

/* support for POLLIN only for now. */
static int offload_poll(struct pollfd *fds, int nfds, int msecs)
{
	int i;
	void *obj;

	/* Only accept modem sockets. */
	for (i = 0; i < nfds; i++) {
		if (fds[i].fd < 0) {
			continue;
		}

		/* If vtable matches, then it's modem socket. */
		obj = z_get_fd_obj(fds[i].fd,
				   (const struct fd_op_vtable *)
						&offload_socket_fd_op_vtable,
				   EINVAL);
		if (obj == NULL) {
			return -1;
		}
	}

	return modem_socket_poll(&mdata.socket_config, fds, nfds, msecs);
}

static ssize_t offload_recvfrom(void *obj, void *buf, size_t len,
				int flags, struct sockaddr *from,
				socklen_t *fromlen)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	int ret, next_packet_size;
	struct modem_cmd cmd[] = {
		MODEM_CMD("+USORF: ", on_cmd_sockreadfrom, 4U, ","),
		MODEM_CMD("+USORD: ", on_cmd_sockread, 2U, ","),
	};
	char sendbuf[sizeof("AT+USORF=#,#####\r")];
	struct socket_read_data sock_data;

	if (!buf || len == 0) {
		errno = EINVAL;
		return -1;
	}

	if (flags & MSG_PEEK) {
		errno = ENOTSUP;
		return -1;
	}

	next_packet_size = modem_socket_next_packet_size(&mdata.socket_config,
							 sock);
	if (!next_packet_size) {
		if (flags & MSG_DONTWAIT) {
			errno = EAGAIN;
			return -1;
		}

		if (!sock->is_connected && sock->ip_proto != IPPROTO_UDP) {
			errno = 0;
			return 0;
		}

		modem_socket_wait_data(&mdata.socket_config, sock);
		next_packet_size = modem_socket_next_packet_size(
			&mdata.socket_config, sock);
	}

	/*
	 * Binary and ASCII mode allows sending MDM_MAX_DATA_LENGTH bytes to
	 * the socket in one command
	 */
	if (next_packet_size > MDM_MAX_DATA_LENGTH) {
		next_packet_size = MDM_MAX_DATA_LENGTH;
	}

	snprintk(sendbuf, sizeof(sendbuf), "AT+USO%s=%d,%d",
		 sock->ip_proto == IPPROTO_UDP ? "RF" : "RD", sock->id,
		 len < next_packet_size ? len : next_packet_size);

	/* socket read settings */
	(void)memset(&sock_data, 0, sizeof(sock_data));
	sock_data.recv_buf = buf;
	sock_data.recv_buf_len = len;
	sock_data.recv_addr = from;
	sock->data = &sock_data;

	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
			     cmd, ARRAY_SIZE(cmd), sendbuf, &mdata.sem_response,
			     MDM_CMD_TIMEOUT);
	if (ret < 0) {
		errno = -ret;
		ret = -1;
		goto exit;
	}

	/* HACK: use dst address as from */
	if (from && fromlen) {
		*fromlen = sizeof(sock->dst);
		memcpy(from, &sock->dst, *fromlen);
	}

	/* return length of received data */
	errno = 0;
	ret = sock_data.recv_read_len;

exit:
	/* clear socket data */
	sock->data = NULL;
	return ret;
}

static ssize_t offload_sendto(void *obj, const void *buf, size_t len,
			      int flags, const struct sockaddr *to,
			      socklen_t tolen)
{
	int ret;
	struct modem_socket *sock = (struct modem_socket *)obj;
	struct modem_cmd cmd[] = {
		MODEM_CMD("+USOST: ", on_cmd_sockwrite, 2U, ","),
		MODEM_CMD("+USOWR: ", on_cmd_sockwrite, 2U, ","),
	};

	if (!buf || len == 0) {
		errno = EINVAL;
		return -1;
	}

	if (!sock->is_connected && sock->ip_proto != IPPROTO_UDP) {
		errno = ENOTCONN;
		return -1;
	}

	if (!to && sock->ip_proto == IPPROTO_UDP) {
		to = &sock->dst;
	}

	ret = send_socket_data(sock, to, cmd, ARRAY_SIZE(cmd), buf, len,
			       MDM_CMD_TIMEOUT);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	errno = 0;
	return ret;
}

static int offload_ioctl(void *obj, unsigned int request, va_list args)
{
	switch (request) {
	/* Handle close specifically. */
	case ZFD_IOCTL_CLOSE:
		return offload_close((struct modem_socket *)obj);

	case ZFD_IOCTL_POLL_PREPARE:
		return -EXDEV;

	case ZFD_IOCTL_POLL_UPDATE:
		return -EOPNOTSUPP;

	case ZFD_IOCTL_POLL_OFFLOAD: {
		struct zsock_pollfd *fds;
		int nfds;
		int timeout;

		fds = va_arg(args, struct zsock_pollfd *);
		nfds = va_arg(args, int);
		timeout = va_arg(args, int);

		return offload_poll(fds, nfds, timeout);
	}

	default:
		errno = EINVAL;
		return -1;
	}
}

static ssize_t offload_read(void *obj, void *buffer, size_t count)
{
	return offload_recvfrom(obj, buffer, count, 0, NULL, 0);
}

static ssize_t offload_write(void *obj, const void *buffer, size_t count)
{
	return offload_sendto(obj, buffer, count, 0, NULL, 0);
}

static const struct socket_op_vtable offload_socket_fd_op_vtable = {
	.fd_vtable = {
		.read = offload_read,
		.write = offload_write,
		.ioctl = offload_ioctl,
	},
	.bind = offload_bind,
	.connect = offload_connect,
	.sendto = offload_sendto,
	.recvfrom = offload_recvfrom,
	.listen = NULL,
	.accept = NULL,
	.sendmsg = NULL,
	.getsockopt = NULL,
	.setsockopt = NULL,
};

static bool offload_is_supported(int family, int type, int proto)
{
	/* TODO offloading always enabled for now. */
	return true;
}

NET_SOCKET_REGISTER(ublox_sara_r4, AF_UNSPEC, offload_is_supported,
		    offload_socket);

#if defined(CONFIG_DNS_RESOLVER)
/* TODO: This is a bare-bones implementation of DNS handling
 * We ignore most of the hints like ai_family, ai_protocol and ai_socktype.
 * Later, we can add additional handling if it makes sense.
 */
static int offload_getaddrinfo(const char *node, const char *service,
			       const struct addrinfo *hints,
			       struct addrinfo **res)
{
	struct modem_cmd cmd = MODEM_CMD("+UDNSRN: ", on_cmd_dns, 1U, ",");
	u32_t port = 0U;
	int ret;
	/* DNS command + 128 bytes for domain name parameter */
	char sendbuf[sizeof("AT+UDNSRN=#,'[]'\r") + 128];

	/* init result */
	(void)memset(&result, 0, sizeof(result));
	(void)memset(&result_addr, 0, sizeof(result_addr));
	/* FIXME: Hard-code DNS to return only IPv4 */
	result.ai_family = AF_INET;
	result_addr.sa_family = AF_INET;
	result.ai_addr = &result_addr;
	result.ai_addrlen = sizeof(result_addr);
	result.ai_canonname = result_canonname;
	result_canonname[0] = '\0';

	/* check to see if node is an IP address */
	if (net_addr_pton(result.ai_family, node,
			  &((struct sockaddr_in *)&result_addr)->sin_addr)
	    == 1) {
		*res = &result;
		return 0;
	}

	/* user flagged node as numeric host, but we failed net_addr_pton */
	if (hints && hints->ai_flags & AI_NUMERICHOST) {
		return EAI_NONAME;
	}

	if (service) {
		port = ATOI(service, 0U, "port");
		if (port < 1 || port > USHRT_MAX) {
			return EAI_SERVICE;
		}
	}

	snprintk(sendbuf, sizeof(sendbuf), "AT+UDNSRN=0,\"%s\"", node);
	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler,
			     &cmd, 1U, sendbuf, &mdata.sem_response,
			     MDM_DNS_TIMEOUT);
	if (ret < 0) {
		return ret;
	}

	if (port > 0U) {
		/* FIXME: DNS is hard-coded to return only IPv4 */
		if (result.ai_family == AF_INET) {
			net_sin(&result_addr)->sin_port = htons(port);
		}
	}

	LOG_DBG("DNS RESULT: %s",
		log_strdup(net_addr_ntop(result.ai_family,
					 &net_sin(&result_addr)->sin_addr,
					 sendbuf, NET_IPV4_ADDR_LEN)));

	*res = (struct addrinfo *)&result;
	return 0;
}

static void offload_freeaddrinfo(struct addrinfo *res)
{
	/* using static result from offload_getaddrinfo() -- no need to free */
	res = NULL;
}

const struct socket_dns_offload offload_dns_ops = {
	.getaddrinfo = offload_getaddrinfo,
	.freeaddrinfo = offload_freeaddrinfo,
};
#endif

static int net_offload_dummy_get(sa_family_t family,
				 enum net_sock_type type,
				 enum net_ip_protocol ip_proto,
				 struct net_context **context)
{

	LOG_ERR("CONFIG_NET_SOCKETS_OFFLOAD must be enabled for this driver");

	return -ENOTSUP;
}

/* placeholders, until Zepyr IP stack updated to handle a NULL net_offload */
static struct net_offload modem_net_offload = {
	.get = net_offload_dummy_get,
};

#define HASH_MULTIPLIER		37
static u32_t hash32(char *str, int len)
{
	u32_t h = 0;
	int i;

	for (i = 0; i < len; ++i) {
		h = (h * HASH_MULTIPLIER) + str[i];
	}

	return h;
}

static inline u8_t *modem_get_mac(struct device *dev)
{
	struct modem_data *data = dev->driver_data;
	u32_t hash_value;

	data->mac_addr[0] = 0x00;
	data->mac_addr[1] = 0x10;

	/* use IMEI for mac_addr */
	hash_value = hash32(mdata.mdm_imei, strlen(mdata.mdm_imei));

	UNALIGNED_PUT(hash_value, (u32_t *)(data->mac_addr + 2));

	return data->mac_addr;
}

static void modem_net_iface_init(struct net_if *iface)
{
	struct device *dev = net_if_get_device(iface);
	struct modem_data *data = dev->driver_data;

	/* Direct socket offload used instead of net offload: */
	iface->if_dev->offload = &modem_net_offload;
	net_if_set_link_addr(iface, modem_get_mac(dev),
			     sizeof(data->mac_addr),
			     NET_LINK_ETHERNET);
	data->net_iface = iface;
#ifdef CONFIG_DNS_RESOLVER
	socket_offload_dns_register(&offload_dns_ops);
#endif
}

static struct net_if_api api_funcs = {
	.init = modem_net_iface_init,
};

static struct modem_cmd response_cmds[] = {
	MODEM_CMD("OK", on_cmd_ok, 0U, ""), /* 3GPP */
	MODEM_CMD("ERROR", on_cmd_error, 0U, ""), /* 3GPP */
	MODEM_CMD("+CME ERROR: ", on_cmd_exterror, 1U, ""),
};

static struct modem_cmd unsol_cmds[] = {
	MODEM_CMD("+UUSOCL: ", on_cmd_socknotifyclose, 1U, ""),
	MODEM_CMD("+UUSORD: ", on_cmd_socknotifydata, 2U, ","),
	MODEM_CMD("+UUSORF: ", on_cmd_socknotifydata, 2U, ","),
	MODEM_CMD("+CREG: ", on_cmd_socknotifycreg, 1U, ""),
};

static int modem_init(struct device *dev)
{
	int ret = 0;

	ARG_UNUSED(dev);

	k_sem_init(&mdata.sem_response, 0, 1);

	/* initialize the work queue */
	k_work_q_start(&modem_workq,
		       modem_workq_stack,
		       K_THREAD_STACK_SIZEOF(modem_workq_stack),
		       K_PRIO_COOP(7));

	/* socket config */
	mdata.socket_config.sockets = &mdata.sockets[0];
	mdata.socket_config.sockets_len = ARRAY_SIZE(mdata.sockets);
	mdata.socket_config.base_socket_num = MDM_BASE_SOCKET_NUM;
	ret = modem_socket_init(&mdata.socket_config,
				&offload_socket_fd_op_vtable);
	if (ret < 0) {
		goto error;
	}

	/* cmd handler */
	mdata.cmd_handler_data.cmds[CMD_RESP] = response_cmds;
	mdata.cmd_handler_data.cmds_len[CMD_RESP] = ARRAY_SIZE(response_cmds);
	mdata.cmd_handler_data.cmds[CMD_UNSOL] = unsol_cmds;
	mdata.cmd_handler_data.cmds_len[CMD_UNSOL] = ARRAY_SIZE(unsol_cmds);
	mdata.cmd_handler_data.read_buf = &mdata.cmd_read_buf[0];
	mdata.cmd_handler_data.read_buf_len = sizeof(mdata.cmd_read_buf);
	mdata.cmd_handler_data.match_buf = &mdata.cmd_match_buf[0];
	mdata.cmd_handler_data.match_buf_len = sizeof(mdata.cmd_match_buf);
	mdata.cmd_handler_data.buf_pool = &mdm_recv_pool;
	mdata.cmd_handler_data.alloc_timeout = BUF_ALLOC_TIMEOUT;
	mdata.cmd_handler_data.eol = "\r";
	ret = modem_cmd_handler_init(&mctx.cmd_handler,
				     &mdata.cmd_handler_data);
	if (ret < 0) {
		goto error;
	}

	/* modem interface */
	mdata.iface_data.isr_buf = &mdata.iface_isr_buf[0];
	mdata.iface_data.isr_buf_len = sizeof(mdata.iface_isr_buf);
	mdata.iface_data.rx_rb_buf = &mdata.iface_rb_buf[0];
	mdata.iface_data.rx_rb_buf_len = sizeof(mdata.iface_rb_buf);
	ret = modem_iface_uart_init(&mctx.iface, &mdata.iface_data,
				    MDM_UART_DEV_NAME);
	if (ret < 0) {
		goto error;
	}

	/* modem data storage */
	mctx.data_manufacturer = mdata.mdm_manufacturer;
	mctx.data_model = mdata.mdm_model;
	mctx.data_revision = mdata.mdm_revision;
	mctx.data_imei = mdata.mdm_imei;

	/* pin setup */
	mctx.pins = modem_pins;
	mctx.pins_len = ARRAY_SIZE(modem_pins);

	mctx.driver_data = &mdata;

	ret = modem_context_register(&mctx);
	if (ret < 0) {
		LOG_ERR("Error registering modem context: %d", ret);
		goto error;
	}

	/* start RX thread */
	k_thread_create(&modem_rx_thread, modem_rx_stack,
			K_THREAD_STACK_SIZEOF(modem_rx_stack),
			(k_thread_entry_t) modem_rx,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	/* init RSSI query */
	k_delayed_work_init(&mdata.rssi_query_work, modem_rssi_query_work);

	modem_reset();

error:
	return ret;
}

NET_DEVICE_OFFLOAD_INIT(modem_sara, CONFIG_MODEM_UBLOX_SARA_R4_NAME,
			modem_init, &mdata, NULL,
			CONFIG_MODEM_UBLOX_SARA_R4_INIT_PRIORITY, &api_funcs,
			MDM_MAX_DATA_LENGTH);

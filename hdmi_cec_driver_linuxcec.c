/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2026 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Raspberry Pi Linux CEC Subsystem-based HAL Implementation
 *
 * This implementation uses the standard Linux CEC kernel API (/dev/cec0).
 * CEC messages are sent and received via POSIX ioctls on the CEC device node.
 * A dedicated receive thread uses poll() + CEC_RECEIVE to deliver callbacks.
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <poll.h>
#include <stdarg.h>

#include <linux/cec.h>

#include "hdmi_cec_driver.h"

#ifndef HAL_VERSION
#define HAL_VERSION "unknown"
#endif
#ifndef GIT_COMMIT_SHA
#define GIT_COMMIT_SHA "unknown"
#endif

#ifndef CEC_MAX_MSG_SIZE
#define CEC_MAX_MSG_SIZE        16
#endif

/* CEC_DEV_NODE is the full CEC device node path, injected at compile time
 * via -DCEC_DEV_NODE="/dev/cecN".  Default is /dev/cec0 (HDMI0).
 * Override at build time via cmake -DCEC_DEV_NODE=/dev/cec1 for HDMI1. */
#ifndef CEC_DEV_NODE
#define CEC_DEV_NODE "/dev/cec0"
#endif
#define CEC_DEVICE_PATH_DEFAULT CEC_DEV_NODE

/* linux/cec.h uses different spellings across kernel versions. */
#if !defined(CEC_MODE_EXCL_FOLLOWER_PASSTHROUGH) && defined(CEC_MODE_EXCL_FOLLOWER_PASSTHRU)
#define CEC_MODE_EXCL_FOLLOWER_PASSTHROUGH CEC_MODE_EXCL_FOLLOWER_PASSTHRU
#endif

#define CEC_TIMESTAMP_FALLBACK  "_TIMESTAMP_UNAVAILABLE_"
#define CEC_TIMESTAMP_SIZE      64

/* CEC identity/profile defaults; override via CMake compile definitions. */
#ifndef CEC_VENDOR_ID
#define CEC_VENDOR_ID           0x00BC44
#endif

#ifndef CEC_PRIMARY_DEVICE_TYPE
#define CEC_PRIMARY_DEVICE_TYPE CEC_OP_PRIM_DEVTYPE_TUNER
#endif

#ifndef CEC_LOG_ADDR_TYPE
#define CEC_LOG_ADDR_TYPE       CEC_LOG_ADDR_TYPE_TUNER
#endif

#ifndef CEC_ALL_DEVICE_TYPE
#define CEC_ALL_DEVICE_TYPE     CEC_OP_ALL_DEVTYPE_TUNER
#endif

/* Blocking TX: wait up to 1000 ms for ACK/NACK */
#define CEC_TX_TIMEOUT_MS       1000

/* Log levels */
#define CEC_LOG_LEVEL_ERROR     0
#define CEC_LOG_LEVEL_WARN      1
#define CEC_LOG_LEVEL_INFO      2
#define CEC_LOG_LEVEL_DEBUG     3
#define CEC_LOG_LEVEL_TRACE     4

#define CEC_LOG(level, ...) cec_log(level, __func__, __LINE__, __VA_ARGS__)
#define CEC_LOG_ERROR(fmt, ...) CEC_LOG(CEC_LOG_LEVEL_ERROR, "RPICECHAL: " fmt, ##__VA_ARGS__)
#define CEC_LOG_WARN(fmt, ...)  CEC_LOG(CEC_LOG_LEVEL_WARN,  "RPICECHAL: " fmt, ##__VA_ARGS__)
#define CEC_LOG_INFO(fmt, ...)  CEC_LOG(CEC_LOG_LEVEL_INFO,  "RPICECHAL: " fmt, ##__VA_ARGS__)
#define CEC_LOG_DEBUG(fmt, ...) CEC_LOG(CEC_LOG_LEVEL_DEBUG, "RPICECHAL: " fmt, ##__VA_ARGS__)
#define CEC_LOG_TRACE(fmt, ...) CEC_LOG(CEC_LOG_LEVEL_TRACE, "RPICECHAL: " fmt, ##__VA_ARGS__)

#define CEC_POLL_EINVAL_LOG_INTERVAL 100

/* Timing constants for cleanup and callback synchronization */
#define CEC_CALLBACK_WAIT_MS              10
#define CEC_CALLBACK_WAIT_US              (CEC_CALLBACK_WAIT_MS * 1000)
#define CEC_CLOSE_MAX_WAIT_ITERATIONS     100
#define CEC_DESTRUCTOR_MAX_WAIT_ITERATIONS 50
#define CEC_MUTEX_TIMEOUT_SEC             1

typedef struct {
	int fd;                       /* /dev/cecX file descriptor */
	int pipe_rd;                  /* read end of shutdown pipe for rx_thread */
	int pipe_wr;                  /* write end of shutdown pipe for rx_thread */
	int lock_fd;                  /* singleton flock() lock file descriptor */
	pthread_t rx_thread;
	bool rx_thread_created;
	bool rx_thread_running;
	bool deferred_cleanup;
	bool closing;
	int handle;
	bool initialized;
	bool running;
	pthread_mutex_t mutex;
	HdmiCecRxCallback_t rx_callback;
	void *rx_callback_data;
	HdmiCecTxCallback_t tx_callback;
	void *tx_callback_data;
	int logical_address;
	uint16_t physical_address;
	bool has_logical_address;
	int callback_active;
} cec_context_t;

static FILE *g_log_file = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_log_init_once = PTHREAD_ONCE_INIT;
static pthread_key_t g_callback_tls_key;
static pthread_once_t g_callback_tls_once = PTHREAD_ONCE_INIT;
static int g_callback_tls_init_status = EINVAL;
static int g_log_level = CEC_LOG_LEVEL_WARN;
static unsigned int g_poll_einval_suppressed = 0;

static void cec_callback_tls_init_impl(void)
{
	g_callback_tls_init_status = pthread_key_create(&g_callback_tls_key, NULL);
}

static bool cec_callback_tls_ready(void)
{
	pthread_once(&g_callback_tls_once, cec_callback_tls_init_impl);
	return (g_callback_tls_init_status == 0);
}

static void cec_set_in_callback_context(bool in_callback)
{
	if (!cec_callback_tls_ready()) {
		return;
	}
	(void)pthread_setspecific(g_callback_tls_key, in_callback ? (void *)1 : NULL);
}

static bool cec_is_in_callback_context(void)
{
	if (!cec_callback_tls_ready()) {
		return false;
	}
	return pthread_getspecific(g_callback_tls_key) != NULL;
}

static void cec_get_timestamp(char *buffer, size_t size)
{
	struct timespec ts;
	struct tm tm_info;
	uint32_t msec;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		snprintf(buffer, size, CEC_TIMESTAMP_FALLBACK);
		return;
	}

	if (localtime_r(&ts.tv_sec, &tm_info) == NULL) {
		snprintf(buffer, size, CEC_TIMESTAMP_FALLBACK);
		return;
	}

	msec = (uint32_t)((ts.tv_nsec / 1000000) % 1000);
	snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03u",
				tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
				tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, msec);
}

static const char *cec_get_log_level_str(int level)
{
	switch (level) {
		case CEC_LOG_LEVEL_ERROR: return "ERROR";
		case CEC_LOG_LEVEL_WARN:  return "WARN";
		case CEC_LOG_LEVEL_INFO:  return "INFO";
		case CEC_LOG_LEVEL_DEBUG: return "DEBUG";
		case CEC_LOG_LEVEL_TRACE: return "TRACE";
		default:                  return "UNKNOWN";
	}
}

/**
 * Initialize logging system based on environment variables.
 * CEC_HAL_LOG_LEVEL: Set log level (ERROR, WARN, INFO, DEBUG, TRACE)
 * CEC_HAL_LOG_FILE:  Set log file path (defaults to stdout).
 * User is responsible for ensuring the directory exists.
 */
static void cec_log_init_impl(void)
{
	pthread_mutex_lock(&g_log_mutex);

	const char *log_level_env = getenv("CEC_HAL_LOG_LEVEL");
	const char *log_file_env  = getenv("CEC_HAL_LOG_FILE");

	if (log_level_env != NULL) {
		if      (strcmp(log_level_env, "ERROR") == 0) g_log_level = CEC_LOG_LEVEL_ERROR;
		else if (strcmp(log_level_env, "WARN")  == 0) g_log_level = CEC_LOG_LEVEL_WARN;
		else if (strcmp(log_level_env, "INFO")  == 0) g_log_level = CEC_LOG_LEVEL_INFO;
		else if (strcmp(log_level_env, "DEBUG") == 0) g_log_level = CEC_LOG_LEVEL_DEBUG;
		else if (strcmp(log_level_env, "TRACE") == 0) g_log_level = CEC_LOG_LEVEL_TRACE;
	}

	if (log_file_env != NULL) {
		int log_fd = -1;
		int log_flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
#ifdef O_NOFOLLOW
		log_flags |= O_NOFOLLOW;
#endif
		log_fd = open(log_file_env, log_flags, 0644);
		if (log_fd >= 0) {
			g_log_file = fdopen(log_fd, "a");
			if (g_log_file == NULL) {
				close(log_fd);
			}
		} else {
			g_log_file = NULL;
		}
		if (g_log_file != NULL) {
			setvbuf(g_log_file, NULL, _IOLBF, 0);
			char timestamp[CEC_TIMESTAMP_SIZE];
			cec_get_timestamp(timestamp, sizeof(timestamp));
			fprintf(g_log_file, "RPICECHAL: Log Started (LINUXCEC): %s (Level: %s)\n",
					timestamp, cec_get_log_level_str(g_log_level));
		} else {
			fprintf(stdout, "RPICECHAL: Failed to open log file: %s\n", log_file_env);
			/* Fallback to stdout to prevent logging blackhole */
			g_log_file = stdout;
			char timestamp[CEC_TIMESTAMP_SIZE];
			cec_get_timestamp(timestamp, sizeof(timestamp));
			fprintf(g_log_file, "RPICECHAL: Log Started (LINUXCEC): %s (Level: %s)\n",
					timestamp, cec_get_log_level_str(g_log_level));
		}
	} else {
		g_log_file = stdout;
		char timestamp[CEC_TIMESTAMP_SIZE];
		cec_get_timestamp(timestamp, sizeof(timestamp));
		fprintf(g_log_file, "RPICECHAL: Log Started (LINUXCEC): %s (Level: %s)\n",
				timestamp, cec_get_log_level_str(g_log_level));
		fflush(g_log_file);
	}

	pthread_mutex_unlock(&g_log_mutex);
}

static void cec_log_init(void)
{
	pthread_once(&g_log_init_once, cec_log_init_impl);
}

static void cec_log_close(void)
{
	pthread_mutex_lock(&g_log_mutex);
	if (g_log_file != NULL) {
		char timestamp[CEC_TIMESTAMP_SIZE];
		cec_get_timestamp(timestamp, sizeof(timestamp));
		fprintf(g_log_file, "RPICECHAL: Log Closed: %s\n", timestamp);
		fflush(g_log_file);
		if (g_log_file != stdout) {
			fclose(g_log_file);
		}
		g_log_file = NULL;
	}
	pthread_mutex_unlock(&g_log_mutex);
}

static void cec_log(int level, const char *func, int line, const char *format, ...)
{
	if (level > g_log_level) return;

	pthread_mutex_lock(&g_log_mutex);
	if (g_log_file != NULL) {
		char timestamp[CEC_TIMESTAMP_SIZE];
		cec_get_timestamp(timestamp, sizeof(timestamp));
		va_list args;
		va_start(args, format);
		fprintf(g_log_file, "[%s] [%s] [%s:%d] ",
				timestamp, cec_get_log_level_str(level), func, line);
		vfprintf(g_log_file, format, args);
		fprintf(g_log_file, "\n");
		va_end(args);
		fflush(g_log_file);
	}
	pthread_mutex_unlock(&g_log_mutex);
}

static int cec_generate_handle(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		int handle = (int)((ts.tv_sec ^ ts.tv_nsec ^ getpid()) & 0x7FFFFFFF);
		return handle ? handle : 1;
	}
	{
		int handle = (int)(time(NULL) & 0x7FFFFFFF);
		return handle ? handle : 1;
	}
}

static bool cec_is_allowed_device_path(const char *path)
{
	static const char prefix[] = "/dev/cec";
	size_t prefix_len = sizeof(prefix) - 1;

	if (path == NULL || strncmp(path, prefix, prefix_len) != 0) {
		return false;
	}

	path += prefix_len;
	if (*path == '\0') {
		return false;
	}

	while (*path != '\0') {
		if (*path < '0' || *path > '9') {
			return false;
		}
		path++;
	}

	return true;
}

static void trace_hexdump(const uint8_t *buf, size_t len)
{
	if (buf == NULL || len == 0 || g_log_level < CEC_LOG_LEVEL_TRACE)
		return;

	pthread_mutex_lock(&g_log_mutex);
	if (g_log_file != NULL) {
		size_t offset = 0;
		while (offset < len) {
			size_t line_len = (len - offset > 16) ? 16 : (len - offset);
			char ascii[17];

			for (size_t i = 0; i < line_len; i++) {
				uint8_t byte = buf[offset + i];
				ascii[i] = (byte >= 32 && byte <= 126) ? (char)byte : '.';
			}
			ascii[line_len] = '\0';

			fprintf(g_log_file, "RPICECHAL: %04zx: ", offset);
			for (size_t i = 0; i < line_len; i++) {
				fprintf(g_log_file, "%02x ", buf[offset + i]);
			}
			for (size_t i = line_len; i < 16; i++) {
				fprintf(g_log_file, "   ");
			}
			fprintf(g_log_file, " %s\n", ascii);
			offset += line_len;
		}
		fflush(g_log_file);
	}
	pthread_mutex_unlock(&g_log_mutex);
}

static cec_context_t g_cec_context = {
	.fd               = -1,
	.pipe_rd          = -1,
	.pipe_wr          = -1,
	.lock_fd          = -1,
	.rx_thread_created = false,
	.rx_thread_running = false,
	.deferred_cleanup = false,
	.closing          = false,
	.handle           = 0,
	.initialized      = false,
	.running          = false,
	.mutex            = PTHREAD_MUTEX_INITIALIZER,
	.rx_callback      = NULL,
	.rx_callback_data = NULL,
	.tx_callback      = NULL,
	.tx_callback_data = NULL,
	.logical_address  = CEC_LOG_ADDR_UNREGISTERED,
	.physical_address = CEC_PHYS_ADDR_INVALID,
	.has_logical_address = false,
	.callback_active  = 0
};

/* Must be called with g_cec_context.mutex held. */
static void cec_refresh_logical_address_locked(void);
static void cec_release_logical_addresses_fd(int fd);

/*
 * Receive thread: blocks on poll() multiplexed between the CEC device fd
 * and a shutdown pipe.  For each CEC_RECEIVE result:
 *   - tx_status set  → TX completion, invoke tx_callback
 *   - rx_status OK   → incoming message, invoke rx_callback
 */
static void *cec_rx_thread(void *arg)
{
	cec_context_t *ctx = (cec_context_t *)arg;

	while (1) {
		/* poll() has no fd upper-bound limit, unlike select()/FD_SET which
		 * causes buffer overflow when fd >= FD_SETSIZE (1024). */
		struct pollfd pfds[2];
		pfds[0].fd     = ctx->fd;
		pfds[0].events = POLLIN | POLLPRI; /* POLLPRI wakes on CEC_EVENT_STATE_CHANGE */
		pfds[1].fd     = ctx->pipe_rd;
		pfds[1].events = POLLIN;

		int ret = poll(pfds, 2, -1);
		if (ret < 0) {
			if (errno == EINTR) continue;
			CEC_LOG_ERROR("poll() failed: %s", strerror(errno));
			break;
		}

		/* Shutdown requested via self-pipe: HdmiCecClose() or destructor writes 1 byte
		 * to pipe_wr, which makes pipe_rd readable (pfds[1].revents & POLLIN),
		 * triggering clean thread exit. This avoids unsafe pthread_cancel(). */
		if (pfds[1].revents & POLLIN)
			break;

		/* Check for CEC device errors (adapter removed, driver reset, etc.) */
		if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			CEC_LOG_ERROR("CEC device poll error: revents=0x%x", pfds[0].revents);
			break;
		}

		/* CEC_EVENT_STATE_CHANGE: physical/logical address changed (hotplug, etc.) */
		if (pfds[0].revents & POLLPRI) {
			struct cec_event ev;
			memset(&ev, 0, sizeof(ev));
			if (ioctl(ctx->fd, CEC_DQEVENT, &ev) == 0 &&
			    ev.event == CEC_EVENT_STATE_CHANGE) {
				pthread_mutex_lock(&ctx->mutex);
				__u16 new_phys = ev.state_change.phys_addr;
				ctx->physical_address = new_phys;
				cec_refresh_logical_address_locked();
				CEC_LOG_INFO("State change event: phys_addr=0x%04x, log_addr=%d",
				             new_phys, ctx->logical_address);
				pthread_mutex_unlock(&ctx->mutex);
			}
		}

		if (!(pfds[0].revents & POLLIN))
			continue;

		struct cec_msg msg;
		memset(&msg, 0, sizeof(msg));
		msg.timeout = 0; /* non-blocking receive */

		if (ioctl(ctx->fd, CEC_RECEIVE, &msg) < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
			CEC_LOG_ERROR("CEC_RECEIVE failed: %s", strerror(errno));
			continue;
		}

		/* TX completion event (async transmit path) */
		if (msg.tx_status != 0) {
			pthread_mutex_lock(&ctx->mutex);
			HdmiCecTxCallback_t tx_cb  = ctx->tx_callback;
			void *tx_cb_data           = ctx->tx_callback_data;
			int cb_handle              = ctx->handle;
			bool should_call = ctx->running && ctx->initialized && tx_cb != NULL;
			if (should_call) ctx->callback_active++;
			pthread_mutex_unlock(&ctx->mutex);

			if (should_call) {
				int result = (msg.tx_status & CEC_TX_STATUS_OK)   ? HDMI_CEC_IO_SENT_AND_ACKD :
				             (msg.tx_status & CEC_TX_STATUS_NACK) ? HDMI_CEC_IO_SENT_BUT_NOT_ACKD :
				             HDMI_CEC_IO_SENT_FAILED;
				tx_cb(cb_handle, tx_cb_data, result);

				bool callback_active_underflow = false;
				pthread_mutex_lock(&ctx->mutex);
				if (ctx->callback_active > 0) {
					ctx->callback_active--;
				} else {
					callback_active_underflow = true;
				}
				pthread_mutex_unlock(&ctx->mutex);
				if (callback_active_underflow) {
					CEC_LOG_WARN("callback_active already 0 after tx callback; skipping decrement");
				}
			}

			CEC_LOG_DEBUG("TX completion: tx_status=0x%02x", msg.tx_status);
		}

		/* Received CEC message */
		if ((msg.rx_status & CEC_RX_STATUS_OK) && msg.len > 0) {
			pthread_mutex_lock(&ctx->mutex);
			HdmiCecRxCallback_t rx_cb = ctx->rx_callback;
			void *rx_cb_data          = ctx->rx_callback_data;
			int cb_handle             = ctx->handle;
			bool should_call = ctx->running && ctx->initialized && rx_cb != NULL;
			if (should_call) ctx->callback_active++;
			pthread_mutex_unlock(&ctx->mutex);

			if (should_call) {
				rx_cb(cb_handle, rx_cb_data, msg.msg, (int)msg.len);

				bool callback_active_underflow = false;
				pthread_mutex_lock(&ctx->mutex);
				if (ctx->callback_active > 0) {
					ctx->callback_active--;
				} else {
					callback_active_underflow = true;
				}
				pthread_mutex_unlock(&ctx->mutex);
				if (callback_active_underflow) {
					CEC_LOG_WARN("callback_active already 0 after rx callback; skipping decrement");
				}
			}

			CEC_LOG_INFO("Received CEC message: len=%u", (unsigned int)msg.len);
			// dump the message being received for debugging.
			trace_hexdump(msg.msg, (size_t)msg.len);
		}
	}

	pthread_mutex_lock(&ctx->mutex);
	if (ctx->deferred_cleanup) {
		if (ctx->pipe_wr >= 0) close(ctx->pipe_wr);
		if (ctx->pipe_rd >= 0) close(ctx->pipe_rd);
		cec_release_logical_addresses_fd(ctx->fd);
		if (ctx->fd >= 0)      close(ctx->fd);
		if (ctx->lock_fd >= 0) close(ctx->lock_fd);

		ctx->fd                  = -1;
		ctx->pipe_rd             = -1;
		ctx->pipe_wr             = -1;
		ctx->lock_fd             = -1;
		ctx->handle              = 0;
		ctx->rx_callback         = NULL;
		ctx->rx_callback_data    = NULL;
		ctx->tx_callback         = NULL;
		ctx->tx_callback_data    = NULL;
		ctx->logical_address     = CEC_LOG_ADDR_UNREGISTERED;
		ctx->has_logical_address = false;
		ctx->physical_address    = CEC_PHYS_ADDR_INVALID;
		ctx->callback_active     = 0;
		ctx->deferred_cleanup    = false;
		ctx->closing             = false;
		ctx->initialized         = false;
		ctx->rx_thread_created   = false;
		ctx->rx_thread           = (pthread_t)0;
	} else {
		/* Unexpected thread exit (poll error / device error) without deferred_cleanup.
		 * Mark the context as no longer running so callers can detect the failure:
		 * running=false stops new callbacks, initialized stays true so HdmiCecClose()
		 * can still clean up properly. */
		ctx->running = false;
	}
	ctx->rx_thread_running = false;
	pthread_mutex_unlock(&ctx->mutex);

	return NULL;
}

/**
 * @brief Initializes the HDMI CEC HAL
 *
 * This function is required to be called before the other APIs in this module.@n
 * Subsequent calls to this API will return HDMI_CEC_IO_ALREADY_OPEN.
 * For HDMI source devices, logical address discovery takes place during HdmiCecOpen() and
 * can be obtained via HdmiCecGetLogicalAddress().
 * For HDMI sink devices, logical address discovery does not occur during HdmiCecOpen() and
 * must be managed by the caller.
 *
 * @param [out] handle                    - The handle used by application to uniquely
 *                                          identify the HAL instance
 *
 * @return HDMI_CEC_STATUS                        - Status
 * @retval HDMI_CEC_IO_SUCCESS                    - Success
 * @retval HDMI_CEC_IO_ALREADY_OPEN               - Function is already open.
 *                                                  This error code will be deprecated in the next phase.
 * @retval HDMI_CEC_IO_INVALID_ARGUMENT           - Parameter passed to this function is invalid
 * @retval HDMI_CEC_IO_LOGICALADDRESS_UNAVAILABLE - Logical address is not available for source devices.
 *
 *
 * @post HdmiCecClose() must be called to release resources.
 * @warning This API is NOT thread safe.
 *
 * @see HdmiCecClose()
 *
 */

/* Must be called with g_cec_context.mutex held.
 * Refreshes logical address from the kernel adapter in case it changed
 * (e.g., after a hotplug event or external CEC_ADAP_S_LOG_ADDRS call). */
static void cec_refresh_logical_address_locked(void)
{
	struct cec_log_addrs la;
	memset(&la, 0, sizeof(la));
	if (ioctl(g_cec_context.fd, CEC_ADAP_G_LOG_ADDRS, &la) < 0) {
		CEC_LOG_WARN("CEC_ADAP_G_LOG_ADDRS failed during refresh: %s", strerror(errno));
		return;
	}

	if (la.num_log_addrs > 0 && la.log_addr[0] != CEC_LOG_ADDR_INVALID) {
		int new_addr = (int)la.log_addr[0];
		if (new_addr != g_cec_context.logical_address) {
			CEC_LOG_WARN("Logical address refreshed: %d -> %d",
			             g_cec_context.logical_address, new_addr);
		}
		g_cec_context.logical_address = new_addr;
		g_cec_context.has_logical_address =
			(new_addr != CEC_LOG_ADDR_UNREGISTERED);
		return;
	}

	if (g_cec_context.has_logical_address ||
	    g_cec_context.logical_address != CEC_LOG_ADDR_UNREGISTERED) {
		CEC_LOG_WARN("Kernel reports no logical address; marking context as Unregistered");
	}
	g_cec_context.logical_address = CEC_LOG_ADDR_UNREGISTERED;
	g_cec_context.has_logical_address = false;
}

static void cec_release_logical_addresses_fd(int fd)
{
	if (fd < 0) {
		return;
	}

	struct cec_log_addrs clear_addrs;
	memset(&clear_addrs, 0, sizeof(clear_addrs));
	if (ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &clear_addrs) < 0) {
		CEC_LOG_WARN("CEC_ADAP_S_LOG_ADDRS(clear) failed during close: %s", strerror(errno));
	}
}

/* Must be called with g_cec_context.mutex held.
 * Logs adapter state at transmit failure time to diagnose EINVAL. */
static void cec_log_tx_diagnostics_locked(const struct cec_msg *msg, int len)
{
	if (msg != NULL) {
		CEC_LOG_WARN("TX diag: fd=%d hdr=0x%02x len=%d timeout=%u",
		             g_cec_context.fd, msg->msg[0], len, msg->timeout);
	}

	struct cec_log_addrs la;
	memset(&la, 0, sizeof(la));
	if (ioctl(g_cec_context.fd, CEC_ADAP_G_LOG_ADDRS, &la) == 0) {
		CEC_LOG_WARN("TX diag: num_log_addrs=%u, primary0=%u, log_addr0=%u",
		             (unsigned int)la.num_log_addrs,
		             (unsigned int)la.primary_device_type[0],
		             (unsigned int)la.log_addr[0]);
	} else {
		CEC_LOG_WARN("TX diag: CEC_ADAP_G_LOG_ADDRS failed: %s", strerror(errno));
	}

	__u32 mode = 0;
	if (ioctl(g_cec_context.fd, CEC_G_MODE, &mode) == 0) {
		CEC_LOG_WARN("TX diag: mode=0x%x", mode);
	} else {
		CEC_LOG_WARN("TX diag: CEC_G_MODE failed: %s", strerror(errno));
	}

	struct cec_caps caps;
	memset(&caps, 0, sizeof(caps));
	if (ioctl(g_cec_context.fd, CEC_ADAP_G_CAPS, &caps) == 0) {
		CEC_LOG_WARN("TX diag: caps=0x%x, avail_log_addrs=%u",
		             (unsigned int)caps.capabilities,
		             (unsigned int)caps.available_log_addrs);
	} else {
		CEC_LOG_WARN("TX diag: CEC_ADAP_G_CAPS failed: %s", strerror(errno));
	}
}

/* Must be called with g_cec_context.mutex held. */
static void cec_log_poll_einval_rate_limited_locked(__u8 header)
{
	g_poll_einval_suppressed++;
	if (g_poll_einval_suppressed == 1) {
		CEC_LOG_WARN("Poll transmit rejected with EINVAL (hdr=0x%02x); treating as NACK",
		             header);
		return;
	}

	if (g_poll_einval_suppressed >= CEC_POLL_EINVAL_LOG_INTERVAL) {
		CEC_LOG_WARN("Poll transmit EINVAL repeated %u times (latest hdr=0x%02x); continuing as NACK",
		             g_poll_einval_suppressed, header);
		g_poll_einval_suppressed = 0;
	}
}

/* Must be called with g_cec_context.mutex held.
 * Re-applies initiator mode and re-claims a logical address if needed. */
static void cec_recover_tx_state_locked(void)
{
	CEC_LOG_WARN("TX recovery: Starting (current log_addr=%d, has=%d)",
	             g_cec_context.logical_address, g_cec_context.has_logical_address);

	__u32 mode = CEC_MODE_INITIATOR | CEC_MODE_EXCL_FOLLOWER_PASSTHROUGH;
	if (ioctl(g_cec_context.fd, CEC_S_MODE, &mode) < 0) {
		CEC_LOG_WARN("TX recovery: CEC_S_MODE re-apply failed: %s", strerror(errno));
	} else {
		CEC_LOG_DEBUG("TX recovery: CEC_S_MODE re-applied successfully");
	}

	cec_refresh_logical_address_locked();
	CEC_LOG_WARN("TX recovery: After refresh, log_addr=%d, has=%d",
	             g_cec_context.logical_address, g_cec_context.has_logical_address);
	if (g_cec_context.has_logical_address) {
		CEC_LOG_WARN("TX recovery: Logical address restored, done");
		return;
	}

	CEC_LOG_WARN("TX recovery: No logical address after refresh, re-claiming...");
	struct cec_log_addrs log_addrs;
	memset(&log_addrs, 0, sizeof(log_addrs));
	log_addrs.num_log_addrs          = 1;
	log_addrs.cec_version            = CEC_OP_CEC_VERSION_1_4;
	log_addrs.vendor_id              = CEC_VENDOR_ID;
	log_addrs.flags                  = CEC_LOG_ADDRS_FL_ALLOW_UNREG_FALLBACK;
	log_addrs.primary_device_type[0] = CEC_PRIMARY_DEVICE_TYPE;
	log_addrs.log_addr_type[0]       = CEC_LOG_ADDR_TYPE;
	log_addrs.all_device_types[0]    = CEC_ALL_DEVICE_TYPE;

	if (ioctl(g_cec_context.fd, CEC_ADAP_S_LOG_ADDRS, &log_addrs) < 0) {
		CEC_LOG_WARN("TX recovery: CEC_ADAP_S_LOG_ADDRS failed: %s", strerror(errno));
		return;
	}

	cec_refresh_logical_address_locked();
	CEC_LOG_WARN("TX recovery: After re-claim, log_addr=%d, has=%d",
	             g_cec_context.logical_address, g_cec_context.has_logical_address);
	if (g_cec_context.has_logical_address) {
		CEC_LOG_WARN("TX recovery: Successfully recovered logical address %d",
		             g_cec_context.logical_address);
	} else {
		CEC_LOG_WARN("TX recovery: Still no logical address after re-claim");
	}
}

HDMI_CEC_STATUS HdmiCecOpen(int *handle)
{
	if (handle == NULL) {
		CEC_LOG_ERROR("Invalid argument: handle is NULL");
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	pthread_mutex_lock(&g_cec_context.mutex);

	if (g_cec_context.initialized || g_cec_context.closing ||
	    g_cec_context.rx_thread_running || g_cec_context.deferred_cleanup) {
		CEC_LOG_ERROR("%s already opened", __func__);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_ALREADY_OPEN;
	}

	/* Allow device path override via environment variable, but only for
	 * known-safe /dev/cecN-style nodes to avoid opening arbitrary devices. */
	const char *dev_path = CEC_DEVICE_PATH_DEFAULT;
	const char *env_dev_path = getenv("CEC_HAL_DEVICE");
	if (env_dev_path != NULL) {
		if (cec_is_allowed_device_path(env_dev_path)) {
			dev_path = env_dev_path;
		} else {
			CEC_LOG_WARN("Ignoring unsafe CEC_HAL_DEVICE override: %s", env_dev_path);
		}
	}

	/* Enforce singleton: one HAL instance per CEC device node across all processes.
	 * An exclusive non-blocking flock on a well-known lock file achieves this.
	 * The OS releases the lock automatically if the process exits or crashes. */
	char lock_path[128];
	const char *base = strrchr(dev_path, '/');
	base = base ? base + 1 : dev_path;
	int lock_path_len = snprintf(lock_path, sizeof(lock_path), "/run/lock/RCECHal_%s.lock", base);
	if (lock_path_len < 0 || (size_t)lock_path_len >= sizeof(lock_path)) {
		CEC_LOG_ERROR("CEC device path too long to form lock file name: %s", dev_path);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	/* Ensure /run/lock directory exists. mkdir() silently succeeds if already present. */
	if (mkdir("/run/lock", 0755) < 0 && errno != EEXIST) {
		CEC_LOG_ERROR("Failed to create /run/lock directory: %s", strerror(errno));
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	int lock_open_flags = O_CREAT | O_RDWR | O_CLOEXEC;
#ifdef O_NOFOLLOW
	lock_open_flags |= O_NOFOLLOW;
#endif
	int lock_fd = open(lock_path, lock_open_flags, 0600);
	if (lock_fd < 0) {
		CEC_LOG_ERROR("Failed to open lock file %s: %s", lock_path, strerror(errno));
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	struct stat lock_st;
	if (fstat(lock_fd, &lock_st) < 0) {
		CEC_LOG_ERROR("Failed to stat lock file %s: %s", lock_path, strerror(errno));
		close(lock_fd);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	if (!S_ISREG(lock_st.st_mode)) {
		CEC_LOG_ERROR("Lock file %s is not a regular file", lock_path);
		close(lock_fd);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
		CEC_LOG_ERROR("CEC device %s is already in use by another process", dev_path);
		close(lock_fd);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_ALREADY_OPEN;
	}

	int fd = open(dev_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		CEC_LOG_ERROR("Failed to open %s: %s", dev_path, strerror(errno));
		close(lock_fd);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	/* Validate that the opened path is a character device. Rejects plain files,
	 * symlink-redirected regular files, directories, etc. when running as root. */
	struct stat dev_st;
	if (fstat(fd, &dev_st) < 0) {
		CEC_LOG_ERROR("Failed to stat %s: %s", dev_path, strerror(errno));
		close(fd);
		close(lock_fd);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	if (!S_ISCHR(dev_st.st_mode)) {
		CEC_LOG_ERROR("%s is not a character device", dev_path);
		close(fd);
		close(lock_fd);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	/* Set mode: exclusive initiator + exclusive follower with passthrough.
	 * Passthrough allows receiving messages addressed to other devices so
	 * that the upper CEC layer can observe all bus traffic. */
	__u32 mode = CEC_MODE_INITIATOR | CEC_MODE_EXCL_FOLLOWER_PASSTHROUGH;
	if (ioctl(fd, CEC_S_MODE, &mode) < 0) {
		CEC_LOG_ERROR("CEC_S_MODE failed: %s", strerror(errno));
		close(fd);
		close(lock_fd);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	/* Read physical address from the adapter (set by the kernel HDMI driver) */
	__u16 phys_addr = CEC_PHYS_ADDR_INVALID;
	if (ioctl(fd, CEC_ADAP_G_PHYS_ADDR, &phys_addr) < 0) {
		CEC_LOG_ERROR("CEC_ADAP_G_PHYS_ADDR failed: %s", strerror(errno));
		phys_addr = CEC_PHYS_ADDR_INVALID;
	}
	CEC_LOG_DEBUG("Physical address: 0x%04x", phys_addr);

	/* Claim a logical address (STB / Tuner device type) */
	struct cec_log_addrs log_addrs;
	memset(&log_addrs, 0, sizeof(log_addrs));
	log_addrs.num_log_addrs          = 1;
	log_addrs.cec_version            = CEC_OP_CEC_VERSION_1_4;
	log_addrs.vendor_id              = CEC_VENDOR_ID;
	log_addrs.flags                  = CEC_LOG_ADDRS_FL_ALLOW_UNREG_FALLBACK;
	log_addrs.primary_device_type[0] = CEC_PRIMARY_DEVICE_TYPE;
	log_addrs.log_addr_type[0]       = CEC_LOG_ADDR_TYPE;
	log_addrs.all_device_types[0]    = CEC_ALL_DEVICE_TYPE;

	/* This ioctl blocks until the logical address claiming process completes */
	if (ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &log_addrs) < 0) {
		int set_log_addrs_errno = errno;
		if (set_log_addrs_errno == EBUSY) {
			/* Some kernels/adapter states report EBUSY when a logical address is
			 * already active for this adapter. Reuse it instead of failing open. */
			struct cec_log_addrs existing_log_addrs;
			memset(&existing_log_addrs, 0, sizeof(existing_log_addrs));
			if (ioctl(fd, CEC_ADAP_G_LOG_ADDRS, &existing_log_addrs) == 0 &&
			    existing_log_addrs.num_log_addrs > 0 &&
			    existing_log_addrs.log_addr[0] != CEC_LOG_ADDR_INVALID) {
				CEC_LOG_WARN("CEC_ADAP_S_LOG_ADDRS busy; reusing existing logical address %d",
				             existing_log_addrs.log_addr[0]);
			} else {
				CEC_LOG_ERROR("CEC_ADAP_S_LOG_ADDRS failed: %s", strerror(set_log_addrs_errno));
				close(fd);
				close(lock_fd);
				pthread_mutex_unlock(&g_cec_context.mutex);
				return HDMI_CEC_IO_LOGICALADDRESS_UNAVAILABLE;
			}
		} else {
			CEC_LOG_ERROR("CEC_ADAP_S_LOG_ADDRS failed: %s", strerror(set_log_addrs_errno));
			close(fd);
			close(lock_fd);
			pthread_mutex_unlock(&g_cec_context.mutex);
			return HDMI_CEC_IO_LOGICALADDRESS_UNAVAILABLE;
		}
	}

	/* Read back the allocated logical address */
	int logical_addr = CEC_LOG_ADDR_UNREGISTERED;
	bool has_logical = false;
	if (ioctl(fd, CEC_ADAP_G_LOG_ADDRS, &log_addrs) == 0 &&
	    log_addrs.num_log_addrs > 0 &&
	    log_addrs.log_addr[0] != CEC_LOG_ADDR_INVALID) {
		logical_addr = (int)log_addrs.log_addr[0];
		has_logical  = (logical_addr != CEC_LOG_ADDR_UNREGISTERED);
		CEC_LOG_INFO("Allocated logical address: %d", logical_addr);
	} else {
		CEC_LOG_DEBUG("No logical address allocated (operating as Unregistered)");
	}

	/* Create self-pipe for clean rx_thread shutdown */
	int pipefd[2];
	bool pipe_created = false;

	/* Prefer atomic CLOEXEC to avoid fork/exec races in multi-threaded processes. */
#if defined(__linux__) && defined(SYS_pipe2) && defined(O_CLOEXEC)
	if (syscall(SYS_pipe2, pipefd, O_CLOEXEC) == 0) {
		pipe_created = true;
	} else if (errno != ENOSYS) {
		CEC_LOG_ERROR("pipe2(O_CLOEXEC) failed: %s", strerror(errno));
		close(fd);
		close(lock_fd);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
#endif

	if (!pipe_created) {
		if (pipe(pipefd) < 0) {
			CEC_LOG_ERROR("pipe() failed: %s", strerror(errno));
			close(fd);
			close(lock_fd);
			pthread_mutex_unlock(&g_cec_context.mutex);
			return HDMI_CEC_IO_GENERAL_ERROR;
		}

		/* Fallback for platforms without pipe2(O_CLOEXEC). */
		int pipe_rd_flags = fcntl(pipefd[0], F_GETFD);
		if (pipe_rd_flags < 0 || fcntl(pipefd[0], F_SETFD, pipe_rd_flags | FD_CLOEXEC) < 0) {
			CEC_LOG_ERROR("fcntl(FD_CLOEXEC) failed for pipe read end: %s", strerror(errno));
			close(pipefd[0]);
			close(pipefd[1]);
			close(fd);
			close(lock_fd);
			pthread_mutex_unlock(&g_cec_context.mutex);
			return HDMI_CEC_IO_GENERAL_ERROR;
		}

		int pipe_wr_flags = fcntl(pipefd[1], F_GETFD);
		if (pipe_wr_flags < 0 || fcntl(pipefd[1], F_SETFD, pipe_wr_flags | FD_CLOEXEC) < 0) {
			CEC_LOG_ERROR("fcntl(FD_CLOEXEC) failed for pipe write end: %s", strerror(errno));
			close(pipefd[0]);
			close(pipefd[1]);
			close(fd);
			close(lock_fd);
			pthread_mutex_unlock(&g_cec_context.mutex);
			return HDMI_CEC_IO_GENERAL_ERROR;
		}
	}

	g_cec_context.fd               = fd;
	g_cec_context.lock_fd          = lock_fd;
	g_cec_context.pipe_rd          = pipefd[0];
	g_cec_context.pipe_wr          = pipefd[1];
	g_cec_context.physical_address = phys_addr;
	g_cec_context.logical_address  = logical_addr;
	g_cec_context.has_logical_address = has_logical;
	g_cec_context.handle           = cec_generate_handle();
	g_cec_context.initialized      = true;
	g_cec_context.running          = true;

	int thread_create_ret = pthread_create(&g_cec_context.rx_thread, NULL,
	                                       cec_rx_thread, &g_cec_context);
	if (thread_create_ret != 0) {
		CEC_LOG_ERROR("pthread_create failed: %s", strerror(thread_create_ret));
		close(pipefd[0]);
		close(pipefd[1]);
		close(fd);
		close(lock_fd);
		g_cec_context.fd          = -1;
		g_cec_context.lock_fd     = -1;
		g_cec_context.pipe_rd     = -1;
		g_cec_context.pipe_wr     = -1;
		g_cec_context.rx_thread_created = false;
		g_cec_context.initialized = false;
		g_cec_context.running     = false;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	g_cec_context.rx_thread_created = true;
	g_cec_context.rx_thread_running = true;
	g_cec_context.deferred_cleanup = false;
	g_cec_context.closing = false;

	*handle = g_cec_context.handle;
	pthread_mutex_unlock(&g_cec_context.mutex);

	CEC_LOG_INFO("HdmiCecOpen successful: handle=%d, phys_addr=0x%04x, log_addr=%d",
	             *handle, phys_addr, logical_addr);
	return HDMI_CEC_IO_SUCCESS;
}

/**
 * @brief Closes an instance of HDMI CEC HAL
 *
 * This function will uninitialise the module.@n
 * Close will clear up registered logical addresses.@n
 * Subsequent calls to this API will return HDMI_CEC_IO_NOT_OPENED.
 *
 * @param[in] handle - The handle returned from the HdmiCecOpen(). Non zero value
 *
 * @return HDMI_CEC_STATUS              - Status
 * @retval HDMI_CEC_IO_SUCCESS          - Success
 * @retval HDMI_CEC_IO_NOT_OPENED       - Module is not initialised
 * @retval HDMI_CEC_IO_INVALID_HANDLE   - An invalid handle argument has been passed
 *
 * @pre HdmiCecOpen() must be called before calling this API.
 * @warning This API is NOT thread safe.
 *
 * @see HdmiCecOpen()
 *
 */
HDMI_CEC_STATUS HdmiCecClose(int handle)
{
	pthread_mutex_lock(&g_cec_context.mutex);

	if (!g_cec_context.initialized && !g_cec_context.closing) {
		CEC_LOG_ERROR("CEC not opened");
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		CEC_LOG_ERROR("Invalid handle: %d (expected: %d)", handle, g_cec_context.handle);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	/* Self-close from within rx_thread callback cannot join itself. */
	bool close_from_rx_thread = g_cec_context.rx_thread_running &&
	                           pthread_equal(pthread_self(), g_cec_context.rx_thread);
	bool rx_thread_created = g_cec_context.rx_thread_created;
	bool rx_thread_was_running = g_cec_context.rx_thread_running;
	pthread_t rx_thread = g_cec_context.rx_thread;
	int pipe_wr = g_cec_context.pipe_wr;

	g_cec_context.closing     = true;
	g_cec_context.running     = false;
	g_cec_context.initialized = false;
	if (close_from_rx_thread) {
		/* Keep logically open until deferred cleanup completes so Open()
		 * cannot race and overwrite the context while rx_thread exits. */
		g_cec_context.deferred_cleanup = true;
	}

	pthread_mutex_unlock(&g_cec_context.mutex);

	/* Signal rx_thread to exit then reap it if it was created and we're not that thread.
	 * Even if closing from within rx_thread, signal the self-pipe so poll() wakes and
	 * executes the deferred cleanup code. */
	if (rx_thread_created && !close_from_rx_thread) {
		char byte = 1;
		if (rx_thread_was_running && pipe_wr >= 0) {
			(void)write(pipe_wr, &byte, 1);
		}
		pthread_join(rx_thread, NULL);
		pthread_mutex_lock(&g_cec_context.mutex);
		g_cec_context.rx_thread_running = false;
		g_cec_context.rx_thread_created = false;
		g_cec_context.rx_thread = (pthread_t)0;
		pthread_mutex_unlock(&g_cec_context.mutex);
	} else if (close_from_rx_thread) {
		/* Signal self-pipe to wake up from poll() so deferred cleanup executes. */
		if (pipe_wr >= 0) {
			char byte = 1;
			(void)write(pipe_wr, &byte, 1);
		}
		CEC_LOG_INFO("HdmiCecClose invoked from rx_thread; deferring FD cleanup to thread exit");
	}

	if (close_from_rx_thread) {
		CEC_LOG_INFO("HdmiCecClose successful (deferred cleanup)");
		return HDMI_CEC_IO_SUCCESS;
	}

	/* Wait for in-progress callbacks before teardown, but keep shutdown bounded.
	 * On timeout, surface an error and leave state open for a later retry. */
	pthread_mutex_lock(&g_cec_context.mutex);
	int self_callback_bias = cec_is_in_callback_context() ? 1 : 0;
	int wait_count = 0;
	while (g_cec_context.callback_active > self_callback_bias &&
	       wait_count < CEC_CLOSE_MAX_WAIT_ITERATIONS) {
		bool should_log = (wait_count == 0);
		int active_callbacks = g_cec_context.callback_active;
		pthread_mutex_unlock(&g_cec_context.mutex);
		if (should_log) {
			CEC_LOG_WARN("Waiting for %d active callback(s) to finish before closing",
			             active_callbacks);
		}
		usleep(CEC_CALLBACK_WAIT_US);
		pthread_mutex_lock(&g_cec_context.mutex);
		wait_count++;
	}
	int callbacks_left = g_cec_context.callback_active;
	bool callbacks_timed_out = (callbacks_left > self_callback_bias);
	pthread_mutex_unlock(&g_cec_context.mutex);

	if (callbacks_timed_out) {
		/* Close did not complete cleanly.
		 * Keep closing=true so HdmiCecClose() can be retried with the same handle:
		 * the entry guard (!initialized && !closing) passes when closing==true.
		 * Keep initialized=false so every other public API (HdmiCecTx,
		 * HdmiCecGetPhysicalAddress, HdmiCecSet*Callback, …) rejects calls with
		 * HDMI_CEC_IO_NOT_OPENED — the RX thread is already joined, so no async
		 * TX completion callbacks or state-change handling can be serviced.
		 * A retry call to Close will skip the join (rx_thread_running==false)
		 * and only wait for the remaining callbacks to quiesce. */
		pthread_mutex_lock(&g_cec_context.mutex);
		g_cec_context.initialized = false;
		g_cec_context.running     = false;
		g_cec_context.closing     = true;
		pthread_mutex_unlock(&g_cec_context.mutex);
		CEC_LOG_ERROR("Timed out waiting for %d active callback(s) during close", callbacks_left);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	/* Release CEC resources. All callbacks from other threads are quiesced.
	 * If Close is called re-entrantly from a callback, the current callback
	 * may still be in progress on this thread. */
	pthread_mutex_lock(&g_cec_context.mutex);
	close(g_cec_context.pipe_wr);
	close(g_cec_context.pipe_rd);
	cec_release_logical_addresses_fd(g_cec_context.fd);
	close(g_cec_context.fd);
	close(g_cec_context.lock_fd);
	g_cec_context.fd      = -1;
	g_cec_context.pipe_rd = -1;
	g_cec_context.pipe_wr = -1;
	g_cec_context.lock_fd = -1;

	g_cec_context.handle              = 0;
	g_cec_context.rx_thread_created   = false;
	g_cec_context.rx_callback         = NULL;
	g_cec_context.rx_callback_data    = NULL;
	g_cec_context.tx_callback         = NULL;
	g_cec_context.tx_callback_data    = NULL;
	g_cec_context.logical_address     = CEC_LOG_ADDR_UNREGISTERED;
	g_cec_context.has_logical_address = false;
	g_cec_context.physical_address    = CEC_PHYS_ADDR_INVALID;
	/* Do NOT reset callback_active to 0; leave accurate refcount. If a callback
	 * somehow completes after wait loop, its decrement will be valid and won't
	 * cause underflow. */
	g_cec_context.deferred_cleanup    = false;
	g_cec_context.closing             = false;
	g_cec_context.initialized         = false;

	pthread_mutex_unlock(&g_cec_context.mutex);
	CEC_LOG_INFO("HdmiCecClose successful");
	return HDMI_CEC_IO_SUCCESS;
}

/**
 * @brief Gets the Physical Address obtained by the module
 *
 * This function gets the Physical address for the specified device type.
 *
 * @param[in] handle            - The handle returned from the HdmiCecOpen(). Non zero value
 * @param[out] physicalAddress  - Physical address acquired
 *    The valid Physical address is less than F.F.F.F
 *    The Sink device at root will take 0.0.0.0 as the Physical Address
 *
 * @pre HdmiCecOpen() must be called before calling this API.
 * @warning This API is NOT thread safe.
 * @see HdmiCecGetLogicalAddress()
 *
 * @return HDMI_CEC_STATUS              - Status
 * @retval HDMI_CEC_IO_SUCCESS          - Success
 * @retval HDMI_CEC_IO_NOT_OPENED       - Module is not initialised
 * @retval HDMI_CEC_IO_INVALID_ARGUMENT - Parameter passed to this function is invalid
 * @retval HDMI_CEC_IO_INVALID_HANDLE   - An invalid handle argument has been passed
 * @retval HDMI_CEC_IO_INVALID_OUTPUT   - Physical address can't be retrieved because it is outside the valid range
 *
 */
HDMI_CEC_STATUS HdmiCecGetPhysicalAddress(int handle, unsigned int *physicalAddress)
{
	pthread_mutex_lock(&g_cec_context.mutex);

	if (!g_cec_context.initialized) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	if (physicalAddress == NULL) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	/* Re-query from the adapter so callers always get the current value,
	 * even if HDMI was disconnected/reconnected after HdmiCecOpen. */
	__u16 phys_addr = CEC_PHYS_ADDR_INVALID;
	if (ioctl(g_cec_context.fd, CEC_ADAP_G_PHYS_ADDR, &phys_addr) < 0) {
		CEC_LOG_WARN("CEC_ADAP_G_PHYS_ADDR failed: %s - returning cached value",
		             strerror(errno));
		phys_addr = g_cec_context.physical_address;
	} else {
		g_cec_context.physical_address = phys_addr;
	}

	if (phys_addr == CEC_PHYS_ADDR_INVALID) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_OUTPUT;
	}
	CEC_LOG_INFO("Physical address: 0x%04x", phys_addr);
	*physicalAddress = phys_addr;
	pthread_mutex_unlock(&g_cec_context.mutex);

	return HDMI_CEC_IO_SUCCESS;
}

/**
 * @brief Sets the logical address assignment for a HDMI sink device.
 *
 * Caller will take care of discovery of Logical Address and sets the available logical addresses through this API.@n
 * This API is only applicable for sink devices.@n
 * Invoking this API in source device must return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED@n@n
 *
 *
 * @param[in] handle                              - The handle returned from the HdmiCecOpen()
 *                                                    function. Non zero value
 * @param[in] logicalAddresses                    - The logical address to be acquired
 *
 * @return HDMI_CEC_STATUS                        - Status
 * @retval HDMI_CEC_IO_SUCCESS                    - POLL message is sent successfully and not
 *                                                    ACK'd by any device on the bus
 * @retval HDMI_CEC_IO_NOT_OPENED                 - Module is not initialised
 * @retval HDMI_CEC_IO_INVALID_ARGUMENT           - Parameter passed to this function is invalid
 *                                                  i.e. be if any logical address less than 0x0 and greater than 0xF is given as argument
 * @retval HDMI_CEC_IO_INVALID_HANDLE             - An invalid handle argument has been passed
 * @retval HDMI_CEC_IO_OPERATION_NOT_SUPPORTED    - The attempted operation is not supported
 *
 * @pre HdmiCecOpen() must be called before calling this API.
 * @warning This API is NOT thread safe.
 *
 * @see HdmiCecRemoveLogicalAddress(), HdmiCecGetLogicalAddress()
 */
HDMI_CEC_STATUS HdmiCecAddLogicalAddress(int handle, int logicalAddresses)
{
	if (!g_cec_context.initialized) {
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	if (logicalAddresses < 0 || logicalAddresses > 0x0F) {
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	/* For source devices, this operation is not supported */
	CEC_LOG_ERROR("HdmiCecAddLogicalAddress not supported for source devices");
	return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;
}

/**
 * @brief Clears the Logical Addresses claimed by the host device
 *
 * This function releases the previously acquired logical address.@n
 * Once released,
 * 1. This API must set the logical address to the default value (0xF).
 * 2. Also the module must not ACK any POLL message destined to the released address.@n
 *
 *
 * This API is only applicable for sink devices. Invoking this API in source device must return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED@n@n
 *
 *
 * @param[in] handle                   - The handle returned from the HdmiCecOpen(). Non zero value
 * @param[in] logicalAddresses         - The logicalAddresses to be released
 *
 * @return HDMI_CEC_STATUS                        - Status
 * @retval HDMI_CEC_IO_SUCCESS                    - Success
 * @retval HDMI_CEC_IO_NOT_OPENED                 - Module is not initialised
 * @retval HDMI_CEC_IO_INVALID_ARGUMENT           - Parameter passed to this function is invalid -
 *                                                  i.e. if any logical address less than 0x0 and greater than 0xF is given as argument
 * @retval HDMI_CEC_IO_NOT_ADDED                  - Logical address was never added before [or] was previously removed successfully
 * @retval HDMI_CEC_IO_INVALID_HANDLE             - An invalid handle argument has been passed
 * @retval HDMI_CEC_IO_OPERATION_NOT_SUPPORTED    - Operation not supported. This API is not required if the SOC is performing the logical address discovery.
 *                                                  This operation is not supported in source devices.
 *
 * @pre HdmiCecOpen() must be called before calling this API.
 * @warning This API is NOT thread safe.
 * @see HdmiCecAddLogicalAddress(), HdmiCecGetLogicalAddress()
 *
 */
HDMI_CEC_STATUS HdmiCecRemoveLogicalAddress(int handle, int logicalAddresses)
{
	if (!g_cec_context.initialized) {
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	if (logicalAddresses < 0 || logicalAddresses > 0x0F) {
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	/* For source devices, this operation is not supported */
	CEC_LOG_ERROR("HdmiCecRemoveLogicalAddress not supported for source devices");
	return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;
}

/**
 * @brief Gets the Logical Address obtained by the module
 *
 * This function gets the logical address for the specified device type. @n
 * For sink devices, if logical address is not added or removed,
 *    the logical address returned will be 0x0F.
 * For source devices, logical address returned must be based on the device type
 *    as defined in HDMI Specification.
 *
 * @param[in] handle                    - The handle returned from the HdmiCecOpen(). Non zero value
 * @param[out] logicalAddress           - The logical address acquired
 *
 * @return HDMI_CEC_STATUS              - Status
 * @retval HDMI_CEC_IO_SUCCESS          - Success
 * @retval HDMI_CEC_IO_NOT_OPENED       - Module is not initialised
 * @retval HDMI_CEC_IO_INVALID_ARGUMENT - Parameter passed to this function is invalid
 * @retval HDMI_CEC_IO_INVALID_HANDLE   - An invalid handle argument has been passed
 *
 * @pre HdmiCecOpen() must be called before calling this API.
 * @warning This API is NOT thread safe.
 * @note This API is not required if the SOC is performing the logical address discovery.
 * @see HdmiCecAddLogicalAddress(), HdmiCecRemoveLogicalAddress()
 *
 *
 */
HDMI_CEC_STATUS HdmiCecGetLogicalAddress(int handle, int *logicalAddress)
{
	pthread_mutex_lock(&g_cec_context.mutex);

	if (!g_cec_context.initialized) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	if (logicalAddress == NULL) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	*logicalAddress = g_cec_context.logical_address;
	CEC_LOG_INFO("Logical address: %d", g_cec_context.logical_address);
	pthread_mutex_unlock(&g_cec_context.mutex);
	return HDMI_CEC_IO_SUCCESS;
}

/**
 * @brief Sets CEC message receive callback
 *
 * This function sets the callback function to be invoked for each message arrival@n
 * The message contained in the buffer will follow this format
 *     (ref <HDMI Specification 1-4> Section <CEC 6.1>) :
 *
 * complete message  = header block + data block@n
 * header block     = source logical address (4-bit) + destination logical address (4-bit)@n
 * data block       = opcode block (8-bit) + operand block (N-bytes)
 *
 * @code
 * |------------------------------------------------
 * | header block  |          data blocks          |
 * |------------------------------------------------
 * |3|2|1|0|3|2|1|0|7|6|5|4|3|2|1|0|7|6|5|4|3|2|1|0|
 * |------------------------------------------------
 * |  src  | Dest  |  opcode block | operand block |
 * |------------------------------------------------
 * @endcode
 *
 * When receiving, the returned buffer should not contain EOM and ACK bits. HAL internal logic.@n
 * HAL implementation should remove the EOM and ACK bits in the returned buffer
 *
 * When transmitting, it is HAL's responsibility to insert EOM bit and ACK bit
 * for each header or data block.
 *
 * When HdmiCecSetRxCallback() is called, it replaces the previous set cbfunc and data
 * values. Setting a value of (cbfunc=null) disables the callback.
 *
 * This function will block if callback invocation is in progress.
 *
 * @param[in] handle                    - The handle returned from the HdmiCecOpen() function. Non zero value
 * @param[in] cbfunc                    - Function pointer to be invoked
 *                                          when a complete message is received
 * @param[in] data                      - Callback data
 *
 * @return HDMI_CEC_STATUS              - Status
 * @retval HDMI_CEC_IO_SUCCESS          - Success
 * @retval HDMI_CEC_IO_NOT_OPENED       - Module is not initialised
 * @retval HDMI_CEC_IO_INVALID_HANDLE   - An invalid handle argument has been passed
 *
 * @pre HdmiCecOpen() must be called before calling this API.
 * @warning This API is NOT thread safe.
 * @see HdmiCecTx(), HdmiCecTxAsync(), HdmiCecSetTxCallback()
 *
 */
HDMI_CEC_STATUS HdmiCecSetRxCallback(int handle, HdmiCecRxCallback_t callback, void *data)
{
	pthread_mutex_lock(&g_cec_context.mutex);

	if (!g_cec_context.initialized) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	g_cec_context.rx_callback      = callback;
	g_cec_context.rx_callback_data = data;

	pthread_mutex_unlock(&g_cec_context.mutex);
	return HDMI_CEC_IO_SUCCESS;
}

/**
 * @note This API is deprecated.
 *
 * @brief Sets CEC message transmit callback
 *
 * This function sets a callback which will be invoked once the async transmit
 * result is available. This is only necessary if the caller chooses to transmit
 * the message asynchronously.
 *
 * This function will block if callback invocation is in progress.
 *
 * @param[in] handle                    - The handle returned from the HdmiCecOpen(). Non zero value.
 * @param[in] cbfunc                    - Function pointer to be invoked
 *                                          when a complete message is transmitted
 * @param[in] data                      - Callback data
 *
 * @return HDMI_CEC_STATUS              - Status
 * @retval HDMI_CEC_IO_SUCCESS          - Success
 * @retval HDMI_CEC_IO_NOT_OPENED       - Module is not initialised
 * @retval HDMI_CEC_IO_INVALID_HANDLE   - An invalid handle argument has been passed
 *
 * @pre HdmiCecOpen() must be called before calling this API.
 * @warning This API is NOT thread safe.
 * @see HdmiCecTx(), HdmiCecTxAsync()
 *
 */
HDMI_CEC_STATUS HdmiCecSetTxCallback(int handle, HdmiCecTxCallback_t callback, void *data)
{
	pthread_mutex_lock(&g_cec_context.mutex);

	if (!g_cec_context.initialized) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	/* NULL is valid — unregisters the callback */
	g_cec_context.tx_callback      = callback;
	g_cec_context.tx_callback_data = data;

	pthread_mutex_unlock(&g_cec_context.mutex);
	return HDMI_CEC_IO_SUCCESS;
}

/**
 * @brief Synchronous transmit call
 *
 * This function writes a complete CEC message onto the bus and waits for ACK.
 *
 * The message contained in the buffer will follow the format detailed in HdmiCecSetRxCallback_t().
 * (ref <HDMI Specification 1-4> Section <CEC 6.1>)
 *
 *
 * @param[in] handle                              - The handle returned from the
 *                                                    HdmiCecOpen() function. Non zero value
 * @param[in] buf                                 - The buffer contains a complete
 *                                                    CEC message to send.
 * @param[in] len                                 - Number of bytes in the message.
 * @param[out] result                             - send status buffer. Possible results(valid only for directly addressed messages) are
 *                    HDMI_CEC_IO_SENT_AND_ACKD,
 *                    HDMI_CEC_IO_SENT_BUT_NOT_ACKD (e.g. no follower at the destination),
 *                    HDMI_CEC_IO_SENT_FAILED (e.g. collision).
 *
 * @return HDMI_CEC_STATUS                        - Status
 * @retval HDMI_CEC_IO_SUCCESS                    - Transmit ioctl succeeded.
 * @retval HDMI_CEC_IO_NOT_OPENED                 - Module is not initialised
 * @retval HDMI_CEC_IO_INVALID_ARGUMENT           - Parameter passed to this function is invalid
 * @retval HDMI_CEC_IO_INVALID_HANDLE             - An invalid handle argument has been passed
 * @retval HDMI_CEC_IO_SENT_FAILED                - Transmit ioctl failed (API/transport failure).
 *
 * Message-delivery outcomes (ACK/NACK/collision) are reported via @p result
 * and tx_callback, not via the function return value.
 *
 * @pre  HdmiCecOpen() should be called before calling this API.
 * @warning  This API is Not thread safe.
 * @see HdmiCecTxAsync(), HdmiCecSetRxCallback()
 *
 */
HDMI_CEC_STATUS HdmiCecTx(int handle, const unsigned char *buf, int len, int *result)
{
	if (buf == NULL || result == NULL || len <= 0 || len > CEC_MAX_MSG_SIZE) {
		CEC_LOG_ERROR("Invalid arguments: buf=%p, result=%p, len=%d (valid range: 1-%d)",
		              (void *)buf, (void *)result, len, CEC_MAX_MSG_SIZE);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	// dump the message being sent for debugging.
	trace_hexdump(buf, (size_t)len);

	pthread_mutex_lock(&g_cec_context.mutex);

	if (!g_cec_context.initialized) {
		CEC_LOG_ERROR("CEC not opened");
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		CEC_LOG_ERROR("Invalid handle: %d", handle);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	struct cec_msg msg;
	memset(&msg, 0, sizeof(msg));
	msg.timeout = CEC_TX_TIMEOUT_MS; /* block until ACK/NACK */
	/* Keep userland compatibility: ignore caller-provided source nibble and
	 * transmit from the adapter's currently allocated logical address.
	 * RDK HAL byte order: (source << 4) | destination */
	__u8 destination = (__u8)(buf[0] & 0x0F);  /* bits 3-0 */
	__u8 source = g_cec_context.has_logical_address ?
		(__u8)(g_cec_context.logical_address & 0x0F) :
		(__u8)CEC_LOG_ADDR_UNREGISTERED;
	msg.msg[0] = (__u8)((source << 4) | destination);
	if (len > 1) {
		memcpy(&msg.msg[1], &buf[1], (size_t)(len - 1));
	}
	msg.len = (__u32)len;

	/* Keep the mutex held during transmit so HdmiCecClose cannot close/reuse
	 * g_cec_context.fd while this ioctl is in progress. */
	int tx_ret = ioctl(g_cec_context.fd, CEC_TRANSMIT, &msg);
	if (tx_ret < 0 && errno == EINVAL) {
		if (len == 1) {
			/* Some Linux CEC drivers reject header-only poll with EINVAL even
			 * in otherwise valid adapter state. Keep userland-compatible behavior. */
			cec_log_poll_einval_rate_limited_locked(msg.msg[0]);
			*result = HDMI_CEC_IO_SENT_BUT_NOT_ACKD;
			pthread_mutex_unlock(&g_cec_context.mutex);
			return HDMI_CEC_IO_SUCCESS;
		}
		/* Adapter state may have changed (mode/logical address). Recover then retry once. */
		cec_log_tx_diagnostics_locked(&msg, len);
		CEC_LOG_WARN("CEC_TRANSMIT returned EINVAL (hdr=0x%02x len=%d), attempting recovery",
		             msg.msg[0], len);
		cec_recover_tx_state_locked();
		source = g_cec_context.has_logical_address ?
			(__u8)(g_cec_context.logical_address & 0x0F) :
			(__u8)CEC_LOG_ADDR_UNREGISTERED;
		msg.msg[0] = (__u8)((source << 4) | destination);
		msg.tx_status = 0;
		msg.tx_arb_lost_cnt = 0;
		msg.tx_nack_cnt = 0;
		msg.tx_low_drive_cnt = 0;
		msg.tx_error_cnt = 0;
		tx_ret = ioctl(g_cec_context.fd, CEC_TRANSMIT, &msg);
	}
	if (tx_ret < 0) {
		CEC_LOG_ERROR("CEC_TRANSMIT failed: %s", strerror(errno));
		pthread_mutex_unlock(&g_cec_context.mutex);
		*result = HDMI_CEC_IO_SENT_FAILED;
		return HDMI_CEC_IO_SENT_FAILED;
	}

	if (msg.tx_status & CEC_TX_STATUS_OK) {
		*result = HDMI_CEC_IO_SENT_AND_ACKD;
	} else if (msg.tx_status & CEC_TX_STATUS_NACK) {
		*result = HDMI_CEC_IO_SENT_BUT_NOT_ACKD;
	} else if (len == 1) {
		/* Some adapters may complete poll transmit without setting explicit
		 * tx_status bits. Preserve poll semantics as not-acknowledged. */
		*result = HDMI_CEC_IO_SENT_BUT_NOT_ACKD;
	} else {
		*result = HDMI_CEC_IO_SENT_FAILED;
	}

	/* HdmiCecTx() is the blocking transmit path: completion for this call is
	 * reported synchronously via *result based on msg.tx_status returned from
	 * CEC_TRANSMIT. Do not fire tx_callback inline here: doing so would risk
	 * deadlock if the callback calls HdmiCecClose(), and some drivers may still
	 * surface TX state asynchronously. Callers that require tx_callback delivery
	 * must use the asynchronous transmit path; synchronous callers must use
	 * *result as the completion status. */
	pthread_mutex_unlock(&g_cec_context.mutex);

	/* Return value: preserve prior observable behavior for callers that check the return code.
	 * Return SENT_FAILED only for actual transmit failures (e.g., arbitration lost,
	 * collision/arb-lost). ACK and NACK outcomes for this synchronous path are
	 * reported via *result while this function still returns HDMI_CEC_IO_SUCCESS. */
	if (*result == HDMI_CEC_IO_SENT_FAILED)
		return HDMI_CEC_IO_SENT_FAILED;
	return HDMI_CEC_IO_SUCCESS;
}

/**
 * @note This API is deprecated.
 *
 * @brief Writes CEC message onto bus asynchronously.
 *
 * This function writes a complete CEC message onto the bus but does not wait
 * for ACK. The result will be reported via HdmiCecTxCallback_t()
 *
 *
 * @param[in] handle                              - The handle returned from the
 *                                                    HdmiCecOpen() function. Non zero value
 * @param[in] buf                                 - The buffer contains a complete
 *                                                    CEC message to send
 * @param[in] len                                 - Number of bytes in the message
 *
 * @return HDMI_CEC_STATUS                        - Status
 * @retval HDMI_CEC_IO_SUCCESS                    - Success
 * @retval HDMI_CEC_IO_NOT_OPENED                 - Module is not initialised
 * @retval HDMI_CEC_IO_INVALID_ARGUMENT           - Parameter passed to this function is invalid
 * @retval HDMI_CEC_IO_INVALID_HANDLE             - An invalid handle argument has been passed
 *
 * @pre  HdmiCecOpen(), HdmiCecSetRxCallback(), HdmiCecSetTxCallback()  should be called before calling this API.
 * @warning  This API is Not thread safe.
 * @see HdmiCecTx(), HdmiCecSetRxCallback()
 *
 */
HDMI_CEC_STATUS HdmiCecTxAsync(int handle, const unsigned char *buf, int len)
{
	if (buf == NULL || len <= 0 || len > CEC_MAX_MSG_SIZE) {
		CEC_LOG_ERROR("Invalid arguments: buf=%p, len=%d (valid range: 1-%d)",
		              (void *)buf, len, CEC_MAX_MSG_SIZE);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	// dump the message being sent for debugging.
	trace_hexdump(buf, (size_t)len);

	pthread_mutex_lock(&g_cec_context.mutex);

	if (!g_cec_context.initialized) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	struct cec_msg msg;
	memset(&msg, 0, sizeof(msg));
	msg.timeout = 0; /* async: do not wait; tx_status returned via CEC_RECEIVE */
	/* Keep userland compatibility: ignore caller-provided source nibble and
	 * transmit from the adapter's currently allocated logical address.
	 * RDK HAL byte order: (source << 4) | destination */
	__u8 destination = (__u8)(buf[0] & 0x0F);  /* bits 3-0 */
	__u8 source = g_cec_context.has_logical_address ?
		(__u8)(g_cec_context.logical_address & 0x0F) :
		(__u8)CEC_LOG_ADDR_UNREGISTERED;
	msg.msg[0] = (__u8)((source << 4) | destination);
	if (len > 1) {
		memcpy(&msg.msg[1], &buf[1], (size_t)(len - 1));
	}
	msg.len = (__u32)len;

	/* Keep the mutex held while submitting async transmit to avoid close-race
	 * on g_cec_context.fd. */
	int tx_ret = ioctl(g_cec_context.fd, CEC_TRANSMIT, &msg);
	if (tx_ret < 0 && errno == EINVAL) {
		if (len == 1) {
			/* Preserve poll behavior symmetry with HdmiCecTx: poll EINVAL means
			 * not-acknowledged (listener could not be found or did not respond).
			 * Synthesize tx_callback completion to maintain async API contract:
			 * callers must be notified of result (they cannot infer from return value).
			 *
			 * Account for this inline completion in callback_active so shutdown
			 * can wait for in-flight callback execution from other threads.
			 * Close re-entry from this same callback thread is handled by
			 * cec_is_in_callback_context() bias in HdmiCecClose(). */
			cec_log_poll_einval_rate_limited_locked(msg.msg[0]);

			HdmiCecTxCallback_t tx_callback = NULL;
			void *tx_callback_data = NULL;
			int callback_handle = 0;
			bool invoke_callback = false;
			if (g_cec_context.running && !g_cec_context.closing &&
			    g_cec_context.initialized && g_cec_context.tx_callback != NULL) {
				tx_callback = g_cec_context.tx_callback;
				tx_callback_data = g_cec_context.tx_callback_data;
				callback_handle = g_cec_context.handle;
				g_cec_context.callback_active++;
				invoke_callback = true;
			}
			pthread_mutex_unlock(&g_cec_context.mutex);

			if (invoke_callback) {
				cec_set_in_callback_context(true);
				tx_callback(callback_handle, tx_callback_data, HDMI_CEC_IO_SENT_BUT_NOT_ACKD);
				cec_set_in_callback_context(false);

				bool callback_active_underflow = false;
				pthread_mutex_lock(&g_cec_context.mutex);
				if (g_cec_context.callback_active > 0) {
					g_cec_context.callback_active--;
				} else {
					callback_active_underflow = true;
				}
				pthread_mutex_unlock(&g_cec_context.mutex);
				if (callback_active_underflow) {
					CEC_LOG_WARN("callback_active already 0 after tx poll callback; skipping decrement");
				}
			}
			return HDMI_CEC_IO_SUCCESS;
		}
		/* Adapter state may have changed (mode/logical address). Recover then retry once. */
		cec_log_tx_diagnostics_locked(&msg, len);
		CEC_LOG_WARN("CEC_TRANSMIT (async) returned EINVAL (hdr=0x%02x len=%d), attempting recovery",
		             msg.msg[0], len);
		cec_recover_tx_state_locked();
		source = g_cec_context.has_logical_address ?
			(__u8)(g_cec_context.logical_address & 0x0F) :
			(__u8)CEC_LOG_ADDR_UNREGISTERED;
		msg.msg[0] = (__u8)((source << 4) | destination);
		msg.tx_status = 0;
		msg.tx_arb_lost_cnt = 0;
		msg.tx_nack_cnt = 0;
		msg.tx_low_drive_cnt = 0;
		msg.tx_error_cnt = 0;
		tx_ret = ioctl(g_cec_context.fd, CEC_TRANSMIT, &msg);
	}
	if (tx_ret < 0) {
		CEC_LOG_ERROR("CEC_TRANSMIT (async) failed: %s", strerror(errno));
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_SENT_FAILED;
	}

	pthread_mutex_unlock(&g_cec_context.mutex);

	/* tx_callback will be invoked from cec_rx_thread when the TX status
	 * is delivered back via CEC_RECEIVE */
	return HDMI_CEC_IO_SUCCESS;
}

static void __attribute__((constructor)) cec_driver_init(void)
{
	cec_log_init();
	CEC_LOG_INFO("RPi4 CEC HAL (LINUXCEC) - Version: %s, Git SHA: %s",
	             HAL_VERSION, GIT_COMMIT_SHA);
}

static void __attribute__((destructor)) cec_driver_term(void)
{
	struct timespec timeout;
	if (clock_gettime(CLOCK_REALTIME, &timeout) == 0) {
		timeout.tv_sec += CEC_MUTEX_TIMEOUT_SEC;
	} else {
		timeout.tv_sec  = time(NULL) + CEC_MUTEX_TIMEOUT_SEC;
		timeout.tv_nsec = 0;
	}

	int lock_result = pthread_mutex_timedlock(&g_cec_context.mutex, &timeout);
	if (lock_result != 0) {
		/* Mutex acquisition timed out — skip all cleanup to avoid undefined
		 * behaviour and rely on the OS to reclaim resources. */
		fprintf(stderr, "RPICECHAL: Error - mutex timeout during shutdown, skipping cleanup.\n");
		return;
	}

	bool need_cleanup = false;
	bool need_join = false;
	pthread_t rx_thread_to_join = (pthread_t)0;
	int pipe_wr = g_cec_context.pipe_wr;

	if (g_cec_context.initialized) {
		CEC_LOG_WARN("CEC device still open during shutdown");
		g_cec_context.running     = false;
		g_cec_context.initialized = false;
		need_cleanup = true;
	}
	if (g_cec_context.rx_thread_running || g_cec_context.deferred_cleanup || g_cec_context.closing) {
		need_cleanup = true;
	}
	if (g_cec_context.rx_thread_created) {
		need_join = true;
		rx_thread_to_join = g_cec_context.rx_thread;
	}

	pthread_mutex_unlock(&g_cec_context.mutex);

	/* Signal and join rx_thread even if it already exited but has not yet been joined. */
	if (need_join) {
		char byte = 1;
		if (pipe_wr >= 0) {
			(void)write(pipe_wr, &byte, 1);
		}
		pthread_join(rx_thread_to_join, NULL);
	}

	/* Reacquire mutex with timeout to ensure cleanup proceeds even if another
	 * thread holds the lock. */
	if (clock_gettime(CLOCK_REALTIME, &timeout) == 0) {
		timeout.tv_sec += CEC_MUTEX_TIMEOUT_SEC;
	} else {
		timeout.tv_sec  = time(NULL) + CEC_MUTEX_TIMEOUT_SEC;
		timeout.tv_nsec = 0;
	}
	lock_result = pthread_mutex_timedlock(&g_cec_context.mutex, &timeout);
	if (lock_result != 0) {
		/* Second mutex acquisition timed out — skip cleanup of FDs and state
		 * to avoid potential race; rely on OS to reclaim resources at process exit. */
		fprintf(stderr, "RPICECHAL: Error - mutex timeout during cleanup phase, skipping FD close.\n");
		return;
	}
	if (need_join) {
		g_cec_context.rx_thread_running = false;
		g_cec_context.rx_thread_created = false;
		g_cec_context.rx_thread = (pthread_t)0;
	}

	if (need_cleanup) {
		/* Wait briefly for callbacks */
		int wait_count = 0;
		while (g_cec_context.callback_active > 0 &&
		       wait_count < CEC_DESTRUCTOR_MAX_WAIT_ITERATIONS) {
			pthread_mutex_unlock(&g_cec_context.mutex);
			usleep(CEC_CALLBACK_WAIT_US);
			pthread_mutex_lock(&g_cec_context.mutex);
			wait_count++;
		}

		if (g_cec_context.pipe_wr >= 0) close(g_cec_context.pipe_wr);
		if (g_cec_context.pipe_rd >= 0) close(g_cec_context.pipe_rd);
		cec_release_logical_addresses_fd(g_cec_context.fd);
		if (g_cec_context.fd >= 0)      close(g_cec_context.fd);
		if (g_cec_context.lock_fd >= 0) close(g_cec_context.lock_fd);
		g_cec_context.fd      = -1;
		g_cec_context.pipe_rd = -1;
		g_cec_context.pipe_wr = -1;
		g_cec_context.lock_fd = -1;
		g_cec_context.deferred_cleanup = false;
		g_cec_context.closing = false;
	}

	pthread_mutex_unlock(&g_cec_context.mutex);
	pthread_mutex_destroy(&g_cec_context.mutex);

	cec_log_close();
	pthread_mutex_destroy(&g_log_mutex);
}

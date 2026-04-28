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
 * A dedicated receive thread uses select() + CEC_RECEIVE to deliver callbacks.
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/file.h>
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

#define CEC_MAX_MSG_SIZE        16

/* CEC_DEV_NODE is the full CEC device node path, injected at compile time
 * via -DCEC_DEV_NODE="/dev/cecN".  Default is /dev/cec0 (HDMI0).
 * Override at build time via cmake -DCEC_DEV_NODE=/dev/cec1 for HDMI1. */
#ifndef CEC_DEV_NODE
#define CEC_DEV_NODE "/dev/cec0"
#endif
#define CEC_DEVICE_PATH_DEFAULT CEC_DEV_NODE

#define CEC_TIMESTAMP_FALLBACK  "_TIMESTAMP_UNAVAILABLE_"
#define CEC_TIMESTAMP_SIZE      64

/* Raspberry Pi CEC Configuration */
#define RPI_CEC_VENDOR_ID       0x00BC44

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
	bool rx_thread_running;
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
static int g_log_level = CEC_LOG_LEVEL_WARN;

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
		g_log_file = fopen(log_file_env, "a");
		if (g_log_file != NULL) {
			setvbuf(g_log_file, NULL, _IOLBF, 0);
			char timestamp[CEC_TIMESTAMP_SIZE];
			cec_get_timestamp(timestamp, sizeof(timestamp));
			fprintf(g_log_file, "RPICECHAL: Log Started (LINUXCEC): %s (Level: %s)\n",
					timestamp, cec_get_log_level_str(g_log_level));
		} else {
			fprintf(stdout, "RPICECHAL: Failed to open log file: %s\n", log_file_env);
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
	return (int)(time(NULL) & 0x7FFFFFFF) ?: 1;
}

static cec_context_t g_cec_context = {
	.fd               = -1,
	.pipe_rd          = -1,
	.pipe_wr          = -1,
	.lock_fd          = -1,
	.rx_thread_running = false,
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

/*
 * Receive thread: blocks on select() multiplexed between the CEC device fd
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
		pfds[0].events = POLLIN;
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
			CEC_LOG_DEBUG("TX completion: tx_status=0x%02x", msg.tx_status);

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

				pthread_mutex_lock(&ctx->mutex);
				ctx->callback_active--;
				pthread_mutex_unlock(&ctx->mutex);
			}
		}

		/* Received CEC message */
		if ((msg.rx_status & CEC_RX_STATUS_OK) && msg.len > 0) {
			CEC_LOG_INFO("Received CEC message: len=%u", (unsigned int)msg.len);

			pthread_mutex_lock(&ctx->mutex);
			HdmiCecRxCallback_t rx_cb = ctx->rx_callback;
			void *rx_cb_data          = ctx->rx_callback_data;
			int cb_handle             = ctx->handle;
			bool should_call = ctx->running && ctx->initialized && rx_cb != NULL;
			if (should_call) ctx->callback_active++;
			pthread_mutex_unlock(&ctx->mutex);

			if (should_call) {
				rx_cb(cb_handle, rx_cb_data, msg.msg, (int)msg.len);

				pthread_mutex_lock(&ctx->mutex);
				ctx->callback_active--;
				pthread_mutex_unlock(&ctx->mutex);
			}
		}
	}

	return NULL;
}

HDMI_CEC_STATUS HdmiCecOpen(int *handle)
{
	if (handle == NULL) {
		CEC_LOG_ERROR("Invalid argument: handle is NULL");
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	pthread_mutex_lock(&g_cec_context.mutex);

	if (g_cec_context.initialized) {
		CEC_LOG_WARN("%s already opened", __func__);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_ALREADY_OPEN;
	}

	/* Allow device path override via environment variable */
	const char *dev_path = getenv("CEC_HAL_DEVICE");
	if (dev_path == NULL)
		dev_path = CEC_DEVICE_PATH_DEFAULT;

	/* Enforce singleton: one HAL instance per CEC device node across all processes.
	 * An exclusive non-blocking flock on a well-known lock file achieves this.
	 * The OS releases the lock automatically if the process exits or crashes. */
	char lock_path[128];
	const char *base = strrchr(dev_path, '/');
	base = base ? base + 1 : dev_path;
	int lock_path_len = snprintf(lock_path, sizeof(lock_path), "/var/lock/RCECHal_%s.lock", base);
	if (lock_path_len < 0 || (size_t)lock_path_len >= sizeof(lock_path)) {
		CEC_LOG_ERROR("CEC device path too long to form lock file name: %s", dev_path);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lock_fd < 0) {
		CEC_LOG_ERROR("Failed to open lock file %s: %s", lock_path, strerror(errno));
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

	/* Set mode: exclusive initiator + exclusive follower with passthrough.
	 * Passthrough allows receiving messages addressed to other devices so
	 * that the upper CEC layer can observe all bus traffic. */
	__u32 mode = CEC_MODE_INITIATOR | CEC_MODE_EXCL_FOLLOWER_PASSTHROUGH;
	if (ioctl(fd, CEC_S_MODE, &mode) < 0) {
		CEC_LOG_WARN("CEC_S_MODE failed: %s", strerror(errno));
	}

	/* Read physical address from the adapter (set by the kernel HDMI driver) */
	__u16 phys_addr = CEC_PHYS_ADDR_INVALID;
	if (ioctl(fd, CEC_ADAP_G_PHYS_ADDR, &phys_addr) < 0) {
		CEC_LOG_WARN("CEC_ADAP_G_PHYS_ADDR failed: %s", strerror(errno));
		phys_addr = CEC_PHYS_ADDR_INVALID;
	}
	CEC_LOG_DEBUG("Physical address: 0x%04x", phys_addr);

	/* Claim a logical address (STB / Tuner device type) */
	struct cec_log_addrs log_addrs;
	memset(&log_addrs, 0, sizeof(log_addrs));
	log_addrs.num_log_addrs          = 1;
	log_addrs.cec_version            = CEC_OP_CEC_VERSION_1_4;
	log_addrs.vendor_id              = RPI_CEC_VENDOR_ID;
	log_addrs.flags                  = CEC_LOG_ADDRS_FL_ALLOW_UNREG_FALLBACK;
	strncpy(log_addrs.osd_name, "RDK-STB", sizeof(log_addrs.osd_name) - 1);
	log_addrs.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_TUNER;
	log_addrs.log_addr_type[0]       = CEC_LOG_ADDR_TYPE_TUNER;
	log_addrs.all_device_types[0]    = CEC_OP_ALL_DEVTYPE_TUNER;

	/* This ioctl blocks until the logical address claiming process completes */
	if (ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &log_addrs) < 0) {
		CEC_LOG_WARN("CEC_ADAP_S_LOG_ADDRS failed: %s", strerror(errno));
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
	if (pipe(pipefd) < 0) {
		CEC_LOG_ERROR("pipe() failed: %s", strerror(errno));
		close(fd);
		close(lock_fd);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
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

	if (pthread_create(&g_cec_context.rx_thread, NULL,
	                   cec_rx_thread, &g_cec_context) != 0) {
		CEC_LOG_ERROR("pthread_create failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		close(fd);
		close(lock_fd);
		g_cec_context.fd          = -1;
		g_cec_context.lock_fd     = -1;
		g_cec_context.pipe_rd     = -1;
		g_cec_context.pipe_wr     = -1;
		g_cec_context.initialized = false;
		g_cec_context.running     = false;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	g_cec_context.rx_thread_running = true;

	*handle = g_cec_context.handle;
	pthread_mutex_unlock(&g_cec_context.mutex);

	CEC_LOG_INFO("HdmiCecOpen successful: handle=%d, phys_addr=0x%04x, log_addr=%d",
	             *handle, phys_addr, logical_addr);
	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecClose(int handle)
{
	pthread_mutex_lock(&g_cec_context.mutex);

	if (!g_cec_context.initialized) {
		CEC_LOG_WARN("CEC not opened");
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		CEC_LOG_ERROR("Invalid handle: %d (expected: %d)", handle, g_cec_context.handle);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	g_cec_context.running     = false;
	g_cec_context.initialized = false;

	pthread_mutex_unlock(&g_cec_context.mutex);

	/* Signal rx_thread to exit then reap it */
	if (g_cec_context.rx_thread_running) {
		char byte = 1;
		(void)write(g_cec_context.pipe_wr, &byte, 1);
		pthread_join(g_cec_context.rx_thread, NULL);
		g_cec_context.rx_thread_running = false;
	}

	/* Wait for any in-progress callbacks to complete */
	pthread_mutex_lock(&g_cec_context.mutex);
	int wait_count = 0;
	while (g_cec_context.callback_active > 0 &&
	       wait_count < CEC_CLOSE_MAX_WAIT_ITERATIONS) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		usleep(CEC_CALLBACK_WAIT_US);
		pthread_mutex_lock(&g_cec_context.mutex);
		wait_count++;
	}
	if (g_cec_context.callback_active > 0) {
		CEC_LOG_WARN("Closing with %d active callbacks still running",
		             g_cec_context.callback_active);
	}

	/* Release CEC resources */
	close(g_cec_context.pipe_wr);
	close(g_cec_context.pipe_rd);
	close(g_cec_context.fd);
	close(g_cec_context.lock_fd);
	g_cec_context.fd      = -1;
	g_cec_context.pipe_rd = -1;
	g_cec_context.pipe_wr = -1;
	g_cec_context.lock_fd = -1;

	g_cec_context.handle              = 0;
	g_cec_context.rx_callback         = NULL;
	g_cec_context.rx_callback_data    = NULL;
	g_cec_context.tx_callback         = NULL;
	g_cec_context.tx_callback_data    = NULL;
	g_cec_context.logical_address     = CEC_LOG_ADDR_UNREGISTERED;
	g_cec_context.has_logical_address = false;
	g_cec_context.physical_address    = CEC_PHYS_ADDR_INVALID;
	g_cec_context.callback_active     = 0;

	pthread_mutex_unlock(&g_cec_context.mutex);

	CEC_LOG_INFO("HdmiCecClose successful");
	return HDMI_CEC_IO_SUCCESS;
}

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
		CEC_LOG_WARN("CEC_ADAP_G_PHYS_ADDR failed: %s — returning cached value",
		             strerror(errno));
		phys_addr = g_cec_context.physical_address;
	} else {
		g_cec_context.physical_address = phys_addr;
	}

	*physicalAddress = phys_addr;
	pthread_mutex_unlock(&g_cec_context.mutex);

	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecAddLogicalAddress(int handle, int logicalAddresses)
{
	/* For source devices, this operation is not supported */
	CEC_LOG_WARN("HdmiCecAddLogicalAddress not supported for source devices");
	return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;
}

HDMI_CEC_STATUS HdmiCecRemoveLogicalAddress(int handle, int logicalAddresses)
{
	/* For source devices, this operation is not supported */
	CEC_LOG_WARN("HdmiCecRemoveLogicalAddress not supported for source devices");
	return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;
}

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
	pthread_mutex_unlock(&g_cec_context.mutex);
	return HDMI_CEC_IO_SUCCESS;
}

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

HDMI_CEC_STATUS HdmiCecTx(int handle, const unsigned char *buf, int len, int *result)
{
	if (buf == NULL || result == NULL || len <= 0 || len > CEC_MAX_MSG_SIZE) {
		CEC_LOG_ERROR("Invalid arguments: buf=%p, result=%p, len=%d (valid range: 1-%d)",
		              (void *)buf, (void *)result, len, CEC_MAX_MSG_SIZE);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

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

	int fd = g_cec_context.fd;
	pthread_mutex_unlock(&g_cec_context.mutex);

	struct cec_msg msg;
	memset(&msg, 0, sizeof(msg));
	msg.timeout = CEC_TX_TIMEOUT_MS; /* block until ACK/NACK */
	memcpy(msg.msg, buf, (size_t)len);
	msg.len = (__u32)len;

	if (ioctl(fd, CEC_TRANSMIT, &msg) < 0) {
		CEC_LOG_ERROR("CEC_TRANSMIT failed: %s", strerror(errno));
		*result = HDMI_CEC_IO_SENT_FAILED;
		return HDMI_CEC_IO_SENT_FAILED;
	}

	if (msg.tx_status & CEC_TX_STATUS_OK) {
		*result = HDMI_CEC_IO_SENT_AND_ACKD;
	} else if (msg.tx_status & CEC_TX_STATUS_NACK) {
		*result = HDMI_CEC_IO_SENT_BUT_NOT_ACKD;
	} else {
		*result = HDMI_CEC_IO_SENT_FAILED;
	}

	/* Fire tx_callback with the transmission result */
	pthread_mutex_lock(&g_cec_context.mutex);
	HdmiCecTxCallback_t tx_cb = g_cec_context.tx_callback;
	void *tx_cb_data           = g_cec_context.tx_callback_data;
	int cb_handle              = g_cec_context.handle;
	bool should_call = g_cec_context.running && g_cec_context.initialized && tx_cb != NULL;
	if (should_call) g_cec_context.callback_active++;
	pthread_mutex_unlock(&g_cec_context.mutex);

	if (should_call) {
		tx_cb(cb_handle, tx_cb_data, *result);
		pthread_mutex_lock(&g_cec_context.mutex);
		g_cec_context.callback_active--;
		pthread_mutex_unlock(&g_cec_context.mutex);
	}

	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecTxAsync(int handle, const unsigned char *buf, int len)
{
	if (buf == NULL || len <= 0 || len > CEC_MAX_MSG_SIZE) {
		CEC_LOG_ERROR("Invalid arguments: buf=%p, len=%d (valid range: 1-%d)",
		              (void *)buf, len, CEC_MAX_MSG_SIZE);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	pthread_mutex_lock(&g_cec_context.mutex);

	if (!g_cec_context.initialized) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	int fd = g_cec_context.fd;
	pthread_mutex_unlock(&g_cec_context.mutex);

	struct cec_msg msg;
	memset(&msg, 0, sizeof(msg));
	msg.timeout = 0; /* async: do not wait; tx_status returned via CEC_RECEIVE */
	memcpy(msg.msg, buf, (size_t)len);
	msg.len = (__u32)len;

	if (ioctl(fd, CEC_TRANSMIT, &msg) < 0) {
		CEC_LOG_ERROR("CEC_TRANSMIT (async) failed: %s", strerror(errno));
		return HDMI_CEC_IO_SENT_FAILED;
	}

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
		fprintf(stderr, "RPiCECHAL: Error - mutex timeout during shutdown, skipping cleanup.\n");
		return;
	}

	if (g_cec_context.initialized) {
		CEC_LOG_WARN("CEC device still open during shutdown");
		g_cec_context.running     = false;
		g_cec_context.initialized = false;
		pthread_mutex_unlock(&g_cec_context.mutex);

		/* Signal and join rx_thread */
		if (g_cec_context.rx_thread_running) {
			char byte = 1;
			(void)write(g_cec_context.pipe_wr, &byte, 1);
			pthread_join(g_cec_context.rx_thread, NULL);
			g_cec_context.rx_thread_running = false;
		}

		/* Wait briefly for callbacks */
		pthread_mutex_lock(&g_cec_context.mutex);
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
		if (g_cec_context.fd >= 0)      close(g_cec_context.fd);
		if (g_cec_context.lock_fd >= 0) close(g_cec_context.lock_fd);
		g_cec_context.fd      = -1;
		g_cec_context.pipe_rd = -1;
		g_cec_context.pipe_wr = -1;
		g_cec_context.lock_fd = -1;
	}

	pthread_mutex_unlock(&g_cec_context.mutex);
	pthread_mutex_destroy(&g_cec_context.mutex);

	cec_log_close();
	pthread_mutex_destroy(&g_log_mutex);
}

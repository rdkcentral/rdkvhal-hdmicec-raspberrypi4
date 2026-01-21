/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2024 RDK Management
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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <linux/cec.h>
#include <poll.h>
#include <time.h>
#include <sys/stat.h>
#include <stdarg.h>
#include "hdmi_cec_driver.h"

#ifndef HAL_VERSION
#define HAL_VERSION "unknown"
#endif
#ifndef GIT_COMMIT_SHA
#define GIT_COMMIT_SHA "unknown"
#endif

#define CEC_DEVICE_PATH "/dev/cec0"
#define CEC_MAX_MSG_SIZE 16
#define RX_POLL_TIMEOUT_MS 100
#define MAX_CONSECUTIVE_ERRORS 10
#define ERROR_RECOVERY_DELAY_MS 100
#define THREAD_JOIN_TIMEOUT_SEC 5
#define CEC_IOCTL_TIMEOUT_MS 1000

// Debug logging configuration
#define CEC_LOG_MAX_SIZE (4 * 1024 * 1024)
#define CEC_LOG_DIR "/opt/logs"
#define CEC_LOG_FILE_DEFAULT CEC_LOG_DIR "/cechal.log"
#define CEC_LOG_FILE_SUFFIX ".old"
#define CEC_TIMESTAMP_FALLBACK "1970-01-01 00:00:00.000"
#define CEC_TIMESTAMP_SIZE 64
#define CEC_LOG_PATH_MAX 256

// Raspberry Pi CEC Configuration
#define RPI_CEC_VENDOR_ID 0x00BC44
#define RPI_CEC_OSD_NAME "Raspberry Pi"
#define RPI_CEC_UNREGISTERED_ADDR 0x0F

// Log levels (numeric constants)
#define CEC_LOG_LEVEL_ERROR   0
#define CEC_LOG_LEVEL_WARN    1
#define CEC_LOG_LEVEL_INFO    2
#define CEC_LOG_LEVEL_DEBUG   3
#define CEC_LOG_LEVEL_TRACE   4

// Logging macros - controlled by environment variables CEC_HAL_LOG_LEVEL and CEC_HAL_LOG_FILE
#define CEC_LOG(level, ...) cec_log(level, __func__, __LINE__, __VA_ARGS__)
#define CEC_LOG_ERROR(...) CEC_LOG(CEC_LOG_LEVEL_ERROR, __VA_ARGS__)
#define CEC_LOG_WARN(...)  CEC_LOG(CEC_LOG_LEVEL_WARN, __VA_ARGS__)
#define CEC_LOG_INFO(...)  CEC_LOG(CEC_LOG_LEVEL_INFO, __VA_ARGS__)
#define CEC_LOG_DEBUG(...) CEC_LOG(CEC_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define CEC_LOG_TRACE(...) CEC_LOG(CEC_LOG_LEVEL_TRACE, __VA_ARGS__)
#define CEC_LOG_BUFFER(prefix, buf, len) cec_log_buffer(prefix, buf, len)

typedef struct {
	int fd;
	int handle;
	bool initialized;
	bool running;
	bool thread_created;
	pthread_t rx_thread;
	pthread_mutex_t mutex;
	HdmiCecRxCallback_t rx_callback;
	void *rx_callback_data;
	HdmiCecTxCallback_t tx_callback;
	void *tx_callback_data;
	unsigned int tx_callback_gen;
	int logical_address;
	unsigned int physical_address;
	bool has_logical_address;
} cec_context_t;

static FILE *g_log_file = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_log_init_once = PTHREAD_ONCE_INIT;

// Default log level
static int g_log_level = CEC_LOG_LEVEL_ERROR;

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
				tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
				msec);
}

static const char* cec_get_log_level_str(int level)
{
	switch(level) {
		case CEC_LOG_LEVEL_ERROR: return "ERROR";
		case CEC_LOG_LEVEL_WARN:  return "WARN ";
		case CEC_LOG_LEVEL_INFO:  return "INFO ";
		case CEC_LOG_LEVEL_DEBUG: return "DEBUG";
		case CEC_LOG_LEVEL_TRACE: return "TRACE";
		default: return "UNKNOWN";
	}
}

static void cec_log_init_impl(void)
{
	pthread_mutex_lock(&g_log_mutex);

	const char *log_level_env = getenv("CEC_HAL_LOG_LEVEL");
	const char *log_file_env = getenv("CEC_HAL_LOG_FILE");

	if (log_level_env == NULL && log_file_env == NULL) {
		pthread_mutex_unlock(&g_log_mutex);
		return;
	}

	if (log_level_env != NULL) {
		if (strcmp(log_level_env, "ERROR") == 0) {
			g_log_level = CEC_LOG_LEVEL_ERROR;
		} else if (strcmp(log_level_env, "WARN") == 0) {
			g_log_level = CEC_LOG_LEVEL_WARN;
		} else if (strcmp(log_level_env, "INFO") == 0) {
			g_log_level = CEC_LOG_LEVEL_INFO;
		} else if (strcmp(log_level_env, "DEBUG") == 0) {
			g_log_level = CEC_LOG_LEVEL_DEBUG;
		} else if (strcmp(log_level_env, "TRACE") == 0) {
			g_log_level = CEC_LOG_LEVEL_TRACE;
		}
	}

	const char *log_file_path = log_file_env ? log_file_env : CEC_LOG_FILE_DEFAULT;
	struct stat st;
	if (stat(CEC_LOG_DIR, &st) != 0) {
		if (errno == ENOENT) {
			mkdir(CEC_LOG_DIR, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
		}
	}

	g_log_file = fopen(log_file_path, "a");
	if (g_log_file != NULL) {
		if (fseek(g_log_file, 0, SEEK_END) == 0) {
			long size = ftell(g_log_file);

			if (size > 0 && size > CEC_LOG_MAX_SIZE) {
				fclose(g_log_file);
				g_log_file = NULL;
				char old_log[CEC_LOG_PATH_MAX];
				snprintf(old_log, sizeof(old_log), "%s%s", log_file_path, CEC_LOG_FILE_SUFFIX);
				if (rename(log_file_path, old_log) != 0) {
					g_log_file = fopen(log_file_path, "w");
				} else {
					g_log_file = fopen(log_file_path, "a");
				}
			}
		}

		if (g_log_file != NULL) {
			setvbuf(g_log_file, NULL, _IOLBF, 0);

			char timestamp[CEC_TIMESTAMP_SIZE];
			cec_get_timestamp(timestamp, sizeof(timestamp));
			fprintf(g_log_file, "\nCEC HAL Log Started: %s (Level: %s)\n",
					timestamp, cec_get_log_level_str(g_log_level));
		}
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
		fprintf(g_log_file, "CEC HAL Log Closed: %s\n", timestamp);
		fclose(g_log_file);
		g_log_file = NULL;
	}

	pthread_mutex_unlock(&g_log_mutex);
}

static void cec_log(int level, const char *func, int line, const char *format, ...)
{
	if (level > g_log_level) {
		return;
	}

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

static void cec_log_buffer(const char *prefix, const unsigned char *buf, int len)
{
	if (buf == NULL || len <= 0) {
		return;
	}

	pthread_mutex_lock(&g_log_mutex);

	if (CEC_LOG_LEVEL_DEBUG > g_log_level || g_log_file == NULL) {
		pthread_mutex_unlock(&g_log_mutex);
		return;
	}

	char timestamp[CEC_TIMESTAMP_SIZE];
	cec_get_timestamp(timestamp, sizeof(timestamp));

	fprintf(g_log_file, "[%s] [DEBUG] %s (%d bytes): ", timestamp, prefix, len);
	for (int i = 0; i < len && i < CEC_MAX_MSG_SIZE; i++) {
		fprintf(g_log_file, "%02X ", buf[i]);
	}
	fprintf(g_log_file, "\n");
	fflush(g_log_file);

	pthread_mutex_unlock(&g_log_mutex);
}

static inline int cec_convert_logical_address(__u8 cec_addr)
{
	return (int)cec_addr;
}

static inline unsigned int cec_convert_physical_address(__u16 cec_phys_addr)
{
	return (unsigned int)cec_phys_addr;
}

static int cec_generate_handle(void)
{
	int handle = 0;
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		handle = (int)((ts.tv_sec ^ ts.tv_nsec ^ getpid()) & 0x7FFFFFFF);
		if (handle == 0) {
			handle = (int)(ts.tv_nsec & 0x7FFFFFFF);
			if (handle == 0) {
				handle = 1;
			}
		}
		return handle;
	}

	handle = (int)(time(NULL) & 0x7FFFFFFF);
	if (handle == 0) {
		handle = 1;
	}

	return handle;
}

static cec_context_t g_cec_context = {
	.fd = -1,
	.handle = 0,
	.initialized = false,
	.running = false,
	.thread_created = false,
	.rx_thread = 0,
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.rx_callback = NULL,
	.rx_callback_data = NULL,
	.tx_callback = NULL,
	.tx_callback_data = NULL,
	.tx_callback_gen = 0,
	.logical_address = RPI_CEC_UNREGISTERED_ADDR,
	.physical_address = 0xFFFF,
	.has_logical_address = false
};

static void *cec_rx_thread(void *arg)
{
	cec_context_t *ctx = (cec_context_t *)arg;
	struct cec_msg msg;
	unsigned char buf[CEC_MAX_MSG_SIZE];
	int len;
	struct pollfd pfd;
	int poll_ret;
	int consecutive_errors = 0;

	if (!ctx) {
		CEC_LOG_ERROR("RX thread started with NULL context");
		return NULL;
	}

	CEC_LOG_INFO("RX thread started, fd=%d", ctx->fd);

	while (true) {
		pthread_mutex_lock(&ctx->mutex);
		int fd = ctx->fd;
		bool running = ctx->running;
		pthread_mutex_unlock(&ctx->mutex);

		if (!running) {
			break;
		}

		if (fd < 0) {
			CEC_LOG_ERROR("Invalid file descriptor: %d", fd);
			usleep(ERROR_RECOVERY_DELAY_MS * 1000);
			consecutive_errors++;
			if (consecutive_errors > MAX_CONSECUTIVE_ERRORS) {
				CEC_LOG_ERROR("Max consecutive errors reached, exiting thread");
				break;
			}
			continue;
		}

		pfd.fd = fd;
		pfd.events = POLLIN | POLLERR | POLLHUP;
		pfd.revents = 0;

		poll_ret = poll(&pfd, 1, RX_POLL_TIMEOUT_MS);

		if (poll_ret < 0) {
			if (errno == EINTR) {
				CEC_LOG_TRACE("poll() interrupted by signal");
				continue;
			}
			if (errno == EBADF) {
				// Device was closed - check if shutdown is in progress
				pthread_mutex_lock(&ctx->mutex);
				bool is_closing = !ctx->running;
				pthread_mutex_unlock(&ctx->mutex);
				if (is_closing) {
					CEC_LOG_INFO("Device closed during poll, exiting thread");
					break;
				}
			}
			CEC_LOG_ERROR("poll() error: %s (errno=%d)", strerror(errno), errno);
			consecutive_errors++;
			if (consecutive_errors > MAX_CONSECUTIVE_ERRORS) {
				CEC_LOG_ERROR("Max consecutive poll errors, exiting thread");
				break;
			}
			usleep(ERROR_RECOVERY_DELAY_MS * 1000);
			continue;
		} else if (poll_ret == 0) {
			consecutive_errors = 0;
			CEC_LOG_TRACE("poll() timeout");
			continue;
		}

		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			CEC_LOG_WARN("poll() error event: revents=0x%x", pfd.revents);
			consecutive_errors++;
			if (consecutive_errors > MAX_CONSECUTIVE_ERRORS) {
				CEC_LOG_ERROR("Max consecutive poll error events, exiting thread");
				break;
			}
			usleep(ERROR_RECOVERY_DELAY_MS * 1000);
			continue;
		}

		if (!(pfd.revents & POLLIN)) {
			CEC_LOG_TRACE("poll() returned but no POLLIN");
			continue;
		}

		memset(&msg, 0, sizeof(msg));
		msg.timeout = CEC_IOCTL_TIMEOUT_MS;

		if (ioctl(fd, CEC_RECEIVE, &msg) < 0) {
			if (errno == ETIMEDOUT || errno == EAGAIN) {
				consecutive_errors = 0;
				CEC_LOG_TRACE("ioctl(CEC_RECEIVE) timeout/again");
				continue;
			}
			if (errno == EINTR) {
				CEC_LOG_TRACE("ioctl(CEC_RECEIVE) interrupted");
				continue;
			}
			if (errno == EBADF) {
				// Device was closed - check if shutdown is in progress
				pthread_mutex_lock(&ctx->mutex);
				bool is_closing = !ctx->running;
				pthread_mutex_unlock(&ctx->mutex);
				if (is_closing) {
					CEC_LOG_INFO("Device closed during receive, exiting thread");
					break;
				}
			}
			CEC_LOG_ERROR("ioctl(CEC_RECEIVE) error: %s (errno=%d)", strerror(errno), errno);
			consecutive_errors++;
			if (consecutive_errors > MAX_CONSECUTIVE_ERRORS) {
				CEC_LOG_ERROR("Max consecutive ioctl errors, exiting thread");
				break;
			}
			usleep(ERROR_RECOVERY_DELAY_MS * 1000);
			continue;
		}

		consecutive_errors = 0;

		if (msg.tx_status == 0) {
			len = msg.len;
			if (len > 0 && len <= CEC_MAX_MSG_SIZE) {
				memcpy(buf, msg.msg, len);

				CEC_LOG_INFO("Received CEC message: len=%d", len);
				CEC_LOG_BUFFER("RX", buf, len);

				// Callback invoked outside mutex to prevent deadlock
				pthread_mutex_lock(&ctx->mutex);
				HdmiCecRxCallback_t rx_callback = ctx->rx_callback;
				void *rx_callback_data = ctx->rx_callback_data;
				int callback_handle = ctx->handle;
				bool should_call = ctx->running && ctx->initialized && rx_callback != NULL;
				pthread_mutex_unlock(&ctx->mutex);

				if (should_call) {
					CEC_LOG_DEBUG("Calling RX callback");
					rx_callback(callback_handle, rx_callback_data, buf, len);
				} else {
					CEC_LOG_WARN("RX callback not called: running/init/callback validation failed");
				}
			} else {
				CEC_LOG_WARN("Invalid message length: %d", len);
			}
		} else {
			CEC_LOG_TRACE("Skipping TX status message: tx_status=0x%x", msg.tx_status);
		}
	}

	CEC_LOG_INFO("RX thread exiting");
	return NULL;
}

HDMI_CEC_STATUS HdmiCecOpen(int* handle)
{
	struct cec_caps caps;
	struct cec_log_addrs log_addrs;
	__u32 mode;
	int ret;

	CEC_LOG_INFO("%s", __func__);

	if (handle == NULL) {
		CEC_LOG_ERROR("Invalid argument: handle is NULL");
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	pthread_mutex_lock(&g_cec_context.mutex);

	if (g_cec_context.initialized) {
		CEC_LOG_WARN("%s already opened", __func__);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_ALREADY_OPEN;
	}

	CEC_LOG_DEBUG("Opening CEC device: %s", CEC_DEVICE_PATH);
	g_cec_context.fd = open(CEC_DEVICE_PATH, O_RDWR | O_NONBLOCK);
	if (g_cec_context.fd < 0) {
		CEC_LOG_ERROR("Failed to open CEC device %s: %s (errno=%d)",
					 CEC_DEVICE_PATH, strerror(errno), errno);

		if (errno == ENOENT) {
			CEC_LOG_ERROR("CEC device not found - CEC not supported on this system");
			CEC_LOG_ERROR("Hint: Incompatible with 'fkms' driver configuration.");
			pthread_mutex_unlock(&g_cec_context.mutex);
			usleep(ERROR_RECOVERY_DELAY_MS * 1000);
			return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;
		}

		pthread_mutex_unlock(&g_cec_context.mutex);
		usleep(ERROR_RECOVERY_DELAY_MS * 1000);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	CEC_LOG_INFO("CEC device opened: %s, fd=%d", CEC_DEVICE_PATH, g_cec_context.fd);

	memset(&caps, 0, sizeof(caps));
	if (ioctl(g_cec_context.fd, CEC_ADAP_G_CAPS, &caps) < 0) {
		CEC_LOG_ERROR("Failed to get CEC capabilities: %s", strerror(errno));
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	CEC_LOG_INFO("CEC capabilities: 0x%x, driver: %s", caps.capabilities, caps.driver);

	if (!(caps.capabilities & (CEC_CAP_LOG_ADDRS | CEC_CAP_TRANSMIT | CEC_CAP_PASSTHROUGH))) {
		CEC_LOG_ERROR("Required CEC capabilities not available: 0x%x", caps.capabilities);
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;
	}

	CEC_LOG_DEBUG("Configuring as PLAYBACK device");
	memset(&log_addrs, 0, sizeof(log_addrs));
	log_addrs.cec_version = CEC_OP_CEC_VERSION_1_4;
	log_addrs.num_log_addrs = 1;
	log_addrs.log_addr_type[0] = CEC_LOG_ADDR_TYPE_PLAYBACK;
	log_addrs.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_PLAYBACK;
	log_addrs.all_device_types[0] = CEC_OP_ALL_DEVTYPE_PLAYBACK;
	log_addrs.flags = CEC_LOG_ADDRS_FL_ALLOW_UNREG_FALLBACK;

	log_addrs.vendor_id = RPI_CEC_VENDOR_ID;

	snprintf(log_addrs.osd_name, sizeof(log_addrs.osd_name), "%s", RPI_CEC_OSD_NAME);

	log_addrs.features[0][0] = 0x00;
	log_addrs.features[0][1] = 0x00;

	CEC_LOG_DEBUG("Setting logical addresses (discovery)");
	if (ioctl(g_cec_context.fd, CEC_ADAP_S_LOG_ADDRS, &log_addrs) < 0) {
		CEC_LOG_ERROR("Failed to set logical addresses: %s", strerror(errno));
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_LOGICALADDRESS_UNAVAILABLE;
	}

	memset(&log_addrs, 0, sizeof(log_addrs));
	if (ioctl(g_cec_context.fd, CEC_ADAP_G_LOG_ADDRS, &log_addrs) == 0) {
		__u16 phys_addr = 0;
		if (ioctl(g_cec_context.fd, CEC_ADAP_G_PHYS_ADDR, &phys_addr) == 0) {
			g_cec_context.physical_address = cec_convert_physical_address(phys_addr);
		} else {
			CEC_LOG_WARN("Failed to get physical address: %s", strerror(errno));
			g_cec_context.physical_address = 0xFFFF;
		}
		if (log_addrs.num_log_addrs > 0) {
			g_cec_context.logical_address = cec_convert_logical_address(log_addrs.log_addr[0]);

			if (g_cec_context.logical_address == CEC_LOG_ADDR_UNREGISTERED || g_cec_context.logical_address > 15) {
				CEC_LOG_ERROR("Invalid logical address: %d (0x%02X) - CEC discovery failed",
							 g_cec_context.logical_address, g_cec_context.logical_address);
				CEC_LOG_ERROR("Possible causes: No CEC-enabled display connected, CEC disabled on TV, or HDMI cable doesn't support CEC");
				g_cec_context.has_logical_address = false;
			} else {
				g_cec_context.has_logical_address = true;
				CEC_LOG_INFO("Logical address assigned: %d, Physical address: 0x%04x",
							g_cec_context.logical_address, g_cec_context.physical_address);
			}
		} else {
			CEC_LOG_WARN("No logical address assigned - CEC will not work");
		}
	} else {
		CEC_LOG_ERROR("Failed to get logical addresses: %s", strerror(errno));
	}

	CEC_LOG_DEBUG("Setting CEC mode: INITIATOR | FOLLOWER");
	mode = CEC_MODE_INITIATOR | CEC_MODE_FOLLOWER;
	if (ioctl(g_cec_context.fd, CEC_S_MODE, &mode) < 0) {
		CEC_LOG_ERROR("Failed to set CEC mode: %s", strerror(errno));
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	g_cec_context.handle = cec_generate_handle();
	g_cec_context.initialized = true;
	g_cec_context.running = true;

	CEC_LOG_DEBUG("Starting RX thread");
	ret = pthread_create(&g_cec_context.rx_thread, NULL, cec_rx_thread, &g_cec_context);
	if (ret != 0) {
		CEC_LOG_ERROR("Failed to create RX thread: %s", strerror(ret));

		memset(&log_addrs, 0, sizeof(log_addrs));
		(void)ioctl(g_cec_context.fd, CEC_ADAP_S_LOG_ADDRS, &log_addrs);

		close(g_cec_context.fd);
		g_cec_context.fd = -1;
		g_cec_context.initialized = false;
		g_cec_context.running = false;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	g_cec_context.thread_created = true;

	*handle = g_cec_context.handle;
	pthread_mutex_unlock(&g_cec_context.mutex);

	CEC_LOG_INFO("HdmiCecOpen successful: handle=%d", *handle);
	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecClose(int handle)
{
	CEC_LOG_INFO("HdmiCecClose called: handle=%d", handle);

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

	CEC_LOG_DEBUG("Stopping RX thread");
	g_cec_context.running = false;
	pthread_t thread_to_join = g_cec_context.rx_thread;
	bool thread_was_created = g_cec_context.thread_created;
	pthread_mutex_unlock(&g_cec_context.mutex);

	if (thread_was_created && thread_to_join) {
		struct timespec ts;
		int join_result;

#ifdef __linux__
		if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
			ts.tv_sec += THREAD_JOIN_TIMEOUT_SEC;
			join_result = pthread_timedjoin_np(thread_to_join, NULL, &ts);
			if (join_result == 0) {
				CEC_LOG_DEBUG("Thread joined successfully");
			} else if (join_result == ETIMEDOUT) {
				CEC_LOG_ERROR("Thread join timeout - thread may leak resources");
			} else if (join_result != ESRCH) {
				CEC_LOG_WARN("Thread join failed: %d, using regular join", join_result);
				pthread_join(thread_to_join, NULL);
			}
		} else {
			CEC_LOG_WARN("clock_gettime failed, using regular join");
			pthread_join(thread_to_join, NULL);
		}
#else
		CEC_LOG_DEBUG("Using portable pthread_join (no timeout)");
		pthread_join(thread_to_join, NULL);
#endif
	}

	pthread_mutex_lock(&g_cec_context.mutex);

	if (g_cec_context.fd >= 0) {
		struct cec_log_addrs log_addrs;
		memset(&log_addrs, 0, sizeof(log_addrs));
		(void)ioctl(g_cec_context.fd, CEC_ADAP_S_LOG_ADDRS, &log_addrs);

		CEC_LOG_DEBUG("Closing CEC device fd=%d", g_cec_context.fd);
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
	}

	g_cec_context.initialized = false;
	g_cec_context.handle = 0;
	g_cec_context.rx_thread = 0;
	g_cec_context.thread_created = false;
	g_cec_context.rx_callback = NULL;
	g_cec_context.rx_callback_data = NULL;
	g_cec_context.tx_callback = NULL;
	g_cec_context.tx_callback_data = NULL;
	g_cec_context.tx_callback_gen = 0;
	g_cec_context.logical_address = RPI_CEC_UNREGISTERED_ADDR;
	g_cec_context.has_logical_address = false;
	g_cec_context.physical_address = 0xFFFF;

	pthread_mutex_unlock(&g_cec_context.mutex);

	CEC_LOG_INFO("HdmiCecClose successful");
	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecGetPhysicalAddress(int handle, unsigned int* physicalAddress)
{
	__u16 phys_addr;

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

	if (ioctl(g_cec_context.fd, CEC_ADAP_G_PHYS_ADDR, &phys_addr) < 0) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	g_cec_context.physical_address = cec_convert_physical_address(phys_addr);

	if (g_cec_context.physical_address == 0xFFFF) {
		CEC_LOG_WARN("Physical address is 0xFFFF (invalid/not connected)");
	}

	*physicalAddress = g_cec_context.physical_address;
	pthread_mutex_unlock(&g_cec_context.mutex);

	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecAddLogicalAddress(int handle, int logicalAddresses)
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

	if (logicalAddresses < 0 || logicalAddresses > 15) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	pthread_mutex_unlock(&g_cec_context.mutex);

	CEC_LOG_INFO("HdmiCecAddLogicalAddress not supported for source devices (Raspberry Pi 4)");
	return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;

}

HDMI_CEC_STATUS HdmiCecRemoveLogicalAddress(int handle, int logicalAddresses)
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

	if (logicalAddresses < 0 || logicalAddresses > 15) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	if (!g_cec_context.has_logical_address) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_ADDED;
	}

	pthread_mutex_unlock(&g_cec_context.mutex);

	CEC_LOG_INFO("HdmiCecRemoveLogicalAddress not supported for source devices (Raspberry Pi 4)");
	return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;
}

HDMI_CEC_STATUS HdmiCecGetLogicalAddress(int handle, int* logicalAddress)
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

HDMI_CEC_STATUS HdmiCecSetRxCallback(int handle, HdmiCecRxCallback_t callback, void* data)
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

	g_cec_context.rx_callback = callback;
	g_cec_context.rx_callback_data = data;

	pthread_mutex_unlock(&g_cec_context.mutex);
	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecSetTxCallback(int handle, HdmiCecTxCallback_t callback, void* data)
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

	g_cec_context.tx_callback = callback;
	g_cec_context.tx_callback_data = data;
	g_cec_context.tx_callback_gen++;

	pthread_mutex_unlock(&g_cec_context.mutex);
	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecTx(int handle, const unsigned char* buf, int len, int* result)
{
	struct cec_msg msg;
	int ret;

	CEC_LOG_DEBUG("HdmiCecTx: handle=%d, len=%d", handle, len);

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

	if (buf == NULL || len <= 0 || len > CEC_MAX_MSG_SIZE || result == NULL) {
		CEC_LOG_ERROR("Invalid arguments: buf=%p, len=%d, result=%p", (void*)buf, len, (void*)result);
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	// Validate file descriptor
	if (g_cec_context.fd < 0) {
		CEC_LOG_ERROR("Invalid file descriptor: %d", g_cec_context.fd);
		pthread_mutex_unlock(&g_cec_context.mutex);
		*result = HDMI_CEC_IO_SENT_FAILED;
		return HDMI_CEC_IO_NOT_OPENED;
	}

	CEC_LOG_BUFFER("TX", buf, len);

	memset(&msg, 0, sizeof(msg));
	msg.len = len;
	memcpy(msg.msg, buf, len);
	msg.timeout = CEC_IOCTL_TIMEOUT_MS;

	int cec_fd = g_cec_context.fd;
	pthread_mutex_unlock(&g_cec_context.mutex);

	ret = ioctl(cec_fd, CEC_TRANSMIT, &msg);

	pthread_mutex_lock(&g_cec_context.mutex);
	if (!g_cec_context.initialized || cec_fd != g_cec_context.fd) {
		CEC_LOG_ERROR("CEC context changed during transmission");
		pthread_mutex_unlock(&g_cec_context.mutex);
		*result = HDMI_CEC_IO_SENT_FAILED;
		return HDMI_CEC_IO_SENT_FAILED;
	}

	if (ret < 0) {
		CEC_LOG_ERROR("ioctl(CEC_TRANSMIT) failed: %s", strerror(errno));
		pthread_mutex_unlock(&g_cec_context.mutex);
		*result = HDMI_CEC_IO_SENT_FAILED;
		return HDMI_CEC_IO_SENT_FAILED;
	}

	if (msg.tx_status & CEC_TX_STATUS_OK) {
		CEC_LOG_INFO("TX successful: ACK received");
		*result = HDMI_CEC_IO_SENT_AND_ACKD;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_SUCCESS;
	} else if (msg.tx_status & CEC_TX_STATUS_NACK) {
		CEC_LOG_WARN("TX: NACK received");
		*result = HDMI_CEC_IO_SENT_BUT_NOT_ACKD;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_SUCCESS;
	} else {
		CEC_LOG_ERROR("TX failed: tx_status=0x%x", msg.tx_status);
		*result = HDMI_CEC_IO_SENT_FAILED;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_SENT_FAILED;
	}
}

HDMI_CEC_STATUS HdmiCecTxAsync(int handle, const unsigned char* buf, int len)
{
	struct cec_msg msg;
	int ret;
	HdmiCecTxCallback_t callback;
	void *callback_data;

	pthread_mutex_lock(&g_cec_context.mutex);

	if (!g_cec_context.initialized) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	if (handle == 0 || handle != g_cec_context.handle) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_HANDLE;
	}

	if (buf == NULL || len <= 0 || len > CEC_MAX_MSG_SIZE) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	// Validate file descriptor
	if (g_cec_context.fd < 0) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_NOT_OPENED;
	}

	memset(&msg, 0, sizeof(msg));
	msg.len = len;
	memcpy(msg.msg, buf, len);
	msg.timeout = CEC_IOCTL_TIMEOUT_MS;

	callback = g_cec_context.tx_callback;
	callback_data = g_cec_context.tx_callback_data;
	unsigned int callback_gen = g_cec_context.tx_callback_gen;
	int saved_handle = handle;
	int saved_fd = g_cec_context.fd;

	pthread_mutex_unlock(&g_cec_context.mutex);

	ret = ioctl(saved_fd, CEC_TRANSMIT, &msg);

	if (ret < 0) {
		CEC_LOG_ERROR("ioctl(CEC_TRANSMIT) failed: %s", strerror(errno));
		usleep(ERROR_RECOVERY_DELAY_MS * 1000);
		return HDMI_CEC_IO_SENT_FAILED;
	}

	pthread_mutex_lock(&g_cec_context.mutex);
	bool callback_valid = (callback != NULL &&
	                       g_cec_context.tx_callback_gen == callback_gen &&
	                       g_cec_context.fd == saved_fd &&
	                       g_cec_context.initialized);
	pthread_mutex_unlock(&g_cec_context.mutex);

	if (callback_valid) {
		int tx_result;
		if (msg.tx_status & CEC_TX_STATUS_OK) {
			tx_result = HDMI_CEC_IO_SENT_AND_ACKD;
		} else if (msg.tx_status & CEC_TX_STATUS_NACK) {
			tx_result = HDMI_CEC_IO_SENT_BUT_NOT_ACKD;
		} else {
			tx_result = HDMI_CEC_IO_SENT_FAILED;
		}
		callback(saved_handle, callback_data, tx_result);
	}

	return HDMI_CEC_IO_SUCCESS;
}

static void __attribute__((constructor)) cec_driver_init(void)
{
	cec_log_init();
	CEC_LOG_INFO("RPi4 CEC HAL - Version: %s, Git SHA: %s", HAL_VERSION, GIT_COMMIT_SHA);
}

static void __attribute__((destructor)) cec_driver_fini(void)
{
	CEC_LOG_INFO("RPi4 CEC HAL driver cleanup");
	pthread_mutex_lock(&g_cec_context.mutex);
	if (g_cec_context.initialized) {
		CEC_LOG_WARN("CEC device still open during shutdown - improper cleanup");
		CEC_LOG_WARN("HdmiCecClose must be called before library unload");
		g_cec_context.running = false;
		bool thread_was_created = g_cec_context.thread_created;
		pthread_t thread_to_join = g_cec_context.rx_thread;
		int fd_to_close = g_cec_context.fd;
		pthread_mutex_unlock(&g_cec_context.mutex);

		if (thread_was_created) {
			struct timespec timeout_ts;
#ifdef __linux__
			if (clock_gettime(CLOCK_REALTIME, &timeout_ts) == 0) {
				timeout_ts.tv_sec += 2;
				if (pthread_timedjoin_np(thread_to_join, NULL, &timeout_ts) != 0) {
					CEC_LOG_WARN("Thread did not exit gracefully, may leak resources");
				}
			} else {
				pthread_join(thread_to_join, NULL);
			}
#else
			pthread_join(thread_to_join, NULL);
#endif
		}

		if (fd_to_close >= 0) {
			struct cec_log_addrs log_addrs;
			memset(&log_addrs, 0, sizeof(log_addrs));
			(void)ioctl(fd_to_close, CEC_ADAP_S_LOG_ADDRS, &log_addrs);
			close(fd_to_close);
		}

		pthread_mutex_lock(&g_cec_context.mutex);
		g_cec_context.initialized = false;
		g_cec_context.fd = -1;
		g_cec_context.rx_thread = 0;
		g_cec_context.thread_created = false;
		pthread_mutex_unlock(&g_cec_context.mutex);
	} else {
		pthread_mutex_unlock(&g_cec_context.mutex);
	}

	cec_log_close();
}

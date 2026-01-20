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

#define CEC_DEVICE_PATH "/dev/cec0"
#define CEC_MAX_MSG_SIZE 16
#define INVALID_HANDLE -1
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

/**
 * @brief CEC driver context structure
 *
 * This structure maintains the runtime state of the HDMI CEC HAL driver.
 * All fields are protected by the mutex except where noted.
 *
 * Threading: The rx_thread runs continuously when initialized is true and
 * running is true. The mutex must be held when accessing most fields except
 * during carefully controlled thread shutdown sequences.
 *
 * Lifetime: Initialized in cec_driver_init() constructor, used during
 * HdmiCecOpen/Close operations, cleaned up in cec_driver_fini() destructor.
 */
typedef struct {
	int fd;                           /**< File descriptor for /dev/cec0, -1 when closed */
	int handle;                       /**< HAL handle returned to caller, 0 when invalid */
	bool initialized;                 /**< True when HdmiCecOpen has succeeded */
	bool running;                     /**< True when RX thread should be running */
	pthread_t rx_thread;              /**< RX thread handle, 0 when not created */
	pthread_mutex_t mutex;            /**< Protects all context fields */
	HdmiCecRxCallback_t rx_callback;  /**< Registered RX callback function */
	void *rx_callback_data;           /**< User data for RX callback */
	HdmiCecTxCallback_t tx_callback;  /**< Registered TX callback function */
	void *tx_callback_data;           /**< User data for TX callback */
	unsigned int tx_callback_gen;     /**< TX callback generation counter for race prevention */
	int logical_address;              /**< Current CEC logical address (0-15, 15=unregistered) */
	unsigned int physical_address;    /**< Current CEC physical address (0x0000-0xFFFF) */
	bool has_logical_address;         /**< True when valid logical address assigned */
} cec_context_t;

static FILE *g_log_file = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Default log level
static int g_log_level = CEC_LOG_LEVEL_ERROR;

// Current timestamp
static void cec_get_timestamp(char *buffer, size_t size)
{
	struct timespec ts;
	struct tm tm_info;
	uint32_t msec;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		// Fallback to epoch if clock_gettime fails
		snprintf(buffer, size, CEC_TIMESTAMP_FALLBACK);
		return;
	}

	if (localtime_r(&ts.tv_sec, &tm_info) == NULL) {
		// Fallback if localtime_r fails
		snprintf(buffer, size, CEC_TIMESTAMP_FALLBACK);
		return;
	}

	// Convert nanoseconds to milliseconds with proper bounds checking
	msec = (uint32_t)((ts.tv_nsec / 1000000) % 1000);

	snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03u",
				tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
				tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
				msec);
}

// Get log level string
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

// Initialize logging
static void cec_log_init(void)
{
	pthread_mutex_lock(&g_log_mutex);

	if (g_log_file == NULL) {
		// Check if logging is explicitly requested via environment variables
		const char *log_level_env = getenv("CEC_HAL_LOG_LEVEL");
		const char *log_file_env = getenv("CEC_HAL_LOG_FILE");

		// Skip logging if no log level defined and log file not explicitly set
		if (log_level_env == NULL && log_file_env == NULL) {
			// No logging requested - keep g_log_file as NULL and return.
			pthread_mutex_unlock(&g_log_mutex);
			return;
		}

		// Read log level from environment variable
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

		// Read log file path from environment variable (default to CEC_LOG_FILE_DEFAULT)
		const char *log_file_path = log_file_env ? log_file_env : CEC_LOG_FILE_DEFAULT;

		// Create directory if it doesn't exist (ignore error if already exists)
		struct stat st;
		if (stat(CEC_LOG_DIR, &st) != 0) {
			(void)mkdir(CEC_LOG_DIR, 0755);
		}

		g_log_file = fopen(log_file_path, "a");
		if (g_log_file != NULL) {
			if (fseek(g_log_file, 0, SEEK_END) == 0) {
				long size = ftell(g_log_file);

				// Rotate log if exceeds max size and ftell succeeded
				if (size > 0 && size > CEC_LOG_MAX_SIZE) {
					fclose(g_log_file);
					g_log_file = NULL;
					char old_log[CEC_LOG_PATH_MAX];
					snprintf(old_log, sizeof(old_log), "%s%s", log_file_path, CEC_LOG_FILE_SUFFIX);
					(void)rename(log_file_path, old_log);
					g_log_file = fopen(log_file_path, "a");
				}
			}

			// Disable buffering for immediate flush
			if (g_log_file != NULL) {
				setvbuf(g_log_file, NULL, _IOLBF, 0);

				char timestamp[CEC_TIMESTAMP_SIZE];
				cec_get_timestamp(timestamp, sizeof(timestamp));
				fprintf(g_log_file, "\nCEC HAL Log Started: %s (Level: %s)\n",
						timestamp, cec_get_log_level_str(g_log_level));
			}
		}
	}

	pthread_mutex_unlock(&g_log_mutex);
}

// Close logging
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

// Main logging function
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

// Log buffer contents in hex
static void cec_log_buffer(const char *prefix, const unsigned char *buf, int len)
{
	if (CEC_LOG_LEVEL_DEBUG > g_log_level || buf == NULL || len <= 0) {
		return;
	}

	// Check if logging is enabled under mutex to avoid TOCTOU race
	pthread_mutex_lock(&g_log_mutex);

	if (g_log_file != NULL) {
		char timestamp[CEC_TIMESTAMP_SIZE];
		cec_get_timestamp(timestamp, sizeof(timestamp));

		fprintf(g_log_file, "[%s] [DEBUG] %s (%d bytes): ", timestamp, prefix, len);
		for (int i = 0; i < len && i < CEC_MAX_MSG_SIZE; i++) {
			fprintf(g_log_file, "%02X ", buf[i]);
		}
		fprintf(g_log_file, "\n");
		fflush(g_log_file);
	}

	pthread_mutex_unlock(&g_log_mutex);
}

// Convert Linux CEC logical address to HAL format
static inline int cec_convert_logical_address(__u8 cec_addr)
{
	// Currently no conversion needed - Linux CEC uses same format as HAL
	// Valid range: 0x0-0xF, with 0xF being unregistered
	return (int)cec_addr;
}

// Convert Linux CEC physical address to HAL format
static inline unsigned int cec_convert_physical_address(__u16 cec_phys_addr)
{
	// Currently no byte swapping needed - Linux CEC returns in host byte order
	// Physical address format: F.F.F.F (4 nibbles), range 0x0000-0xFFFF
	return (unsigned int)cec_phys_addr;
}

// Generate a secure random handle value (non-zero)
static int cec_generate_handle(void)
{
	int handle = 0;
	int urandom_fd;

	// Try to read from /dev/urandom for cryptographically secure random number
	urandom_fd = open("/dev/urandom", O_RDONLY);
	if (urandom_fd >= 0) {
		if (read(urandom_fd, &handle, sizeof(handle)) == sizeof(handle)) {
			close(urandom_fd);
			// Ensure handle is positive and non-zero
			handle = (handle & 0x7FFFFFFF);
			if (handle == 0) {
				handle = 1;
			}
			return handle;
		}
		close(urandom_fd);
	}

	// Fallback: use time-based randomization if /dev/urandom fails
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		// Combine seconds, nanoseconds, and process ID for uniqueness
		handle = (int)((ts.tv_sec ^ ts.tv_nsec ^ getpid()) & 0x7FFFFFFF);
		if (handle == 0) {
			handle = (int)(ts.tv_nsec & 0x7FFFFFFF);
		}
	}

	// Last resort: use current time
	if (handle == 0) {
		handle = (int)(time(NULL) & 0x7FFFFFFF);
		if (handle == 0) {
			handle = 1; // Absolute fallback
		}
	}

	return handle;
}

// Global CEC context - mutex initialized statically for thread safety
static cec_context_t g_cec_context = {
	.fd = -1,
	.handle = 0,
	.initialized = false,
	.running = false,
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

	while (ctx->running) {
		// Read fd under mutex protection to avoid race with HdmiCecClose
		pthread_mutex_lock(&ctx->mutex);
		int fd = ctx->fd;
		bool running = ctx->running;
		pthread_mutex_unlock(&ctx->mutex);

		if (!running) {
			break;
		}

		// Validate file descriptor
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

		// Use poll to avoid busy-waiting and detect errors
		pfd.fd = fd;
		pfd.events = POLLIN | POLLERR | POLLHUP;
		pfd.revents = 0;

		poll_ret = poll(&pfd, 1, RX_POLL_TIMEOUT_MS);

		if (poll_ret < 0) {
			if (errno == EINTR) {
				CEC_LOG_TRACE("poll() interrupted by signal");
				continue;
			}
			// Poll error - increment error counter
			CEC_LOG_ERROR("poll() error: %s (errno=%d)", strerror(errno), errno);
			consecutive_errors++;
			if (consecutive_errors > MAX_CONSECUTIVE_ERRORS) {
				CEC_LOG_ERROR("Max consecutive poll errors, exiting thread");
				break;
			}
			usleep(ERROR_RECOVERY_DELAY_MS * 1000);
			continue;
		} else if (poll_ret == 0) {
			// Timeout - normal, reset error counter
			consecutive_errors = 0;
			CEC_LOG_TRACE("poll() timeout");
			continue;
		}

		// Check for error conditions
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

		// Data available to read
		if (!(pfd.revents & POLLIN)) {
			CEC_LOG_TRACE("poll() returned but no POLLIN");
			continue;
		}

		memset(&msg, 0, sizeof(msg));
		msg.timeout = CEC_IOCTL_TIMEOUT_MS;

		// Receive CEC message
		if (ioctl(fd, CEC_RECEIVE, &msg) < 0) {
			if (errno == ETIMEDOUT || errno == EAGAIN) {
				consecutive_errors = 0; // Not a critical error
				CEC_LOG_TRACE("ioctl(CEC_RECEIVE) timeout/again");
				continue;
			}
			if (errno == EINTR) {
				CEC_LOG_TRACE("ioctl(CEC_RECEIVE) interrupted");
				continue;
			}
			// Critical error
			CEC_LOG_ERROR("ioctl(CEC_RECEIVE) error: %s (errno=%d)", strerror(errno), errno);
			consecutive_errors++;
			if (consecutive_errors > MAX_CONSECUTIVE_ERRORS) {
				CEC_LOG_ERROR("Max consecutive ioctl errors, exiting thread");
				break;
			}
			usleep(ERROR_RECOVERY_DELAY_MS * 1000);
			continue;
		}

		// Reset error counter on successful receive
		consecutive_errors = 0;

		// Only process received messages (not transmit status reports)
		// Received messages have tx_status == 0; TX status reports have flags set
		if (!(msg.tx_status & CEC_TX_STATUS_OK) &&
			!(msg.tx_status & CEC_TX_STATUS_NACK) &&
			!(msg.tx_status & CEC_TX_STATUS_ERROR)) {

			// Convert CEC message to HAL format
			len = msg.len;
			if (len > 0 && len <= CEC_MAX_MSG_SIZE) {
				memcpy(buf, msg.msg, len);

				CEC_LOG_INFO("Received CEC message: len=%d", len);
				CEC_LOG_BUFFER("RX", buf, len);

				// Call receive callback if registered - with validation
				pthread_mutex_lock(&ctx->mutex);
				if (ctx->running && ctx->initialized && ctx->rx_callback) {
					CEC_LOG_DEBUG("Calling RX callback");
					ctx->rx_callback(ctx->handle, ctx->rx_callback_data, buf, len);
				} else {
					CEC_LOG_WARN("RX callback not called: running=%d, init=%d, callback=%p",
								ctx->running, ctx->initialized, (void*)ctx->rx_callback);
				}
				pthread_mutex_unlock(&ctx->mutex);
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

	// Open CEC device (HDMI0)
	CEC_LOG_DEBUG("Opening CEC device: %s", CEC_DEVICE_PATH);
	g_cec_context.fd = open(CEC_DEVICE_PATH, O_RDWR | O_NONBLOCK);
	if (g_cec_context.fd < 0) {
		CEC_LOG_ERROR("Failed to open CEC device %s: %s (errno=%d)",
					 CEC_DEVICE_PATH, strerror(errno), errno);

		// Check if device doesn't exist (ENOENT) - return NOT_SUPPORTED
		// This happens when kernel doesn't have CEC support or wrong display driver
		if (errno == ENOENT) {
			CEC_LOG_ERROR("CEC device not found - CEC not supported on this system");
			CEC_LOG_ERROR("Hint: Check if dtoverlay=vc4-kms-v3d is enabled in /boot/config.txt");
			pthread_mutex_unlock(&g_cec_context.mutex);
			return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;
		}

		// Other errors (permissions, device busy, etc.)
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	CEC_LOG_INFO("CEC device opened: %s, fd=%d", CEC_DEVICE_PATH, g_cec_context.fd);

	// Get capabilities
	memset(&caps, 0, sizeof(caps));
	if (ioctl(g_cec_context.fd, CEC_ADAP_G_CAPS, &caps) < 0) {
		CEC_LOG_ERROR("Failed to get CEC capabilities: %s", strerror(errno));
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}
	CEC_LOG_INFO("CEC capabilities: 0x%x, driver: %s", caps.capabilities, caps.driver);

	// Validate capabilities
	if (!(caps.capabilities & (CEC_CAP_LOG_ADDRS | CEC_CAP_TRANSMIT | CEC_CAP_PASSTHROUGH))) {
		CEC_LOG_ERROR("Required CEC capabilities not available: 0x%x", caps.capabilities);
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;
	}

	// Configure as PLAYBACK DEVICE (source device) for Raspberry Pi 4
	CEC_LOG_DEBUG("Configuring as PLAYBACK device");
	memset(&log_addrs, 0, sizeof(log_addrs));
	log_addrs.cec_version = CEC_OP_CEC_VERSION_1_4;
	log_addrs.num_log_addrs = 1;
	log_addrs.log_addr_type[0] = CEC_LOG_ADDR_TYPE_PLAYBACK;
	log_addrs.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_PLAYBACK;
	log_addrs.all_device_types[0] = CEC_OP_ALL_DEVTYPE_PLAYBACK;
	log_addrs.flags = CEC_LOG_ADDRS_FL_ALLOW_UNREG_FALLBACK;

	// Set vendor ID (Raspberry Pi Foundation)
	log_addrs.vendor_id = RPI_CEC_VENDOR_ID;

	// Configure OSD name (snprintf handles both copying and null termination)
	snprintf(log_addrs.osd_name, sizeof(log_addrs.osd_name), "%s", RPI_CEC_OSD_NAME);

	// Configure CEC features for playback device
	// RC Profile: Source has deck control
	log_addrs.features[0][0] = 0x00; // RC Profile Source
	log_addrs.features[0][1] = 0x00; // Device Features

	CEC_LOG_DEBUG("Setting logical addresses (discovery)");
	// Set logical addresses and perform discovery
	if (ioctl(g_cec_context.fd, CEC_ADAP_S_LOG_ADDRS, &log_addrs) < 0) {
		CEC_LOG_ERROR("Failed to set logical addresses: %s", strerror(errno));
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_LOGICALADDRESS_UNAVAILABLE;
	}

	// Get the assigned logical address and physical address
	memset(&log_addrs, 0, sizeof(log_addrs));
	if (ioctl(g_cec_context.fd, CEC_ADAP_G_LOG_ADDRS, &log_addrs) == 0) {
		// Get physical address using separate ioctl (older CEC API compatibility)
		__u16 phys_addr = 0;
		if (ioctl(g_cec_context.fd, CEC_ADAP_G_PHYS_ADDR, &phys_addr) == 0) {
			g_cec_context.physical_address = cec_convert_physical_address(phys_addr);
		} else {
			CEC_LOG_WARN("Failed to get physical address: %s", strerror(errno));
			g_cec_context.physical_address = 0xFFFF;
		}
		if (log_addrs.num_log_addrs > 0) {
			g_cec_context.logical_address = cec_convert_logical_address(log_addrs.log_addr[0]);

			// Check if logical address is valid (0-14) or unregistered (15/255)
			if (g_cec_context.logical_address == CEC_LOG_ADDR_UNREGISTERED ||
			    g_cec_context.logical_address > 15) {
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

	// Set mode to allow receive and transmit
	CEC_LOG_DEBUG("Setting CEC mode: INITIATOR | FOLLOWER");
	mode = CEC_MODE_INITIATOR | CEC_MODE_FOLLOWER;
	if (ioctl(g_cec_context.fd, CEC_S_MODE, &mode) < 0) {
		CEC_LOG_ERROR("Failed to set CEC mode: %s", strerror(errno));
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	// Initialize context with dynamically generated handle for security
	g_cec_context.handle = cec_generate_handle();
	g_cec_context.initialized = true;
	g_cec_context.running = true;
	// logical_address and has_logical_address already set above

	CEC_LOG_DEBUG("Starting RX thread");
	// Start receive thread
	ret = pthread_create(&g_cec_context.rx_thread, NULL, cec_rx_thread, &g_cec_context);
	if (ret != 0) {
		CEC_LOG_ERROR("Failed to create RX thread: %s", strerror(ret));
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
		g_cec_context.initialized = false;
		g_cec_context.running = false;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

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

	// Stop receive thread
	CEC_LOG_DEBUG("Stopping RX thread");
	g_cec_context.running = false;
	pthread_t thread_to_join = g_cec_context.rx_thread;
	pthread_mutex_unlock(&g_cec_context.mutex);

	// Wait for thread to finish with timeout (Note: pthread_timedjoin_np is GNU/Linux specific)
	if (thread_to_join) {
		struct timespec ts;
		int join_result;

		if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
			ts.tv_sec += THREAD_JOIN_TIMEOUT_SEC;
			join_result = pthread_timedjoin_np(thread_to_join, NULL, &ts);
			if (join_result == 0) {
				CEC_LOG_DEBUG("Thread joined successfully");
			} else if (join_result == ETIMEDOUT) {
				CEC_LOG_ERROR("Thread join timeout - thread may leak resources");
				// Do NOT use pthread_cancel - it's unsafe and can cause deadlocks
				// Thread will be abandoned; OS will clean up at process exit
			} else if (join_result != ESRCH) {
				CEC_LOG_WARN("Thread join failed: %d, using regular join", join_result);
				// Fall back to regular join
				pthread_join(thread_to_join, NULL);
			}
		} else {
			CEC_LOG_WARN("clock_gettime failed, using regular join");
			// clock_gettime failed, use regular join
			pthread_join(thread_to_join, NULL);
		}
	}

	pthread_mutex_lock(&g_cec_context.mutex);

	// Close CEC device
	if (g_cec_context.fd >= 0) {
		CEC_LOG_DEBUG("Closing CEC device fd=%d", g_cec_context.fd);
		close(g_cec_context.fd);
		g_cec_context.fd = -1;
	}

	// Reset context
	g_cec_context.initialized = false;
	g_cec_context.handle = 0;
	g_cec_context.rx_thread = 0;
	g_cec_context.rx_callback = NULL;
	g_cec_context.rx_callback_data = NULL;
	g_cec_context.tx_callback = NULL;
	g_cec_context.tx_callback_data = NULL;
	g_cec_context.logical_address = RPI_CEC_UNREGISTERED_ADDR;
	g_cec_context.has_logical_address = false;

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

	// Get current physical address from driver (older CEC API compatibility)
	if (ioctl(g_cec_context.fd, CEC_ADAP_G_PHYS_ADDR, &phys_addr) < 0) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	g_cec_context.physical_address = cec_convert_physical_address(phys_addr);

	// Warn if physical address is invalid (0xFFFF indicates not connected)
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

	// This API is only supported for sink devices.
	// Source devices get their logical address automatically during HdmiCecOpen()
	// Per HAL test suite requirements, source devices must return OPERATION_NOT_SUPPORTED.
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

	// This API is only supported for sink devices.
	// Source devices get their logical address automatically during HdmiCecOpen()
	// and cannot manually remove it.
	// Per HAL test suite requirements, source devices must return OPERATION_NOT_SUPPORTED.
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

HDMI_CEC_STATUS HdmiCecSetRxCallback(int handle, HdmiCecRxCallback_t cbfunc, void* data)
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

	g_cec_context.rx_callback = cbfunc;
	g_cec_context.rx_callback_data = data;

	pthread_mutex_unlock(&g_cec_context.mutex);
	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecSetTxCallback(int handle, HdmiCecTxCallback_t cbfunc, void* data)
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

	g_cec_context.tx_callback = cbfunc;
	g_cec_context.tx_callback_data = data;
	g_cec_context.tx_callback_gen++; // Invalidate any in-flight async callbacks

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

	// Prepare CEC message
	memset(&msg, 0, sizeof(msg));
	msg.len = len;
	memcpy(msg.msg, buf, len);
	msg.timeout = CEC_IOCTL_TIMEOUT_MS;

	// Save fd before releasing mutex
	int cec_fd = g_cec_context.fd;
	pthread_mutex_unlock(&g_cec_context.mutex);

	// Send message synchronously (release mutex to avoid blocking other threads)
	ret = ioctl(cec_fd, CEC_TRANSMIT, &msg);

	// Reacquire mutex and revalidate state
	pthread_mutex_lock(&g_cec_context.mutex);
	if (!g_cec_context.initialized || cec_fd != g_cec_context.fd) {
		CEC_LOG_ERROR("CEC context changed during transmission");
		pthread_mutex_unlock(&g_cec_context.mutex);
		*result = HDMI_CEC_IO_SENT_FAILED;
		return HDMI_CEC_IO_SENT_FAILED;
	}

	// Note: If fd was closed during ioctl, ret will be negative (EBADF)
	if (ret < 0) {
		CEC_LOG_ERROR("ioctl(CEC_TRANSMIT) failed: %s", strerror(errno));
		pthread_mutex_unlock(&g_cec_context.mutex);
		*result = HDMI_CEC_IO_SENT_FAILED;
		return HDMI_CEC_IO_SENT_FAILED;
	}

	// Check transmit status
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

	// Prepare CEC message
	memset(&msg, 0, sizeof(msg));
	msg.len = len;
	memcpy(msg.msg, buf, len);
	msg.timeout = CEC_IOCTL_TIMEOUT_MS;

	// Save callback references and generation counter to detect changes
	callback = g_cec_context.tx_callback;
	callback_data = g_cec_context.tx_callback_data;
	unsigned int callback_gen = g_cec_context.tx_callback_gen;

	// Send message (non-blocking mode already set on fd)
	ret = ioctl(g_cec_context.fd, CEC_TRANSMIT, &msg);

	pthread_mutex_unlock(&g_cec_context.mutex);

	if (ret < 0) {
		return HDMI_CEC_IO_SENT_FAILED;
	}

	// Call tx callback if registered and not changed during transmission
	pthread_mutex_lock(&g_cec_context.mutex);
	bool callback_valid = (callback != NULL && g_cec_context.tx_callback_gen == callback_gen);
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
		callback(handle, callback_data, tx_result);
	}

	return HDMI_CEC_IO_SUCCESS;
}

// Initialize logging at startup (mutex already initialized statically)
static void __attribute__((constructor)) cec_driver_init(void)
{
	cec_log_init();
	CEC_LOG_INFO("RPi4 CEC HAL - Version: %s, Git SHA: %s", HAL_VERSION, GIT_COMMIT_SHA);
}

// Cleanup at shutdown - ensure resources are released even if HdmiCecClose not called
// Note: This destructor assumes single-threaded cleanup or that no other threads
// are actively using CEC APIs when the library is unloaded.
static void __attribute__((destructor)) cec_driver_fini(void)
{
	CEC_LOG_INFO("RPi4 CEC HAL driver cleanup");

	// Check if CEC device is still open and clean up
	pthread_mutex_lock(&g_cec_context.mutex);
	if (g_cec_context.initialized) {
		CEC_LOG_WARN("CEC device still open during shutdown, forcing cleanup");
		g_cec_context.running = false;
		pthread_t thread_to_join = g_cec_context.rx_thread;
		int fd_to_close = g_cec_context.fd;
		pthread_mutex_unlock(&g_cec_context.mutex);

		// Wait for RX thread to exit gracefully (do NOT use pthread_cancel)
		// The thread will exit when it sees running = false
		if (thread_to_join) {
			struct timespec timeout_ts;
			if (clock_gettime(CLOCK_REALTIME, &timeout_ts) == 0) {
				timeout_ts.tv_sec += 2; // 2 second timeout
				// Use timed join with short timeout; if it fails, thread will be abandoned
				if (pthread_timedjoin_np(thread_to_join, NULL, &timeout_ts) != 0) {
					CEC_LOG_WARN("Thread did not exit gracefully, may leak resources");
					// Thread will be abandoned - this is safer than pthread_cancel
				}
			} else {
				// Fallback to regular join with no timeout
				pthread_join(thread_to_join, NULL);
			}
		}

		// Close device file descriptor
		if (fd_to_close >= 0) {
			close(fd_to_close);
		}

		pthread_mutex_lock(&g_cec_context.mutex);
		g_cec_context.initialized = false;
		g_cec_context.fd = -1;
		g_cec_context.rx_thread = 0;
		pthread_mutex_unlock(&g_cec_context.mutex);
	} else {
		pthread_mutex_unlock(&g_cec_context.mutex);
	}

	// Close logging
	cec_log_close();

	// Note: We don't destroy statically initialized mutexes here.
	// At process exit, the OS will clean up all resources.
	// Destroying mutexes could cause issues if other code is still using them.
}

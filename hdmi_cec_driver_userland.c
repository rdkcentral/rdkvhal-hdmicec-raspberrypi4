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

/*
 * Raspberry Pi Userland-based CEC HAL Implementation
 *
 * This implementation:
 * - Uses the Raspberry Pi Userland VCHI interface to communicate with the VideoCore firmware for CEC operations.
 * - Supports basic CEC operations for source devices (e.g., playback devices).
 */

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <stdarg.h>

// Userland includes
#include "bcm_host.h"
#include "interface/vchi/vchi.h"
#include "interface/vmcs_host/vc_cec.h"
#include "interface/vmcs_host/vc_cecservice.h"

#include "hdmi_cec_driver.h"

#ifndef HAL_VERSION
#define HAL_VERSION "unknown"
#endif
#ifndef GIT_COMMIT_SHA
#define GIT_COMMIT_SHA "unknown"
#endif

#define CEC_MAX_MSG_SIZE 16

#define CEC_LOG_DIR "/opt/logs"
#define CEC_LOG_FILE_DEFAULT CEC_LOG_DIR "/cechal_userland.log"
#define CEC_TIMESTAMP_FALLBACK "_TIMESTAMP_UNAVAILABLE_"
#define CEC_TIMESTAMP_SIZE 64

// Raspberry Pi CEC Configuration
#define RPI_CEC_VENDOR_ID 0x00BC44

// Log levels
#define CEC_LOG_LEVEL_ERROR   0
#define CEC_LOG_LEVEL_WARN    1
#define CEC_LOG_LEVEL_INFO    2
#define CEC_LOG_LEVEL_DEBUG   3
#define CEC_LOG_LEVEL_TRACE   4

#define CEC_LOG(level, ...) cec_log(level, __func__, __LINE__, __VA_ARGS__)
#define CEC_LOG_ERROR(...) CEC_LOG(CEC_LOG_LEVEL_ERROR, __VA_ARGS__)
#define CEC_LOG_WARN(...)  CEC_LOG(CEC_LOG_LEVEL_WARN, __VA_ARGS__)
#define CEC_LOG_INFO(...)  CEC_LOG(CEC_LOG_LEVEL_INFO, __VA_ARGS__)
#define CEC_LOG_DEBUG(...) CEC_LOG(CEC_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define CEC_LOG_TRACE(...) CEC_LOG(CEC_LOG_LEVEL_TRACE, __VA_ARGS__)

// Timing constants for cleanup and callback synchronization
#define CEC_CALLBACK_WAIT_MS 10
#define CEC_CALLBACK_WAIT_US (CEC_CALLBACK_WAIT_MS * 1000)
#define CEC_CLOSE_MAX_WAIT_ITERATIONS 100
#define CEC_DESTRUCTOR_MAX_WAIT_ITERATIONS 50
#define CEC_DESTRUCTOR_FORCE_WAIT_MS 50
#define CEC_DESTRUCTOR_FORCE_WAIT_US (CEC_DESTRUCTOR_FORCE_WAIT_MS * 1000)
#define CEC_MUTEX_TIMEOUT_SEC 1
#define CEC_WORDS_PER_MESSAGE 4
#define CEC_BITS_PER_BYTE 8

typedef struct {
	VCHI_INSTANCE_T vchi_instance;
	VCHI_CONNECTION_T *vchi_connection;
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
static int g_log_level = CEC_LOG_LEVEL_TRACE;

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

	if (log_level_env != NULL) {
		if (strcmp(log_level_env, "ERROR") == 0) g_log_level = CEC_LOG_LEVEL_ERROR;
		else if (strcmp(log_level_env, "WARN") == 0) g_log_level = CEC_LOG_LEVEL_WARN;
		else if (strcmp(log_level_env, "INFO") == 0) g_log_level = CEC_LOG_LEVEL_INFO;
		else if (strcmp(log_level_env, "DEBUG") == 0) g_log_level = CEC_LOG_LEVEL_DEBUG;
		else if (strcmp(log_level_env, "TRACE") == 0) g_log_level = CEC_LOG_LEVEL_TRACE;
	}

	const char *log_file_path = log_file_env ? log_file_env : CEC_LOG_FILE_DEFAULT;
	struct stat st;
	if (stat(CEC_LOG_DIR, &st) != 0 && errno == ENOENT) {
		mkdir(CEC_LOG_DIR, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
	}

	g_log_file = fopen(log_file_path, "a");
	if (g_log_file != NULL) {
		setvbuf(g_log_file, NULL, _IOLBF, 0);
		char timestamp[CEC_TIMESTAMP_SIZE];
		cec_get_timestamp(timestamp, sizeof(timestamp));
		fprintf(g_log_file, "CEC HAL Log Started (USERLAND): %s (Level: %s)\n",
				timestamp, cec_get_log_level_str(g_log_level));
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
	.vchi_instance = NULL,
	.vchi_connection = NULL,
	.handle = 0,
	.initialized = false,
	.running = false,
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.rx_callback = NULL,
	.rx_callback_data = NULL,
	.tx_callback = NULL,
	.tx_callback_data = NULL,
	.logical_address = CEC_AllDevices_eUnRegistered,
	.physical_address = CEC_CLEAR_ADDR,
	.has_logical_address = false,
	.callback_active = 0
};

static void cec_rx_callback_handler(void *callback_data, uint32_t reason, uint32_t param1, uint32_t param2, uint32_t param3, uint32_t param4)
{
	if (callback_data == NULL) {
		CEC_LOG_ERROR("cec_rx_callback_handler: callback_data is NULL");
		return;
	}
	cec_context_t *ctx = (cec_context_t *)callback_data;

	if (reason == VC_CEC_RX) {
		unsigned char *buf = NULL;
		uint32_t msg_len = param1;

		if (msg_len > 0 && msg_len <= CEC_MAX_MSG_SIZE) {
			// Allocate buffer on heap to avoid stack lifetime issues
			buf = (unsigned char *)malloc(msg_len);
			if (buf == NULL) {
				CEC_LOG_ERROR("Failed to allocate buffer for CEC message");
				return;
			}

			uint32_t words[CEC_WORDS_PER_MESSAGE];
			words[0] = param2;
			words[1] = param3;
			words[2] = param4;
			words[3] = 0; // Initialize for safety
			for (uint32_t i = 0; i < msg_len; ++i) {
				uint32_t word_index = i / CEC_WORDS_PER_MESSAGE;
				uint32_t byte_shift = (i % CEC_WORDS_PER_MESSAGE) * CEC_BITS_PER_BYTE;
				buf[i] = (unsigned char)((words[word_index] >> byte_shift) & 0xFFU);
			}

			CEC_LOG_INFO("Received CEC message: len=%d", msg_len);

			pthread_mutex_lock(&ctx->mutex);
			HdmiCecRxCallback_t rx_callback = ctx->rx_callback;
			void *rx_callback_data = ctx->rx_callback_data;
			int callback_handle = ctx->handle;
			bool should_call = ctx->running && ctx->initialized && rx_callback != NULL;
			if (should_call) {
				ctx->callback_active++;
			}
			pthread_mutex_unlock(&ctx->mutex);

			if (should_call) {
				// Buffer is heap-allocated, callback can safely store or process it
				// Callback is responsible for freeing the buffer when done
				rx_callback(callback_handle, rx_callback_data, buf, msg_len);

				pthread_mutex_lock(&ctx->mutex);
				ctx->callback_active--;
				pthread_mutex_unlock(&ctx->mutex);
			} else {
				// No callback to invoke, free the buffer
				free(buf);
			}
		}
	} else if (reason == VC_CEC_TX) {
		CEC_LOG_DEBUG("TX callback: result=%d", param1);

		pthread_mutex_lock(&ctx->mutex);
		HdmiCecTxCallback_t tx_callback = ctx->tx_callback;
		void *tx_callback_data = ctx->tx_callback_data;
		int callback_handle = ctx->handle;
		bool should_call = ctx->running && ctx->initialized && tx_callback != NULL;
		if (should_call) {
			ctx->callback_active++;
		}
		pthread_mutex_unlock(&ctx->mutex);

		if (should_call) {
			int result = (param1 == 0) ? HDMI_CEC_IO_SENT_AND_ACKD :
						 (param1 == 1) ? HDMI_CEC_IO_SENT_BUT_NOT_ACKD :
						 HDMI_CEC_IO_SENT_FAILED;
			tx_callback(callback_handle, tx_callback_data, result);

			pthread_mutex_lock(&ctx->mutex);
			ctx->callback_active--;
			pthread_mutex_unlock(&ctx->mutex);
		}
	} else if (reason == VC_CEC_LOGICAL_ADDR) {
		// Logical address allocated/changed by firmware
		// param1 contains the new logical address
		CEC_LOG_INFO("Logical address changed: %d", param1);

		pthread_mutex_lock(&ctx->mutex);
		if (ctx->initialized && ctx->running) {
			ctx->logical_address = (int)param1;
			ctx->has_logical_address = (param1 != CEC_AllDevices_eUnRegistered);
			CEC_LOG_INFO("Updated logical address: %d (has_address=%d)",
						 ctx->logical_address, ctx->has_logical_address);
		}
		pthread_mutex_unlock(&ctx->mutex);
	} else if (reason == VC_CEC_TOPOLOGY) {
		// Physical address/topology changed (HDMI hot-plug event)
		// Query the new physical address from firmware
		CEC_LOG_INFO("Topology changed, querying new physical address");

		pthread_mutex_lock(&ctx->mutex);
		if (ctx->initialized && ctx->running) {
			uint16_t new_phys_addr;
			if (vc_cec_get_physical_address(&new_phys_addr) == 0) {
				ctx->physical_address = new_phys_addr;
				if (new_phys_addr == CEC_CLEAR_ADDR) {
					// HDMI disconnected - mark logical address as unavailable
					ctx->logical_address = CEC_AllDevices_eUnRegistered;
					ctx->has_logical_address = false;
					CEC_LOG_INFO("HDMI disconnected: phys_addr=0x%04x, logical_addr=0x%02x",
								 new_phys_addr, ctx->logical_address);
				} else {
					// HDMI connected - logical address will be updated via VC_CEC_LOGICAL_ADDR callback
					CEC_LOG_INFO("HDMI connected: phys_addr=0x%04x", new_phys_addr);
				}
			}
		}
		pthread_mutex_unlock(&ctx->mutex);
	}
}

HDMI_CEC_STATUS HdmiCecOpen(int* handle)
{
	int32_t ret;

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

	ret = vchi_initialise(&g_cec_context.vchi_instance);
	if (ret != 0) {
		CEC_LOG_ERROR("Failed to initialize VCHI: %d", ret);
		pthread_mutex_unlock(&g_cec_context.mutex);
		g_cec_context.vchi_instance = NULL;
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	ret = vchi_connect(NULL, 0, g_cec_context.vchi_instance);
	if (ret != 0) {
		CEC_LOG_ERROR("Failed to connect VCHI: %d", ret);
		// Note: Don't call vchi_disconnect on failed connection
		g_cec_context.vchi_instance = NULL;
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	CEC_LOG_DEBUG("Initializing CEC service");
	vc_vchi_cec_init(g_cec_context.vchi_instance, &g_cec_context.vchi_connection, 1);
	vc_cec_register_callback(cec_rx_callback_handler, &g_cec_context);

	CEC_LOG_DEBUG("Setting CEC logical address");
	ret = vc_cec_set_logical_address(CEC_AllDevices_eSTB1, CEC_DeviceType_Tuner, RPI_CEC_VENDOR_ID);
	if (ret != 0) {
		CEC_LOG_WARN("Failed to set logical address, error: %d", ret);
	}

	uint16_t phys_addr;
	if (vc_cec_get_physical_address(&phys_addr) == 0) {
		g_cec_context.physical_address = phys_addr;
		CEC_LOG_DEBUG("Retrieved physical address from firmware: 0x%04x", g_cec_context.physical_address);
		if (phys_addr != CEC_CLEAR_ADDR) {
			CEC_AllDevices_T device_type = CEC_AllDevices_eUnRegistered;
			if (vc_cec_get_logical_address(&device_type) == 0) {
				g_cec_context.logical_address = (int)device_type;
				g_cec_context.has_logical_address = (device_type != CEC_AllDevices_eUnRegistered);
				CEC_LOG_INFO("Firmware allocated logical address: %d", g_cec_context.logical_address);
			} else {
				CEC_LOG_WARN("Failed to query logical address");
			}
		} else {
			CEC_LOG_DEBUG("No HDMI sink connected - HAL opened in disconnected state");
		}
	} else {
		g_cec_context.physical_address = CEC_CLEAR_ADDR;
		CEC_LOG_WARN("Get physical address failed, assuming disconnected (0x%04x)", CEC_CLEAR_ADDR);
	}

	g_cec_context.handle = cec_generate_handle();
	g_cec_context.initialized = true;
	g_cec_context.running = true;

	*handle = g_cec_context.handle;
	pthread_mutex_unlock(&g_cec_context.mutex);

	CEC_LOG_INFO("HdmiCecOpen successful: handle=%d", *handle);
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

	g_cec_context.running = false;
	g_cec_context.initialized = false;

	// Unregister callback first to prevent new callbacks
	vc_cec_register_callback(NULL, NULL);

	// Wait for active callbacks to complete (with timeout)
	int wait_count = 0;
	while (g_cec_context.callback_active > 0 && wait_count < CEC_CLOSE_MAX_WAIT_ITERATIONS) {
		pthread_mutex_unlock(&g_cec_context.mutex);
		usleep(CEC_CALLBACK_WAIT_US);
		pthread_mutex_lock(&g_cec_context.mutex);
		wait_count++;
	}

	if (g_cec_context.callback_active > 0) {
		CEC_LOG_WARN("Closing with %d active callbacks still running", g_cec_context.callback_active);
	}

	// Now safe to cleanup VCHI resources
	vc_vchi_cec_stop();
	if (g_cec_context.vchi_instance != NULL) {
		vchi_disconnect(g_cec_context.vchi_instance);
	}
	g_cec_context.vchi_instance = NULL;
	g_cec_context.vchi_connection = NULL;

	g_cec_context.handle = 0;
	g_cec_context.rx_callback = NULL;
	g_cec_context.rx_callback_data = NULL;
	g_cec_context.tx_callback = NULL;
	g_cec_context.tx_callback_data = NULL;
	g_cec_context.logical_address = CEC_AllDevices_eUnRegistered;
	g_cec_context.has_logical_address = false;
	g_cec_context.physical_address = CEC_CLEAR_ADDR;
	g_cec_context.callback_active = 0;

	pthread_mutex_unlock(&g_cec_context.mutex);

	CEC_LOG_INFO("HdmiCecClose successful");
	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecGetPhysicalAddress(int handle, unsigned int* physicalAddress)
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

	// Get cached physical address (topology fields vary by userland version)
	*physicalAddress = g_cec_context.physical_address;
	pthread_mutex_unlock(&g_cec_context.mutex);

	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecAddLogicalAddress(int handle, int logicalAddresses)
{
	// For source devices, this operation is not supported
	// Return OPERATION_NOT_SUPPORTED regardless of other conditions
	CEC_LOG_INFO("HdmiCecAddLogicalAddress not supported for source devices");
	return HDMI_CEC_IO_OPERATION_NOT_SUPPORTED;
}

HDMI_CEC_STATUS HdmiCecRemoveLogicalAddress(int handle, int logicalAddresses)
{
	// For source devices, this operation is not supported.
	CEC_LOG_INFO("HdmiCecRemoveLogicalAddress not supported for source devices");
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

	// NULL callback is valid for unregistering the transmit callback
	g_cec_context.tx_callback = callback;
	g_cec_context.tx_callback_data = data;

	pthread_mutex_unlock(&g_cec_context.mutex);
	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecTx(int handle, const unsigned char* buf, int len, int* result)
{
	if (buf == NULL || result == NULL || len <= 0 || len > CEC_MAX_MSG_SIZE) {
		CEC_LOG_ERROR("Invalid arguments: buf=%p, result=%p, len=%d (valid range: 1-%d)",
					  (void*)buf, (void*)result, len, CEC_MAX_MSG_SIZE);
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

	// Keep mutex held during send to prevent context deinitialization
	uint8_t follower = buf[0] & 0x0F;
	uint8_t payload_buf[CEC_MAX_MSG_SIZE - 1];
	uint8_t *payload = NULL;
	uint32_t payload_len = 0;
	if (len > 1) {
		payload_len = (uint32_t)(len - 1);
		memcpy(payload_buf, &buf[1], payload_len);
		payload = payload_buf;
	}

	int32_t ret = vc_cec_send_message(follower, payload, payload_len, VC_TRUE);
	pthread_mutex_unlock(&g_cec_context.mutex);

	if (ret == 0) {
		*result = HDMI_CEC_IO_SENT_AND_ACKD;
		return HDMI_CEC_IO_SUCCESS;
	} else if (ret == 1) {
		*result = HDMI_CEC_IO_SENT_BUT_NOT_ACKD;
		return HDMI_CEC_IO_SUCCESS;
	} else {
		*result = HDMI_CEC_IO_SENT_FAILED;
		return HDMI_CEC_IO_SENT_FAILED;
	}
}

HDMI_CEC_STATUS HdmiCecTxAsync(int handle, const unsigned char* buf, int len)
{
	if (buf == NULL || len <= 0 || len > CEC_MAX_MSG_SIZE) {
		CEC_LOG_ERROR("Invalid arguments: buf=%p, len=%d (valid range: 1-%d)",
					  (void*)buf, len, CEC_MAX_MSG_SIZE);
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

	// Keep mutex held during send to prevent context deinitialization
	// vc_cec_send_message expects: follower, payload (opcode+params), length, is_reply
	uint8_t follower = buf[0] & 0x0F;
	uint8_t payload_buf[CEC_MAX_MSG_SIZE - 1];
	uint32_t payload_len = (len > 1) ? (len - 1) : 0;
	int32_t ret;

	if (payload_len > 0) {
		memcpy(payload_buf, &buf[1], payload_len);
		// Note: vc_cec_send_message copies the buffer internally, so it's safe
		// for payload_buf to go out of scope after this call
		ret = vc_cec_send_message(follower, payload_buf, payload_len, VC_FALSE);
	} else {
		ret = vc_cec_send_message(follower, NULL, 0, VC_FALSE);
	}

	pthread_mutex_unlock(&g_cec_context.mutex);

	if (ret != 0) {
		CEC_LOG_ERROR("Failed to send CEC message asynchronously");
		return HDMI_CEC_IO_SENT_FAILED;
	}
	return HDMI_CEC_IO_SUCCESS;
}

static void __attribute__((constructor)) cec_driver_init(void)
{
	cec_log_init();
	CEC_LOG_INFO("RPi4 CEC HAL (USERLAND) - Version: %s, Git SHA: %s", HAL_VERSION, GIT_COMMIT_SHA);
}

static void __attribute__((destructor)) cec_driver_fini(void)
{
	// Use pthread_mutex_timedlock for efficient blocking wait with timeout
	struct timespec timeout;
	if (clock_gettime(CLOCK_REALTIME, &timeout) == 0) {
		timeout.tv_sec += CEC_MUTEX_TIMEOUT_SEC;
	} else {
		// Fallback if clock_gettime fails
		timeout.tv_sec = time(NULL) + CEC_MUTEX_TIMEOUT_SEC;
		timeout.tv_nsec = 0;
	}

	int lock_result = pthread_mutex_timedlock(&g_cec_context.mutex, &timeout);

	if (lock_result == 0) {
		if (g_cec_context.initialized) {
			CEC_LOG_WARN("CEC device still open during shutdown");
			g_cec_context.running = false;
			vc_cec_register_callback(NULL, NULL);

			// Wait briefly for callbacks to complete
			int wait_count = 0;
			while (g_cec_context.callback_active > 0 && wait_count < CEC_DESTRUCTOR_MAX_WAIT_ITERATIONS) {
				pthread_mutex_unlock(&g_cec_context.mutex);
				usleep(CEC_CALLBACK_WAIT_US);
				pthread_mutex_lock(&g_cec_context.mutex);
				wait_count++;
			}

			vc_vchi_cec_stop();
			if (g_cec_context.vchi_instance != NULL) {
				vchi_disconnect(g_cec_context.vchi_instance);
			}
			g_cec_context.vchi_instance = NULL;
			g_cec_context.vchi_connection = NULL;
			g_cec_context.initialized = false;
		}
		pthread_mutex_unlock(&g_cec_context.mutex);
	} else {
		// Mutex acquisition timed out - perform cleanup without mutex to prevent resource leaks
		// This is risky (potential race conditions) but better than leaking resources
		fprintf(stderr, "CEC HAL: Warning - mutex timeout during shutdown, forcing cleanup\n");
		if (g_cec_context.initialized) {
			g_cec_context.running = false;
			vc_cec_register_callback(NULL, NULL);
			usleep(CEC_DESTRUCTOR_FORCE_WAIT_US);
			vc_vchi_cec_stop();
			if (g_cec_context.vchi_instance != NULL) {
				vchi_disconnect(g_cec_context.vchi_instance);
			}
			g_cec_context.vchi_instance = NULL;
			g_cec_context.vchi_connection = NULL;
			g_cec_context.initialized = false;
		}
	}

	// Always close log
	cec_log_close();
}

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
 * EXPERIMENTAL: Raspberry Pi Userland-based CEC HAL Implementation
 *
 * WARNING: This is an experimental implementation attempting to use VideoCore
 * firmware CEC capabilities through userland libraries. The userland library
 * does not have public CEC APIs, so this implementation may not work.
 *
 * This implementation tries to:
 * 1. Use bcm_host to initialize VideoCore
 * 2. Attempt VCHI/VCHIQ communication for CEC
 * 3. Use mailbox properties for CEC control (if available)
 *
 * For production, use the kernel-based implementation (hdmi_cec_driver.c)
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
#include "interface/vmcs_host/vc_cecservice.h"

#include "hdmi_cec_driver.h"

#ifndef HAL_VERSION
#define HAL_VERSION "unknown"
#endif
#ifndef GIT_COMMIT_SHA
#define GIT_COMMIT_SHA "unknown"
#endif

#define CEC_MAX_MSG_SIZE 16
#define THREAD_JOIN_TIMEOUT_SEC 5

// Debug logging configuration
#define CEC_LOG_MAX_SIZE (4 * 1024 * 1024)
#define CEC_LOG_DIR "/opt/logs"
#define CEC_LOG_FILE_DEFAULT CEC_LOG_DIR "/cechal_userland.log"
#define CEC_LOG_FILE_SUFFIX ".old"
#define CEC_TIMESTAMP_FALLBACK "1970-01-01 00:00:00.000"
#define CEC_TIMESTAMP_SIZE 64
#define CEC_LOG_PATH_MAX 256

// Raspberry Pi CEC Configuration
#define RPI_CEC_VENDOR_ID 0x00BC44
#define RPI_CEC_OSD_NAME "Raspberry Pi"
#define RPI_CEC_UNREGISTERED_ADDR 0x0F

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

typedef struct {
	VCHI_INSTANCE_T vchi_instance;
	VCHI_CONNECTION_T *vchi_connection;
	CECSERVICE_HOST_STATE_T cec_host_state;
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

	if (log_level_env == NULL && log_file_env == NULL) {
		pthread_mutex_unlock(&g_log_mutex);
		return;
	}

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
		fprintf(g_log_file, "\nCEC HAL Log Started (USERLAND): %s (Level: %s)\n",
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

static void cec_rx_callback_handler(void *callback_data, uint32_t reason, uint32_t param1, uint32_t param2, uint32_t param3, uint32_t param4)
{
	cec_context_t *ctx = (cec_context_t *)callback_data;

	if (reason == VC_CEC_RX) {
		unsigned char buf[CEC_MAX_MSG_SIZE];
		uint32_t msg_len = param1;

		if (msg_len > 0 && msg_len <= CEC_MAX_MSG_SIZE) {
			memcpy(buf, (void*)param2, msg_len);

			CEC_LOG_INFO("Received CEC message: len=%d", msg_len);

			pthread_mutex_lock(&ctx->mutex);
			HdmiCecRxCallback_t rx_callback = ctx->rx_callback;
			void *rx_callback_data = ctx->rx_callback_data;
			int callback_handle = ctx->handle;
			bool should_call = ctx->running && ctx->initialized && rx_callback != NULL;
			pthread_mutex_unlock(&ctx->mutex);

			if (should_call) {
				rx_callback(callback_handle, rx_callback_data, buf, msg_len);
			}
		}
	} else if (reason == VC_CEC_TX) {
		CEC_LOG_DEBUG("TX callback: result=%d", param1);

		pthread_mutex_lock(&ctx->mutex);
		HdmiCecTxCallback_t tx_callback = ctx->tx_callback;
		void *tx_callback_data = ctx->tx_callback_data;
		int callback_handle = ctx->handle;
		unsigned int callback_gen = ctx->tx_callback_gen;
		pthread_mutex_unlock(&ctx->mutex);

		if (tx_callback) {
			int result = (param1 == 0) ? HDMI_CEC_IO_SENT_AND_ACKD :
			             (param1 == 1) ? HDMI_CEC_IO_SENT_BUT_NOT_ACKD :
			             HDMI_CEC_IO_SENT_FAILED;
			tx_callback(callback_handle, tx_callback_data, result);
		}
	}
}

HDMI_CEC_STATUS HdmiCecOpen(int* handle)
{
	int32_t ret;

	CEC_LOG_INFO("%s (USERLAND implementation)", __func__);

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

	CEC_LOG_DEBUG("Initializing BCM host");
	bcm_host_init();

	CEC_LOG_DEBUG("Initializing VCHI");
	ret = vchi_initialise(&g_cec_context.vchi_instance);
	if (ret != 0) {
		CEC_LOG_ERROR("Failed to initialize VCHI: %d", ret);
		bcm_host_deinit();
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	ret = vchi_connect(NULL, 0, g_cec_context.vchi_instance);
	if (ret != 0) {
		CEC_LOG_ERROR("Failed to connect VCHI: %d", ret);
		vchi_disconnect(g_cec_context.vchi_instance);
		bcm_host_deinit();
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	CEC_LOG_DEBUG("Initializing CEC service");
	vc_vchi_cec_init(g_cec_context.vchi_instance, &g_cec_context.vchi_connection, 1);

	ret = vc_cec_register_callback(cec_rx_callback_handler, &g_cec_context);
	if (ret != 0) {
		CEC_LOG_ERROR("Failed to register CEC callback: %d", ret);
		vc_vchi_cec_stop();
		vchi_disconnect(g_cec_context.vchi_instance);
		bcm_host_deinit();
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_GENERAL_ERROR;
	}

	CEC_LOG_DEBUG("Setting CEC logical address");
	ret = vc_cec_set_logical_address(CEC_AllDevices_ePlayback, CEC_DeviceType_Playback, RPI_CEC_VENDOR_ID);
	if (ret != 0) {
		CEC_LOG_WARN("Failed to set logical address: %d", ret);
	}

	CEC_TOPOLOGY_T topology;
	ret = vc_cec_get_topology(&topology);
	if (ret == 0) {
		g_cec_context.physical_address = topology.physical_address;
		g_cec_context.logical_address = topology.device_attr;
		g_cec_context.has_logical_address = (g_cec_context.logical_address < 15);
		CEC_LOG_INFO("Logical address: %d, Physical address: 0x%04x",
					g_cec_context.logical_address, g_cec_context.physical_address);
	} else {
		CEC_LOG_WARN("Failed to get topology: %d", ret);
		g_cec_context.physical_address = 0xFFFF;
		g_cec_context.logical_address = RPI_CEC_UNREGISTERED_ADDR;
		g_cec_context.has_logical_address = false;
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

	g_cec_context.running = false;
	g_cec_context.initialized = false;

	vc_cec_register_callback(NULL, NULL);
	vc_vchi_cec_stop();
	vchi_disconnect(g_cec_context.vchi_instance);
	bcm_host_deinit();

	g_cec_context.handle = 0;
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

	CEC_TOPOLOGY_T topology;
	if (vc_cec_get_topology(&topology) == 0) {
		g_cec_context.physical_address = topology.physical_address;
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

	pthread_mutex_unlock(&g_cec_context.mutex);
	CEC_LOG_INFO("HdmiCecAddLogicalAddress not supported for source devices");
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

	pthread_mutex_unlock(&g_cec_context.mutex);
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

	g_cec_context.tx_callback = callback;
	g_cec_context.tx_callback_data = data;
	g_cec_context.tx_callback_gen++;

	pthread_mutex_unlock(&g_cec_context.mutex);
	return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecTx(int handle, const unsigned char* buf, int len, int* result)
{
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
		CEC_LOG_ERROR("Invalid arguments");
		pthread_mutex_unlock(&g_cec_context.mutex);
		return HDMI_CEC_IO_INVALID_ARGUMENT;
	}

	pthread_mutex_unlock(&g_cec_context.mutex);

	uint8_t follower = buf[0] & 0x0F;
	uint8_t opcode = (len > 1) ? buf[1] : 0xFF;
	uint8_t *params = (len > 2) ? (uint8_t*)&buf[2] : NULL;
	uint32_t param_len = (len > 2) ? (len - 2) : 0;

	int32_t ret = vc_cec_send_message(follower, opcode, params, param_len, VC_TRUE);

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

	pthread_mutex_unlock(&g_cec_context.mutex);

	uint8_t follower = buf[0] & 0x0F;
	uint8_t opcode = (len > 1) ? buf[1] : 0xFF;
	uint8_t *params = (len > 2) ? (uint8_t*)&buf[2] : NULL;
	uint32_t param_len = (len > 2) ? (len - 2) : 0;

	vc_cec_send_message(follower, opcode, params, param_len, VC_FALSE);

	return HDMI_CEC_IO_SUCCESS;
}

static void __attribute__((constructor)) cec_driver_init(void)
{
	cec_log_init();
	CEC_LOG_INFO("RPi4 CEC HAL (USERLAND) - Version: %s, Git SHA: %s", HAL_VERSION, GIT_COMMIT_SHA);
}

static void __attribute__((destructor)) cec_driver_fini(void)
{
	CEC_LOG_INFO("RPi4 CEC HAL driver cleanup");
	pthread_mutex_lock(&g_cec_context.mutex);
	if (g_cec_context.initialized) {
		CEC_LOG_WARN("CEC device still open during shutdown");
		g_cec_context.running = false;
		vc_cec_register_callback(NULL, NULL);
		vc_vchi_cec_stop();
		vchi_disconnect(g_cec_context.vchi_instance);
		bcm_host_deinit();
		g_cec_context.initialized = false;
	}
	pthread_mutex_unlock(&g_cec_context.mutex);
	cec_log_close();
}

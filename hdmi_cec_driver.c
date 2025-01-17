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

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "ccec/drivers/hdmi_cec_driver.h"

static bool _bhdmicecintialized = false;
HDMI_CEC_STATUS HdmiCecOpen(int* handle)
{
  if (_bhdmicecintialized)
  {
  	return HDMI_CEC_IO_ALREADY_OPEN;
  }
  if (handle == NULL)
  {
	  return HDMI_CEC_IO_INVALID_ARGUMENT;
  }
  _bhdmicecintialized = true;
  return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecClose(int handle)
{
  if (!_bhdmicecintialized)
  {
  	return HDMI_CEC_IO_NOT_OPENED;
  }
  if (handle == 0)
  {
    return HDMI_CEC_IO_INVALID_HANDLE;
  }
  _bhdmicecintialized = false;
  return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecGetPhysicalAddress(int handle, unsigned int* physicalAddress)
{
  if (!_bhdmicecintialized)
  {
  	return HDMI_CEC_IO_NOT_OPENED;
  }
  if (handle == 0 || physicalAddress == NULL)
  {
    return HDMI_CEC_IO_INVALID_ARGUMENT;
  }
  return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecAddLogicalAddress(int handle, int logicalAddresses)
{
  if (!_bhdmicecintialized)
  {
  	return HDMI_CEC_IO_NOT_OPENED;
  }
  if (handle == 0 || logicalAddresses < 0 || logicalAddresses > 15)
  {
	  return HDMI_CEC_IO_INVALID_ARGUMENT;
  }
  return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecRemoveLogicalAddress(int handle, int logicalAddresses)
{
  if (!_bhdmicecintialized)
  {
  	return HDMI_CEC_IO_NOT_OPENED;
  }
  if (handle == 0 || logicalAddresses < 0 || logicalAddresses > 15)
  {
    return HDMI_CEC_IO_INVALID_ARGUMENT;
  }
  return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecGetLogicalAddress(int handle, int* logicalAddress)
{
  if (!_bhdmicecintialized)
  {
  	return HDMI_CEC_IO_NOT_OPENED;
  }
  if (handle == 0 || logicalAddress == NULL)
  {
    return HDMI_CEC_IO_INVALID_ARGUMENT;
  }
  return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecSetRxCallback(int handle, HdmiCecRxCallback_t cbfunc, void* data)
{
  (void)cbfunc;
  (void)data;
  if (!_bhdmicecintialized)
  {
  	return HDMI_CEC_IO_NOT_OPENED;
  }
  if (handle == 0)
  {
    return HDMI_CEC_IO_INVALID_HANDLE;
  }
  return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecSetTxCallback(int handle, HdmiCecTxCallback_t cbfunc, void* data)
{
  (void)cbfunc;
  (void)data;
  if (!_bhdmicecintialized)
  {
  	return HDMI_CEC_IO_NOT_OPENED;
  }
  if (handle == 0)
  {
    return HDMI_CEC_IO_INVALID_HANDLE;
  }
  return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecTx(int handle, const unsigned char* buf, int len, int* result)
{
  if (!_bhdmicecintialized)
  {
  	return HDMI_CEC_IO_NOT_OPENED;
  }
  if (handle == 0 || buf == NULL || len == 0 || result == NULL)
  {
    return HDMI_CEC_IO_INVALID_ARGUMENT;
  }
  *result = HDMI_CEC_IO_SENT_FAILED;
  return HDMI_CEC_IO_SUCCESS;
}

HDMI_CEC_STATUS HdmiCecTxAsync(int handle, const unsigned char* buf, int len)
{
  if (!_bhdmicecintialized)
  {
  	return HDMI_CEC_IO_NOT_OPENED;
  }
  if (handle == 0 || buf == NULL || len == 0)
  {
    return HDMI_CEC_IO_INVALID_ARGUMENT;
  }
  return HDMI_CEC_IO_SUCCESS;
}

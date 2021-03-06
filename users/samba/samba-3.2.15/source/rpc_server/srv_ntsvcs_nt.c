/*
 *  Unix SMB/CIFS implementation.
 *  RPC Pipe client / server routines
 *
 *  Copyright (C) Gerald (Jerry) Carter             2005.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "includes.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_RPC_SRV

/********************************************************************
********************************************************************/

static char* get_device_path(TALLOC_CTX *mem_ctx, const char *device )
{
	return talloc_asprintf(mem_ctx, "ROOT\\Legacy_%s\\0000", device);
}

/********************************************************************
********************************************************************/

WERROR _PNP_GetVersion(pipes_struct *p,
		       struct PNP_GetVersion *r)
{
	*r->out.version = 0x0400;      /* no idea what this means */

	return WERR_OK;
}

/********************************************************************
********************************************************************/

WERROR _PNP_GetDeviceListSize(pipes_struct *p,
			      struct PNP_GetDeviceListSize *r)
{
	char *devicepath;

	if (!r->in.devicename) {
		return WERR_ACCESS_DENIED;
	}

	if (!(devicepath = get_device_path(p->mem_ctx, r->in.devicename))) {
		return WERR_NOMEM;
	}

	*r->out.size = strlen(devicepath) + 2;

	TALLOC_FREE(devicepath);

	return WERR_OK;
}


/********************************************************************
********************************************************************/

WERROR _ntsvcs_get_device_list( pipes_struct *p, NTSVCS_Q_GET_DEVICE_LIST *q_u, NTSVCS_R_GET_DEVICE_LIST *r_u )
{
	fstring device;
	char *devicepath;

	if ( !q_u->devicename )
		return WERR_ACCESS_DENIED;

	rpcstr_pull(device, q_u->devicename->buffer, sizeof(device), q_u->devicename->uni_str_len*2, 0);

	if (!(devicepath = get_device_path(p->mem_ctx, device))) {
		return WERR_NOMEM;
	}

	/* This has to be DOUBLE NULL terminated */

	init_unistr2( &r_u->devicepath, devicepath, UNI_STR_DBLTERMINATE );
	TALLOC_FREE(devicepath);
	r_u->needed = r_u->devicepath.uni_str_len;

	return WERR_OK;
}

/********************************************************************
********************************************************************/

WERROR _ntsvcs_get_device_reg_property( pipes_struct *p, NTSVCS_Q_GET_DEVICE_REG_PROPERTY *q_u, NTSVCS_R_GET_DEVICE_REG_PROPERTY *r_u )
{
	fstring devicepath;
	char *ptr;
	REGVAL_CTR *values;
	REGISTRY_VALUE *val;

	rpcstr_pull(devicepath, q_u->devicepath.buffer, sizeof(devicepath), q_u->devicepath.uni_str_len*2, 0);

	switch( q_u->property ) {
	case DEV_REGPROP_DESC:
		/* just parse the service name from the device path and then
		   lookup the display name */
		if ( !(ptr = strrchr_m( devicepath, '\\' )) )
			return WERR_GENERAL_FAILURE;
		*ptr = '\0';

		if ( !(ptr = strrchr_m( devicepath, '_' )) )
			return WERR_GENERAL_FAILURE;
		ptr++;

		if ( !(values = svcctl_fetch_regvalues( ptr, p->pipe_user.nt_user_token )) )
			return WERR_GENERAL_FAILURE;

		if ( !(val = regval_ctr_getvalue( values, "DisplayName" )) ) {
			TALLOC_FREE( values );
			return WERR_GENERAL_FAILURE;
		}

		r_u->unknown1 = 0x1;	/* always 1...tested using a remove device manager connection */
		r_u->size = reg_init_regval_buffer( &r_u->value, val );
		r_u->needed = r_u->size;

		TALLOC_FREE(values);

		break;

	default:
		r_u->unknown1 = 0x00437c98;
		return WERR_CM_NO_SUCH_VALUE;
	}

	return WERR_OK;
}

/********************************************************************
********************************************************************/

WERROR _PNP_ValidateDeviceInstance(pipes_struct *p,
				   struct PNP_ValidateDeviceInstance *r)
{
	/* whatever dude */
	return WERR_OK;
}

/********************************************************************
********************************************************************/

WERROR _PNP_GetHwProfInfo(pipes_struct *p,
			  struct PNP_GetHwProfInfo *r)
{
	/* steal the incoming buffer */

	r->out.info = r->in.info;

	/* Take the 5th Ammentment */

	return WERR_CM_NO_MORE_HW_PROFILES;
}

/********************************************************************
********************************************************************/

WERROR _PNP_HwProfFlags(pipes_struct *p,
			struct PNP_HwProfFlags *r)
{
	/* just nod your head */

	return WERR_OK;
}

/****************************************************************
****************************************************************/

WERROR _PNP_Disconnect(pipes_struct *p,
		       struct PNP_Disconnect *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_Connect(pipes_struct *p,
		    struct PNP_Connect *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetGlobalState(pipes_struct *p,
			   struct PNP_GetGlobalState *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_InitDetection(pipes_struct *p,
			  struct PNP_InitDetection *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_ReportLogOn(pipes_struct *p,
			struct PNP_ReportLogOn *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetRootDeviceInstance(pipes_struct *p,
				  struct PNP_GetRootDeviceInstance *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetRelatedDeviceInstance(pipes_struct *p,
				     struct PNP_GetRelatedDeviceInstance *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_EnumerateSubKeys(pipes_struct *p,
			     struct PNP_EnumerateSubKeys *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetDeviceList(pipes_struct *p,
			  struct PNP_GetDeviceList *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetDepth(pipes_struct *p,
		     struct PNP_GetDepth *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetDeviceRegProp(pipes_struct *p,
			     struct PNP_GetDeviceRegProp *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_SetDeviceRegProp(pipes_struct *p,
			     struct PNP_SetDeviceRegProp *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetClassInstance(pipes_struct *p,
			     struct PNP_GetClassInstance *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_CreateKey(pipes_struct *p,
		      struct PNP_CreateKey *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_DeleteRegistryKey(pipes_struct *p,
			      struct PNP_DeleteRegistryKey *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetClassCount(pipes_struct *p,
			  struct PNP_GetClassCount *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetClassName(pipes_struct *p,
			 struct PNP_GetClassName *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_DeleteClassKey(pipes_struct *p,
			   struct PNP_DeleteClassKey *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetInterfaceDeviceAlias(pipes_struct *p,
				    struct PNP_GetInterfaceDeviceAlias *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetInterfaceDeviceList(pipes_struct *p,
				   struct PNP_GetInterfaceDeviceList *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetInterfaceDeviceListSize(pipes_struct *p,
				       struct PNP_GetInterfaceDeviceListSize *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_RegisterDeviceClassAssociation(pipes_struct *p,
					   struct PNP_RegisterDeviceClassAssociation *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_UnregisterDeviceClassAssociation(pipes_struct *p,
					     struct PNP_UnregisterDeviceClassAssociation *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetClassRegProp(pipes_struct *p,
			    struct PNP_GetClassRegProp *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_SetClassRegProp(pipes_struct *p,
			    struct PNP_SetClassRegProp *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_CreateDevInst(pipes_struct *p,
			  struct PNP_CreateDevInst *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_DeviceInstanceAction(pipes_struct *p,
				 struct PNP_DeviceInstanceAction *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetDeviceStatus(pipes_struct *p,
			    struct PNP_GetDeviceStatus *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_SetDeviceProblem(pipes_struct *p,
			     struct PNP_SetDeviceProblem *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_DisableDevInst(pipes_struct *p,
			   struct PNP_DisableDevInst *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_UninstallDevInst(pipes_struct *p,
			     struct PNP_UninstallDevInst *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_AddID(pipes_struct *p,
		  struct PNP_AddID *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_RegisterDriver(pipes_struct *p,
			   struct PNP_RegisterDriver *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_QueryRemove(pipes_struct *p,
			struct PNP_QueryRemove *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_RequestDeviceEject(pipes_struct *p,
			       struct PNP_RequestDeviceEject *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_IsDockStationPresent(pipes_struct *p,
				 struct PNP_IsDockStationPresent *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_RequestEjectPC(pipes_struct *p,
			   struct PNP_RequestEjectPC *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_AddEmptyLogConf(pipes_struct *p,
			    struct PNP_AddEmptyLogConf *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_FreeLogConf(pipes_struct *p,
			struct PNP_FreeLogConf *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetFirstLogConf(pipes_struct *p,
			    struct PNP_GetFirstLogConf *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetNextLogConf(pipes_struct *p,
			   struct PNP_GetNextLogConf *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetLogConfPriority(pipes_struct *p,
			       struct PNP_GetLogConfPriority *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_AddResDes(pipes_struct *p,
		      struct PNP_AddResDes *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_FreeResDes(pipes_struct *p,
		       struct PNP_FreeResDes *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetNextResDes(pipes_struct *p,
			  struct PNP_GetNextResDes *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetResDesData(pipes_struct *p,
			  struct PNP_GetResDesData *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetResDesDataSize(pipes_struct *p,
			      struct PNP_GetResDesDataSize *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_ModifyResDes(pipes_struct *p,
			 struct PNP_ModifyResDes *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_DetectResourceLimit(pipes_struct *p,
				struct PNP_DetectResourceLimit *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_QueryResConfList(pipes_struct *p,
			     struct PNP_QueryResConfList *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_SetHwProf(pipes_struct *p,
		      struct PNP_SetHwProf *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_QueryArbitratorFreeData(pipes_struct *p,
				    struct PNP_QueryArbitratorFreeData *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_QueryArbitratorFreeSize(pipes_struct *p,
				    struct PNP_QueryArbitratorFreeSize *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_RunDetection(pipes_struct *p,
			 struct PNP_RunDetection *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_RegisterNotification(pipes_struct *p,
				 struct PNP_RegisterNotification *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_UnregisterNotification(pipes_struct *p,
				   struct PNP_UnregisterNotification *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetCustomDevProp(pipes_struct *p,
			     struct PNP_GetCustomDevProp *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetVersionInternal(pipes_struct *p,
			       struct PNP_GetVersionInternal *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetBlockedDriverInfo(pipes_struct *p,
				 struct PNP_GetBlockedDriverInfo *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}

/****************************************************************
****************************************************************/

WERROR _PNP_GetServerSideDeviceInstallFlags(pipes_struct *p,
					    struct PNP_GetServerSideDeviceInstallFlags *r)
{
	p->rng_fault_state = true;
	return WERR_NOT_SUPPORTED;
}


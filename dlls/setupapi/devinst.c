/*
 * SetupAPI device installer
 *
 * Copyright 2000 Andreas Mohr for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"
 
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winreg.h"
#include "winternl.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "setupapi.h"
#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/list.h"
#include "wine/unicode.h"
#include "cfgmgr32.h"
#include "winioctl.h"
#include "rpc.h"
#include "rpcdce.h"

#include "setupapi_private.h"


WINE_DEFAULT_DEBUG_CHANNEL(setupapi);

/* Unicode constants */
static const WCHAR Chicago[]  = {'$','C','h','i','c','a','g','o','$',0};
static const WCHAR ClassGUID[]  = {'C','l','a','s','s','G','U','I','D',0};
static const WCHAR Class[]  = {'C','l','a','s','s',0};
static const WCHAR ClassInstall32[]  = {'C','l','a','s','s','I','n','s','t','a','l','l','3','2',0};
static const WCHAR NoDisplayClass[]  = {'N','o','D','i','s','p','l','a','y','C','l','a','s','s',0};
static const WCHAR NoInstallClass[]  = {'N','o','I','n','s','t','a','l','l','C','l','a','s','s',0};
static const WCHAR NoUseClass[]  = {'N','o','U','s','e','C','l','a','s','s',0};
static const WCHAR NtExtension[]  = {'.','N','T',0};
static const WCHAR NtPlatformExtension[]  = {'.','N','T','x','8','6',0};
static const WCHAR Signature[]  = {'S','i','g','n','a','t','u','r','e',0};
static const WCHAR Version[]  = {'V','e','r','s','i','o','n',0};
static const WCHAR WinExtension[]  = {'.','W','i','n',0};
static const WCHAR WindowsNT[]  = {'$','W','i','n','d','o','w','s',' ','N','T','$',0};

/* Registry key and value names */
static const WCHAR ControlClass[] = {'S','y','s','t','e','m','\\',
                                  'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
                                  'C','o','n','t','r','o','l','\\',
                                  'C','l','a','s','s',0};

static const WCHAR DeviceClasses[] = {'S','y','s','t','e','m','\\',
                                  'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
                                  'C','o','n','t','r','o','l','\\',
                                  'D','e','v','i','c','e','C','l','a','s','s','e','s',0};
static const WCHAR Enum[] = {'S','y','s','t','e','m','\\',
                                  'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
				  'E','n','u','m',0};
static const WCHAR DeviceDesc[] = {'D','e','v','i','c','e','D','e','s','c',0};
static const WCHAR DeviceInstance[] = {'D','e','v','i','c','e','I','n','s','t','a','n','c','e',0};
static const WCHAR DeviceParameters[] = {'D','e','v','i','c','e',' ','P','a','r','a','m','e','t','e','r','s',0};
static const WCHAR HardwareId[] = {'H','a','r','d','w','a','r','e','I','D',0};
static const WCHAR CompatibleIDs[] = {'C','o','m','p','a','t','i','b','l','e','I','d','s',0};
static const WCHAR Service[] = {'S','e','r','v','i','c','e',0};
static const WCHAR Driver[] = {'D','r','i','v','e','r',0};
static const WCHAR ConfigFlags[] = {'C','o','n','f','i','g','F','l','a','g','s',0};
static const WCHAR Mfg[] = {'M','f','g',0};
static const WCHAR FriendlyName[] = {'F','r','i','e','n','d','l','y','N','a','m','e',0};
static const WCHAR LocationInformation[] = {'L','o','c','a','t','i','o','n','I','n','f','o','r','m','a','t','i','o','n',0};
static const WCHAR Capabilities[] = {'C','a','p','a','b','i','l','i','t','i','e','s',0};
static const WCHAR UINumber[] = {'U','I','N','u','m','b','e','r',0};
static const WCHAR UpperFilters[] = {'U','p','p','e','r','F','i','l','t','e','r','s',0};
static const WCHAR LowerFilters[] = {'L','o','w','e','r','F','i','l','t','e','r','s',0};
static const WCHAR Phantom[] = {'P','h','a','n','t','o','m',0};
static const WCHAR SymbolicLink[] = {'S','y','m','b','o','l','i','c','L','i','n','k',0};

/* is used to identify if a DeviceInfoSet pointer is
valid or not */
#define SETUP_DEVICE_INFO_SET_MAGIC 0xd00ff056

struct DeviceInfoSet
{
    DWORD magic;        /* if is equal to SETUP_DEVICE_INFO_SET_MAGIC struct is okay */
    GUID ClassGuid;
    HWND hwndParent;
    DWORD cDevices;
    struct list devices;
};

struct device
{
    struct DeviceInfoSet *set;
    HKEY                  key;
    BOOL                  phantom;
    WCHAR                *instanceId;
    struct list           interfaces;
    GUID                  class;
    DEVINST               devnode;
    struct list           entry;
};

struct device_iface
{
    WCHAR           *refstr;
    WCHAR           *symlink;
    struct device   *device;
    GUID             class;
    DWORD            flags;
    struct list      entry;
};

static inline void copy_device_data(SP_DEVINFO_DATA *data, const struct device *device)
{
    data->ClassGuid = device->class;
    data->DevInst = device->devnode;
    data->Reserved = (ULONG_PTR)device;
}

static inline void copy_device_iface_data(SP_DEVICE_INTERFACE_DATA *data,
    const struct device_iface *iface)
{
    data->InterfaceClassGuid = iface->class;
    data->Flags = iface->flags;
    data->Reserved = (ULONG_PTR)iface;
}

static struct device **devnode_table;
static unsigned int devnode_table_size;

static DEVINST alloc_devnode(struct device *device)
{
    unsigned int i;

    for (i = 0; i < devnode_table_size; ++i)
    {
        if (!devnode_table[i])
            break;
    }

    if (i == devnode_table_size)
    {
        if (devnode_table)
        {
            devnode_table_size *= 2;
            devnode_table = heap_realloc_zero(devnode_table,
                devnode_table_size * sizeof(*devnode_table));
        }
        else
        {
            devnode_table_size = 256;
            devnode_table = heap_alloc_zero(devnode_table_size * sizeof(*devnode_table));
        }
    }

    devnode_table[i] = device;
    return i;
}

static void free_devnode(DEVINST devnode)
{
    devnode_table[devnode] = NULL;
}

static struct device *get_devnode_device(DEVINST devnode)
{
    if (devnode < devnode_table_size)
        return devnode_table[devnode];

    WARN("device node %u not found\n", devnode);
    return NULL;
}

static void SETUPDI_GuidToString(const GUID *guid, LPWSTR guidStr)
{
    static const WCHAR fmt[] = {'{','%','0','8','X','-','%','0','4','X','-',
        '%','0','4','X','-','%','0','2','X','%','0','2','X','-','%','0','2',
        'X','%','0','2','X','%','0','2','X','%','0','2','X','%','0','2','X','%',
        '0','2','X','}',0};

    sprintfW(guidStr, fmt, guid->Data1, guid->Data2, guid->Data3,
        guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
        guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
}

static WCHAR *get_iface_key_path(struct device_iface *iface)
{
    const WCHAR slashW[] = {'\\',0};
    WCHAR *path, *ptr;
    size_t len = strlenW(DeviceClasses) + 1 + 38 + 1 + strlenW(iface->symlink);

    if (!(path = heap_alloc((len + 1) * sizeof(WCHAR))))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    strcpyW(path, DeviceClasses);
    strcatW(path, slashW);
    SETUPDI_GuidToString(&iface->class, path + strlenW(path));
    strcatW(path, slashW);
    ptr = path + strlenW(path);
    strcatW(path, iface->symlink);
    if (strlenW(iface->symlink) > 3)
        ptr[0] = ptr[1] = ptr[3] = '#';

    ptr = strchrW(ptr, '\\');
    if (ptr) *ptr = 0;

    return path;
}

static WCHAR *get_refstr_key_path(struct device_iface *iface)
{
    const WCHAR hashW[] = {'#',0};
    const WCHAR slashW[] = {'\\',0};
    WCHAR *path, *ptr;
    size_t len = strlenW(DeviceClasses) + 1 + 38 + 1 + strlenW(iface->symlink) + 1 + 1;

    if (iface->refstr)
        len += strlenW(iface->refstr);

    if (!(path = heap_alloc((len + 1) * sizeof(WCHAR))))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    strcpyW(path, DeviceClasses);
    strcatW(path, slashW);
    SETUPDI_GuidToString(&iface->class, path + strlenW(path));
    strcatW(path, slashW);
    ptr = path + strlenW(path);
    strcatW(path, iface->symlink);
    if (strlenW(iface->symlink) > 3)
        ptr[0] = ptr[1] = ptr[3] = '#';

    ptr = strchrW(ptr, '\\');
    if (ptr) *ptr = 0;

    strcatW(path, slashW);
    strcatW(path, hashW);

    if (iface->refstr)
        strcatW(path, iface->refstr);

    return path;
}

static LPWSTR SETUPDI_CreateSymbolicLinkPath(LPCWSTR instanceId,
        const GUID *InterfaceClassGuid, LPCWSTR ReferenceString)
{
    static const WCHAR fmt[] = {'\\','\\','?','\\','%','s','#','%','s',0};
    WCHAR guidStr[39];
    DWORD len;
    LPWSTR ret;

    SETUPDI_GuidToString(InterfaceClassGuid, guidStr);
    /* omit length of format specifiers, but include NULL terminator: */
    len = lstrlenW(fmt) - 4 + 1;
    len += lstrlenW(instanceId) + lstrlenW(guidStr);
    if (ReferenceString && *ReferenceString)
    {
        /* space for a hash between string and reference string: */
        len += lstrlenW(ReferenceString) + 1;
    }
    ret = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
    if (ret)
    {
        int printed = sprintfW(ret, fmt, instanceId, guidStr);
        LPWSTR ptr;

        /* replace '\\' with '#' after the "\\\\?\\" beginning */
        for (ptr = strchrW(ret + 4, '\\'); ptr; ptr = strchrW(ptr + 1, '\\'))
            *ptr = '#';
        if (ReferenceString && *ReferenceString)
        {
            ret[printed] = '\\';
            lstrcpyW(ret + printed + 1, ReferenceString);
        }
    }
    return ret;
}

static struct device_iface *SETUPDI_CreateDeviceInterface(struct device *device,
        const GUID *class, const WCHAR *refstr)
{
    struct device_iface *iface = NULL;
    WCHAR *refstr2 = NULL, *symlink = NULL, *path = NULL;
    HKEY key = NULL;
    LONG ret;

    TRACE("%p %s %s\n", device, debugstr_guid(class), debugstr_w(refstr));

    /* check if it already exists */
    LIST_FOR_EACH_ENTRY(iface, &device->interfaces, struct device_iface, entry)
    {
        if (IsEqualGUID(&iface->class, class) && !lstrcmpiW(iface->refstr, refstr))
            return iface;
    }

    iface = heap_alloc(sizeof(*iface));
    symlink = SETUPDI_CreateSymbolicLinkPath(device->instanceId, class, refstr);

    if (!iface || !symlink)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        goto err;
    }

    if (refstr && !(refstr2 = strdupW(refstr)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        goto err;
    }
    iface->refstr = refstr2;
    iface->symlink = symlink;
    iface->device = device;
    iface->class = *class;
    iface->flags = SPINT_ACTIVE; /* FIXME */

    if (!(path = get_iface_key_path(iface)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        goto err;
    }

    if ((ret = RegCreateKeyW(HKEY_LOCAL_MACHINE, path, &key)))
    {
        SetLastError(ret);
        goto err;
    }
    RegSetValueExW(key, DeviceInstance, 0, REG_SZ, (BYTE *)device->instanceId,
        lstrlenW(device->instanceId) * sizeof(WCHAR));
    RegCloseKey(key);
    heap_free(path);

    if (!(path = get_refstr_key_path(iface)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        goto err;
    }

    if ((ret = RegCreateKeyW(HKEY_LOCAL_MACHINE, path, &key)))
    {
        SetLastError(ret);
        goto err;
    }
    RegSetValueExW(key, SymbolicLink, 0, REG_SZ, (BYTE *)iface->symlink,
        lstrlenW(iface->symlink) * sizeof(WCHAR));
    RegCloseKey(key);
    heap_free(path);

    list_add_tail(&device->interfaces, &iface->entry);
    return iface;

err:
    heap_free(iface);
    heap_free(refstr2);
    heap_free(symlink);
    heap_free(path);
    return NULL;
}

static BOOL SETUPDI_SetInterfaceSymbolicLink(struct device_iface *iface,
    const WCHAR *symlink)
{
    heap_free(iface->symlink);
    if ((iface->symlink = strdupW(symlink)))
        return TRUE;
    return FALSE;
}

static HKEY SETUPDI_CreateDevKey(struct device *device)
{
    HKEY enumKey, key = INVALID_HANDLE_VALUE;
    LONG l;

    l = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Enum, 0, NULL, 0, KEY_ALL_ACCESS,
            NULL, &enumKey, NULL);
    if (!l)
    {
        RegCreateKeyExW(enumKey, device->instanceId, 0, NULL, 0,
                KEY_READ | KEY_WRITE, NULL, &key, NULL);
        RegCloseKey(enumKey);
    }
    return key;
}

static HKEY SETUPDI_CreateDrvKey(struct device *device)
{
    static const WCHAR slash[] = { '\\',0 };
    WCHAR classKeyPath[MAX_PATH];
    HKEY classKey, key = INVALID_HANDLE_VALUE;
    LONG l;

    lstrcpyW(classKeyPath, ControlClass);
    lstrcatW(classKeyPath, slash);
    SETUPDI_GuidToString(&device->set->ClassGuid,
            classKeyPath + lstrlenW(classKeyPath));
    l = RegCreateKeyExW(HKEY_LOCAL_MACHINE, classKeyPath, 0, NULL, 0,
            KEY_ALL_ACCESS, NULL, &classKey, NULL);
    if (!l)
    {
        static const WCHAR fmt[] = { '%','0','4','u',0 };
        WCHAR devId[10];

        sprintfW(devId, fmt, device->devnode);
        RegCreateKeyExW(classKey, devId, 0, NULL, 0, KEY_READ | KEY_WRITE,
                NULL, &key, NULL);
        RegCloseKey(classKey);
    }
    return key;
}

struct PropertyMapEntry
{
    DWORD   regType;
    LPCSTR  nameA;
    LPCWSTR nameW;
};

static const struct PropertyMapEntry PropertyMap[] = {
    { REG_SZ, "DeviceDesc", DeviceDesc },
    { REG_MULTI_SZ, "HardwareId", HardwareId },
    { REG_MULTI_SZ, "CompatibleIDs", CompatibleIDs },
    { 0, NULL, NULL }, /* SPDRP_UNUSED0 */
    { REG_SZ, "Service", Service },
    { 0, NULL, NULL }, /* SPDRP_UNUSED1 */
    { 0, NULL, NULL }, /* SPDRP_UNUSED2 */
    { REG_SZ, "Class", Class },
    { REG_SZ, "ClassGUID", ClassGUID },
    { REG_SZ, "Driver", Driver },
    { REG_DWORD, "ConfigFlags", ConfigFlags },
    { REG_SZ, "Mfg", Mfg },
    { REG_SZ, "FriendlyName", FriendlyName },
    { REG_SZ, "LocationInformation", LocationInformation },
    { 0, NULL, NULL }, /* SPDRP_PHYSICAL_DEVICE_OBJECT_NAME */
    { REG_DWORD, "Capabilities", Capabilities },
    { REG_DWORD, "UINumber", UINumber },
    { REG_MULTI_SZ, "UpperFilters", UpperFilters },
    { REG_MULTI_SZ, "LowerFilters", LowerFilters },
};

static BOOL SETUPDI_SetDeviceRegistryPropertyW(struct device *device,
    DWORD prop, const BYTE *buffer, DWORD size)
{
    if (prop < ARRAY_SIZE(PropertyMap) && PropertyMap[prop].nameW)
    {
        LONG ret = RegSetValueExW(device->key, PropertyMap[prop].nameW, 0,
                PropertyMap[prop].regType, buffer, size);
        if (!ret)
            return TRUE;

        SetLastError(ret);
    }
    return FALSE;
}

static void SETUPDI_RemoveDevice(struct device *device)
{
    struct device_iface *iface, *next;
    WCHAR *path;

    if (device->key != INVALID_HANDLE_VALUE)
        RegCloseKey(device->key);
    if (device->phantom)
    {
        HKEY enumKey;
        LONG l;

        l = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Enum, 0, NULL, 0,
                KEY_ALL_ACCESS, NULL, &enumKey, NULL);
        if (!l)
        {
            RegDeleteTreeW(enumKey, device->instanceId);
            RegCloseKey(enumKey);
        }
    }
    heap_free(device->instanceId);
    LIST_FOR_EACH_ENTRY_SAFE(iface, next, &device->interfaces,
            struct device_iface, entry)
    {
        list_remove(&iface->entry);
        if (device->phantom && (path = get_refstr_key_path(iface)))
        {
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);
            heap_free(path);
        }
        heap_free(iface->refstr);
        heap_free(iface->symlink);
        heap_free(iface);
    }
    free_devnode(device->devnode);
    list_remove(&device->entry);
    heap_free(device);
}

static struct device *SETUPDI_CreateDeviceInfo(struct DeviceInfoSet *set,
    const GUID *class, const WCHAR *instanceid, BOOL phantom)
{
    struct device *device;
    WCHAR guidstr[39];

    TRACE("%p, %s, %s, %d\n", set, debugstr_guid(class),
        debugstr_w(instanceid), phantom);

    if (!(device = heap_alloc(sizeof(*device))))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    if (!(device->instanceId = strdupW(instanceid)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        heap_free(device);
        return NULL;
    }

    struprW(device->instanceId);
    device->set = set;
    device->key = SETUPDI_CreateDevKey(device);
    device->phantom = phantom;
    list_init(&device->interfaces);
    device->class = *class;
    device->devnode = alloc_devnode(device);
    list_add_tail(&set->devices, &device->entry);
    set->cDevices++;

    SETUPDI_GuidToString(class, guidstr);
    SETUPDI_SetDeviceRegistryPropertyW(device, SPDRP_CLASSGUID,
        (const BYTE *)guidstr, sizeof(guidstr));
    return device;
}

/***********************************************************************
 *              SetupDiBuildClassInfoList  (SETUPAPI.@)
 *
 * Returns a list of setup class GUIDs that identify the classes
 * that are installed on a local machine.
 *
 * PARAMS
 *   Flags [I] control exclusion of classes from the list.
 *   ClassGuidList [O] pointer to a GUID-typed array that receives a list of setup class GUIDs.
 *   ClassGuidListSize [I] The number of GUIDs in the array (ClassGuidList).
 *   RequiredSize [O] pointer, which receives the number of GUIDs that are returned.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE.
 */
BOOL WINAPI SetupDiBuildClassInfoList(
        DWORD Flags,
        LPGUID ClassGuidList,
        DWORD ClassGuidListSize,
        PDWORD RequiredSize)
{
    TRACE("\n");
    return SetupDiBuildClassInfoListExW(Flags, ClassGuidList,
                                        ClassGuidListSize, RequiredSize,
                                        NULL, NULL);
}

/***********************************************************************
 *              SetupDiBuildClassInfoListExA  (SETUPAPI.@)
 *
 * Returns a list of setup class GUIDs that identify the classes
 * that are installed on a local or remote machine.
 *
 * PARAMS
 *   Flags [I] control exclusion of classes from the list.
 *   ClassGuidList [O] pointer to a GUID-typed array that receives a list of setup class GUIDs.
 *   ClassGuidListSize [I] The number of GUIDs in the array (ClassGuidList).
 *   RequiredSize [O] pointer, which receives the number of GUIDs that are returned.
 *   MachineName [I] name of a remote machine.
 *   Reserved [I] must be NULL.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE.
 */
BOOL WINAPI SetupDiBuildClassInfoListExA(
        DWORD Flags,
        LPGUID ClassGuidList,
        DWORD ClassGuidListSize,
        PDWORD RequiredSize,
        LPCSTR MachineName,
        PVOID Reserved)
{
    LPWSTR MachineNameW = NULL;
    BOOL bResult;

    TRACE("\n");

    if (MachineName)
    {
        MachineNameW = MultiByteToUnicode(MachineName, CP_ACP);
        if (MachineNameW == NULL) return FALSE;
    }

    bResult = SetupDiBuildClassInfoListExW(Flags, ClassGuidList,
                                           ClassGuidListSize, RequiredSize,
                                           MachineNameW, Reserved);

    MyFree(MachineNameW);

    return bResult;
}

/***********************************************************************
 *              SetupDiBuildClassInfoListExW  (SETUPAPI.@)
 *
 * Returns a list of setup class GUIDs that identify the classes
 * that are installed on a local or remote machine.
 *
 * PARAMS
 *   Flags [I] control exclusion of classes from the list.
 *   ClassGuidList [O] pointer to a GUID-typed array that receives a list of setup class GUIDs.
 *   ClassGuidListSize [I] The number of GUIDs in the array (ClassGuidList).
 *   RequiredSize [O] pointer, which receives the number of GUIDs that are returned.
 *   MachineName [I] name of a remote machine.
 *   Reserved [I] must be NULL.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE.
 */
BOOL WINAPI SetupDiBuildClassInfoListExW(
        DWORD Flags,
        LPGUID ClassGuidList,
        DWORD ClassGuidListSize,
        PDWORD RequiredSize,
        LPCWSTR MachineName,
        PVOID Reserved)
{
    WCHAR szKeyName[40];
    HKEY hClassesKey;
    HKEY hClassKey;
    DWORD dwLength;
    DWORD dwIndex;
    LONG lError;
    DWORD dwGuidListIndex = 0;

    TRACE("\n");

    if (RequiredSize != NULL)
	*RequiredSize = 0;

    hClassesKey = SetupDiOpenClassRegKeyExW(NULL,
                                            KEY_ALL_ACCESS,
                                            DIOCR_INSTALLER,
                                            MachineName,
                                            Reserved);
    if (hClassesKey == INVALID_HANDLE_VALUE)
    {
	return FALSE;
    }

    for (dwIndex = 0; ; dwIndex++)
    {
	dwLength = 40;
	lError = RegEnumKeyExW(hClassesKey,
			       dwIndex,
			       szKeyName,
			       &dwLength,
			       NULL,
			       NULL,
			       NULL,
			       NULL);
	TRACE("RegEnumKeyExW() returns %d\n", lError);
	if (lError == ERROR_SUCCESS || lError == ERROR_MORE_DATA)
	{
	    TRACE("Key name: %p\n", szKeyName);

	    if (RegOpenKeyExW(hClassesKey,
			      szKeyName,
			      0,
			      KEY_ALL_ACCESS,
			      &hClassKey))
	    {
		RegCloseKey(hClassesKey);
		return FALSE;
	    }

	    if (!RegQueryValueExW(hClassKey,
				  NoUseClass,
				  NULL,
				  NULL,
				  NULL,
				  NULL))
	    {
		TRACE("'NoUseClass' value found!\n");
		RegCloseKey(hClassKey);
		continue;
	    }

	    if ((Flags & DIBCI_NOINSTALLCLASS) &&
		(!RegQueryValueExW(hClassKey,
				   NoInstallClass,
				   NULL,
				   NULL,
				   NULL,
				   NULL)))
	    {
		TRACE("'NoInstallClass' value found!\n");
		RegCloseKey(hClassKey);
		continue;
	    }

	    if ((Flags & DIBCI_NODISPLAYCLASS) &&
		(!RegQueryValueExW(hClassKey,
				   NoDisplayClass,
				   NULL,
				   NULL,
				   NULL,
				   NULL)))
	    {
		TRACE("'NoDisplayClass' value found!\n");
		RegCloseKey(hClassKey);
		continue;
	    }

	    RegCloseKey(hClassKey);

	    TRACE("Guid: %p\n", szKeyName);
	    if (dwGuidListIndex < ClassGuidListSize)
	    {
		if (szKeyName[0] == '{' && szKeyName[37] == '}')
		{
		    szKeyName[37] = 0;
		}
		TRACE("Guid: %p\n", &szKeyName[1]);

		UuidFromStringW(&szKeyName[1],
				&ClassGuidList[dwGuidListIndex]);
	    }

	    dwGuidListIndex++;
	}

	if (lError != ERROR_SUCCESS)
	    break;
    }

    RegCloseKey(hClassesKey);

    if (RequiredSize != NULL)
	*RequiredSize = dwGuidListIndex;

    if (ClassGuidListSize < dwGuidListIndex)
    {
	SetLastError(ERROR_INSUFFICIENT_BUFFER);
	return FALSE;
    }

    return TRUE;
}

/***********************************************************************
 *		SetupDiClassGuidsFromNameA  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiClassGuidsFromNameA(
        LPCSTR ClassName,
        LPGUID ClassGuidList,
        DWORD ClassGuidListSize,
        PDWORD RequiredSize)
{
  return SetupDiClassGuidsFromNameExA(ClassName, ClassGuidList,
                                      ClassGuidListSize, RequiredSize,
                                      NULL, NULL);
}

/***********************************************************************
 *		SetupDiClassGuidsFromNameW  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiClassGuidsFromNameW(
        LPCWSTR ClassName,
        LPGUID ClassGuidList,
        DWORD ClassGuidListSize,
        PDWORD RequiredSize)
{
  return SetupDiClassGuidsFromNameExW(ClassName, ClassGuidList,
                                      ClassGuidListSize, RequiredSize,
                                      NULL, NULL);
}

/***********************************************************************
 *		SetupDiClassGuidsFromNameExA  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiClassGuidsFromNameExA(
        LPCSTR ClassName,
        LPGUID ClassGuidList,
        DWORD ClassGuidListSize,
        PDWORD RequiredSize,
        LPCSTR MachineName,
        PVOID Reserved)
{
    LPWSTR ClassNameW = NULL;
    LPWSTR MachineNameW = NULL;
    BOOL bResult;

    ClassNameW = MultiByteToUnicode(ClassName, CP_ACP);
    if (ClassNameW == NULL)
        return FALSE;

    if (MachineName)
    {
        MachineNameW = MultiByteToUnicode(MachineName, CP_ACP);
        if (MachineNameW == NULL)
        {
            MyFree(ClassNameW);
            return FALSE;
        }
    }

    bResult = SetupDiClassGuidsFromNameExW(ClassNameW, ClassGuidList,
                                           ClassGuidListSize, RequiredSize,
                                           MachineNameW, Reserved);

    MyFree(MachineNameW);
    MyFree(ClassNameW);

    return bResult;
}

/***********************************************************************
 *		SetupDiClassGuidsFromNameExW  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiClassGuidsFromNameExW(
        LPCWSTR ClassName,
        LPGUID ClassGuidList,
        DWORD ClassGuidListSize,
        PDWORD RequiredSize,
        LPCWSTR MachineName,
        PVOID Reserved)
{
    WCHAR szKeyName[40];
    WCHAR szClassName[256];
    HKEY hClassesKey;
    HKEY hClassKey;
    DWORD dwLength;
    DWORD dwIndex;
    LONG lError;
    DWORD dwGuidListIndex = 0;

    if (RequiredSize != NULL)
	*RequiredSize = 0;

    hClassesKey = SetupDiOpenClassRegKeyExW(NULL,
                                            KEY_ALL_ACCESS,
                                            DIOCR_INSTALLER,
                                            MachineName,
                                            Reserved);
    if (hClassesKey == INVALID_HANDLE_VALUE)
    {
	return FALSE;
    }

    for (dwIndex = 0; ; dwIndex++)
    {
	dwLength = sizeof(szKeyName) / sizeof(WCHAR);
	lError = RegEnumKeyExW(hClassesKey,
			       dwIndex,
			       szKeyName,
			       &dwLength,
			       NULL,
			       NULL,
			       NULL,
			       NULL);
	TRACE("RegEnumKeyExW() returns %d\n", lError);
	if (lError == ERROR_SUCCESS || lError == ERROR_MORE_DATA)
	{
	    TRACE("Key name: %p\n", szKeyName);

	    if (RegOpenKeyExW(hClassesKey,
			      szKeyName,
			      0,
			      KEY_ALL_ACCESS,
			      &hClassKey))
	    {
		RegCloseKey(hClassesKey);
		return FALSE;
	    }

	    dwLength = sizeof(szClassName);
	    if (!RegQueryValueExW(hClassKey,
				  Class,
				  NULL,
				  NULL,
				  (LPBYTE)szClassName,
				  &dwLength))
	    {
		TRACE("Class name: %p\n", szClassName);

		if (strcmpiW(szClassName, ClassName) == 0)
		{
		    TRACE("Found matching class name\n");

		    TRACE("Guid: %p\n", szKeyName);
		    if (dwGuidListIndex < ClassGuidListSize)
		    {
			if (szKeyName[0] == '{' && szKeyName[37] == '}')
			{
			    szKeyName[37] = 0;
			}
			TRACE("Guid: %p\n", &szKeyName[1]);

			UuidFromStringW(&szKeyName[1],
					&ClassGuidList[dwGuidListIndex]);
		    }

		    dwGuidListIndex++;
		}
	    }

	    RegCloseKey(hClassKey);
	}

	if (lError != ERROR_SUCCESS)
	    break;
    }

    RegCloseKey(hClassesKey);

    if (RequiredSize != NULL)
	*RequiredSize = dwGuidListIndex;

    if (ClassGuidListSize < dwGuidListIndex)
    {
	SetLastError(ERROR_INSUFFICIENT_BUFFER);
	return FALSE;
    }

    return TRUE;
}

/***********************************************************************
 *              SetupDiClassNameFromGuidA  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiClassNameFromGuidA(
        const GUID* ClassGuid,
        PSTR ClassName,
        DWORD ClassNameSize,
        PDWORD RequiredSize)
{
  return SetupDiClassNameFromGuidExA(ClassGuid, ClassName,
                                     ClassNameSize, RequiredSize,
                                     NULL, NULL);
}

/***********************************************************************
 *              SetupDiClassNameFromGuidW  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiClassNameFromGuidW(
        const GUID* ClassGuid,
        PWSTR ClassName,
        DWORD ClassNameSize,
        PDWORD RequiredSize)
{
  return SetupDiClassNameFromGuidExW(ClassGuid, ClassName,
                                     ClassNameSize, RequiredSize,
                                     NULL, NULL);
}

/***********************************************************************
 *              SetupDiClassNameFromGuidExA  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiClassNameFromGuidExA(
        const GUID* ClassGuid,
        PSTR ClassName,
        DWORD ClassNameSize,
        PDWORD RequiredSize,
        PCSTR MachineName,
        PVOID Reserved)
{
    WCHAR ClassNameW[MAX_CLASS_NAME_LEN];
    LPWSTR MachineNameW = NULL;
    BOOL ret;

    if (MachineName)
        MachineNameW = MultiByteToUnicode(MachineName, CP_ACP);
    ret = SetupDiClassNameFromGuidExW(ClassGuid, ClassNameW, MAX_CLASS_NAME_LEN,
     NULL, MachineNameW, Reserved);
    if (ret)
    {
        int len = WideCharToMultiByte(CP_ACP, 0, ClassNameW, -1, ClassName,
         ClassNameSize, NULL, NULL);

        if (!ClassNameSize && RequiredSize)
            *RequiredSize = len;
    }
    MyFree(MachineNameW);
    return ret;
}

/***********************************************************************
 *		SetupDiClassNameFromGuidExW  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiClassNameFromGuidExW(
        const GUID* ClassGuid,
        PWSTR ClassName,
        DWORD ClassNameSize,
        PDWORD RequiredSize,
        PCWSTR MachineName,
        PVOID Reserved)
{
    HKEY hKey;
    DWORD dwLength;

    hKey = SetupDiOpenClassRegKeyExW(ClassGuid,
                                     KEY_ALL_ACCESS,
                                     DIOCR_INSTALLER,
                                     MachineName,
                                     Reserved);
    if (hKey == INVALID_HANDLE_VALUE)
    {
	return FALSE;
    }

    if (RequiredSize != NULL)
    {
	dwLength = 0;
	if (RegQueryValueExW(hKey,
			     Class,
			     NULL,
			     NULL,
			     NULL,
			     &dwLength))
	{
	    RegCloseKey(hKey);
	    return FALSE;
	}

	*RequiredSize = dwLength / sizeof(WCHAR);
    }

    dwLength = ClassNameSize * sizeof(WCHAR);
    if (RegQueryValueExW(hKey,
			 Class,
			 NULL,
			 NULL,
			 (LPBYTE)ClassName,
			 &dwLength))
    {
	RegCloseKey(hKey);
	return FALSE;
    }

    RegCloseKey(hKey);

    return TRUE;
}

/***********************************************************************
 *		SetupDiCreateDeviceInfoList (SETUPAPI.@)
 */
HDEVINFO WINAPI
SetupDiCreateDeviceInfoList(const GUID *ClassGuid,
			    HWND hwndParent)
{
  return SetupDiCreateDeviceInfoListExW(ClassGuid, hwndParent, NULL, NULL);
}

/***********************************************************************
 *		SetupDiCreateDeviceInfoListExA (SETUPAPI.@)
 */
HDEVINFO WINAPI
SetupDiCreateDeviceInfoListExA(const GUID *ClassGuid,
			       HWND hwndParent,
			       PCSTR MachineName,
			       PVOID Reserved)
{
    LPWSTR MachineNameW = NULL;
    HDEVINFO hDevInfo;

    TRACE("\n");

    if (MachineName)
    {
        MachineNameW = MultiByteToUnicode(MachineName, CP_ACP);
        if (MachineNameW == NULL)
            return INVALID_HANDLE_VALUE;
    }

    hDevInfo = SetupDiCreateDeviceInfoListExW(ClassGuid, hwndParent,
                                              MachineNameW, Reserved);

    MyFree(MachineNameW);

    return hDevInfo;
}

/***********************************************************************
 *		SetupDiCreateDeviceInfoListExW (SETUPAPI.@)
 *
 * Create an empty DeviceInfoSet list.
 *
 * PARAMS
 *   ClassGuid [I] if not NULL only devices with GUID ClassGuid are associated
 *                 with this list.
 *   hwndParent [I] hwnd needed for interface related actions.
 *   MachineName [I] name of machine to create empty DeviceInfoSet list, if NULL
 *                   local registry will be used.
 *   Reserved [I] must be NULL
 *
 * RETURNS
 *   Success: empty list.
 *   Failure: INVALID_HANDLE_VALUE.
 */
HDEVINFO WINAPI
SetupDiCreateDeviceInfoListExW(const GUID *ClassGuid,
			       HWND hwndParent,
			       PCWSTR MachineName,
			       PVOID Reserved)
{
    struct DeviceInfoSet *list = NULL;
    DWORD size = sizeof(struct DeviceInfoSet);

    TRACE("%s %p %s %p\n", debugstr_guid(ClassGuid), hwndParent,
      debugstr_w(MachineName), Reserved);

    if (MachineName && *MachineName)
    {
        FIXME("remote support is not implemented\n");
        SetLastError(ERROR_INVALID_MACHINENAME);
        return INVALID_HANDLE_VALUE;
    }

    if (Reserved != NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    list = HeapAlloc(GetProcessHeap(), 0, size);
    if (!list)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    list->magic = SETUP_DEVICE_INFO_SET_MAGIC;
    list->hwndParent = hwndParent;
    memcpy(&list->ClassGuid,
            ClassGuid ? ClassGuid : &GUID_NULL,
            sizeof(list->ClassGuid));
    list->cDevices = 0;
    list_init(&list->devices);

    return list;
}

/***********************************************************************
 *              SetupDiCreateDevRegKeyA (SETUPAPI.@)
 */
HKEY WINAPI SetupDiCreateDevRegKeyA(
        HDEVINFO DeviceInfoSet,
        PSP_DEVINFO_DATA DeviceInfoData,
        DWORD Scope,
        DWORD HwProfile,
        DWORD KeyType,
        HINF InfHandle,
        PCSTR InfSectionName)
{
    PWSTR InfSectionNameW = NULL;
    HKEY key;

    TRACE("%p %p %d %d %d %p %s\n", DeviceInfoSet, DeviceInfoData, Scope,
            HwProfile, KeyType, InfHandle, debugstr_a(InfSectionName));

    if (InfHandle)
    {
        if (!InfSectionName)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
        }
        else
        {
            InfSectionNameW = MultiByteToUnicode(InfSectionName, CP_ACP);
            if (InfSectionNameW == NULL) return INVALID_HANDLE_VALUE;
        }
    }
    key = SetupDiCreateDevRegKeyW(DeviceInfoSet, DeviceInfoData, Scope,
            HwProfile, KeyType, InfHandle, InfSectionNameW);
    MyFree(InfSectionNameW);
    return key;
}

/***********************************************************************
 *              SetupDiCreateDevRegKeyW (SETUPAPI.@)
 */
HKEY WINAPI SetupDiCreateDevRegKeyW(
        HDEVINFO DeviceInfoSet,
        PSP_DEVINFO_DATA DeviceInfoData,
        DWORD Scope,
        DWORD HwProfile,
        DWORD KeyType,
        HINF InfHandle,
        PCWSTR InfSectionName)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device *device;
    HKEY key = INVALID_HANDLE_VALUE;

    TRACE("%p %p %d %d %d %p %s\n", DeviceInfoSet, DeviceInfoData, Scope,
            HwProfile, KeyType, InfHandle, debugstr_w(InfSectionName));

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    if (!DeviceInfoData || DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)
            || !DeviceInfoData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    device = (struct device *)DeviceInfoData->Reserved;
    if (device->set != set)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (Scope != DICS_FLAG_GLOBAL && Scope != DICS_FLAG_CONFIGSPECIFIC)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return INVALID_HANDLE_VALUE;
    }
    if (KeyType != DIREG_DEV && KeyType != DIREG_DRV)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return INVALID_HANDLE_VALUE;
    }
    if (device->phantom)
    {
        SetLastError(ERROR_DEVINFO_NOT_REGISTERED);
        return INVALID_HANDLE_VALUE;
    }
    if (Scope != DICS_FLAG_GLOBAL)
        FIXME("unimplemented for scope %d\n", Scope);
    switch (KeyType)
    {
        case DIREG_DEV:
            key = SETUPDI_CreateDevKey(device);
            break;
        case DIREG_DRV:
            key = SETUPDI_CreateDrvKey(device);
            break;
        default:
            WARN("unknown KeyType %d\n", KeyType);
    }
    if (InfHandle)
        SetupInstallFromInfSectionW(NULL, InfHandle, InfSectionName, SPINST_ALL,
                NULL, NULL, SP_COPY_NEWER_ONLY, NULL, NULL, DeviceInfoSet,
                DeviceInfoData);
    return key;
}

/***********************************************************************
 *              SetupDiCreateDeviceInfoA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiCreateDeviceInfoA(HDEVINFO DeviceInfoSet, PCSTR DeviceName,
        const GUID *ClassGuid, PCSTR DeviceDescription, HWND hwndParent, DWORD CreationFlags,
        PSP_DEVINFO_DATA DeviceInfoData)
{
    BOOL ret = FALSE;
    LPWSTR DeviceNameW = NULL;
    LPWSTR DeviceDescriptionW = NULL;

    if (DeviceName)
    {
        DeviceNameW = MultiByteToUnicode(DeviceName, CP_ACP);
        if (DeviceNameW == NULL) return FALSE;
    }
    if (DeviceDescription)
    {
        DeviceDescriptionW = MultiByteToUnicode(DeviceDescription, CP_ACP);
        if (DeviceDescriptionW == NULL)
        {
            MyFree(DeviceNameW);
            return FALSE;
        }
    }

    ret = SetupDiCreateDeviceInfoW(DeviceInfoSet, DeviceNameW, ClassGuid, DeviceDescriptionW,
            hwndParent, CreationFlags, DeviceInfoData);

    MyFree(DeviceNameW);
    MyFree(DeviceDescriptionW);

    return ret;
}

static DWORD SETUPDI_DevNameToDevID(LPCWSTR devName)
{
    LPCWSTR ptr;
    int devNameLen = lstrlenW(devName);
    DWORD devInst = 0;
    BOOL valid = TRUE;

    TRACE("%s\n", debugstr_w(devName));
    for (ptr = devName; valid && *ptr && ptr - devName < devNameLen; )
    {
	if (isdigitW(*ptr))
	{
	    devInst *= 10;
	    devInst |= *ptr - '0';
	    ptr++;
	}
	else
	    valid = FALSE;
    }
    TRACE("%d\n", valid ? devInst : 0xffffffff);
    return valid ? devInst : 0xffffffff;
}

/***********************************************************************
 *              SetupDiCreateDeviceInfoW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiCreateDeviceInfoW(HDEVINFO DeviceInfoSet, PCWSTR DeviceName,
        const GUID *ClassGuid, PCWSTR DeviceDescription, HWND hwndParent, DWORD CreationFlags,
        SP_DEVINFO_DATA *device_data)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    BOOL ret = FALSE, allocatedInstanceId = FALSE;
    LPCWSTR instanceId = NULL;

    TRACE("%p %s %s %s %p %x %p\n", DeviceInfoSet, debugstr_w(DeviceName),
        debugstr_guid(ClassGuid), debugstr_w(DeviceDescription),
        hwndParent, CreationFlags, device_data);

    if (!DeviceName)
    {
        SetLastError(ERROR_INVALID_DEVINST_NAME);
        return FALSE;
    }
    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!ClassGuid)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!IsEqualGUID(&set->ClassGuid, &GUID_NULL) &&
        !IsEqualGUID(ClassGuid, &set->ClassGuid))
    {
        SetLastError(ERROR_CLASS_MISMATCH);
        return FALSE;
    }
    if ((CreationFlags & DICD_GENERATE_ID))
    {
        if (strchrW(DeviceName, '\\'))
            SetLastError(ERROR_INVALID_DEVINST_NAME);
        else
        {
            static const WCHAR newDeviceFmt[] = {'R','O','O','T','\\','%','s',
                '\\','%','0','4','d',0};
            DWORD devId;

            if (set->cDevices)
            {
                DWORD highestDevID = 0;
                struct device *device;

                LIST_FOR_EACH_ENTRY(device, &set->devices, struct device, entry)
                {
                    const WCHAR *devName = strrchrW(device->instanceId, '\\');
                    DWORD id;

                    if (devName)
                        devName++;
                    else
                        devName = device->instanceId;
                    id = SETUPDI_DevNameToDevID(devName);
                    if (id != 0xffffffff && id > highestDevID)
                        highestDevID = id;
                }
                devId = highestDevID + 1;
            }
            else
                devId = 0;
            /* 17 == lstrlenW(L"Root\\") + lstrlenW("\\") + 1 + %d max size */
            instanceId = HeapAlloc(GetProcessHeap(), 0,
                    (17 + lstrlenW(DeviceName)) * sizeof(WCHAR));
            if (instanceId)
            {
                sprintfW((LPWSTR)instanceId, newDeviceFmt, DeviceName,
                        devId);
                allocatedInstanceId = TRUE;
                ret = TRUE;
            }
            else
                ret = FALSE;
        }
    }
    else
    {
        struct device *device;

        ret = TRUE;
        instanceId = DeviceName;
        LIST_FOR_EACH_ENTRY(device, &set->devices, struct device, entry)
        {
            if (!lstrcmpiW(DeviceName, device->instanceId))
            {
                SetLastError(ERROR_DEVINST_ALREADY_EXISTS);
                ret = FALSE;
            }
        }
    }
    if (ret)
    {
        struct device *device = NULL;

        if ((device = SETUPDI_CreateDeviceInfo(set, ClassGuid, instanceId, TRUE)))
        {
            if (DeviceDescription)
                SETUPDI_SetDeviceRegistryPropertyW(device, SPDRP_DEVICEDESC,
                    (const BYTE *)DeviceDescription,
                    lstrlenW(DeviceDescription) * sizeof(WCHAR));
            if (device_data)
            {
                if (device_data->cbSize != sizeof(SP_DEVINFO_DATA))
                {
                    SetLastError(ERROR_INVALID_USER_BUFFER);
                    ret = FALSE;
                }
                else
                    copy_device_data(device_data, device);
            }
        }
    }
    if (allocatedInstanceId)
        HeapFree(GetProcessHeap(), 0, (LPWSTR)instanceId);

    return ret;
}

/***********************************************************************
 *		SetupDiRegisterDeviceInfo (SETUPAPI.@)
 */
BOOL WINAPI SetupDiRegisterDeviceInfo(
        HDEVINFO DeviceInfoSet,
        PSP_DEVINFO_DATA DeviceInfoData,
        DWORD Flags,
        PSP_DETSIG_CMPPROC CompareProc,
        PVOID CompareContext,
        PSP_DEVINFO_DATA DupDeviceInfoData)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device *device;

    TRACE("%p %p %08x %p %p %p\n", DeviceInfoSet, DeviceInfoData, Flags,
            CompareProc, CompareContext, DupDeviceInfoData);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DeviceInfoData || DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)
            || !DeviceInfoData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    device = (struct device *)DeviceInfoData->Reserved;
    if (device->set != set)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (device->phantom)
    {
        device->phantom = FALSE;
        RegDeleteValueW(device->key, Phantom);
    }
    return TRUE;
}

/***********************************************************************
 *              SetupDiRemoveDevice (SETUPAPI.@)
 */
BOOL WINAPI SetupDiRemoveDevice(
        HDEVINFO devinfo,
        PSP_DEVINFO_DATA info)
{
    FIXME("(%p, %p): stub\n", devinfo, info);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *              SetupDiRemoveDeviceInterface (SETUPAPI.@)
 */
BOOL WINAPI SetupDiRemoveDeviceInterface(HDEVINFO info, PSP_DEVICE_INTERFACE_DATA data)
{
    FIXME("(%p, %p): stub\n", info, data);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *		SetupDiEnumDeviceInfo (SETUPAPI.@)
 */
BOOL WINAPI SetupDiEnumDeviceInfo(
        HDEVINFO  devinfo,
        DWORD  index,
        PSP_DEVINFO_DATA info)
{
    BOOL ret = FALSE;

    TRACE("%p %d %p\n", devinfo, index, info);

    if(info==NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (devinfo && devinfo != INVALID_HANDLE_VALUE)
    {
        struct DeviceInfoSet *list = devinfo;
        if (list->magic == SETUP_DEVICE_INFO_SET_MAGIC)
        {
            if (index < list->cDevices)
            {
                if (info->cbSize == sizeof(SP_DEVINFO_DATA))
                {
                    struct device *device;
                    DWORD i = 0;

                    LIST_FOR_EACH_ENTRY(device, &list->devices, struct device, entry)
                    {
                        if (i++ == index)
                        {
                            copy_device_data(info, device);
                            break;
                        }
                    }
                    ret = TRUE;
                }
                else
                    SetLastError(ERROR_INVALID_USER_BUFFER);
            }
            else
                SetLastError(ERROR_NO_MORE_ITEMS);
        }
        else
            SetLastError(ERROR_INVALID_HANDLE);
    }
    else
        SetLastError(ERROR_INVALID_HANDLE);
    return ret;
}

/***********************************************************************
 *		SetupDiGetDeviceInstanceIdA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDeviceInstanceIdA(
	HDEVINFO DeviceInfoSet,
	PSP_DEVINFO_DATA DeviceInfoData,
	PSTR DeviceInstanceId,
	DWORD DeviceInstanceIdSize,
	PDWORD RequiredSize)
{
    BOOL ret = FALSE;
    DWORD size;
    PWSTR instanceId;

    TRACE("%p %p %p %d %p\n", DeviceInfoSet, DeviceInfoData, DeviceInstanceId,
	    DeviceInstanceIdSize, RequiredSize);

    SetupDiGetDeviceInstanceIdW(DeviceInfoSet,
                                DeviceInfoData,
                                NULL,
                                0,
                                &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return FALSE;
    instanceId = HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR));
    if (instanceId)
    {
        ret = SetupDiGetDeviceInstanceIdW(DeviceInfoSet,
                                          DeviceInfoData,
                                          instanceId,
                                          size,
                                          &size);
        if (ret)
        {
            int len = WideCharToMultiByte(CP_ACP, 0, instanceId, -1,
                                          DeviceInstanceId,
                                          DeviceInstanceIdSize, NULL, NULL);

            if (!len)
                ret = FALSE;
            else
            {
                if (len > DeviceInstanceIdSize)
                {
                    SetLastError(ERROR_INSUFFICIENT_BUFFER);
                    ret = FALSE;
                }
                if (RequiredSize)
                    *RequiredSize = len;
            }
        }
        HeapFree(GetProcessHeap(), 0, instanceId);
    }
    return ret;
}

/***********************************************************************
 *		SetupDiGetDeviceInstanceIdW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDeviceInstanceIdW(
	HDEVINFO DeviceInfoSet,
	PSP_DEVINFO_DATA DeviceInfoData,
	PWSTR DeviceInstanceId,
	DWORD DeviceInstanceIdSize,
	PDWORD RequiredSize)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device *device;

    TRACE("%p %p %p %d %p\n", DeviceInfoSet, DeviceInfoData, DeviceInstanceId,
	    DeviceInstanceIdSize, RequiredSize);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DeviceInfoData || DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)
            || !DeviceInfoData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    device = (struct device *)DeviceInfoData->Reserved;
    if (device->set != set)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    TRACE("instance ID: %s\n", debugstr_w(device->instanceId));
    if (DeviceInstanceIdSize < strlenW(device->instanceId) + 1)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        if (RequiredSize)
            *RequiredSize = lstrlenW(device->instanceId) + 1;
        return FALSE;
    }
    lstrcpyW(DeviceInstanceId, device->instanceId);
    if (RequiredSize)
        *RequiredSize = lstrlenW(device->instanceId) + 1;
    return TRUE;
}

/***********************************************************************
 *		SetupDiGetActualSectionToInstallA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetActualSectionToInstallA(
        HINF InfHandle,
        PCSTR InfSectionName,
        PSTR InfSectionWithExt,
        DWORD InfSectionWithExtSize,
        PDWORD RequiredSize,
        PSTR *Extension)
{
    FIXME("\n");
    return FALSE;
}

/***********************************************************************
 *		SetupDiGetActualSectionToInstallW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetActualSectionToInstallW(
        HINF InfHandle,
        PCWSTR InfSectionName,
        PWSTR InfSectionWithExt,
        DWORD InfSectionWithExtSize,
        PDWORD RequiredSize,
        PWSTR *Extension)
{
    WCHAR szBuffer[MAX_PATH];
    DWORD dwLength;
    DWORD dwFullLength;
    LONG lLineCount = -1;

    lstrcpyW(szBuffer, InfSectionName);
    dwLength = lstrlenW(szBuffer);

    if (OsVersionInfo.dwPlatformId == VER_PLATFORM_WIN32_NT)
    {
	/* Test section name with '.NTx86' extension */
	lstrcpyW(&szBuffer[dwLength], NtPlatformExtension);
	lLineCount = SetupGetLineCountW(InfHandle, szBuffer);

	if (lLineCount == -1)
	{
	    /* Test section name with '.NT' extension */
	    lstrcpyW(&szBuffer[dwLength], NtExtension);
	    lLineCount = SetupGetLineCountW(InfHandle, szBuffer);
	}
    }
    else
    {
	/* Test section name with '.Win' extension */
	lstrcpyW(&szBuffer[dwLength], WinExtension);
	lLineCount = SetupGetLineCountW(InfHandle, szBuffer);
    }

    if (lLineCount == -1)
    {
	/* Test section name without extension */
	szBuffer[dwLength] = 0;
	lLineCount = SetupGetLineCountW(InfHandle, szBuffer);
    }

    if (lLineCount == -1)
    {
	SetLastError(ERROR_INVALID_PARAMETER);
	return FALSE;
    }

    dwFullLength = lstrlenW(szBuffer);

    if (InfSectionWithExt != NULL && InfSectionWithExtSize != 0)
    {
	if (InfSectionWithExtSize < (dwFullLength + 1))
	{
	    SetLastError(ERROR_INSUFFICIENT_BUFFER);
	    return FALSE;
	}

	lstrcpyW(InfSectionWithExt, szBuffer);
	if (Extension != NULL)
	{
	    *Extension = (dwLength == dwFullLength) ? NULL : &InfSectionWithExt[dwLength];
	}
    }

    if (RequiredSize != NULL)
    {
	*RequiredSize = dwFullLength + 1;
    }

    return TRUE;
}

/***********************************************************************
 *		SetupDiGetClassDescriptionA  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetClassDescriptionA(
        const GUID* ClassGuid,
        PSTR ClassDescription,
        DWORD ClassDescriptionSize,
        PDWORD RequiredSize)
{
  return SetupDiGetClassDescriptionExA(ClassGuid, ClassDescription,
                                       ClassDescriptionSize,
                                       RequiredSize, NULL, NULL);
}

/***********************************************************************
 *		SetupDiGetClassDescriptionW  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetClassDescriptionW(
        const GUID* ClassGuid,
        PWSTR ClassDescription,
        DWORD ClassDescriptionSize,
        PDWORD RequiredSize)
{
  return SetupDiGetClassDescriptionExW(ClassGuid, ClassDescription,
                                       ClassDescriptionSize,
                                       RequiredSize, NULL, NULL);
}

/***********************************************************************
 *		SetupDiGetClassDescriptionExA  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetClassDescriptionExA(
        const GUID* ClassGuid,
        PSTR ClassDescription,
        DWORD ClassDescriptionSize,
        PDWORD RequiredSize,
        PCSTR MachineName,
        PVOID Reserved)
{
    HKEY hKey;
    DWORD dwLength;
    BOOL ret;

    hKey = SetupDiOpenClassRegKeyExA(ClassGuid,
                                     KEY_ALL_ACCESS,
                                     DIOCR_INSTALLER,
                                     MachineName,
                                     Reserved);
    if (hKey == INVALID_HANDLE_VALUE)
    {
	WARN("SetupDiOpenClassRegKeyExA() failed (Error %u)\n", GetLastError());
	return FALSE;
    }

    dwLength = ClassDescriptionSize;
    ret = !RegQueryValueExA( hKey, NULL, NULL, NULL,
                             (LPBYTE)ClassDescription, &dwLength );
    if (RequiredSize) *RequiredSize = dwLength;
    RegCloseKey(hKey);
    return ret;
}

/***********************************************************************
 *		SetupDiGetClassDescriptionExW  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetClassDescriptionExW(
        const GUID* ClassGuid,
        PWSTR ClassDescription,
        DWORD ClassDescriptionSize,
        PDWORD RequiredSize,
        PCWSTR MachineName,
        PVOID Reserved)
{
    HKEY hKey;
    DWORD dwLength;
    BOOL ret;

    hKey = SetupDiOpenClassRegKeyExW(ClassGuid,
                                     KEY_ALL_ACCESS,
                                     DIOCR_INSTALLER,
                                     MachineName,
                                     Reserved);
    if (hKey == INVALID_HANDLE_VALUE)
    {
	WARN("SetupDiOpenClassRegKeyExW() failed (Error %u)\n", GetLastError());
	return FALSE;
    }

    dwLength = ClassDescriptionSize * sizeof(WCHAR);
    ret = !RegQueryValueExW( hKey, NULL, NULL, NULL,
                             (LPBYTE)ClassDescription, &dwLength );
    if (RequiredSize) *RequiredSize = dwLength / sizeof(WCHAR);
    RegCloseKey(hKey);
    return ret;
}

/***********************************************************************
 *		SetupDiGetClassDevsA (SETUPAPI.@)
 */
HDEVINFO WINAPI SetupDiGetClassDevsA(const GUID *class, LPCSTR enumstr, HWND parent, DWORD flags)
{
    HDEVINFO ret;
    LPWSTR enumstrW = NULL;

    if (enumstr)
    {
        int len = MultiByteToWideChar(CP_ACP, 0, enumstr, -1, NULL, 0);
        enumstrW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
        if (!enumstrW)
        {
            ret = INVALID_HANDLE_VALUE;
            goto end;
        }
        MultiByteToWideChar(CP_ACP, 0, enumstr, -1, enumstrW, len);
    }
    ret = SetupDiGetClassDevsExW(class, enumstrW, parent, flags, NULL, NULL,
            NULL);
    HeapFree(GetProcessHeap(), 0, enumstrW);

end:
    return ret;
}

/***********************************************************************
 *		  SetupDiGetClassDevsExA (SETUPAPI.@)
 */
HDEVINFO WINAPI SetupDiGetClassDevsExA(
        const GUID *class,
        PCSTR enumstr,
        HWND parent,
        DWORD flags,
        HDEVINFO deviceset,
        PCSTR machine,
        PVOID reserved)
{
    HDEVINFO ret;
    LPWSTR enumstrW = NULL, machineW = NULL;

    if (enumstr)
    {
        int len = MultiByteToWideChar(CP_ACP, 0, enumstr, -1, NULL, 0);
        enumstrW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
        if (!enumstrW)
        {
            ret = INVALID_HANDLE_VALUE;
            goto end;
        }
        MultiByteToWideChar(CP_ACP, 0, enumstr, -1, enumstrW, len);
    }
    if (machine)
    {
        int len = MultiByteToWideChar(CP_ACP, 0, machine, -1, NULL, 0);
        machineW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
        if (!machineW)
        {
            HeapFree(GetProcessHeap(), 0, enumstrW);
            ret = INVALID_HANDLE_VALUE;
            goto end;
        }
        MultiByteToWideChar(CP_ACP, 0, machine, -1, machineW, len);
    }
    ret = SetupDiGetClassDevsExW(class, enumstrW, parent, flags, deviceset,
            machineW, reserved);
    HeapFree(GetProcessHeap(), 0, enumstrW);
    HeapFree(GetProcessHeap(), 0, machineW);

end:
    return ret;
}

static void SETUPDI_AddDeviceInterfaces(struct device *device, HKEY key, const GUID *guid)
{
    DWORD i, len;
    WCHAR subKeyName[MAX_PATH];
    LONG l = ERROR_SUCCESS;

    for (i = 0; !l; i++)
    {
        len = sizeof(subKeyName) / sizeof(subKeyName[0]);
        l = RegEnumKeyExW(key, i, subKeyName, &len, NULL, NULL, NULL, NULL);
        if (!l)
        {
            HKEY subKey;
            struct device_iface *iface;

            if (*subKeyName == '#')
            {
                /* The subkey name is the reference string, with a '#' prepended */
                iface = SETUPDI_CreateDeviceInterface(device, guid, subKeyName + 1);
                l = RegOpenKeyExW(key, subKeyName, 0, KEY_READ, &subKey);
                if (!l)
                {
                    WCHAR symbolicLink[MAX_PATH];
                    DWORD dataType;

                    len = sizeof(symbolicLink);
                    l = RegQueryValueExW(subKey, SymbolicLink, NULL, &dataType,
                            (BYTE *)symbolicLink, &len);
                    if (!l && dataType == REG_SZ)
                        SETUPDI_SetInterfaceSymbolicLink(iface, symbolicLink);
                    RegCloseKey(subKey);
                }
            }
            /* Allow enumeration to continue */
            l = ERROR_SUCCESS;
        }
    }
    /* FIXME: find and add all the device's interfaces to the device */
}

static void SETUPDI_EnumerateMatchingInterfaces(HDEVINFO DeviceInfoSet,
        HKEY key, const GUID *guid, LPCWSTR enumstr)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    DWORD i, len;
    WCHAR subKeyName[MAX_PATH];
    LONG l;
    HKEY enumKey = INVALID_HANDLE_VALUE;

    TRACE("%s\n", debugstr_w(enumstr));

    l = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Enum, 0, NULL, 0, KEY_READ, NULL,
            &enumKey, NULL);
    for (i = 0; !l; i++)
    {
        len = sizeof(subKeyName) / sizeof(subKeyName[0]);
        l = RegEnumKeyExW(key, i, subKeyName, &len, NULL, NULL, NULL, NULL);
        if (!l)
        {
            HKEY subKey;

            l = RegOpenKeyExW(key, subKeyName, 0, KEY_READ, &subKey);
            if (!l)
            {
                WCHAR deviceInst[MAX_PATH * 3];
                DWORD dataType;

                len = sizeof(deviceInst);
                l = RegQueryValueExW(subKey, DeviceInstance, NULL, &dataType,
                        (BYTE *)deviceInst, &len);
                if (!l && dataType == REG_SZ)
                {
                    TRACE("found instance ID %s\n", debugstr_w(deviceInst));
                    if (!enumstr || !lstrcmpiW(enumstr, deviceInst))
                    {
                        HKEY deviceKey;

                        l = RegOpenKeyExW(enumKey, deviceInst, 0, KEY_READ,
                                &deviceKey);
                        if (!l)
                        {
                            WCHAR deviceClassStr[40];

                            len = sizeof(deviceClassStr);
                            l = RegQueryValueExW(deviceKey, ClassGUID, NULL,
                                    &dataType, (BYTE *)deviceClassStr, &len);
                            if (!l && dataType == REG_SZ &&
                                    deviceClassStr[0] == '{' &&
                                    deviceClassStr[37] == '}')
                            {
                                GUID deviceClass;
                                struct device *device;

                                deviceClassStr[37] = 0;
                                UuidFromStringW(&deviceClassStr[1],
                                        &deviceClass);
                                if ((device = SETUPDI_CreateDeviceInfo(set,
                                        &deviceClass, deviceInst, FALSE)))
                                    SETUPDI_AddDeviceInterfaces(device, subKey, guid);
                            }
                            RegCloseKey(deviceKey);
                        }
                    }
                }
                RegCloseKey(subKey);
            }
            /* Allow enumeration to continue */
            l = ERROR_SUCCESS;
        }
    }
    if (enumKey != INVALID_HANDLE_VALUE)
        RegCloseKey(enumKey);
}

static void SETUPDI_EnumerateInterfaces(HDEVINFO DeviceInfoSet,
        const GUID *guid, LPCWSTR enumstr, DWORD flags)
{
    HKEY interfacesKey = SetupDiOpenClassRegKeyExW(guid, KEY_READ,
            DIOCR_INTERFACE, NULL, NULL);

    TRACE("%p, %s, %s, %08x\n", DeviceInfoSet, debugstr_guid(guid),
            debugstr_w(enumstr), flags);

    if (interfacesKey != INVALID_HANDLE_VALUE)
    {
        if (flags & DIGCF_ALLCLASSES)
        {
            DWORD i, len;
            WCHAR interfaceGuidStr[40];
            LONG l = ERROR_SUCCESS;

            for (i = 0; !l; i++)
            {
                len = sizeof(interfaceGuidStr) / sizeof(interfaceGuidStr[0]);
                l = RegEnumKeyExW(interfacesKey, i, interfaceGuidStr, &len,
                        NULL, NULL, NULL, NULL);
                if (!l)
                {
                    if (interfaceGuidStr[0] == '{' &&
                            interfaceGuidStr[37] == '}')
                    {
                        HKEY interfaceKey;
                        GUID interfaceGuid;

                        interfaceGuidStr[37] = 0;
                        UuidFromStringW(&interfaceGuidStr[1], &interfaceGuid);
                        interfaceGuidStr[37] = '}';
                        interfaceGuidStr[38] = 0;
                        l = RegOpenKeyExW(interfacesKey, interfaceGuidStr, 0,
                                KEY_READ, &interfaceKey);
                        if (!l)
                        {
                            SETUPDI_EnumerateMatchingInterfaces(DeviceInfoSet,
                                    interfaceKey, &interfaceGuid, enumstr);
                            RegCloseKey(interfaceKey);
                        }
                    }
                }
            }
        }
        else
        {
            /* In this case, SetupDiOpenClassRegKeyExW opened the specific
             * interface's key, so just pass that long
             */
            SETUPDI_EnumerateMatchingInterfaces(DeviceInfoSet,
                    interfacesKey, guid, enumstr);
        }
        RegCloseKey(interfacesKey);
    }
}

static void SETUPDI_EnumerateMatchingDeviceInstances(struct DeviceInfoSet *set,
        LPCWSTR enumerator, LPCWSTR deviceName, HKEY deviceKey,
        const GUID *class, DWORD flags)
{
    DWORD i, len;
    WCHAR deviceInstance[MAX_PATH];
    LONG l = ERROR_SUCCESS;

    TRACE("%s %s\n", debugstr_w(enumerator), debugstr_w(deviceName));

    for (i = 0; !l; i++)
    {
        len = sizeof(deviceInstance) / sizeof(deviceInstance[0]);
        l = RegEnumKeyExW(deviceKey, i, deviceInstance, &len, NULL, NULL, NULL,
                NULL);
        if (!l)
        {
            HKEY subKey;

            l = RegOpenKeyExW(deviceKey, deviceInstance, 0, KEY_READ, &subKey);
            if (!l)
            {
                WCHAR classGuid[40];
                DWORD dataType;

                len = sizeof(classGuid);
                l = RegQueryValueExW(subKey, ClassGUID, NULL, &dataType,
                        (BYTE *)classGuid, &len);
                if (!l && dataType == REG_SZ)
                {
                    if (classGuid[0] == '{' && classGuid[37] == '}')
                    {
                        GUID deviceClass;

                        classGuid[37] = 0;
                        UuidFromStringW(&classGuid[1], &deviceClass);
                        if ((flags & DIGCF_ALLCLASSES) ||
                                IsEqualGUID(class, &deviceClass))
                        {
                            static const WCHAR fmt[] =
                             {'%','s','\\','%','s','\\','%','s',0};
                            LPWSTR instanceId;

                            instanceId = HeapAlloc(GetProcessHeap(), 0,
                                (lstrlenW(enumerator) + lstrlenW(deviceName) +
                                lstrlenW(deviceInstance) + 3) * sizeof(WCHAR));
                            if (instanceId)
                            {
                                sprintfW(instanceId, fmt, enumerator,
                                        deviceName, deviceInstance);
                                SETUPDI_CreateDeviceInfo(set, &deviceClass,
                                        instanceId, FALSE);
                                HeapFree(GetProcessHeap(), 0, instanceId);
                            }
                        }
                    }
                }
                RegCloseKey(subKey);
            }
            /* Allow enumeration to continue */
            l = ERROR_SUCCESS;
        }
    }
}

static void SETUPDI_EnumerateMatchingDevices(HDEVINFO DeviceInfoSet,
        LPCWSTR parent, HKEY key, const GUID *class, DWORD flags)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    DWORD i, len;
    WCHAR subKeyName[MAX_PATH];
    LONG l = ERROR_SUCCESS;

    TRACE("%s\n", debugstr_w(parent));

    for (i = 0; !l; i++)
    {
        len = sizeof(subKeyName) / sizeof(subKeyName[0]);
        l = RegEnumKeyExW(key, i, subKeyName, &len, NULL, NULL, NULL, NULL);
        if (!l)
        {
            HKEY subKey;

            l = RegOpenKeyExW(key, subKeyName, 0, KEY_READ, &subKey);
            if (!l)
            {
                TRACE("%s\n", debugstr_w(subKeyName));
                SETUPDI_EnumerateMatchingDeviceInstances(set, parent,
                        subKeyName, subKey, class, flags);
                RegCloseKey(subKey);
            }
            /* Allow enumeration to continue */
            l = ERROR_SUCCESS;
        }
    }
}

static void SETUPDI_EnumerateDevices(HDEVINFO DeviceInfoSet, const GUID *class,
        LPCWSTR enumstr, DWORD flags)
{
    HKEY enumKey;
    LONG l;

    TRACE("%p, %s, %s, %08x\n", DeviceInfoSet, debugstr_guid(class),
            debugstr_w(enumstr), flags);

    l = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Enum, 0, NULL, 0, KEY_READ, NULL,
            &enumKey, NULL);
    if (enumKey != INVALID_HANDLE_VALUE)
    {
        if (enumstr)
        {
            HKEY enumStrKey;

            l = RegOpenKeyExW(enumKey, enumstr, 0, KEY_READ,
                    &enumStrKey);
            if (!l)
            {
                SETUPDI_EnumerateMatchingDevices(DeviceInfoSet, enumstr,
                        enumStrKey, class, flags);
                RegCloseKey(enumStrKey);
            }
        }
        else
        {
            DWORD i, len;
            WCHAR subKeyName[MAX_PATH];

            l = ERROR_SUCCESS;
            for (i = 0; !l; i++)
            {
                len = sizeof(subKeyName) / sizeof(subKeyName[0]);
                l = RegEnumKeyExW(enumKey, i, subKeyName, &len, NULL,
                        NULL, NULL, NULL);
                if (!l)
                {
                    HKEY subKey;

                    l = RegOpenKeyExW(enumKey, subKeyName, 0, KEY_READ,
                            &subKey);
                    if (!l)
                    {
                        SETUPDI_EnumerateMatchingDevices(DeviceInfoSet,
                                subKeyName, subKey, class, flags);
                        RegCloseKey(subKey);
                    }
                    /* Allow enumeration to continue */
                    l = ERROR_SUCCESS;
                }
            }
        }
        RegCloseKey(enumKey);
    }
}

/***********************************************************************
 *		SetupDiGetClassDevsW (SETUPAPI.@)
 */
HDEVINFO WINAPI SetupDiGetClassDevsW(const GUID *class, LPCWSTR enumstr, HWND parent, DWORD flags)
{
    return SetupDiGetClassDevsExW(class, enumstr, parent, flags, NULL, NULL,
            NULL);
}

/***********************************************************************
 *              SetupDiGetClassDevsExW (SETUPAPI.@)
 */
HDEVINFO WINAPI SetupDiGetClassDevsExW(const GUID *class, PCWSTR enumstr, HWND parent, DWORD flags,
        HDEVINFO deviceset, PCWSTR machine, void *reserved)
{
    static const DWORD unsupportedFlags = DIGCF_DEFAULT | DIGCF_PRESENT |
        DIGCF_PROFILE;
    HDEVINFO set;

    TRACE("%s %s %p 0x%08x %p %s %p\n", debugstr_guid(class),
            debugstr_w(enumstr), parent, flags, deviceset, debugstr_w(machine),
            reserved);

    if (!(flags & DIGCF_ALLCLASSES) && !class)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (flags & unsupportedFlags)
        WARN("unsupported flags %08x\n", flags & unsupportedFlags);
    if (deviceset)
        set = deviceset;
    else
        set = SetupDiCreateDeviceInfoListExW(class, parent, machine, reserved);
    if (set != INVALID_HANDLE_VALUE)
    {
        if (machine && *machine)
            FIXME("%s: unimplemented for remote machines\n",
                    debugstr_w(machine));
        else if (flags & DIGCF_DEVICEINTERFACE)
            SETUPDI_EnumerateInterfaces(set, class, enumstr, flags);
        else
            SETUPDI_EnumerateDevices(set, class, enumstr, flags);
    }
    return set;
}

/***********************************************************************
 *		SetupDiGetDeviceInfoListDetailA  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDeviceInfoListDetailA(
        HDEVINFO DeviceInfoSet,
        PSP_DEVINFO_LIST_DETAIL_DATA_A DevInfoData )
{
    struct DeviceInfoSet *set = DeviceInfoSet;

    TRACE("%p %p\n", DeviceInfoSet, DevInfoData);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DevInfoData ||
            DevInfoData->cbSize != sizeof(SP_DEVINFO_LIST_DETAIL_DATA_A))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DevInfoData->ClassGuid = set->ClassGuid;
    DevInfoData->RemoteMachineHandle = NULL;
    DevInfoData->RemoteMachineName[0] = '\0';
    return TRUE;
}

/***********************************************************************
 *		SetupDiGetDeviceInfoListDetailW  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDeviceInfoListDetailW(
        HDEVINFO DeviceInfoSet,
        PSP_DEVINFO_LIST_DETAIL_DATA_W DevInfoData )
{
    struct DeviceInfoSet *set = DeviceInfoSet;

    TRACE("%p %p\n", DeviceInfoSet, DevInfoData);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DevInfoData ||
            DevInfoData->cbSize != sizeof(SP_DEVINFO_LIST_DETAIL_DATA_W))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DevInfoData->ClassGuid = set->ClassGuid;
    DevInfoData->RemoteMachineHandle = NULL;
    DevInfoData->RemoteMachineName[0] = '\0';
    return TRUE;
}

/***********************************************************************
 *		SetupDiCreateDeviceInterfaceA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiCreateDeviceInterfaceA(
        HDEVINFO DeviceInfoSet,
        PSP_DEVINFO_DATA DeviceInfoData,
        const GUID *InterfaceClassGuid,
        PCSTR ReferenceString,
        DWORD CreationFlags,
        PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData)
{
    BOOL ret;
    LPWSTR ReferenceStringW = NULL;

    TRACE("%p %p %s %s %08x %p\n", DeviceInfoSet, DeviceInfoData,
            debugstr_guid(InterfaceClassGuid), debugstr_a(ReferenceString),
            CreationFlags, DeviceInterfaceData);

    if (ReferenceString)
    {
        ReferenceStringW = MultiByteToUnicode(ReferenceString, CP_ACP);
        if (ReferenceStringW == NULL) return FALSE;
    }

    ret = SetupDiCreateDeviceInterfaceW(DeviceInfoSet, DeviceInfoData,
            InterfaceClassGuid, ReferenceStringW, CreationFlags,
            DeviceInterfaceData);

    MyFree(ReferenceStringW);

    return ret;
}

/***********************************************************************
 *		SetupDiCreateDeviceInterfaceW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiCreateDeviceInterfaceW(
        HDEVINFO DeviceInfoSet,
        PSP_DEVINFO_DATA DeviceInfoData,
        const GUID *InterfaceClassGuid,
        PCWSTR ReferenceString,
        DWORD CreationFlags,
        SP_DEVICE_INTERFACE_DATA *iface_data)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device *device;
    struct device_iface *iface;

    TRACE("%p %p %s %s %08x %p\n", DeviceInfoSet, DeviceInfoData,
            debugstr_guid(InterfaceClassGuid), debugstr_w(ReferenceString),
            CreationFlags, iface_data);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DeviceInfoData || DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)
            || !DeviceInfoData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    device = (struct device *)DeviceInfoData->Reserved;
    if (device->set != set)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!InterfaceClassGuid)
    {
        SetLastError(ERROR_INVALID_USER_BUFFER);
        return FALSE;
    }
    if (!(iface = SETUPDI_CreateDeviceInterface(device, InterfaceClassGuid,
                    ReferenceString)))
        return FALSE;

    if (iface_data)
    {
        if (iface_data->cbSize != sizeof(SP_DEVICE_INTERFACE_DATA))
        {
            SetLastError(ERROR_INVALID_USER_BUFFER);
            return FALSE;
        }

        copy_device_iface_data(iface_data, iface);
    }
    return TRUE;
}

/***********************************************************************
 *		SetupDiCreateDeviceInterfaceRegKeyA (SETUPAPI.@)
 */
HKEY WINAPI SetupDiCreateDeviceInterfaceRegKeyA(
        HDEVINFO DeviceInfoSet,
        PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
        DWORD Reserved,
        REGSAM samDesired,
        HINF InfHandle,
        PCSTR InfSectionName)
{
    HKEY key;
    PWSTR InfSectionNameW = NULL;

    TRACE("%p %p %d %08x %p %p\n", DeviceInfoSet, DeviceInterfaceData, Reserved,
            samDesired, InfHandle, InfSectionName);
    if (InfHandle)
    {
        if (!InfSectionName)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
        }
        InfSectionNameW = MultiByteToUnicode(InfSectionName, CP_ACP);
        if (!InfSectionNameW)
            return INVALID_HANDLE_VALUE;
    }
    key = SetupDiCreateDeviceInterfaceRegKeyW(DeviceInfoSet,
            DeviceInterfaceData, Reserved, samDesired, InfHandle,
            InfSectionNameW);
    MyFree(InfSectionNameW);
    return key;
}

/***********************************************************************
 *		SetupDiCreateDeviceInterfaceRegKeyW (SETUPAPI.@)
 */
HKEY WINAPI SetupDiCreateDeviceInterfaceRegKeyW(HDEVINFO devinfo,
    SP_DEVICE_INTERFACE_DATA *iface_data, DWORD reserved, REGSAM access,
    HINF hinf, const WCHAR *section)
{
    struct DeviceInfoSet *set = devinfo;
    struct device_iface *iface;
    HKEY refstr_key, params_key;
    WCHAR *path;
    LONG ret;

    TRACE("%p %p %d %#x %p %s\n", devinfo, iface_data, reserved, access, hinf,
        debugstr_w(section));

    if (!devinfo || devinfo == INVALID_HANDLE_VALUE ||
            set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    if (!iface_data || iface_data->cbSize != sizeof(SP_DEVICE_INTERFACE_DATA) ||
            !iface_data->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (hinf && !section)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    iface = (struct device_iface *)iface_data->Reserved;
    if (!(path = get_refstr_key_path(iface)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return INVALID_HANDLE_VALUE;
    }

    ret = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, 0, NULL,
        &refstr_key, NULL);
    heap_free(path);
    if (ret)
    {
        SetLastError(ret);
        return INVALID_HANDLE_VALUE;
    }

    ret = RegCreateKeyExW(refstr_key, DeviceParameters, 0, NULL, 0, access,
        NULL, &params_key, NULL);
    RegCloseKey(refstr_key);
    if (ret)
    {
        SetLastError(ret);
        return INVALID_HANDLE_VALUE;
    }

    return params_key;
}

/***********************************************************************
 *		SetupDiDeleteDeviceInterfaceRegKey (SETUPAPI.@)
 */
BOOL WINAPI SetupDiDeleteDeviceInterfaceRegKey(HDEVINFO devinfo,
    SP_DEVICE_INTERFACE_DATA *iface_data, DWORD reserved)
{
    struct DeviceInfoSet *set = devinfo;
    struct device_iface *iface;
    HKEY refstr_key;
    WCHAR *path;
    LONG ret;

    TRACE("%p %p %d\n", devinfo, iface_data, reserved);

    if (!devinfo || devinfo == INVALID_HANDLE_VALUE ||
            set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!iface_data || iface_data->cbSize != sizeof(SP_DEVICE_INTERFACE_DATA) ||
            !iface_data->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    iface = (struct device_iface *)iface_data->Reserved;
    if (!(path = get_refstr_key_path(iface)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }

    ret = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, 0, NULL,
        &refstr_key, NULL);
    heap_free(path);
    if (ret)
    {
        SetLastError(ret);
        return FALSE;
    }

    ret = RegDeleteKeyW(refstr_key, DeviceParameters);
    RegCloseKey(refstr_key);
    if (ret)
    {
        SetLastError(ret);
        return FALSE;
    }

    return TRUE;
}

/***********************************************************************
 *      SetupDiOpenDeviceInterfaceRegKey (SETUPAPI.@)
 */
HKEY WINAPI SetupDiOpenDeviceInterfaceRegKey(HDEVINFO devinfo,
    SP_DEVICE_INTERFACE_DATA *iface_data, DWORD reserved, REGSAM access)
{
    struct device_iface *iface;
    HKEY refstr_key, params_key;
    WCHAR *path;
    LONG ret;

    TRACE("%p %p %d %#x\n", devinfo, iface_data, reserved, access);

    iface = (struct device_iface *)iface_data->Reserved;
    if (!(path = get_refstr_key_path(iface)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }

    ret = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, 0, NULL,
        &refstr_key, NULL);
    heap_free(path);
    if (ret)
    {
        SetLastError(ret);
        return INVALID_HANDLE_VALUE;
    }

    ret = RegCreateKeyExW(refstr_key, DeviceParameters, 0, NULL, 0, access,
        NULL, &params_key, NULL);
    RegCloseKey(refstr_key);
    if (ret)
    {
        SetLastError(ret);
        return INVALID_HANDLE_VALUE;
    }

    return params_key;
}

/***********************************************************************
 *		SetupDiEnumDeviceInterfaces (SETUPAPI.@)
 *
 * PARAMS
 *   DeviceInfoSet      [I]    Set of devices from which to enumerate
 *                             interfaces
 *   DeviceInfoData     [I]    (Optional) If specified, a specific device
 *                             instance from which to enumerate interfaces.
 *                             If it isn't specified, all interfaces for all
 *                             devices in the set are enumerated.
 *   InterfaceClassGuid [I]    The interface class to enumerate.
 *   MemberIndex        [I]    An index of the interface instance to enumerate.
 *                             A caller should start with MemberIndex set to 0,
 *                             and continue until the function fails with
 *                             ERROR_NO_MORE_ITEMS.
 *   DeviceInterfaceData [I/O] Returns an enumerated interface.  Its cbSize
 *                             member must be set to
 *                             sizeof(SP_DEVICE_INTERFACE_DATA).
 *
 * RETURNS
 *   Success: non-zero value.
 *   Failure: FALSE.  Call GetLastError() for more info.
 */
BOOL WINAPI SetupDiEnumDeviceInterfaces(HDEVINFO devinfo,
    SP_DEVINFO_DATA *device_data, const GUID *class, DWORD index,
    SP_DEVICE_INTERFACE_DATA *iface_data)
{
    struct DeviceInfoSet *set = devinfo;
    struct device *device;
    struct device_iface *iface;
    DWORD i = 0;

    TRACE("%p, %p, %s, %u, %p\n", devinfo, device_data, debugstr_guid(class),
        index, iface_data);

    if (!devinfo || devinfo == INVALID_HANDLE_VALUE ||
            set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (device_data && (device_data->cbSize != sizeof(SP_DEVINFO_DATA) ||
                !device_data->Reserved))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!iface_data || iface_data->cbSize != sizeof(SP_DEVICE_INTERFACE_DATA))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* In case application fails to check return value, clear output */
    memset(iface_data, 0, sizeof(*iface_data));
    iface_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    if (device_data)
    {
        device = (struct device *)device_data->Reserved;

        LIST_FOR_EACH_ENTRY(iface, &device->interfaces, struct device_iface, entry)
        {
            if (IsEqualGUID(&iface->class, class))
            {
                if (i == index)
                {
                    copy_device_iface_data(iface_data, iface);
                    return TRUE;
                }
                i++;
            }
        }
    }
    else
    {
        LIST_FOR_EACH_ENTRY(device, &set->devices, struct device, entry)
        {
            LIST_FOR_EACH_ENTRY(iface, &device->interfaces, struct device_iface, entry)
            {
                if (IsEqualGUID(&iface->class, class))
                {
                    if (i == index)
                    {
                        copy_device_iface_data(iface_data, iface);
                        return TRUE;
                    }
                    i++;
                }
            }
        }
    }

    SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

/***********************************************************************
 *		SetupDiDestroyDeviceInfoList (SETUPAPI.@)
  *
 * Destroy a DeviceInfoList and free all used memory of the list.
 *
 * PARAMS
 *   devinfo [I] DeviceInfoList pointer to list to destroy
 *
 * RETURNS
 *   Success: non zero value.
 *   Failure: zero value.
 */
BOOL WINAPI SetupDiDestroyDeviceInfoList(HDEVINFO devinfo)
{
    BOOL ret = FALSE;

    TRACE("%p\n", devinfo);
    if (devinfo && devinfo != INVALID_HANDLE_VALUE)
    {
        struct DeviceInfoSet *list = devinfo;

        if (list->magic == SETUP_DEVICE_INFO_SET_MAGIC)
        {
            struct device *device, *device2;

            LIST_FOR_EACH_ENTRY_SAFE(device, device2, &list->devices,
                    struct device, entry)
            {
                SETUPDI_RemoveDevice(device);
            }
            HeapFree(GetProcessHeap(), 0, list);
            ret = TRUE;
        }
    }

    if (!ret)
        SetLastError(ERROR_INVALID_HANDLE);

    return ret;
}

/***********************************************************************
 *		SetupDiGetDeviceInterfaceDetailA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDeviceInterfaceDetailA(
      HDEVINFO DeviceInfoSet,
      PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
      PSP_DEVICE_INTERFACE_DETAIL_DATA_A DeviceInterfaceDetailData,
      DWORD DeviceInterfaceDetailDataSize,
      PDWORD RequiredSize,
      SP_DEVINFO_DATA *device_data)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device_iface *iface;
    DWORD bytesNeeded = FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath[1]);
    BOOL ret = FALSE;

    TRACE("(%p, %p, %p, %d, %p, %p)\n", DeviceInfoSet,
     DeviceInterfaceData, DeviceInterfaceDetailData,
     DeviceInterfaceDetailDataSize, RequiredSize, device_data);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE ||
            set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DeviceInterfaceData ||
            DeviceInterfaceData->cbSize != sizeof(SP_DEVICE_INTERFACE_DATA) ||
            !DeviceInterfaceData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (DeviceInterfaceDetailData &&
        DeviceInterfaceDetailData->cbSize != sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A))
    {
        SetLastError(ERROR_INVALID_USER_BUFFER);
        return FALSE;
    }
    if (!DeviceInterfaceDetailData && DeviceInterfaceDetailDataSize)
    {
        SetLastError(ERROR_INVALID_USER_BUFFER);
        return FALSE;
    }
    iface = (struct device_iface *)DeviceInterfaceData->Reserved;
    if (iface->symlink)
        bytesNeeded += WideCharToMultiByte(CP_ACP, 0, iface->symlink, -1,
                NULL, 0, NULL, NULL);
    if (DeviceInterfaceDetailDataSize >= bytesNeeded)
    {
        if (iface->symlink)
            WideCharToMultiByte(CP_ACP, 0, iface->symlink, -1,
                    DeviceInterfaceDetailData->DevicePath,
                    DeviceInterfaceDetailDataSize -
                    offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath),
                    NULL, NULL);
        else
            DeviceInterfaceDetailData->DevicePath[0] = '\0';

        if (device_data && device_data->cbSize == sizeof(SP_DEVINFO_DATA))
            copy_device_data(device_data, iface->device);

        ret = TRUE;
    }
    else
    {
        if (RequiredSize)
            *RequiredSize = bytesNeeded;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
    }
    return ret;
}

/***********************************************************************
 *		SetupDiGetDeviceInterfaceDetailW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDeviceInterfaceDetailW(
      HDEVINFO DeviceInfoSet,
      PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
      PSP_DEVICE_INTERFACE_DETAIL_DATA_W DeviceInterfaceDetailData,
      DWORD DeviceInterfaceDetailDataSize,
      PDWORD RequiredSize,
      SP_DEVINFO_DATA *device_data)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device_iface *iface;
    DWORD bytesNeeded = offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath)
        + sizeof(WCHAR); /* include NULL terminator */
    BOOL ret = FALSE;

    TRACE("(%p, %p, %p, %d, %p, %p)\n", DeviceInfoSet,
     DeviceInterfaceData, DeviceInterfaceDetailData,
     DeviceInterfaceDetailDataSize, RequiredSize, device_data);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE ||
            set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DeviceInterfaceData ||
            DeviceInterfaceData->cbSize != sizeof(SP_DEVICE_INTERFACE_DATA) ||
            !DeviceInterfaceData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (DeviceInterfaceDetailData && (DeviceInterfaceDetailData->cbSize <
            offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) + sizeof(WCHAR) ||
            DeviceInterfaceDetailData->cbSize > sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)))
    {
        SetLastError(ERROR_INVALID_USER_BUFFER);
        return FALSE;
    }
    if (!DeviceInterfaceDetailData && DeviceInterfaceDetailDataSize)
    {
        SetLastError(ERROR_INVALID_USER_BUFFER);
        return FALSE;
    }
    iface = (struct device_iface *)DeviceInterfaceData->Reserved;
    if (iface->symlink)
        bytesNeeded += sizeof(WCHAR) * lstrlenW(iface->symlink);
    if (DeviceInterfaceDetailDataSize >= bytesNeeded)
    {
        if (iface->symlink)
            lstrcpyW(DeviceInterfaceDetailData->DevicePath, iface->symlink);
        else
            DeviceInterfaceDetailData->DevicePath[0] = '\0';

        if (device_data && device_data->cbSize == sizeof(SP_DEVINFO_DATA))
            copy_device_data(device_data, iface->device);

        ret = TRUE;
    }
    else
    {
        if (RequiredSize)
            *RequiredSize = bytesNeeded;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
    }
    return ret;
}

/***********************************************************************
 *		SetupDiGetDeviceRegistryPropertyA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDeviceRegistryPropertyA(
        HDEVINFO  DeviceInfoSet,
        PSP_DEVINFO_DATA  DeviceInfoData,
        DWORD   Property,
        PDWORD  PropertyRegDataType,
        PBYTE   PropertyBuffer,
        DWORD   PropertyBufferSize,
        PDWORD  RequiredSize)
{
    BOOL ret = FALSE;
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device *device;

    TRACE("%p %p %d %p %p %d %p\n", DeviceInfoSet, DeviceInfoData,
        Property, PropertyRegDataType, PropertyBuffer, PropertyBufferSize,
        RequiredSize);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DeviceInfoData || DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)
            || !DeviceInfoData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (PropertyBufferSize && PropertyBuffer == NULL)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    device = (struct device *)DeviceInfoData->Reserved;
    if (Property < sizeof(PropertyMap) / sizeof(PropertyMap[0])
        && PropertyMap[Property].nameA)
    {
        DWORD size = PropertyBufferSize;
        LONG l = RegQueryValueExA(device->key, PropertyMap[Property].nameA,
                NULL, PropertyRegDataType, PropertyBuffer, &size);

        if (l == ERROR_FILE_NOT_FOUND)
            SetLastError(ERROR_INVALID_DATA);
        else if (l == ERROR_MORE_DATA || !PropertyBufferSize)
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
        else if (!l)
            ret = TRUE;
        else
            SetLastError(l);
        if (RequiredSize)
            *RequiredSize = size;
    }
    return ret;
}

/***********************************************************************
 *		SetupDiGetDeviceRegistryPropertyW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDeviceRegistryPropertyW(
        HDEVINFO  DeviceInfoSet,
        PSP_DEVINFO_DATA  DeviceInfoData,
        DWORD   Property,
        PDWORD  PropertyRegDataType,
        PBYTE   PropertyBuffer,
        DWORD   PropertyBufferSize,
        PDWORD  RequiredSize)
{
    BOOL ret = FALSE;
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device *device;

    TRACE("%p %p %d %p %p %d %p\n", DeviceInfoSet, DeviceInfoData,
        Property, PropertyRegDataType, PropertyBuffer, PropertyBufferSize,
        RequiredSize);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DeviceInfoData || DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)
            || !DeviceInfoData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (PropertyBufferSize && PropertyBuffer == NULL)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    device = (struct device *)DeviceInfoData->Reserved;
    if (Property < sizeof(PropertyMap) / sizeof(PropertyMap[0])
        && PropertyMap[Property].nameW)
    {
        DWORD size = PropertyBufferSize;
        LONG l = RegQueryValueExW(device->key, PropertyMap[Property].nameW,
                NULL, PropertyRegDataType, PropertyBuffer, &size);

        if (l == ERROR_FILE_NOT_FOUND)
            SetLastError(ERROR_INVALID_DATA);
        else if (l == ERROR_MORE_DATA || !PropertyBufferSize)
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
        else if (!l)
            ret = TRUE;
        else
            SetLastError(l);
        if (RequiredSize)
            *RequiredSize = size;
    }
    return ret;
}

/***********************************************************************
 *		SetupDiSetDeviceRegistryPropertyA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiSetDeviceRegistryPropertyA(
	HDEVINFO DeviceInfoSet,
	PSP_DEVINFO_DATA DeviceInfoData,
	DWORD Property,
	const BYTE *PropertyBuffer,
	DWORD PropertyBufferSize)
{
    BOOL ret = FALSE;
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device *device;

    TRACE("%p %p %d %p %d\n", DeviceInfoSet, DeviceInfoData, Property,
        PropertyBuffer, PropertyBufferSize);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DeviceInfoData || DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)
            || !DeviceInfoData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    device = (struct device *)DeviceInfoData->Reserved;
    if (Property < sizeof(PropertyMap) / sizeof(PropertyMap[0])
        && PropertyMap[Property].nameA)
    {
        LONG l = RegSetValueExA(device->key, PropertyMap[Property].nameA, 0,
                PropertyMap[Property].regType, PropertyBuffer,
                PropertyBufferSize);
        if (!l)
            ret = TRUE;
        else
            SetLastError(l);
    }
    return ret;
}

/***********************************************************************
 *		SetupDiSetDeviceRegistryPropertyW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiSetDeviceRegistryPropertyW(HDEVINFO devinfo,
    SP_DEVINFO_DATA *device_data, DWORD prop, const BYTE *buffer, DWORD size)
{
    struct DeviceInfoSet *set = devinfo;
    struct device *device;

    TRACE("%p %p %d %p %d\n", devinfo, device_data, prop, buffer, size);

    if (!devinfo || devinfo == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!device_data || device_data->cbSize != sizeof(SP_DEVINFO_DATA)
            || !device_data->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    device = (struct device *)device_data->Reserved;
    return SETUPDI_SetDeviceRegistryPropertyW(device, prop, buffer, size);
}

/***********************************************************************
 *		SetupDiInstallClassA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiInstallClassA(
        HWND hwndParent,
        PCSTR InfFileName,
        DWORD Flags,
        HSPFILEQ FileQueue)
{
    UNICODE_STRING FileNameW;
    BOOL Result;

    if (!InfFileName)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!RtlCreateUnicodeStringFromAsciiz(&FileNameW, InfFileName))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    Result = SetupDiInstallClassW(hwndParent, FileNameW.Buffer, Flags, FileQueue);

    RtlFreeUnicodeString(&FileNameW);

    return Result;
}

static HKEY CreateClassKey(HINF hInf)
{
    static const WCHAR slash[] = { '\\',0 };
    WCHAR FullBuffer[MAX_PATH];
    WCHAR Buffer[MAX_PATH];
    DWORD RequiredSize;
    HKEY hClassKey;

    if (!SetupGetLineTextW(NULL,
			   hInf,
			   Version,
			   ClassGUID,
			   Buffer,
			   MAX_PATH,
			   &RequiredSize))
    {
	return INVALID_HANDLE_VALUE;
    }

    lstrcpyW(FullBuffer, ControlClass);
    lstrcatW(FullBuffer, slash);
    lstrcatW(FullBuffer, Buffer);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		      FullBuffer,
		      0,
		      KEY_ALL_ACCESS,
		      &hClassKey))
    {
	if (!SetupGetLineTextW(NULL,
			       hInf,
			       Version,
			       Class,
			       Buffer,
			       MAX_PATH,
			       &RequiredSize))
	{
	    return INVALID_HANDLE_VALUE;
	}

	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
			    FullBuffer,
			    0,
			    NULL,
			    REG_OPTION_NON_VOLATILE,
			    KEY_ALL_ACCESS,
			    NULL,
			    &hClassKey,
			    NULL))
	{
	    return INVALID_HANDLE_VALUE;
	}

    }

    if (RegSetValueExW(hClassKey,
		       Class,
		       0,
		       REG_SZ,
		       (LPBYTE)Buffer,
		       RequiredSize * sizeof(WCHAR)))
    {
	RegCloseKey(hClassKey);
	RegDeleteKeyW(HKEY_LOCAL_MACHINE,
		      FullBuffer);
	return INVALID_HANDLE_VALUE;
    }

    return hClassKey;
}

/***********************************************************************
 *		SetupDiInstallClassW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiInstallClassW(
        HWND hwndParent,
        PCWSTR InfFileName,
        DWORD Flags,
        HSPFILEQ FileQueue)
{
    WCHAR SectionName[MAX_PATH];
    DWORD SectionNameLength = 0;
    HINF hInf;
    BOOL bFileQueueCreated = FALSE;
    HKEY hClassKey;


    FIXME("\n");

    if (!InfFileName)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((Flags & DI_NOVCP) && (FileQueue == NULL || FileQueue == INVALID_HANDLE_VALUE))
    {
	SetLastError(ERROR_INVALID_PARAMETER);
	return FALSE;
    }

    /* Open the .inf file */
    hInf = SetupOpenInfFileW(InfFileName,
			     NULL,
			     INF_STYLE_WIN4,
			     NULL);
    if (hInf == INVALID_HANDLE_VALUE)
    {

	return FALSE;
    }

    /* Create or open the class registry key 'HKLM\\CurrentControlSet\\Class\\{GUID}' */
    hClassKey = CreateClassKey(hInf);
    if (hClassKey == INVALID_HANDLE_VALUE)
    {
	SetupCloseInfFile(hInf);
	return FALSE;
    }


    /* Try to append a layout file */
    SetupOpenAppendInfFileW(NULL, hInf, NULL);

    /* Retrieve the actual section name */
    SetupDiGetActualSectionToInstallW(hInf,
				      ClassInstall32,
				      SectionName,
				      MAX_PATH,
				      &SectionNameLength,
				      NULL);

#if 0
    if (!(Flags & DI_NOVCP))
    {
	FileQueue = SetupOpenFileQueue();
	if (FileQueue == INVALID_HANDLE_VALUE)
	{
	    SetupCloseInfFile(hInf);
	    return FALSE;
	}

	bFileQueueCreated = TRUE;

    }
#endif

    SetupInstallFromInfSectionW(NULL,
				hInf,
				SectionName,
				SPINST_COPYINF | SPINST_FILES | SPINST_REGISTRY,
				hClassKey,
				NULL,
				0,
				NULL,
				NULL,
				INVALID_HANDLE_VALUE,
				NULL);

    /* FIXME: More code! */

    if (bFileQueueCreated)
	SetupCloseFileQueue(FileQueue);

    SetupCloseInfFile(hInf);

    return TRUE;
}


/***********************************************************************
 *		SetupDiOpenClassRegKey  (SETUPAPI.@)
 */
HKEY WINAPI SetupDiOpenClassRegKey(
        const GUID* ClassGuid,
        REGSAM samDesired)
{
    return SetupDiOpenClassRegKeyExW(ClassGuid, samDesired,
                                     DIOCR_INSTALLER, NULL, NULL);
}


/***********************************************************************
 *		SetupDiOpenClassRegKeyExA  (SETUPAPI.@)
 */
HKEY WINAPI SetupDiOpenClassRegKeyExA(
        const GUID* ClassGuid,
        REGSAM samDesired,
        DWORD Flags,
        PCSTR MachineName,
        PVOID Reserved)
{
    PWSTR MachineNameW = NULL;
    HKEY hKey;

    TRACE("\n");

    if (MachineName)
    {
        MachineNameW = MultiByteToUnicode(MachineName, CP_ACP);
        if (MachineNameW == NULL)
            return INVALID_HANDLE_VALUE;
    }

    hKey = SetupDiOpenClassRegKeyExW(ClassGuid, samDesired,
                                     Flags, MachineNameW, Reserved);

    MyFree(MachineNameW);

    return hKey;
}


/***********************************************************************
 *		SetupDiOpenClassRegKeyExW  (SETUPAPI.@)
 */
HKEY WINAPI SetupDiOpenClassRegKeyExW(
        const GUID* ClassGuid,
        REGSAM samDesired,
        DWORD Flags,
        PCWSTR MachineName,
        PVOID Reserved)
{
    HKEY hClassesKey;
    HKEY key;
    LPCWSTR lpKeyName;
    LONG l;

    if (MachineName && *MachineName)
    {
        FIXME("Remote access not supported yet!\n");
        return INVALID_HANDLE_VALUE;
    }

    if (Flags == DIOCR_INSTALLER)
    {
        lpKeyName = ControlClass;
    }
    else if (Flags == DIOCR_INTERFACE)
    {
        lpKeyName = DeviceClasses;
    }
    else
    {
        ERR("Invalid Flags parameter!\n");
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    if (!ClassGuid)
    {
        if ((l = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          lpKeyName,
                          0,
                          samDesired,
                          &hClassesKey)))
        {
            SetLastError(l);
            hClassesKey = INVALID_HANDLE_VALUE;
        }
        key = hClassesKey;
    }
    else
    {
        WCHAR bracedGuidString[39];

        SETUPDI_GuidToString(ClassGuid, bracedGuidString);

        if (!(l = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          lpKeyName,
                          0,
                          samDesired,
                          &hClassesKey)))
        {
            if ((l = RegOpenKeyExW(hClassesKey,
                              bracedGuidString,
                              0,
                              samDesired,
                              &key)))
            {
                SetLastError(l);
                key = INVALID_HANDLE_VALUE;
            }
            RegCloseKey(hClassesKey);
        }
        else
        {
            SetLastError(l);
            key = INVALID_HANDLE_VALUE;
        }
    }
    return key;
}

/***********************************************************************
 *		SetupDiOpenDeviceInterfaceW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiOpenDeviceInterfaceW(
       HDEVINFO DeviceInfoSet,
       PCWSTR DevicePath,
       DWORD OpenFlags,
       PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData)
{
    FIXME("%p %s %08x %p\n",
        DeviceInfoSet, debugstr_w(DevicePath), OpenFlags, DeviceInterfaceData);
    return FALSE;
}

/***********************************************************************
 *		SetupDiOpenDeviceInterfaceA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiOpenDeviceInterfaceA(
       HDEVINFO DeviceInfoSet,
       PCSTR DevicePath,
       DWORD OpenFlags,
       PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData)
{
    FIXME("%p %s %08x %p\n", DeviceInfoSet,
        debugstr_a(DevicePath), OpenFlags, DeviceInterfaceData);
    return FALSE;
}

/***********************************************************************
 *		SetupDiSetClassInstallParamsA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiSetClassInstallParamsA(
       HDEVINFO  DeviceInfoSet,
       PSP_DEVINFO_DATA DeviceInfoData,
       PSP_CLASSINSTALL_HEADER ClassInstallParams,
       DWORD ClassInstallParamsSize)
{
    FIXME("%p %p %x %u\n",DeviceInfoSet, DeviceInfoData,
          ClassInstallParams->InstallFunction, ClassInstallParamsSize);
    return FALSE;
}

/***********************************************************************
 *		SetupDiSetClassInstallParamsW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiSetClassInstallParamsW(
       HDEVINFO  DeviceInfoSet,
       PSP_DEVINFO_DATA DeviceInfoData,
       PSP_CLASSINSTALL_HEADER ClassInstallParams,
       DWORD ClassInstallParamsSize)
{
    FIXME("%p %p %x %u\n",DeviceInfoSet, DeviceInfoData,
          ClassInstallParams->InstallFunction, ClassInstallParamsSize);
    return FALSE;
}

/***********************************************************************
 *		SetupDiCallClassInstaller (SETUPAPI.@)
 */
BOOL WINAPI SetupDiCallClassInstaller(
       DI_FUNCTION InstallFunction,
       HDEVINFO DeviceInfoSet,
       PSP_DEVINFO_DATA DeviceInfoData)
{
    FIXME("%d %p %p\n", InstallFunction, DeviceInfoSet, DeviceInfoData);
    return FALSE;
}

/***********************************************************************
 *		SetupDiGetDeviceInstallParamsW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDeviceInstallParamsW(
       HDEVINFO DeviceInfoSet,
       PSP_DEVINFO_DATA DeviceInfoData,
       PSP_DEVINSTALL_PARAMS_W DeviceInstallParams)
{
    FIXME("%p %p %p\n", DeviceInfoSet, DeviceInfoData, DeviceInstallParams);
    return FALSE;
}

/***********************************************************************
 *		SetupDiGetDeviceInstallParamsA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDeviceInstallParamsA(
       HDEVINFO DeviceInfoSet,
       PSP_DEVINFO_DATA DeviceInfoData,
       PSP_DEVINSTALL_PARAMS_A DeviceInstallParams)
{
    FIXME("%p %p %p\n", DeviceInfoSet, DeviceInfoData, DeviceInstallParams);
    return FALSE;
}

/***********************************************************************
 *              SetupDiSetDeviceInstallParamsA  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiSetDeviceInstallParamsA(
       HDEVINFO DeviceInfoSet,
       PSP_DEVINFO_DATA DeviceInfoData,
       PSP_DEVINSTALL_PARAMS_A DeviceInstallParams)
{
    FIXME("(%p, %p, %p) stub\n", DeviceInfoSet, DeviceInfoData, DeviceInstallParams);

    return TRUE;
}

/***********************************************************************
 *              SetupDiSetDeviceInstallParamsW  (SETUPAPI.@)
 */
BOOL WINAPI SetupDiSetDeviceInstallParamsW(
       HDEVINFO DeviceInfoSet,
       PSP_DEVINFO_DATA DeviceInfoData,
       PSP_DEVINSTALL_PARAMS_W DeviceInstallParams)
{
    FIXME("(%p, %p, %p) stub\n", DeviceInfoSet, DeviceInfoData, DeviceInstallParams);

    return TRUE;
}

static HKEY SETUPDI_OpenDevKey(struct device *device, REGSAM samDesired)
{
    HKEY enumKey, key = INVALID_HANDLE_VALUE;
    LONG l;

    l = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Enum, 0, NULL, 0, KEY_ALL_ACCESS,
            NULL, &enumKey, NULL);
    if (!l)
    {
        RegOpenKeyExW(enumKey, device->instanceId, 0, samDesired, &key);
        RegCloseKey(enumKey);
    }
    return key;
}

static HKEY SETUPDI_OpenDrvKey(struct device *device, REGSAM samDesired)
{
    static const WCHAR slash[] = { '\\',0 };
    WCHAR classKeyPath[MAX_PATH];
    HKEY classKey, key = INVALID_HANDLE_VALUE;
    LONG l;

    lstrcpyW(classKeyPath, ControlClass);
    lstrcatW(classKeyPath, slash);
    SETUPDI_GuidToString(&device->set->ClassGuid,
            classKeyPath + lstrlenW(classKeyPath));
    l = RegCreateKeyExW(HKEY_LOCAL_MACHINE, classKeyPath, 0, NULL, 0,
            KEY_ALL_ACCESS, NULL, &classKey, NULL);
    if (!l)
    {
        static const WCHAR fmt[] = { '%','0','4','u',0 };
        WCHAR devId[10];

        sprintfW(devId, fmt, device->devnode);
        l = RegOpenKeyExW(classKey, devId, 0, samDesired, &key);
        RegCloseKey(classKey);
        if (l)
        {
            SetLastError(ERROR_KEY_DOES_NOT_EXIST);
            return INVALID_HANDLE_VALUE;
        }
    }
    return key;
}

/***********************************************************************
 *		SetupDiOpenDevRegKey (SETUPAPI.@)
 */
HKEY WINAPI SetupDiOpenDevRegKey(
       HDEVINFO DeviceInfoSet,
       PSP_DEVINFO_DATA DeviceInfoData,
       DWORD Scope,
       DWORD HwProfile,
       DWORD KeyType,
       REGSAM samDesired)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device *device;
    HKEY key = INVALID_HANDLE_VALUE;

    TRACE("%p %p %d %d %d %x\n", DeviceInfoSet, DeviceInfoData,
          Scope, HwProfile, KeyType, samDesired);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    if (!DeviceInfoData || DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)
            || !DeviceInfoData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (Scope != DICS_FLAG_GLOBAL && Scope != DICS_FLAG_CONFIGSPECIFIC)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return INVALID_HANDLE_VALUE;
    }
    if (KeyType != DIREG_DEV && KeyType != DIREG_DRV)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return INVALID_HANDLE_VALUE;
    }
    device = (struct device *)DeviceInfoData->Reserved;
    if (device->set != set)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (device->phantom)
    {
        SetLastError(ERROR_DEVINFO_NOT_REGISTERED);
        return INVALID_HANDLE_VALUE;
    }
    if (Scope != DICS_FLAG_GLOBAL)
        FIXME("unimplemented for scope %d\n", Scope);
    switch (KeyType)
    {
        case DIREG_DEV:
            key = SETUPDI_OpenDevKey(device, samDesired);
            break;
        case DIREG_DRV:
            key = SETUPDI_OpenDrvKey(device, samDesired);
            break;
        default:
            WARN("unknown KeyType %d\n", KeyType);
    }
    return key;
}

static BOOL SETUPDI_DeleteDevKey(struct device *device)
{
    HKEY enumKey;
    BOOL ret = FALSE;
    LONG l;

    l = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Enum, 0, NULL, 0, KEY_ALL_ACCESS,
            NULL, &enumKey, NULL);
    if (!l)
    {
        ret = RegDeleteTreeW(enumKey, device->instanceId);
        RegCloseKey(enumKey);
    }
    else
        SetLastError(l);
    return ret;
}

static BOOL SETUPDI_DeleteDrvKey(struct device *device)
{
    static const WCHAR slash[] = { '\\',0 };
    WCHAR classKeyPath[MAX_PATH];
    HKEY classKey;
    LONG l;
    BOOL ret = FALSE;

    lstrcpyW(classKeyPath, ControlClass);
    lstrcatW(classKeyPath, slash);
    SETUPDI_GuidToString(&device->set->ClassGuid,
            classKeyPath + lstrlenW(classKeyPath));
    l = RegCreateKeyExW(HKEY_LOCAL_MACHINE, classKeyPath, 0, NULL, 0,
            KEY_ALL_ACCESS, NULL, &classKey, NULL);
    if (!l)
    {
        static const WCHAR fmt[] = { '%','0','4','u',0 };
        WCHAR devId[10];

        sprintfW(devId, fmt, device->devnode);
        ret = RegDeleteTreeW(classKey, devId);
        RegCloseKey(classKey);
    }
    else
        SetLastError(l);
    return ret;
}

/***********************************************************************
 *		SetupDiDeleteDevRegKey (SETUPAPI.@)
 */
BOOL WINAPI SetupDiDeleteDevRegKey(
       HDEVINFO DeviceInfoSet,
       PSP_DEVINFO_DATA DeviceInfoData,
       DWORD Scope,
       DWORD HwProfile,
       DWORD KeyType)
{
    struct DeviceInfoSet *set = DeviceInfoSet;
    struct device *device;
    BOOL ret = FALSE;

    TRACE("%p %p %d %d %d\n", DeviceInfoSet, DeviceInfoData, Scope, HwProfile,
            KeyType);

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (set->magic != SETUP_DEVICE_INFO_SET_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!DeviceInfoData || DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)
            || !DeviceInfoData->Reserved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (Scope != DICS_FLAG_GLOBAL && Scope != DICS_FLAG_CONFIGSPECIFIC)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }
    if (KeyType != DIREG_DEV && KeyType != DIREG_DRV && KeyType != DIREG_BOTH)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }
    device = (struct device *)DeviceInfoData->Reserved;
    if (device->set != set)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (device->phantom)
    {
        SetLastError(ERROR_DEVINFO_NOT_REGISTERED);
        return FALSE;
    }
    if (Scope != DICS_FLAG_GLOBAL)
        FIXME("unimplemented for scope %d\n", Scope);
    switch (KeyType)
    {
        case DIREG_DEV:
            ret = SETUPDI_DeleteDevKey(device);
            break;
        case DIREG_DRV:
            ret = SETUPDI_DeleteDrvKey(device);
            break;
        case DIREG_BOTH:
            ret = SETUPDI_DeleteDevKey(device);
            if (ret)
                ret = SETUPDI_DeleteDrvKey(device);
            break;
        default:
            WARN("unknown KeyType %d\n", KeyType);
    }
    return ret;
}

/***********************************************************************
 *              CM_Get_Device_IDA  (SETUPAPI.@)
 */
CONFIGRET WINAPI CM_Get_Device_IDA(DEVINST devnode, char *buffer, ULONG len, ULONG flags)
{
    struct device *device = get_devnode_device(devnode);

    TRACE("%u, %p, %u, %#x\n", devnode, buffer, len, flags);

    if (!device)
        return CR_NO_SUCH_DEVINST;

    WideCharToMultiByte(CP_ACP, 0, device->instanceId, -1, buffer, len, 0, 0);
    TRACE("Returning %s\n", debugstr_a(buffer));
    return CR_SUCCESS;
}

/***********************************************************************
 *              CM_Get_Device_IDW  (SETUPAPI.@)
 */
CONFIGRET WINAPI CM_Get_Device_IDW(DEVINST devnode, WCHAR *buffer, ULONG len, ULONG flags)
{
    struct device *device = get_devnode_device(devnode);

    TRACE("%u, %p, %u, %#x\n", devnode, buffer, len, flags);

    if (!device)
        return CR_NO_SUCH_DEVINST;

    lstrcpynW(buffer, device->instanceId, len);
    TRACE("Returning %s\n", debugstr_w(buffer));
    return CR_SUCCESS;
}

/***********************************************************************
 *              CM_Get_Device_ID_Size  (SETUPAPI.@)
 */
CONFIGRET WINAPI CM_Get_Device_ID_Size(ULONG *len, DEVINST devnode, ULONG flags)
{
    struct device *device = get_devnode_device(devnode);

    TRACE("%p, %u, %#x\n", len, devnode, flags);

    if (!device)
        return CR_NO_SUCH_DEVINST;

    *len = lstrlenW(device->instanceId);
    return CR_SUCCESS;
}

/***********************************************************************
 *      SetupDiGetINFClassA (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetINFClassA(PCSTR inf, LPGUID class_guid, PSTR class_name,
        DWORD size, PDWORD required_size)
{
    BOOL retval;
    DWORD required_sizeA, required_sizeW;
    PWSTR class_nameW = NULL;
    UNICODE_STRING infW;

    if (inf)
    {
        if (!RtlCreateUnicodeStringFromAsciiz(&infW, inf))
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
    }
    else
        infW.Buffer = NULL;

    if (class_name && size)
    {
        if (!(class_nameW = HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR))))
        {
            RtlFreeUnicodeString(&infW);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
    }

    retval = SetupDiGetINFClassW(infW.Buffer, class_guid, class_nameW, size, &required_sizeW);

    if (retval)
    {
        required_sizeA = WideCharToMultiByte( CP_ACP, 0, class_nameW, required_sizeW,
                                              class_name, size, NULL, NULL);

        if(required_size) *required_size = required_sizeA;
    }
    else
        if(required_size) *required_size = required_sizeW;

    HeapFree(GetProcessHeap(), 0, class_nameW);
    RtlFreeUnicodeString(&infW);
    return retval;
}

/***********************************************************************
 *              SetupDiGetINFClassW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetINFClassW(PCWSTR inf, LPGUID class_guid, PWSTR class_name,
        DWORD size, PDWORD required_size)
{
    BOOL have_guid, have_name;
    DWORD dret;
    WCHAR buffer[MAX_PATH];

    if (!inf)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(inf))
    {
        FIXME("%s not found. Searching via DevicePath not implemented\n", debugstr_w(inf));
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }

    if (!class_guid || !class_name || !size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!GetPrivateProfileStringW(Version, Signature, NULL, buffer, MAX_PATH, inf))
        return FALSE;

    if (lstrcmpiW(buffer, Chicago) && lstrcmpiW(buffer, WindowsNT))
        return FALSE;

    buffer[0] = '\0';
    have_guid = 0 < GetPrivateProfileStringW(Version, ClassGUID, NULL, buffer, MAX_PATH, inf);
    if (have_guid)
    {
        buffer[lstrlenW(buffer)-1] = 0;
        if (RPC_S_OK != UuidFromStringW(buffer + 1, class_guid))
        {
            FIXME("failed to convert \"%s\" into a guid\n", debugstr_w(buffer));
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    buffer[0] = '\0';
    dret = GetPrivateProfileStringW(Version, Class, NULL, buffer, MAX_PATH, inf);
    have_name = 0 < dret;

    if (dret >= MAX_PATH -1) FIXME("buffer might be too small\n");
    if (have_guid && !have_name) FIXME("class name lookup via guid not implemented\n");

    if (have_name)
    {
        if (dret < size) lstrcpyW(class_name, buffer);
        else
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            have_name = FALSE;
        }
    }

    if (required_size) *required_size = dret + ((dret) ? 1 : 0);

    return (have_guid || have_name);
}

/***********************************************************************
 *              SetupDiGetDevicePropertyW (SETUPAPI.@)
 */
BOOL WINAPI SetupDiGetDevicePropertyW(HDEVINFO info_set, PSP_DEVINFO_DATA info_data,
                const DEVPROPKEY *prop_key, DEVPROPTYPE *prop_type, BYTE *prop_buff,
                DWORD prop_buff_size, DWORD *required_size, DWORD flags)
{
    FIXME("%p, %p, %p, %p, %p, %d, %p, 0x%08x stub\n", info_set, info_data, prop_key,
               prop_type, prop_buff, prop_buff_size, required_size, flags);

    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}

/*
 * Resources
 *
 * Copyright 1993 Robert J. Amstadt
 * Copyright 1995 Alexandre Julliard
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "arch.h"
#include "windows.h"
#include "gdi.h"
#include "global.h"
#include "neexe.h"
#include "accel.h"
#include "module.h"
#include "resource.h"
#include "stddebug.h"
#include "debug.h"
#include "libres.h"

#define PrintId(name) \
    if (HIWORD((DWORD)name)) \
        dprintf_resource( stddeb, "'%s'", (char *)PTR_SEG_TO_LIN(name)); \
    else \
        dprintf_resource( stddeb, "#%04x", LOWORD(name)); 


/**********************************************************************
 *	    FindResource    (KERNEL.60)
 */
HRSRC FindResource( HMODULE hModule, SEGPTR name, SEGPTR type )
{
    NE_MODULE *pModule;

    hModule = GetExePtr( hModule );  /* In case we were passed an hInstance */
    dprintf_resource(stddeb, "FindResource: module=%04x type=", hModule );
    PrintId( type );
    if (HIWORD(name))  /* Check for '#xxx' name */
    {
	char *ptr = PTR_SEG_TO_LIN( name );
	if (ptr[0] == '#') {
	    if (!(name = (SEGPTR)atoi( ptr + 1 ))) return 0;
	}
    }
    dprintf_resource( stddeb, " name=" );
    PrintId( name );
    dprintf_resource( stddeb, "\n" );
    if (!(pModule = MODULE_GetPtr( hModule ))) return 0;
#ifndef WINELIB
    if (pModule->flags & NE_FFLAGS_WIN32)
    {
        fprintf(stderr,"Don't know how to FindResource() for Win32 module\n");
        return 0;
    }
    return NE_FindResource( hModule, type, name );
#else
    return LIBRES_FindResource( hModule, name, type );
#endif
}


/**********************************************************************
 *	    LoadResource    (KERNEL.61)
 */
HGLOBAL LoadResource( HMODULE hModule, HRSRC hRsrc )
{
    NE_MODULE *pModule;

    hModule = GetExePtr( hModule );  /* In case we were passed an hInstance */
    dprintf_resource(stddeb, "LoadResource: module=%04x res=%04x\n",
                     hModule, hRsrc );
    if (!hRsrc) return 0;
    if (!(pModule = MODULE_GetPtr( hModule ))) return 0;
#ifndef WINELIB
    if (pModule->flags & NE_FFLAGS_WIN32)
    {
        fprintf(stderr,"Don't know how to LoadResource() for Win32 module\n");
        return 0;
    }
    return NE_LoadResource( hModule, hRsrc );
#else
    return LIBRES_LoadResource( hModule, hRsrc );
#endif
}


/**********************************************************************
 *	    LockResource    (KERNEL.62)
 */
/* 16-bit version */
SEGPTR WIN16_LockResource( HGLOBAL handle )
{
#ifndef WINELIB
    HMODULE hModule;
    NE_MODULE *pModule;

    dprintf_resource(stddeb, "LockResource: handle=%04x\n", handle );
    if (!handle) return (SEGPTR)0;
    hModule = GetExePtr( handle );
    if (!(pModule = MODULE_GetPtr( hModule ))) return 0;
    if (pModule->flags & NE_FFLAGS_WIN32)
    {
        fprintf(stderr,"Don't know how to LockResource() for Win32 module\n");
        return 0;
    }
    return NE_LockResource( hModule, handle );
#else
    return LIBRES_LockResource( handle );
#endif
}

/* 32-bit version */
LPVOID LockResource( HGLOBAL handle )
{
#ifndef WINELIB
    HMODULE hModule;
    NE_MODULE *pModule;

    dprintf_resource(stddeb, "LockResource: handle=%04x\n", handle );
    if (!handle) return NULL;
    hModule = GetExePtr( handle );
    if (!(pModule = MODULE_GetPtr( hModule ))) return 0;
    if (pModule->flags & NE_FFLAGS_WIN32)
    {
        fprintf(stderr,"Don't know how to LockResource() for Win32 module\n");
        return 0;
    }
    return (LPSTR)PTR_SEG_TO_LIN( NE_LockResource( hModule, handle ) );
#else
    return LIBRES_LockResource( handle );
#endif
}


/**********************************************************************
 *	    FreeResource    (KERNEL.63)
 */
BOOL FreeResource( HGLOBAL handle )
{
#ifndef WINELIB
    HMODULE hModule;
    NE_MODULE *pModule;

    dprintf_resource(stddeb, "FreeResource: handle=%04x\n", handle );
    if (!handle) return FALSE;
    hModule = GetExePtr( handle );
    if (!(pModule = MODULE_GetPtr( hModule ))) return 0;
    if (pModule->flags & NE_FFLAGS_WIN32)
    {
        fprintf(stderr,"Don't know how to FreeResource() for Win32 module\n");
        return 0;
    }
    return NE_FreeResource( hModule, handle );
#else
    return LIBRES_FreeResource( handle );
#endif
}


/**********************************************************************
 *	    AccessResource    (KERNEL.64)
 */
INT AccessResource( HINSTANCE hModule, HRSRC hRsrc )
{
    NE_MODULE *pModule;

    hModule = GetExePtr( hModule );  /* In case we were passed an hInstance */
    dprintf_resource(stddeb, "AccessResource: module=%04x res=%04x\n",
                     hModule, hRsrc );
    if (!hRsrc) return 0;
    if (!(pModule = MODULE_GetPtr( hModule ))) return 0;
#ifndef WINELIB
    if (pModule->flags & NE_FFLAGS_WIN32)
    {
        fprintf(stderr,"Don't know how to AccessResource() for Win32 module\n");
        return 0;
    }
    return NE_AccessResource( hModule, hRsrc );
#else
    return LIBRES_AccessResource( hModule, hRsrc );
#endif
}


/**********************************************************************
 *	    SizeofResource    (KERNEL.65)
 */
DWORD SizeofResource( HMODULE hModule, HRSRC hRsrc )
{
    NE_MODULE *pModule;

    hModule = GetExePtr( hModule );  /* In case we were passed an hInstance */
    dprintf_resource(stddeb, "SizeofResource: module=%04x res=%04x\n",
                     hModule, hRsrc );
    if (!(pModule = MODULE_GetPtr( hModule ))) return 0;
#ifndef WINELIB
    if (pModule->flags & NE_FFLAGS_WIN32)
    {
        fprintf(stderr,"Don't know how to SizeOfResource() for Win32 module\n");
        return 0;
    }
    return NE_SizeofResource( hModule, hRsrc );
#else
    return LIBRES_SizeofResource( hModule, hRsrc );
#endif
}


/**********************************************************************
 *	    AllocResource    (KERNEL.66)
 */
HGLOBAL AllocResource( HMODULE hModule, HRSRC hRsrc, DWORD size )
{
    NE_MODULE *pModule;

    hModule = GetExePtr( hModule );  /* In case we were passed an hInstance */
    dprintf_resource(stddeb, "AllocResource: module=%04x res=%04x size=%ld\n",
                     hModule, hRsrc, size );
    if (!hRsrc) return 0;
    if (!(pModule = MODULE_GetPtr( hModule ))) return 0;
#ifndef WINELIB
    if (pModule->flags & NE_FFLAGS_WIN32)
    {
        fprintf(stderr,"Don't know how to AllocResource() for Win32 module\n");
        return 0;
    }
    return NE_AllocResource( hModule, hRsrc, size );
#else
    return LIBRES_AllocResource( hModule, hRsrc, size );
#endif
}

/**********************************************************************
 *      DirectResAlloc    (KERNEL.168)
 *
 * Check Schulman, p. 232 for details
 */
HANDLE DirectResAlloc(HANDLE hInstance, WORD wType, WORD wSize)
{
    dprintf_resource(stddeb,"DirectResAlloc(%04x,%04x,%04x)\n",
                     hInstance, wType, wSize );
    hInstance = GetExePtr(hInstance);
    if(!hInstance)return 0;
    if(wType != 0x10)	/* 0x10 is the only observed value, passed from
                           CreateCursorIndirect. */
        fprintf(stderr, "DirectResAlloc: wType = %x\n", wType);
    return GLOBAL_Alloc(GMEM_MOVEABLE, wSize, hInstance, FALSE, FALSE, FALSE);
}


/**********************************************************************
 *			LoadAccelerators	[USER.177]
 */
HANDLE LoadAccelerators(HANDLE instance, SEGPTR lpTableName)
{
    HANDLE 	hAccel;
    HANDLE 	rsc_mem;
    HRSRC hRsrc;
    BYTE 	*lp;
    ACCELHEADER	*lpAccelTbl;
    int 	i, n;

    if (HIWORD(lpTableName))
        dprintf_accel( stddeb, "LoadAccelerators: %04x '%s'\n",
                      instance, (char *)PTR_SEG_TO_LIN( lpTableName ) );
    else
        dprintf_accel( stddeb, "LoadAccelerators: %04x %04x\n",
                       instance, LOWORD(lpTableName) );

    if (!(hRsrc = FindResource( instance, lpTableName, RT_ACCELERATOR )))
      return 0;
    if (!(rsc_mem = LoadResource( instance, hRsrc ))) return 0;

    lp = (BYTE *)LockResource(rsc_mem);
    n = SizeofResource( instance, hRsrc ) / sizeof(ACCELENTRY);
    hAccel = GlobalAlloc16(GMEM_MOVEABLE, 
    	sizeof(ACCELHEADER) + (n + 1)*sizeof(ACCELENTRY));
    lpAccelTbl = (LPACCELHEADER)GlobalLock16(hAccel);
    lpAccelTbl->wCount = 0;
    for (i = 0; i < n; i++) {
	lpAccelTbl->tbl[i].type = *(lp++);
	lpAccelTbl->tbl[i].wEvent = *((WORD *)lp);
	lp += 2;
	lpAccelTbl->tbl[i].wIDval = *((WORD *)lp);
	lp += 2;
    	if (lpAccelTbl->tbl[i].wEvent == 0) break;
	dprintf_accel(stddeb,
		"Accelerator #%u / event=%04X id=%04X type=%02X \n", 
		i, lpAccelTbl->tbl[i].wEvent, lpAccelTbl->tbl[i].wIDval, 
		lpAccelTbl->tbl[i].type);
	lpAccelTbl->wCount++;
 	}
    GlobalUnlock16(hAccel);
    FreeResource( rsc_mem );
    return hAccel;
}

/**********************************************************************
 *			TranslateAccelerator 	[USER.178]
 */
int TranslateAccelerator(HWND hWnd, HANDLE hAccel, LPMSG msg)
{
    ACCELHEADER	*lpAccelTbl;
    int 	i;
    
    if (hAccel == 0 || msg == NULL) return 0;
    if (msg->message != WM_KEYDOWN &&
    	msg->message != WM_KEYUP &&
	msg->message != WM_SYSKEYDOWN &&
	msg->message != WM_SYSKEYUP &&
    	msg->message != WM_CHAR) return 0;

    dprintf_accel(stddeb, "TranslateAccelerators hAccel=%04x !\n", hAccel);

    lpAccelTbl = (LPACCELHEADER)GlobalLock16(hAccel);
    for (i = 0; i < lpAccelTbl->wCount; i++) {
	if(lpAccelTbl->tbl[i].type & VIRTKEY_ACCEL) {
	    if(msg->wParam == lpAccelTbl->tbl[i].wEvent &&
	       (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN)) {
		INT mask = 0;

		if(GetKeyState(VK_SHIFT) & 0x8000) mask |= SHIFT_ACCEL;
		if(GetKeyState(VK_CONTROL) & 0x8000) mask |= CONTROL_ACCEL;
		if(GetKeyState(VK_MENU) & 0x8000) mask |= ALT_ACCEL;
		if(mask == (lpAccelTbl->tbl[i].type &
			    (SHIFT_ACCEL | CONTROL_ACCEL | ALT_ACCEL))) {
		    SendMessage16(hWnd, WM_COMMAND, lpAccelTbl->tbl[i].wIDval,
				0x00010000L);
		    GlobalUnlock16(hAccel);
		    return 1;
	        }
		if (msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP)
		    return 1;
	    }
	}
	else {
	    if (msg->wParam == lpAccelTbl->tbl[i].wEvent &&
		msg->message == WM_CHAR) {
		SendMessage16(hWnd, WM_COMMAND, lpAccelTbl->tbl[i].wIDval, 0x00010000L);
		GlobalUnlock16(hAccel);
		return 1;
		}
	    }
	}
    GlobalUnlock16(hAccel);
    return 0;
}

/**********************************************************************
 *					LoadString
 */
int
LoadString(HANDLE instance, WORD resource_id, LPSTR buffer, int buflen)
{
    HANDLE hmem, hrsrc;
    unsigned char *p;
    int string_num;
    int i;

    dprintf_resource(stddeb,"LoadString: inst=%04x id=%04x buff=%08x len=%d\n",
                     instance, resource_id, (int) buffer, buflen);

    hrsrc = FindResource( instance, (SEGPTR)((resource_id>>4)+1), RT_STRING );
    if (!hrsrc) return 0;
    hmem = LoadResource( instance, hrsrc );
    if (!hmem) return 0;
    
    p = LockResource(hmem);
    string_num = resource_id & 0x000f;
    for (i = 0; i < string_num; i++)
	p += *p + 1;
    
    dprintf_resource( stddeb, "strlen = %d\n", (int)*p );
    
    i = MIN(buflen - 1, *p);
    if (buffer == NULL)
	return i;
    if (i > 0) {
	memcpy(buffer, p + 1, i);
	buffer[i] = '\0';
    } else {
	if (buflen > 1) {
	    buffer[0] = '\0';
	    return 0;
	}
	fprintf(stderr,"LoadString // I dont know why , but caller give buflen=%d *p=%d !\n", buflen, *p);
	fprintf(stderr,"LoadString // and try to obtain string '%s'\n", p + 1);
    }
    FreeResource( hmem );

    dprintf_resource(stddeb,"LoadString // '%s' copied !\n", buffer);
    return i;
}




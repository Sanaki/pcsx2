///////////////////////////////////////////////////////////////////////////////
// Name:        wx/msw/wrapcctl.h
// Purpose:     Wrapper for the standard <commctrl.h> header
// Author:      Vadim Zeitlin
// Modified by:
// Created:     03.08.2003
// Copyright:   (c) 2003 Vadim Zeitlin <vadim@wxwidgets.org>
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

#ifndef _WX_MSW_WRAPCCTL_H_
#define _WX_MSW_WRAPCCTL_H_

#include "wx/msw/wrapwin.h"

#include <commctrl.h>

// define things which might be missing from our commctrl.h
#include "wx/msw/missing.h"

// Set Unicode format for a common control
inline void wxSetCCUnicodeFormat(HWND WXUNUSED_IN_WINCE(hwnd))
{
    ::SendMessage(hwnd, CCM_SETUNICODEFORMAT, wxUSE_UNICODE, 0);
}

#endif // _WX_MSW_WRAPCCTL_H_

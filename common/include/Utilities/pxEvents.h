/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once
#include "wx/wx.h"

typedef void FnType_Void();

// --------------------------------------------------------------------------------------
//  SynchronousActionState
// --------------------------------------------------------------------------------------
class SynchronousActionState
{
    DeclareNoncopyableObject(SynchronousActionState);

protected:
    bool m_posted;
    Threading::Semaphore m_sema;

public:
    sptr return_value;

    SynchronousActionState()
    {
        m_posted = false;
        return_value = 0;
    }

    virtual ~SynchronousActionState() = default;

    int WaitForResult();
    void PostResult(int res);
    void ClearResult();
    void PostResult();
};


// --------------------------------------------------------------------------------------
//  pxActionEvent
// --------------------------------------------------------------------------------------
class pxActionEvent;

wxDECLARE_EVENT(pxEvt_InvokeAction, pxActionEvent);

class pxActionEvent : public wxEvent
{
    wxDECLARE_DYNAMIC_CLASS_NO_ASSIGN(pxActionEvent);

protected:
    SynchronousActionState *m_state;

public:
    virtual ~pxActionEvent() = default;
    virtual pxActionEvent *Clone() const { return new pxActionEvent(*this); }

    explicit pxActionEvent(SynchronousActionState *sema = NULL, int msgtype = pxEvt_InvokeAction);
    explicit pxActionEvent(SynchronousActionState &sema, int msgtype = pxEvt_InvokeAction);
    pxActionEvent(const pxActionEvent &src);

    void SetSyncState(SynchronousActionState *obj) { m_state = obj; }
    void SetSyncState(SynchronousActionState &obj) { m_state = &obj; }
};

/*
 *  ReactOS W32 Subsystem
 *  Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  $Id$
 *
 *  COPYRIGHT:        See COPYING in the top level directory
 *  PROJECT:          ReactOS kernel
 *  PURPOSE:          Window painting function
 *  FILE:             subsys/win32k/ntuser/painting.c
 *  PROGRAMER:        Filip Navara (xnavara@volny.cz)
 *  REVISION HISTORY:
 *       06/06/2001   Created (?)
 *       18/11/2003   Complete rewrite
 */

/* INCLUDES ******************************************************************/

#include <w32k.h>

#define NDEBUG
#include <debug.h>

#define DCX_USESTYLE 0x10000

/* PRIVATE FUNCTIONS **********************************************************/

VOID FASTCALL
IntValidateParent(PWINDOW_OBJECT Child, HRGN ValidRegion)
{
   PWINDOW_OBJECT ParentWindow = Child->ParentWnd, OldWindow;

   while (ParentWindow)
   {
      if (!(ParentWindow->Style & WS_CLIPCHILDREN))
      {
         //         IntLockWindowUpdate(ParentWindow);
         if (ParentWindow->UpdateRegion != 0)
         {
            INT OffsetX, OffsetY;

            /*
             * We must offset the child region by the offset of the
             * child rect in the parent.
             */
            OffsetX = Child->WindowRect.left - ParentWindow->WindowRect.left;
            OffsetY = Child->WindowRect.top - ParentWindow->WindowRect.top;
            NtGdiOffsetRgn(ValidRegion, OffsetX, OffsetY);
            NtGdiCombineRgn(ParentWindow->UpdateRegion, ParentWindow->UpdateRegion,
                            ValidRegion, RGN_DIFF);
            /* FIXME: If the resulting region is empty, remove fake posted paint message */
            NtGdiOffsetRgn(ValidRegion, -OffsetX, -OffsetY);
         }
         //         IntUnLockWindowUpdate(ParentWindow);
      }
      OldWindow = ParentWindow;
      ParentWindow = ParentWindow->ParentWnd;
   }
}

/*
 * coUserPaintWindows
 *
 * Internal function used by UserRedrawWindow.
 */

STATIC VOID FASTCALL
co_UserPaintWindows(PWINDOW_OBJECT Wnd, ULONG Flags)
{
   HDC hDC;
   HRGN TempRegion;

   if (Flags & (RDW_ERASENOW | RDW_UPDATENOW))
   {
      if (Wnd->Flags & WINDOWOBJECT_NEED_NCPAINT)
      {
         //          IntLockWindowUpdate(Window);
         if (Wnd->NCUpdateRegion)
         {
            IntValidateParent(Wnd, Wnd->NCUpdateRegion);
         }
         TempRegion = Wnd->NCUpdateRegion;
         if ((HANDLE) 1 != TempRegion && NULL != TempRegion)
         {
            GDIOBJ_SetOwnership(TempRegion, PsGetCurrentProcess());
         }
         Wnd->NCUpdateRegion = NULL;
         Wnd->Flags &= ~WINDOWOBJECT_NEED_NCPAINT;
         MsqDecPaintCountQueue(Wnd->Queue);
         //          IntUnLockWindowUpdate(Window);
         co_UserSendMessage(Wnd->hSelf, WM_NCPAINT, (WPARAM)TempRegion, 0);
         if ((HANDLE) 1 != TempRegion && NULL != TempRegion)
         {
            /* NOTE: The region can already be deleted! */
            GDIOBJ_FreeObj(TempRegion, GDI_OBJECT_TYPE_REGION | GDI_OBJECT_TYPE_SILENT);
         }
      }

      if (Wnd->Flags & WINDOWOBJECT_NEED_ERASEBKGND)
      {
         if (Wnd->UpdateRegion)
         {
            /*
             * This surely wrong! Why we would want to validate the parent?
             * It breaks quite a few things including dummy WM_ERASEBKGND
             * implementations that return only TRUE and have corresponding
             * WM_PAINT that doesn't paint the whole client area.
             * I left the code here so that no one will readd it again!
             * - Filip
             */
            /* IntValidateParent(Window, Window->UpdateRegion); */
            hDC = UserGetDCEx(Wnd, 0, DCX_CACHE | DCX_USESTYLE |
                              DCX_INTERSECTUPDATE);
            if (hDC != NULL)
            {
               if (co_UserSendMessage(Wnd->hSelf, WM_ERASEBKGND, (WPARAM)hDC, 0))
               {
                  Wnd->Flags &= ~WINDOWOBJECT_NEED_ERASEBKGND;
               }
               UserReleaseDC(Wnd, hDC);//FIXME!
            }
         }
      }

      if (Flags & RDW_UPDATENOW)
      {
         if (Wnd->UpdateRegion != NULL ||
               Wnd->Flags & WINDOWOBJECT_NEED_INTERNALPAINT)
         {
            co_UserSendMessage(Wnd->hSelf, WM_PAINT, 0, 0);
         }
      }
   }

   /*
    * Check that the window is still valid at this point
    */

   //  if (! IntIsWindow(hWnd))
   //    {
   //      return;
   //    }

   /*
    * Paint child windows.
    */
   if (!(Flags & RDW_NOCHILDREN) && !(Wnd->Style & WS_MINIMIZE) &&
         ((Flags & RDW_ALLCHILDREN) || !(Wnd->Style & WS_CLIPCHILDREN)))
   {
      HWND *List, *phWnd;

      if ((List = IntWinListChildren(Wnd)))
      {
         for (phWnd = List; *phWnd; ++phWnd)
         {
            Wnd = UserGetWindowObject(*phWnd);
            if (Wnd && (Wnd->Style & WS_VISIBLE))
            {
               //recursion is bad
               co_UserPaintWindows(Wnd, Flags);
            }
         }
         ExFreePool(List);
      }
   }
}

/*
 * IntInvalidateWindows
 *
 * Internal function used by UserRedrawWindow.
 */

VOID FASTCALL
IntInvalidateWindows(PWINDOW_OBJECT Window, HRGN hRgn, ULONG Flags)
{
   INT RgnType;
   BOOL HadPaintMessage, HadNCPaintMessage;
   BOOL HasPaintMessage, HasNCPaintMessage;

   /*
    * Clip the given region with window rectangle (or region)
    */

   //   IntLockWindowUpdate(Window);
   if (!Window->WindowRegion || (Window->Style & WS_MINIMIZE))
   {
      HRGN hRgnWindow;

      //      IntUnLockWindowUpdate(Window);
      hRgnWindow = UnsafeIntCreateRectRgnIndirect(&Window->WindowRect);
      NtGdiOffsetRgn(hRgnWindow,
                     -Window->WindowRect.left,
                     -Window->WindowRect.top);
      RgnType = NtGdiCombineRgn(hRgn, hRgn, hRgnWindow, RGN_AND);
      NtGdiDeleteObject(hRgnWindow);
   }
   else
   {
      RgnType = NtGdiCombineRgn(hRgn, hRgn, Window->WindowRegion, RGN_AND);
      //      IntUnLockWindowUpdate(Window);
   }

   /*
    * Save current state of pending updates
    */

   //   IntLockWindowUpdate(Window);
   HadPaintMessage = Window->UpdateRegion != NULL ||
                     Window->Flags & WINDOWOBJECT_NEED_INTERNALPAINT;
   HadNCPaintMessage = Window->Flags & WINDOWOBJECT_NEED_NCPAINT;

   /*
    * Update the region and flags
    */

   if (Flags & RDW_INVALIDATE && RgnType != NULLREGION)
   {
      if (Window->UpdateRegion == NULL)
      {
         Window->UpdateRegion = NtGdiCreateRectRgn(0, 0, 0, 0);
         GDIOBJ_SetOwnership(Window->UpdateRegion, NULL);
      }

      if (NtGdiCombineRgn(Window->UpdateRegion, Window->UpdateRegion,
                          hRgn, RGN_OR) == NULLREGION)
      {
         GDIOBJ_SetOwnership(Window->UpdateRegion, PsGetCurrentProcess());
         NtGdiDeleteObject(Window->UpdateRegion);
         Window->UpdateRegion = NULL;
      }

      if (Flags & RDW_FRAME)
         Window->Flags |= WINDOWOBJECT_NEED_NCPAINT;
      if (Flags & RDW_ERASE)
         Window->Flags |= WINDOWOBJECT_NEED_ERASEBKGND;

      Flags |= RDW_FRAME;
   }

   if (Flags & RDW_VALIDATE && RgnType != NULLREGION)
   {
      if (Window->UpdateRegion != NULL)
      {
         if (NtGdiCombineRgn(Window->UpdateRegion, Window->UpdateRegion,
                             hRgn, RGN_DIFF) == NULLREGION)
         {
            GDIOBJ_SetOwnership(Window->UpdateRegion, PsGetCurrentProcess());
            NtGdiDeleteObject(Window->UpdateRegion);
            Window->UpdateRegion = NULL;
         }
      }

      if (Window->UpdateRegion == NULL)
         Window->Flags &= ~WINDOWOBJECT_NEED_ERASEBKGND;
      if (Flags & RDW_NOFRAME)
         Window->Flags &= ~WINDOWOBJECT_NEED_NCPAINT;
      if (Flags & RDW_NOERASE)
         Window->Flags &= ~WINDOWOBJECT_NEED_ERASEBKGND;
   }

   if (Flags & RDW_INTERNALPAINT)
   {
      Window->Flags |= WINDOWOBJECT_NEED_INTERNALPAINT;
   }

   if (Flags & RDW_NOINTERNALPAINT)
   {
      Window->Flags &= ~WINDOWOBJECT_NEED_INTERNALPAINT;
   }

   /*
    * Split the nonclient update region.
    */

   if (NULL != Window->UpdateRegion)
   {
      HRGN hRgnWindow, hRgnNonClient;

      hRgnWindow = UnsafeIntCreateRectRgnIndirect(&Window->ClientRect);
      NtGdiOffsetRgn(hRgnWindow,
                     -Window->WindowRect.left,
                     -Window->WindowRect.top);

      hRgnNonClient = NtGdiCreateRectRgn(0, 0, 0, 0);
      if (NtGdiCombineRgn(hRgnNonClient, Window->UpdateRegion,
                          hRgnWindow, RGN_DIFF) == NULLREGION)
      {
         NtGdiDeleteObject(hRgnNonClient);
         hRgnNonClient = NULL;
      }
      else
      {
         GDIOBJ_SetOwnership(hRgnNonClient, NULL);
      }

      /*
       * Remove the nonclient region from the standard update region.
       */
      if (NtGdiCombineRgn(Window->UpdateRegion, Window->UpdateRegion,
                          hRgnWindow, RGN_AND) == NULLREGION)
      {
         GDIOBJ_SetOwnership(Window->UpdateRegion, PsGetCurrentProcess());
         NtGdiDeleteObject(Window->UpdateRegion);
         Window->UpdateRegion = NULL;
      }

      if (Window->NCUpdateRegion == NULL)
      {
         Window->NCUpdateRegion = hRgnNonClient;
      }
      else
      {
         if(NULL != hRgnNonClient)
         {
            NtGdiCombineRgn(Window->NCUpdateRegion, Window->NCUpdateRegion,
                            hRgnNonClient, RGN_OR);
            GDIOBJ_SetOwnership(hRgnNonClient, PsGetCurrentProcess());
            NtGdiDeleteObject(hRgnNonClient);
         }
      }

      NtGdiDeleteObject(hRgnWindow);
   }

   /*
    * Process children if needed
    */

   if (!(Flags & RDW_NOCHILDREN) && !(Window->Style & WS_MINIMIZE) &&
         ((Flags & RDW_ALLCHILDREN) || !(Window->Style & WS_CLIPCHILDREN)))
   {
      HWND *List, *phWnd;
      PWINDOW_OBJECT Child;

      if ((List = IntWinListChildren(Window)))
      {
         for (phWnd = List; *phWnd; ++phWnd)
         {
            Child = UserGetWindowObject(*phWnd);
            if(!Child)
            {
               continue;
            }
            if (Child->Style & WS_VISIBLE)
            {
               /*
                * Recursive call to update children UpdateRegion
                */
               HRGN hRgnTemp = NtGdiCreateRectRgn(0, 0, 0, 0);
               NtGdiCombineRgn(hRgnTemp, hRgn, 0, RGN_COPY);
               NtGdiOffsetRgn(hRgnTemp,
                              Window->WindowRect.left - Child->WindowRect.left,
                              Window->WindowRect.top - Child->WindowRect.top);
               IntInvalidateWindows(Child, hRgnTemp, Flags);
               NtGdiDeleteObject(hRgnTemp);
            }

         }
         ExFreePool(List);
      }
   }

   /*
    * Fake post paint messages to window message queue if needed
    */

   HasPaintMessage = Window->UpdateRegion != NULL ||
                     Window->Flags & WINDOWOBJECT_NEED_INTERNALPAINT;
   HasNCPaintMessage = Window->Flags & WINDOWOBJECT_NEED_NCPAINT;

   if (HasPaintMessage != HadPaintMessage)
   {
      if (HadPaintMessage)
         MsqDecPaintCountQueue(Window->Queue);
      else
         MsqIncPaintCountQueue(Window->Queue);
   }

   if (HasNCPaintMessage != HadNCPaintMessage)
   {
      if (HadNCPaintMessage)
         MsqDecPaintCountQueue(Window->Queue);
      else
         MsqIncPaintCountQueue(Window->Queue);
   }

   //   IntUnLockWindowUpdate(Window);
}

/*
 * IntIsWindowDrawable
 *
 * Remarks
 *    Window is drawable when it is visible and all parents are not
 *    minimized.
 */

BOOL FASTCALL
IntIsWindowDrawable(PWINDOW_OBJECT Window)
{
   PWINDOW_OBJECT Old, Wnd = Window;

   do
   {
      if (!(Wnd->Style & WS_VISIBLE) ||
            ((Wnd->Style & WS_MINIMIZE) && (Wnd != Window)))
      {
         return FALSE;
      }
      Old = Wnd;
      Wnd = Wnd->ParentWnd;

   }
   while(Wnd);

   return TRUE;
}

/*
 * UserRedrawWindow
 *
 * Internal version of NtUserRedrawWindow that takes WINDOW_OBJECT as
 * first parameter.
 */

/* should be callout safe */
BOOL FASTCALL
co_UserRedrawWindow(PWINDOW_OBJECT Wnd, const RECT* UpdateRect, HRGN UpdateRgn,
                    ULONG Flags)
{
   HRGN hRgn = NULL;

   /*
    * Step 1.
    * Validation of passed parameters.
    */

   if (!IntIsWindowDrawable(Wnd) ||
         (Flags & (RDW_VALIDATE | RDW_INVALIDATE)) ==
         (RDW_VALIDATE | RDW_INVALIDATE))
   {
      return FALSE;
   }

   /*
    * Step 2.
    * Transform the parameters UpdateRgn and UpdateRect into
    * a region hRgn specified in window coordinates.
    */

   if (Flags & (RDW_INVALIDATE | RDW_VALIDATE))
   {
      if (UpdateRgn != NULL)
      {
         hRgn = NtGdiCreateRectRgn(0, 0, 0, 0);
         NtGdiCombineRgn(hRgn, UpdateRgn, NULL, RGN_COPY);
         NtGdiOffsetRgn(hRgn,
                        Wnd->ClientRect.left - Wnd->WindowRect.left,
                        Wnd->ClientRect.top - Wnd->WindowRect.top);
      }
      else
         if (UpdateRect != NULL)
         {
            hRgn = UnsafeIntCreateRectRgnIndirect((RECT *)UpdateRect);
            NtGdiOffsetRgn(hRgn,
                           Wnd->ClientRect.left - Wnd->WindowRect.left,
                           Wnd->ClientRect.top - Wnd->WindowRect.top);
         }
         else
            if ((Flags & (RDW_INVALIDATE | RDW_FRAME)) == (RDW_INVALIDATE | RDW_FRAME) ||
                  (Flags & (RDW_VALIDATE | RDW_NOFRAME)) == (RDW_VALIDATE | RDW_NOFRAME))
            {
               hRgn = UnsafeIntCreateRectRgnIndirect(&Wnd->WindowRect);
               NtGdiOffsetRgn(hRgn,
                              -Wnd->WindowRect.left,
                              -Wnd->WindowRect.top);
            }
            else
            {
               hRgn = UnsafeIntCreateRectRgnIndirect(&Wnd->ClientRect);
               NtGdiOffsetRgn(hRgn,
                              -Wnd->WindowRect.left,
                              -Wnd->WindowRect.top);
            }
   }

   /*
    * Step 3.
    * Adjust the window update region depending on hRgn and flags.
    */

   if (Flags & (RDW_INVALIDATE | RDW_VALIDATE | RDW_INTERNALPAINT | RDW_NOINTERNALPAINT))
   {
      IntInvalidateWindows(Wnd, hRgn, Flags);
   }

   /*
    * Step 4.
    * Repaint and erase windows if needed.
    */

   if (Flags & (RDW_ERASENOW | RDW_UPDATENOW))
   {
      co_UserPaintWindows(Wnd, Flags);
   }

   /*
    * Step 5.
    * Cleanup ;-)
    */

   if (hRgn != NULL)
   {
      NtGdiDeleteObject(hRgn);
   }

   return TRUE;
}

BOOL FASTCALL
IntIsWindowDirty(PWINDOW_OBJECT Window)
{
   return (Window->Style & WS_VISIBLE) &&
          ((Window->UpdateRegion != NULL) ||
           (Window->Flags & WINDOWOBJECT_NEED_INTERNALPAINT) ||
           (Window->Flags & WINDOWOBJECT_NEED_NCPAINT));
}

HWND STDCALL
IntFindWindowToRepaint(HWND hWnd, PW32THREAD Thread)
{
   PWINDOW_OBJECT Window;
   PWINDOW_OBJECT Child;
   HWND hFoundWnd = NULL;

   Window = UserGetWindowObject(hWnd);
   if (Window == NULL)
      return NULL;

   if (IntIsWindowDirty(Window) &&
         IntWndBelongsToThread(Window, Thread))
   {
      return hWnd;
   }

   for (Child = Window->FirstChild; Child; Child = Child->NextSibling)
   {
      if (IntIsWindowDirty(Child) &&
            IntWndBelongsToThread(Child, Thread))
      {
         hFoundWnd = Child->hSelf;
         break;
      }
   }

   if (hFoundWnd == NULL)
   {
      HWND *List;
      INT i;

      List = IntWinListChildren(Window);
      if (List != NULL)
      {
         for (i = 0; List[i]; i++)
         {
            hFoundWnd = IntFindWindowToRepaint(List[i], Thread);
            if (hFoundWnd != NULL)
               break;
         }
         ExFreePool(List);
      }
   }

   return hFoundWnd;
}

BOOL FASTCALL
IntGetPaintMessage(HWND hWnd, UINT MsgFilterMin, UINT MsgFilterMax,
                   PW32THREAD Thread, MSG *Message, BOOL Remove)
{
   PWINDOW_OBJECT Window;
   PUSER_MESSAGE_QUEUE MessageQueue = &Thread->Queue;

   if (!MessageQueue->PaintPosted)
      return FALSE;

   if ((MsgFilterMin != 0 || MsgFilterMax != 0) &&
         (MsgFilterMin > WM_PAINT || MsgFilterMax < WM_PAINT))
      return FALSE;

   if (hWnd)
      Message->hwnd = IntFindWindowToRepaint(hWnd, PsGetWin32Thread());
   else
      Message->hwnd = IntFindWindowToRepaint(GetHwndSafe(UserGetDesktopWindow()), PsGetWin32Thread());

   if (Message->hwnd == NULL)
   {
      if (NULL == hWnd)
      {
         DPRINT1("PAINTING BUG: Thread marked as containing dirty windows, but no dirty windows found!\n");
         MessageQueue->PaintPosted = 0;
         MessageQueue->PaintCount = 0;
      }
      return FALSE;
   }

   Window = UserGetWindowObject(Message->hwnd);
   if (Window != NULL)
   {
      Message->message = WM_PAINT;
      Message->wParam = Message->lParam = 0;

      return TRUE;
   }

   return FALSE;
}

HWND FASTCALL
co_IntFixCaret(PWINDOW_OBJECT Wnd, LPRECT lprc, UINT flags)
{
   PTHRDCARETINFO CaretInfo;
   PUSER_THREAD_INPUT Input;
   HWND hWndCaret;

   ASSERT(Wnd);

   Input = UserGetCurrentQueue()->Input;
   if (!Input)
      return 0; //FIXME: can Input ever be NULL??
   if (!Input->CaretInfo.hWnd)
      return 0;

   hWndCaret = Input->CaretInfo.hWnd;
   CaretInfo = &Input->CaretInfo;

   if (hWndCaret == Wnd->hSelf ||
         ((flags & SW_SCROLLCHILDREN) && UserIsChildWindow(Wnd, GetWnd(hWndCaret))))
   {
      POINT pt, FromOffset, ToOffset, Offset;
      RECT rcCaret;

      pt.x = CaretInfo->Pos.x;
      pt.y = CaretInfo->Pos.y;
      UserGetClientOrigin(GetWnd(hWndCaret), &FromOffset);
      UserGetClientOrigin(Wnd, &ToOffset);
      Offset.x = FromOffset.x - ToOffset.x;
      Offset.y = FromOffset.y - ToOffset.y;
      rcCaret.left = pt.x;
      rcCaret.top = pt.y;
      rcCaret.right = pt.x + CaretInfo->Size.cx;
      rcCaret.bottom = pt.y + CaretInfo->Size.cy;
      if (IntGdiIntersectRect(lprc, lprc, &rcCaret))
      {
         co_UserHideCaret(0);
         lprc->left = pt.x;
         lprc->top = pt.y;
         return hWndCaret;
      }
   }

   return 0;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * NtUserBeginPaint
 *
 * Status
 *    @implemented
 */

HDC STDCALL
NtUserBeginPaint(HWND hWnd, PAINTSTRUCT* UnsafePs)
{
   PWINDOW_OBJECT Window;
   PAINTSTRUCT Ps;
   PROSRGNDATA Rgn;
   NTSTATUS Status;
   DECLARE_RETURN(HDC);

   DPRINT("Enter NtUserBeginPaint\n");
   UserEnterExclusive();

   if (!(Window = UserGetWindowObject(hWnd)))
   {
      SetLastWin32Error(ERROR_INVALID_WINDOW_HANDLE);
      RETURN( NULL);
   }
   
   co_UserHideCaret(Window);

   if (Window->Flags & WINDOWOBJECT_NEED_NCPAINT)
   {
      HRGN hRgn;

      if (Window->NCUpdateRegion != (HANDLE)1 && Window->NCUpdateRegion != NULL)
      {
         GDIOBJ_SetOwnership(Window->NCUpdateRegion, PsGetCurrentProcess());
      }
      
      hRgn = Window->NCUpdateRegion;
      IntValidateParent(Window, Window->NCUpdateRegion);
      Window->NCUpdateRegion = NULL;
      Window->Flags &= ~WINDOWOBJECT_NEED_NCPAINT;
      MsqDecPaintCountQueue(Window->Queue);
      
      co_UserSendMessage(hWnd, WM_NCPAINT, (WPARAM)hRgn, 0);
      
      if (hRgn != (HANDLE)1 && hRgn != NULL)
      {
         /* NOTE: The region can already by deleted! */
         GDIOBJ_FreeObj(hRgn, GDI_OBJECT_TYPE_REGION | GDI_OBJECT_TYPE_SILENT);
      }
   }

   RtlZeroMemory(&Ps, sizeof(PAINTSTRUCT));
   //FIXME
   Ps.hdc = UserGetDCEx(Window, 0, DCX_INTERSECTUPDATE | DCX_WINDOWPAINT |
                        DCX_USESTYLE);

   if (!Ps.hdc)
   {
      RETURN( NULL);
   }

   //   IntLockWindowUpdate(Window);
   if (Window->UpdateRegion != NULL)
   {
      MsqDecPaintCountQueue(Window->Queue);
      IntValidateParent(Window, Window->UpdateRegion);
      Rgn = RGNDATA_LockRgn(Window->UpdateRegion);
      if (NULL != Rgn)
      {
         UnsafeIntGetRgnBox(Rgn, &Ps.rcPaint);
         RGNDATA_UnlockRgn(Rgn);
         IntGdiOffsetRect(&Ps.rcPaint,
                          Window->WindowRect.left - Window->ClientRect.left,
                          Window->WindowRect.top - Window->ClientRect.top);
      }
      else
      {
         IntGetClientRect(Window, &Ps.rcPaint);
      }
      GDIOBJ_SetOwnership(Window->UpdateRegion, PsGetCurrentProcess());
      NtGdiDeleteObject(Window->UpdateRegion);
      Window->UpdateRegion = NULL;
   }
   else
   {
      if (Window->Flags & WINDOWOBJECT_NEED_INTERNALPAINT)
         MsqDecPaintCountQueue(Window->Queue);
      IntGetClientRect(Window, &Ps.rcPaint);
   }
   Window->Flags &= ~WINDOWOBJECT_NEED_INTERNALPAINT;
   //   IntUnLockWindowUpdate(Window);

   if (Window->Flags & WINDOWOBJECT_NEED_ERASEBKGND)
   {
      Window->Flags &= ~WINDOWOBJECT_NEED_ERASEBKGND;
      
      Ps.fErase = !co_UserSendMessage(hWnd, WM_ERASEBKGND, (WPARAM)Ps.hdc, 0);
   }
   else
   {
      Ps.fErase = FALSE;
   }

   Status = MmCopyToCaller(UnsafePs, &Ps, sizeof(PAINTSTRUCT));
   if (! NT_SUCCESS(Status))
   {
      SetLastNtError(Status);
      RETURN( NULL);
   }

   RETURN( Ps.hdc);

CLEANUP:
   DPRINT("Leave NtUserBeginPaint, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

/*
 * NtUserEndPaint
 *
 * Status
 *    @implemented
 */

BOOL STDCALL
NtUserEndPaint(HWND hWnd, CONST PAINTSTRUCT* lPs)
{
   PWINDOW_OBJECT Wnd;
   DECLARE_RETURN(BOOLEAN);

   DPRINT("Enter NtUserEndPaint\n");
   UserEnterExclusive();

   if (!(Wnd = UserGetWindowObject(hWnd)))
   {
      SetLastWin32Error(ERROR_INVALID_WINDOW_HANDLE);
      RETURN(FALSE);
   }

   //FIXME!!
   UserReleaseDC(Wnd, lPs->hdc);
   
   co_UserShowCaret(Wnd);
   
   RETURN(TRUE);//FIXME!!retval

CLEANUP:
   DPRINT("Leave NtUserEndPaint, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

/*
 * NtUserInvalidateRect
 *
 * Status
 *    @implemented
 */

DWORD STDCALL
NtUserInvalidateRect(HWND hWnd, CONST RECT *Rect, BOOL Erase)
{
   return NtUserRedrawWindow(hWnd, Rect, 0, RDW_INVALIDATE | (Erase ? RDW_ERASE : 0));
}


DWORD FASTCALL
co_UserInvalidateRect(PWINDOW_OBJECT Wnd, CONST RECT *Rect, BOOL Erase)
{
   return co_UserRedrawWindow(Wnd, Rect, 0, RDW_INVALIDATE | (Erase ? RDW_ERASE : 0));
}


/*
 * NtUserInvalidateRgn
 *
 * Status
 *    @implemented
 */

DWORD STDCALL
NtUserInvalidateRgn(HWND hWnd, HRGN Rgn, BOOL Erase)
{
   return NtUserRedrawWindow(hWnd, NULL, Rgn, RDW_INVALIDATE | (Erase ? RDW_ERASE : 0));
}

/*
 * NtUserValidateRgn
 *
 * Status
 *    @implemented
 */

BOOL STDCALL
NtUserValidateRgn(HWND hWnd, HRGN hRgn)
{
   return NtUserRedrawWindow(hWnd, NULL, hRgn, RDW_VALIDATE | RDW_NOCHILDREN);
}

BOOL FASTCALL
co_UserValidateRgn(PWINDOW_OBJECT Wnd, HRGN hRgn)
{
   return co_UserRedrawWindow(Wnd, NULL, hRgn, RDW_VALIDATE | RDW_NOCHILDREN);
}


/*
 * NtUserUpdateWindow
 *
 * Status
 *    @implemented
 */

BOOL STDCALL
NtUserUpdateWindow(HWND hWnd)
{
   return NtUserRedrawWindow(hWnd, NULL, 0, RDW_UPDATENOW | RDW_ALLCHILDREN);
}

/*
 * NtUserGetUpdateRgn
 *
 * Status
 *    @implemented
 */

INT STDCALL //FIX: retval correct?
NtUserGetUpdateRgn(HWND hWnd, HRGN hRgn, BOOL bErase)
{
   PWINDOW_OBJECT Window;
   DECLARE_RETURN(INT);

   DPRINT("Enter NtUserGetUpdateRgn\n");
   UserEnterExclusive();

   if (!(Window = UserGetWindowObject(hWnd)))
   {
      SetLastWin32Error(ERROR_INVALID_WINDOW_HANDLE);
      RETURN(ERROR);
   }

   RETURN(co_UserGetUpdateRgn(Window, hRgn, bErase));

CLEANUP:
   DPRINT("Leave NtUserGetUpdateRgn, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}



INT FASTCALL
co_UserGetUpdateRgn(PWINDOW_OBJECT Window, HRGN hRgn, BOOL bErase)
{
   int RegionType;

   //   IntLockWindowUpdate(Window);
   if (Window->UpdateRegion == NULL)
   {
      RegionType = (NtGdiSetRectRgn(hRgn, 0, 0, 0, 0) ? NULLREGION : ERROR);
   }
   else
   {
      RegionType = NtGdiCombineRgn(hRgn, Window->UpdateRegion, hRgn, RGN_COPY);
      NtGdiOffsetRgn(
         hRgn,
         Window->WindowRect.left - Window->ClientRect.left,
         Window->WindowRect.top - Window->ClientRect.top);
   }
   //   IntUnLockWindowUpdate(Window);

   if (bErase && RegionType != NULLREGION && RegionType != ERROR)
   {
      co_UserRedrawWindow(Window, NULL, NULL, RDW_ERASENOW | RDW_NOCHILDREN);
   }

   return(RegionType);

}




/*
 * NtUserGetUpdateRect
 *
 * Status
 *    @implemented
 */

BOOL STDCALL
NtUserGetUpdateRect(HWND hWnd, LPRECT UnsafeRect, BOOL bErase)
{
   PWINDOW_OBJECT Wnd;
   RECT Rect;
   INT RegionType;
   PROSRGNDATA RgnData;
   BOOL AlwaysPaint;
   NTSTATUS Status;
   DECLARE_RETURN(BOOL);

   DPRINT("Enter NtUserGetUpdateRect\n");
   UserEnterExclusive();

   if (!(Wnd = UserGetWindowObject(hWnd)))
   {
      SetLastWin32Error(ERROR_INVALID_WINDOW_HANDLE);
      RETURN( ERROR);
   }

   if (Wnd->UpdateRegion == NULL)
   {
      Rect.left = Rect.top = Rect.right = Rect.bottom = 0;
   }
   else
   {
      RgnData = RGNDATA_LockRgn(Wnd->UpdateRegion);
      ASSERT(RgnData != NULL);
      RegionType = UnsafeIntGetRgnBox(RgnData, &Rect);
      ASSERT(RegionType != ERROR);
      RGNDATA_UnlockRgn(RgnData);
   }
   
   AlwaysPaint = (Wnd->Flags & WINDOWOBJECT_NEED_NCPAINT) ||
                 (Wnd->Flags & WINDOWOBJECT_NEED_INTERNALPAINT);

   if (bErase && Rect.left < Rect.right && Rect.top < Rect.bottom)
   {
      co_UserRedrawWindow(Wnd, NULL, NULL, RDW_ERASENOW | RDW_NOCHILDREN);
      /* dont care if Wnd is gone */
   }

   if (UnsafeRect != NULL)
   {
      Status = MmCopyToCaller(UnsafeRect, &Rect, sizeof(RECT));
      if (!NT_SUCCESS(Status))
      {
         SetLastWin32Error(ERROR_INVALID_PARAMETER);
         RETURN( FALSE);
      }
   }

   RETURN( (Rect.left < Rect.right && Rect.top < Rect.bottom) || AlwaysPaint);


CLEANUP:
   DPRINT("Leave NtUserGetUpdateRect, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

/*
 * NtUserRedrawWindow
 *
 * Status
 *    @implemented
 */

BOOL STDCALL
NtUserRedrawWindow(HWND hWnd, CONST RECT *lprcUpdate, HRGN hrgnUpdate,
                   UINT flags)
{
   RECT SafeUpdateRect;
   NTSTATUS Status;
   PWINDOW_OBJECT Wnd;
   DECLARE_RETURN(BOOL);

   DPRINT("Enter NtUserRedrawWindow\n");
   UserEnterExclusive();

   if (!hWnd)
   {
      Wnd = UserGetDesktopWindow();
   }
   else if (!(Wnd = UserGetWindowObject(hWnd)))
   {
      SetLastWin32Error(ERROR_INVALID_WINDOW_HANDLE);
      RETURN( FALSE);
   }

   if (lprcUpdate != NULL)
   {
      Status = MmCopyFromCaller(&SafeUpdateRect, (PRECT)lprcUpdate,
                                sizeof(RECT));

      if (!NT_SUCCESS(Status))
      {
         SetLastWin32Error(ERROR_INVALID_PARAMETER);
         RETURN( FALSE);
      }
   }

   Status = co_UserRedrawWindow(Wnd, NULL == lprcUpdate ? NULL : &SafeUpdateRect,
                                hrgnUpdate, flags);
   /* dont care if Wnd is gone */

   if (!NT_SUCCESS(Status))
   {
      /* UserRedrawWindow fails only in case that flags are invalid */
      SetLastWin32Error(ERROR_INVALID_PARAMETER);
      RETURN( FALSE);
   }

   RETURN( TRUE);

CLEANUP:
   DPRINT("Leave NtUserRedrawWindow, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

/*
 * NtUserScrollDC
 *
 * Status
 *    @implemented
 */

DWORD STDCALL
NtUserScrollDC(HDC hDC, INT dx, INT dy, const RECT *lprcScroll,
               const RECT *lprcClip, HRGN hrgnUpdate, LPRECT lprcUpdate)
{
   DECLARE_RETURN(DWORD);

   DPRINT("Enter NtUserScrollDC\n");
   UserEnterExclusive();

   RETURN(UserScrollDC(hDC, dx, dy, lprcScroll, lprcClip, hrgnUpdate, lprcUpdate));

CLEANUP:
   DPRINT("Leave NtUserScrollDC, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}







DWORD FASTCALL
UserScrollDC(HDC hDC, INT dx, INT dy, const RECT *lprcScroll,
             const RECT *lprcClip, HRGN hrgnUpdate, LPRECT lprcUpdate)
{
   RECT rSrc, rClipped_src, rClip, rDst, offset;
   PDC DC;

   /*
    * Compute device clipping region (in device coordinates).
    */

   DC = DC_LockDc(hDC);
   if (NULL == DC)
   {
      return( FALSE);
   }
   if (lprcScroll)
      rSrc = *lprcScroll;
   else
      IntGdiGetClipBox(hDC, &rSrc);
   IntLPtoDP(DC, (LPPOINT)&rSrc, 2);

   if (lprcClip)
      rClip = *lprcClip;
   else
      IntGdiGetClipBox(hDC, &rClip);
   IntLPtoDP(DC, (LPPOINT)&rClip, 2);

   IntGdiIntersectRect(&rClipped_src, &rSrc, &rClip);

   rDst = rClipped_src;
   IntGdiSetRect(&offset, 0, 0, dx, dy);
   IntLPtoDP(DC, (LPPOINT)&offset, 2);
   IntGdiOffsetRect(&rDst, offset.right - offset.left,  offset.bottom - offset.top);
   IntGdiIntersectRect(&rDst, &rDst, &rClip);

   /*
    * Copy bits, if possible.
    */

   if (rDst.bottom > rDst.top && rDst.right > rDst.left)
   {
      RECT rDst_lp = rDst, rSrc_lp = rDst;

      IntGdiOffsetRect(&rSrc_lp, offset.left - offset.right, offset.top - offset.bottom);
      IntDPtoLP(DC, (LPPOINT)&rDst_lp, 2);
      IntDPtoLP(DC, (LPPOINT)&rSrc_lp, 2);
      DC_UnlockDc(DC);

      if (!NtGdiBitBlt(hDC, rDst_lp.left, rDst_lp.top, rDst_lp.right - rDst_lp.left,
                       rDst_lp.bottom - rDst_lp.top, hDC, rSrc_lp.left, rSrc_lp.top,
                       SRCCOPY))
         return( FALSE);
   }
   else
   {
      DC_UnlockDc(DC);
   }

   /*
    * Compute update areas.  This is the clipped source or'ed with the
    * unclipped source translated minus the clipped src translated (rDst)
    * all clipped to rClip.
    */

   if (hrgnUpdate || lprcUpdate)
   {
      HRGN hRgn = hrgnUpdate, hRgn2;

      if (hRgn)
         NtGdiSetRectRgn(hRgn, rClipped_src.left, rClipped_src.top, rClipped_src.right, rClipped_src.bottom);
      else
         hRgn = NtGdiCreateRectRgn(rClipped_src.left, rClipped_src.top, rClipped_src.right, rClipped_src.bottom);

      hRgn2 = UnsafeIntCreateRectRgnIndirect(&rSrc);
      NtGdiOffsetRgn(hRgn2, offset.right - offset.left,  offset.bottom - offset.top);
      NtGdiCombineRgn(hRgn, hRgn, hRgn2, RGN_OR);

      NtGdiSetRectRgn(hRgn2, rDst.left, rDst.top, rDst.right, rDst.bottom);
      NtGdiCombineRgn(hRgn, hRgn, hRgn2, RGN_DIFF);

      NtGdiSetRectRgn(hRgn2, rClip.left, rClip.top, rClip.right, rClip.bottom);
      NtGdiCombineRgn(hRgn, hRgn, hRgn2, RGN_AND);

      if (lprcUpdate)
      {
         NtGdiGetRgnBox(hRgn, lprcUpdate);

         /* Put the lprcUpdate in logical coordinate */
         NtGdiDPtoLP(hDC, (LPPOINT)lprcUpdate, 2);
      }
      if (!hrgnUpdate)
         NtGdiDeleteObject(hRgn);
      NtGdiDeleteObject(hRgn2);
   }
   return(TRUE);


}




/*
 * NtUserScrollWindowEx
 *
 * Status
 *    @implemented
 */

DWORD STDCALL
NtUserScrollWindowEx(HWND hWnd, INT dx, INT dy, const RECT *UnsafeRect,
                     const RECT *UnsafeClipRect, HRGN hrgnUpdate, LPRECT rcUpdate, UINT flags)
{
   RECT rc, cliprc, caretrc, rect, clipRect;
   INT Result;
   PWINDOW_OBJECT Wnd;
   HDC hDC;
   HRGN hrgnTemp;
   HWND hwndCaret;
   BOOL bUpdate = (rcUpdate || hrgnUpdate || flags & (SW_INVALIDATE | SW_ERASE));
   BOOL bOwnRgn = TRUE;
   NTSTATUS Status;
   DECLARE_RETURN(DWORD);

   DPRINT("Enter NtUserScrollWindowEx\n");
   UserEnterExclusive();

   Wnd = UserGetWindowObject(hWnd);
   if (!Wnd || !IntIsWindowDrawable(Wnd))
   {
      RETURN( ERROR);
   }

   IntGetClientRect(Wnd, &rc);
   if (NULL != UnsafeRect)
   {
      Status = MmCopyFromCaller(&rect, UnsafeRect, sizeof(RECT));
      if (! NT_SUCCESS(Status))
      {
         SetLastNtError(Status);
         RETURN( ERROR);
      }
      IntGdiIntersectRect(&rc, &rc, &rect);
   }

   if (NULL != UnsafeClipRect)
   {
      Status = MmCopyFromCaller(&clipRect, UnsafeClipRect, sizeof(RECT));
      if (! NT_SUCCESS(Status))
      {
         SetLastNtError(Status);
         RETURN(ERROR);
      }
      IntGdiIntersectRect(&cliprc, &rc, &clipRect);
   }
   else
      cliprc = rc;

   if (cliprc.right <= cliprc.left || cliprc.bottom <= cliprc.top ||
         (dx == 0 && dy == 0))
   {
      RETURN(NULLREGION);
   }

   caretrc = rc;
   hwndCaret = co_IntFixCaret(Wnd, &caretrc, flags);

   if (hrgnUpdate)
      bOwnRgn = FALSE;
   else if (bUpdate)
      hrgnUpdate = NtGdiCreateRectRgn(0, 0, 0, 0);

   hDC = UserGetDCEx(Wnd, 0, DCX_CACHE | DCX_USESTYLE);
   if (hDC)
   {
      UserScrollDC(hDC, dx, dy, &rc, &cliprc, hrgnUpdate, rcUpdate);
      UserReleaseDC(Wnd, hDC);
   }

   /*
    * Take into account the fact that some damage may have occurred during
    * the scroll.
    */

   hrgnTemp = NtGdiCreateRectRgn(0, 0, 0, 0);
   Result = co_UserGetUpdateRgn(Wnd, hrgnTemp, FALSE);
   
   if (Result != NULLREGION)
   {
      HRGN hrgnClip = UnsafeIntCreateRectRgnIndirect(&cliprc);
      NtGdiOffsetRgn(hrgnTemp, dx, dy);
      NtGdiCombineRgn(hrgnTemp, hrgnTemp, hrgnClip, RGN_AND);
      
      co_UserRedrawWindow(Wnd, NULL, hrgnTemp, RDW_INVALIDATE | RDW_ERASE);
      
      NtGdiDeleteObject(hrgnClip);
   }
   
   NtGdiDeleteObject(hrgnTemp);

   if (flags & SW_SCROLLCHILDREN)
   {
      HWND *List = IntWinListChildren(Wnd);
      if (List)
      {
         int i;
         RECT r, dummy;
         POINT ClientOrigin;
         PWINDOW_OBJECT WindowObject;

         UserGetClientOrigin(Wnd, &ClientOrigin);
         for (i = 0; List[i]; i++)
         {
            WindowObject = UserGetWindowObject(List[i]);
            if (!WindowObject)
               continue;
            r = WindowObject->WindowRect;
            r.left -= ClientOrigin.x;
            r.top -= ClientOrigin.y;
            r.right -= ClientOrigin.x;
            r.bottom -= ClientOrigin.y;

            if (! UnsafeRect || IntGdiIntersectRect(&dummy, &r, &rc))
               co_WinPosSetWindowPos(List[i], 0, r.left + dx, r.top + dy, 0, 0,
                                     SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE |
                                     SWP_NOREDRAW);
         }
         ExFreePool(List);
      }
   }

   if (flags & (SW_INVALIDATE | SW_ERASE))
   {
      co_UserRedrawWindow(Wnd, NULL, hrgnUpdate, RDW_INVALIDATE | RDW_ERASE |
                          ((flags & SW_ERASE) ? RDW_ERASENOW : 0) |
                          ((flags & SW_SCROLLCHILDREN) ? RDW_ALLCHILDREN : 0));
   }

//   if (bOwnRgn && hrgnUpdate)
//      NtGdiDeleteObject(hrgnUpdate);

   if (hwndCaret)
   {
      co_UserSetCaretPos(caretrc.left + dx, caretrc.top + dy);
      co_UserShowCaret(GetWnd(hwndCaret));
   }

   RETURN(Result);

CLEANUP:
   DPRINT("Leave NtUserScrollWindowEx, ret=%i\n",_ret_);
   
   if (bOwnRgn && hrgnUpdate)
      NtGdiDeleteObject(hrgnUpdate);
   
   UserLeave();
   END_CLEANUP;
}

/* EOF */

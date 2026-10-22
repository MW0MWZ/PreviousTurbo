/*
  PreviousTurbo - gui.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The window's menus, dialogs and status bar (Dear ImGui), drawn around the
  emulated screen. All of these run on the main thread.
*/

#ifndef PREV_GUI_H
#define PREV_GUI_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

extern void  Gui_Init(SDL_Window *window, SDL_Renderer *renderer);
extern void  Gui_UnInit(void);

/* Height of the menu bar and status bar together, in window coordinates. */
extern float Gui_BarsHeight(void);

/* Gives an event to the menus and dialogs first: true if they took it. */
extern bool  Gui_HandleEvent(SDL_Event *event);
/* A request sent with GuiEvent_SendGuiRequest(). */
extern void  Gui_HandleRequest(void *request);

/* How long the event loop may wait before the next frame is due. */
extern int   Gui_WaitTimeMs(void);
/* Draws the window if a new frame is due. */
extern void  Gui_Frame(void);
/* Runs what menu items asked for, outside the frame. */
extern void  Gui_RunPosted(void);
/* The emulated screen (or the status bar) changed. */
extern void  Gui_RequestRepaint(void);

/* Between window coordinates and emulated screen pixels. */
extern void  Gui_WindowToScreen(float *x, float *y);
extern void  Gui_ScreenToWindow(float *x, float *y);

/* Lets go of the keys and mouse buttons the machine was told are held
   (the window lost the focus). */
extern void  Gui_ReleaseHeld(void);

/* True while a dialog is open (the emulated machine gets no input). */
extern bool  Gui_ModalActive(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PREV_GUI_H */

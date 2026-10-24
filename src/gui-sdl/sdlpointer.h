/*
  PreviousTurbo - sdlpointer.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef PREV_SDLPOINTER_H
#define PREV_SDLPOINTER_H

#include <stdbool.h>

extern bool Pointer_Known(void);
extern void Pointer_CaptureAt(float x, float y);
extern bool Pointer_Enter(float x, float y);
extern void Pointer_Leave(void);
extern void Pointer_Released(void);
extern bool Pointer_Motion(float wx, float wy, int dx, int dy);
extern void Pointer_Frame(void);
extern int  Pointer_WaitMs(void);

#endif

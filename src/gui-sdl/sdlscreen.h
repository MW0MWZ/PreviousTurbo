/*
  Previous - sdlscreen.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef PREV_SDLSCREEN_H
#define PREV_SDLSCREEN_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <SDL3/SDL.h>

extern SDL_Window  *sdlWindow;

extern void Screen_BlitDimension(uint32_t* vram, SDL_Texture* tex);
extern void Screen_Blank(SDL_Texture* tex);

extern bool Screen_Repaint(void);
extern bool Screen_UpdatePictures(void);
extern int  Screen_GetPictures(SDL_Texture** tex, SDL_FRect* rects, int max);
extern SDL_Renderer* Screen_GetRenderer(void);
extern void Screen_SizeChanged(void);

extern void Screen_Init(void);
extern void Screen_UnInit(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PREV_SDLSCREEN_H */

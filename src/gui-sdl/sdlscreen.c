/*
  Previous - sdlscreen.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This file contains the SDL interface for video output.
*/
const char SDLscreen_fileid[] = "Previous sdlscreen.c";

#include "main.h"
#include "configuration.h"
#include "screen.h"
#include "sdlscreen.h"
#include "sdlpointer.h"
#include "statusbar.h"
#include "event.h"
#include "dimension.hpp"
#include "nd_sdl.hpp"
#include "video.h"
#include "keymap.h"
#include "m68000.h"
#include "gui.h"


/* NeXT screen resolution */
const int NeXT_SCRN_W = 1120;
const int NeXT_SCRN_H = 832;

SDL_Window*   sdlWindow;

/* extern for shortcuts */
volatile bool bGrabMouse    = false; /* Grab the mouse cursor in the window */
volatile bool bInFullScreen = false; /* true if in full screen */

/* extern for tablet */
int screen_w = NeXT_SCRN_W;
int screen_h = NeXT_SCRN_H;

static int width  = NeXT_SCRN_W;
static int height = NeXT_SCRN_H;

static SDL_Renderer* sdlRenderer;
static SDL_Texture*  fbTexture;
static SDL_Texture*  groupTexture[NUM_MONITORS];
static SDL_FRect     fbRect;
static SDL_FRect     groupRect[NUM_MONITORS];
static SDL_Rect      saveWindowBounds; /* Window bounds before going fullscreen. Used to restore window size & position. */
static SCREENMODE    saveScreenMode;   /* Save screen mode to restore on return from fullscreen */
static SCREENMODE    initScreenMode;   /* Save screen mode present at last init */
static int           initScreenWidth;  /* Save screen width at last init */
static int           initScreenHeight; /* Save screen height at last init */
static float         initBarsHeight;   /* Save height of menu and status bars at last init */
static bool          initDone;         /* Window size set at least once */


static uint32_t BW2RGB[0x100][4];
static uint32_t COL2RGB[0x10000];

static uint32_t bw2rgb(const SDL_PixelFormatDetails* fmt, int bw) {
	int c = (~bw & 3) * 0x55;
	return SDL_MapRGB(fmt, NULL, c, c, c);
}

static uint32_t col2rgb(const SDL_PixelFormatDetails* fmt, int col) {
	int r = ((col >> 12) & 0x0F) * 0x11;
	int g = ((col >>  8) & 0x0F) * 0x11;
	int b = ((col >>  4) & 0x0F) * 0x11;
	return SDL_MapRGB(fmt, NULL, r, g, b);
}

/*
 The main screen is converted where its video memory changed: the rows from
 first to last. All of it after the texture is made or the video is turned
 back on.
 */
static bool fbConvertAll = true;  /* convert the whole picture next time */
static bool fbVideoOn;            /* the video was on at the last conversion */

/* Bytes per row of the main screen's video memory */
static int vramRowBytes(void) {
	if (ConfigureParams.System.bColor) {
		return (NeXT_SCRN_W + (ConfigureParams.System.bTurbo ? 0 : 32)) * 2;
	}
	return NeXT_SCRN_W / 4 + (ConfigureParams.System.bTurbo ? 0 : 32 / 4);
}

/* The rows written since the last call; false if none. Each block is
 cleared before its rows are converted, so a write meanwhile is seen next
 time. */
static bool vramDirtyRows(int* first, int* last) {
	int stride = vramRowBytes();
	int blocks = (stride * NeXT_SCRN_H + 511) >> 9;
	int b;

	*first = NeXT_SCRN_H;
	*last  = -1;
	for (b = 0; b < blocks && b < VRAM_DIRTY_BLOCKS; b++) {
		if (vram_dirty[b]) {
			int top    = (b << 9) / stride;
			int bottom = ((b << 9) + 511) / stride;
			vram_dirty[b] = 0;
			if (top < *first) *first = top;
			if (bottom > *last) *last = bottom;
		}
	}
	if (*last >= NeXT_SCRN_H) {
		*last = NeXT_SCRN_H - 1;
	}
	return *last >= *first;
}

static void vramClearDirty(void) {
	int b;
	for (b = 0; b < VRAM_DIRTY_BLOCKS; b++) {
		vram_dirty[b] = 0;
	}
}

/*
 BW format is 2 bit per pixel
 */
static void blitBW(SDL_Texture* tex, int first, int last) {
	void* pixels;
	uint8_t* src;
	uint8_t* dst;
	int pitch, src_padding, dst_padding, x, y;
	SDL_Rect rows = { 0, first, NeXT_SCRN_W, last - first + 1 };

	SDL_LockTexture(tex, &rows, &pixels, &pitch);
	src_padding = ConfigureParams.System.bTurbo ? 0 : (32 / 4);
	src = NEXTVideo + first * (NeXT_SCRN_W / 4 + src_padding);
	dst = (uint8_t*)pixels;
	dst_padding = pitch - NeXT_SCRN_W * 4;
	for (y = first; y <= last; y++) {
		for (x = 0; x < NeXT_SCRN_W / 4; x++) {
			memcpy(dst, BW2RGB[*src++], 16);
			dst += 16;
		}
		src += src_padding;
		dst += dst_padding;
	}
	SDL_UnlockTexture(tex);
}

/*
 Color format is 4 bit per pixel, big-endian: RGBX
 */
static void blitColor(SDL_Texture* tex, int first, int last) {
	void* pixels;
	uint16_t* src;
	uint32_t* dst;
	int pitch, src_padding, dst_padding, x, y;
	SDL_Rect rows = { 0, first, NeXT_SCRN_W, last - first + 1 };

	SDL_LockTexture(tex, &rows, &pixels, &pitch);
	src_padding = ConfigureParams.System.bTurbo ? 0 : 32;
	src = (uint16_t*)NEXTVideo + first * (NeXT_SCRN_W + src_padding);
	dst = (uint32_t*)pixels;
	dst_padding = pitch / 4 - NeXT_SCRN_W;
	for (y = first; y <= last; y++) {
		for (x = 0; x < NeXT_SCRN_W; x++) {
			*dst++ = COL2RGB[*src++];
		}
		src += src_padding;
		dst += dst_padding;
	}
	SDL_UnlockTexture(tex);
}

/*
 Dimension format is 8 bit per pixel, big-endian: BBGGRRAA
 */
void Screen_BlitDimension(uint32_t* vram, SDL_Texture* tex) {
	void* src;
	void* dst;
	int src_pitch, dst_pitch;
	SDL_PixelFormat src_format, dst_format;

#if ND_STEP
	src = &vram[0];
#else
	src = &vram[4];
#endif
	src_pitch  = (NeXT_SCRN_W + 32) * 4;
	src_format = SDL_PIXELFORMAT_BGRA32;
	dst_format = tex->format;

	SDL_LockTexture(tex, NULL, &dst, &dst_pitch);
	SDL_ConvertPixels(NeXT_SCRN_W, NeXT_SCRN_H, src_format, src, src_pitch, dst_format, dst, dst_pitch);
	SDL_UnlockTexture(tex);
}

/*
 Blank screen
 */
void Screen_Blank(SDL_Texture* tex) {
	void* pixels;
	int   pitch;
	SDL_LockTexture(tex, NULL, &pixels, &pitch);
	SDL_memset4(pixels, COL2RGB[0], pitch * NeXT_SCRN_H / 4);
	SDL_UnlockTexture(tex);
}

/*
 Blit NeXT framebuffer to texture.
 */
static bool blitScreen(int slot, SDL_Texture* tex) {
	if (slot > 0) {
		uint32_t* vram = nd_vram_for_slot(slot);
		if (vram) {
			if (nd_video_enabled(slot)) {
				Screen_BlitDimension(vram, tex);
			} else {
				Screen_Blank(tex);
			}
			return true;
		}
	} else {
		if (NEXTVideo) {
			int first = 0, last = NeXT_SCRN_H - 1;
			if (!Video_Enabled()) {
				if (!fbVideoOn && !fbConvertAll) {
					return false;
				}
				Screen_Blank(tex);
				fbVideoOn = fbConvertAll = false;
				return true;
			}
			if (fbConvertAll || !fbVideoOn) {
				vramClearDirty();
				fbConvertAll = false;
				fbVideoOn = true;
			} else if (!vramDirtyRows(&first, &last)) {
				return false;  /* nothing changed */
			}
			if (ConfigureParams.System.bColor) {
				blitColor(tex, first, last);
			} else {
				blitBW(tex, first, last);
			}
			return true;
		}
	}
	return false;
}

/*
 Blit the NeXT framebuffer(s) to their textures. Returns true if any
 texture changed. Called from the main thread before a frame is drawn.
 */
bool Screen_UpdatePictures(void) {
	bool updated = false;
	int i;

	if (!bEmulationActive) {
		return false;
	}
	if (initScreenMode == SCREEN_GROUP) {
		for (i = 0; i < NUM_MONITORS; i++) {
			if (groupTexture[i] && blitScreen(i * 2, groupTexture[i])) {
				updated = true;
			}
		}
	} else if (fbTexture) {
		static int lastSlot = -1;  /* shown in fbTexture */
		if (ConfigureParams.Screen.nSingleModeSlot != lastSlot) {
			lastSlot = ConfigureParams.Screen.nSingleModeSlot;
			fbConvertAll = true;
		}
		updated = blitScreen(ConfigureParams.Screen.nSingleModeSlot, fbTexture);
	}
	return updated;
}

/*
 The textures making up the emulated screen and where each goes, in
 NeXT screen pixels within a screen_w x screen_h area. Returns the count.
 */
int Screen_GetPictures(SDL_Texture** tex, SDL_FRect* rects, int max) {
	int i, n = 0;

	if (initScreenMode == SCREEN_GROUP) {
		for (i = 0; i < NUM_MONITORS && n < max; i++) {
			if (groupTexture[i]) {
				tex[n] = groupTexture[i];
				rects[n] = groupRect[i];
				n++;
			}
		}
	} else if (fbTexture && max > 0) {
		tex[n] = fbTexture;
		rects[n] = fbRect;
		n++;
	}
	return n;
}

SDL_Renderer* Screen_GetRenderer(void) {
	return sdlRenderer;
}

/* Kept for the emulator's repaint requests: the next frame shows the screen. */
bool Screen_Repaint(void) {
	Gui_RequestRepaint();
	return true;
}

/*-----------------------------------------------------------------------*/
/**
 * Force repaint after window size or full screen change
 */
static void Screen_ForceRepaint(void) {
	Gui_RequestRepaint();
}

/*-----------------------------------------------------------------------*/
/**
 * Set Previous window title. Use NULL for default
 */
static void Screen_SetTitle(const char *title) {
	if (title)
		SDL_SetWindowTitle(sdlWindow, title);
	else
		SDL_SetWindowTitle(sdlWindow, PROG_NAME);
}

/*-----------------------------------------------------------------------*/
/**
 * Force things associated with changing between single and all screens.
 */
static void Screen_ModeChanged(void) {
	if (!sdlWindow) {
		/* screen not yet initialized */
		return;
	}

	/* Do not use multiple windows in full screen mode */
	if (bInFullScreen) {
		saveScreenMode = ConfigureParams.Screen.nMode;
		if (ConfigureParams.Screen.nMode == SCREEN_ALL) {
			ConfigureParams.Screen.nMode = SCREEN_SINGLE;
		}
	}
	if (ConfigureParams.Screen.nMode == SCREEN_ALL) {
		nd_sdl_show();
	} else {
		nd_sdl_hide();
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Calculate window size and position to fit into host display bounds.
 */
static void Screen_GetWindowBounds(SDL_Rect* r) {
	SDL_DisplayID d;
	SDL_Rect usable;

	float maxscale = 1.0;
	float scale    = 1.0;
	float bars     = Gui_BarsHeight();

	d = SDL_GetDisplayForWindow(sdlWindow);

	r->x = SDL_WINDOWPOS_CENTERED_DISPLAY(d);
	r->y = SDL_WINDOWPOS_CENTERED_DISPLAY(d);

	/* Keep the picture's current scale if the screen size did not change much */
	if (initDone && width == initScreenWidth && abs(height - initScreenHeight) < NeXT_SCRN_H) {
		int x, y, w, h;
		if (bInFullScreen) {
			r->x = saveWindowBounds.x;
			r->y = saveWindowBounds.y;
			scale = (float)saveWindowBounds.w / width;
		} else {
			if (SDL_GetWindowPosition(sdlWindow, &x, &y)) {
				r->x = x;
				r->y = y;
			}
			if (SDL_GetWindowSize(sdlWindow, &w, &h)) {
				scale = (float)w / width;
			}
		}
	}
	if (SDL_GetDisplayUsableBounds(d, &usable)) {
		int top, left, bottom, right;
		float hscale, wscale;
		if (SDL_GetWindowBordersSize(sdlWindow, &top, &left, &bottom, &right) == false) {
			top = bottom = 50;
			left = right = 25;
		}
		hscale = (float)(usable.h - top - bottom - bars) / height;
		wscale = (float)(usable.w - left - right) / width;
		maxscale = wscale < hscale ? wscale : hscale;
	}
	if (scale > maxscale) {
		scale = maxscale;
		r->x = SDL_WINDOWPOS_CENTERED_DISPLAY(d);
		r->y = SDL_WINDOWPOS_CENTERED_DISPLAY(d);
	}
	if (scale <= 0.0) {
		scale = 1.0;
	}
	r->w = (int)SDL_lroundf((float)width * scale);
	r->h = (int)SDL_lroundf((float)height * scale + bars);
}

/*-----------------------------------------------------------------------*/
/**
 * Create texture with default parameters.
 */
static SDL_Texture* Screen_CreateFramebufferTexture(SDL_PixelFormat format, int w, int h) {
	SDL_Texture* tex;
	
	tex = SDL_CreateTexture(sdlRenderer, format, SDL_TEXTUREACCESS_STREAMING, w, h);
	if (tex == NULL) {
		Main_ErrorExit("Failed to create texture:", SDL_GetError(), -1);
	}
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
	fbConvertAll = true;
	
	return tex;
}

/*-----------------------------------------------------------------------*/
/**
 * (Re-)initialise screen or handle mode change 
 */
void Screen_Reset(void) {
	int i;

	SDL_PixelFormat format = SDL_PIXELFORMAT_BGRA32;

	/* Set initial window resolution */
	if (ConfigureParams.Screen.nMode == SCREEN_GROUP) {
		int xmax = 0;
		int ymax = 0;

		int xpos, ypos;

		for (i = 0; i < NUM_MONITORS; i++) {
			if (ConfigureParams.Screen.nGroupModePos[i] >= 0) {
				assert(ConfigureParams.Screen.nGroupModePos[i] < (NUM_MONITORS * NUM_MONITORS));

				xpos = ConfigureParams.Screen.nGroupModePos[i] % NUM_MONITORS;
				ypos = ConfigureParams.Screen.nGroupModePos[i] / NUM_MONITORS;

				xmax = xpos > xmax ? xpos : xmax;
				ymax = ypos > ymax ? ypos : ymax;

				groupRect[i].w = NeXT_SCRN_W;
				groupRect[i].h = NeXT_SCRN_H;
				groupRect[i].x = xpos * NeXT_SCRN_W;
				groupRect[i].y = ypos * NeXT_SCRN_H;
			}
		}
		screen_w = (xmax + 1) * NeXT_SCRN_W;
		screen_h = (ymax + 1) * NeXT_SCRN_H;
	} else {
		fbRect.x = 0;
		fbRect.y = 0;
		fbRect.w = NeXT_SCRN_W;
		fbRect.h = NeXT_SCRN_H;

		screen_w = NeXT_SCRN_W;
		screen_h = NeXT_SCRN_H;
	}

	width  = screen_w;
	height = screen_h;

	/* Set new window size only if necessary (the menu and status bars are
	 * added by Screen_GetWindowBounds) */
	if (!initDone || width != initScreenWidth || height != initScreenHeight ||
	    Gui_BarsHeight() != initBarsHeight) {
		SDL_Rect windowBounds;

		fprintf(stderr, "SDL screen request: %d x %d (%s)\n", width, height, bInFullScreen ? "fullscreen" : "windowed");

		Screen_GetWindowBounds(&windowBounds);

		if (bInFullScreen) {
			/* If we are in full screen change saved window sizes */
			saveWindowBounds = windowBounds;
		} else {
			SDL_SetWindowSize(sdlWindow, windowBounds.w, windowBounds.h);
			SDL_SetWindowPosition(sdlWindow, windowBounds.x, windowBounds.y);
		}
	}

	/* Handle mode change */
	if (ConfigureParams.Screen.nMode != initScreenMode) {
		Screen_ModeChanged();
	}

	/* Create framebuffer textures and start with blank screen */
	for (i = 0; i < NUM_MONITORS; i++) {
		if (ConfigureParams.Screen.nGroupModePos[i] < 0 || ConfigureParams.Screen.nMode != SCREEN_GROUP) {
			if (groupTexture[i]) {
				SDL_DestroyTexture(groupTexture[i]);
				groupTexture[i] = NULL;
			}
		} else if (groupTexture[i] == NULL) {
			groupTexture[i] = Screen_CreateFramebufferTexture(format, NeXT_SCRN_W, NeXT_SCRN_H);
			Screen_Blank(groupTexture[i]);
		}
	}
	if (ConfigureParams.Screen.nMode == SCREEN_GROUP) {
		if (fbTexture) {
			SDL_DestroyTexture(fbTexture);
			fbTexture = NULL;
		}
	} else if (fbTexture == NULL) {
		fbTexture = Screen_CreateFramebufferTexture(format, NeXT_SCRN_W, NeXT_SCRN_H);
		Screen_Blank(fbTexture);
	}

	/* Save mode and sizes */
	initScreenMode   = ConfigureParams.Screen.nMode;
	initScreenWidth  = width;
	initScreenHeight = height;
	initBarsHeight   = Gui_BarsHeight();
	initDone         = true;

	/* Make sure screen is painted in case emulation is paused */
	Screen_ForceRepaint();
}

/*-----------------------------------------------------------------------*/
/**
 * Init Screen, create window, renderer and textures
 */
void Screen_Init(void) {
	int i;
	const SDL_PixelFormatDetails* fmt;

	SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (SDL_CreateWindowAndRenderer(PROG_NAME, width, height, flags, &sdlWindow, &sdlRenderer) == false) {
		Main_ErrorExit("Failed to create window and renderer:", SDL_GetError(), -1);
	}

	/* Setup lookup tables */
	fmt = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_BGRA32);
	for (i = 0; i < 0x100; i++) {
		BW2RGB[i][0] = bw2rgb(fmt, i>>6);
		BW2RGB[i][1] = bw2rgb(fmt, i>>4);
		BW2RGB[i][2] = bw2rgb(fmt, i>>2);
		BW2RGB[i][3] = bw2rgb(fmt, i>>0);
	}
	for (i = 0; i < 0x10000; i++) {
		COL2RGB[SDL_Swap16BE(i)] = col2rgb(fmt, i);
	}

	/* The menus, dialogs and status bar */
	Gui_Init(sdlWindow, sdlRenderer);

	/* Initialise textures and window size */
	Screen_Reset();

	/* Set title, cursor visibility and mouse grab */
	Screen_SetTitle(NULL);
	Screen_ShowCursor(false);
	Screen_SetMouseGrab(bGrabMouse);

	/* Set titlebar visibility and change to fullscreen if requested */
	if (!ConfigureParams.Screen.bShowTitlebar) {
		Screen_TitlebarChanged();
	}
	if (ConfigureParams.Screen.bFullScreen) {
		Screen_EnterFullScreen();
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Free screen bitmap and allocated resources
 */
void Screen_UnInit(void) {
	int i;
	Gui_UnInit();
	if (fbTexture) {
		SDL_DestroyTexture(fbTexture);
	}
	for (i = 0; i < NUM_MONITORS; i++) {
		if (groupTexture[i]) {
			SDL_DestroyTexture(groupTexture[i]);
		}
	}
	SDL_DestroyRenderer(sdlRenderer);
	SDL_DestroyWindow(sdlWindow);
}

/*-----------------------------------------------------------------------*/
/**
 * Enter Full screen mode
 */
void Screen_EnterFullScreen(void) {
	bool bWasRunning;

	if (!bInFullScreen) {
		/* Hold things... */
		bWasRunning = Main_PauseEmulation(false);
		bInFullScreen = true;

		SDL_GetWindowPosition(sdlWindow, &saveWindowBounds.x, &saveWindowBounds.y);
		SDL_GetWindowSize(sdlWindow, &saveWindowBounds.w, &saveWindowBounds.h);
		SDL_SetWindowFullscreen(sdlWindow, true);
		SDL_SyncWindow(sdlWindow); /* To give monitor time to change to new resolution */

		/* If using multiple screen windows, save and go to single window mode */
		Screen_ModeChanged();

		if (bWasRunning) {
			/* And off we go... */
			Main_UnPauseEmulation();
		}

		/* Always grab mouse pointer in full screen mode */
		Screen_SetMouseGrab(true);

		/* Make sure screen is painted in case emulation is paused */
		Screen_ForceRepaint();
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Return from Full screen mode back to a window
 */
void Screen_ReturnFromFullScreen(void) {
	bool bWasRunning;

	if (bInFullScreen) {
		/* Hold things... */
		bWasRunning = Main_PauseEmulation(false);
		bInFullScreen = false;

		SDL_SetWindowFullscreen(sdlWindow, false);
		SDL_SyncWindow(sdlWindow); /* To give monitor time to switch resolution */
		SDL_SetWindowSize(sdlWindow, saveWindowBounds.w, saveWindowBounds.h);
		SDL_SetWindowPosition(sdlWindow, saveWindowBounds.x, saveWindowBounds.y);

		/* Return to windowed monitor mode */
		if (saveScreenMode == SCREEN_ALL) {
			ConfigureParams.Screen.nMode = saveScreenMode;
			Screen_ModeChanged();
		}

		if (bWasRunning) {
			/* And off we go... */
			Main_UnPauseEmulation();
		}

		/* Go back to windowed mode mouse grab settings */
		Screen_SetMouseGrab(bGrabMouse);

		/* Make sure screen is painted in case emulation is paused */
		Screen_ForceRepaint();
	}
}

/* ----------------------------------------------------------------------- */
/**
 * Set mouse grab.
 */
void Screen_SetMouseGrab(bool grab) {
	/* If emulation is active, set the mouse cursor mode now: */
	if (grab) {
		if (bEmulationActive) {
			Screen_CenterCursor(); /* Cursor must be inside window */
			SDL_SetWindowRelativeMouseMode(sdlWindow, true);
			SDL_SetWindowKeyboardGrab(sdlWindow, true);
			SDL_SetWindowMouseGrab(sdlWindow, true);
			{
				char message[64];
#ifdef __APPLE__
				const char *alt = "Option";
#else
				const char *alt = "Alt";
#endif
				snprintf(message, sizeof(message), "Mouse is locked. Ctrl+%s+%s releases it.", alt,
						 Keymap_GetKeyName(ConfigureParams.Shortcut.withModifier[SHORTCUT_MOUSEGRAB]));
				Screen_SetTitle(message);
			}
		}
	} else {
		SDL_SetWindowRelativeMouseMode(sdlWindow, false);
		SDL_SetWindowKeyboardGrab(sdlWindow, false);
		SDL_SetWindowMouseGrab(sdlWindow, false);
		Screen_SetTitle(NULL);
		Pointer_Released();   /* the host's pointer to where the guest's is */
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Show main window
 */
void Screen_ShowMainWindow(void) {
	if (!bInFullScreen) {
		SDL_RestoreWindow(sdlWindow);
		SDL_RaiseWindow(sdlWindow);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Force things associated with changing screen size
 */
void Screen_SizeChanged(void) {
	int h;

	if (!bInFullScreen) {
		SDL_GetWindowSize(sdlWindow, NULL, &h);
		nd_sdl_resize(((float)h - Gui_BarsHeight())/height);
	}
	Gui_RequestRepaint();
}

/*-----------------------------------------------------------------------*/
/**
 * Set visibilty of title bar.
 */
void Screen_TitlebarChanged(void) {
	if (sdlWindow && !bInFullScreen) {
		SDL_SetWindowBordered(sdlWindow, ConfigureParams.Screen.bShowTitlebar);
		nd_sdl_titlebar(ConfigureParams.Screen.bShowTitlebar);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Wrapper for Statusbar_AddMessage() and Statusbar_Update() in one go.
 */
void Screen_StatusbarMessage(const char *msg, uint32_t msecs)
{
	Statusbar_AddMessage(msg, msecs);
}

/*-----------------------------------------------------------------------*/
/**
 * Wrapper for Statusbar_Update().
 */
void Screen_StatusbarUpdate(void) {
	Gui_RequestRepaint();
}

/* ----------------------------------------------------------------------- */
/**
 * Set mouse cursor visibility and return if it was visible before.
 */
bool Screen_ShowCursor(bool show) {
	bool bOldVisibility;
	
	bOldVisibility = SDL_CursorVisible();
	if (bOldVisibility != show) {
		if (show) {
			SDL_ShowCursor();
		} else {
			SDL_HideCursor();
		}
	}
	return bOldVisibility;
}

/* ----------------------------------------------------------------------- */
/**
 * Set mouse cursor to the center of the screen.
 */
void Screen_CenterCursor(void) {
	float x = width / 2.0f, y = height / 2.0f;
	Gui_ScreenToWindow(&x, &y);
	SDL_WarpMouseInWindow(sdlWindow, x, y);
	GuiEvent_WarpMouse();
}

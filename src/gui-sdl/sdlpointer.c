/*
  PreviousTurbo - sdlpointer.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The guest's pointer, as its system keeps it. The NeXT draws its pointer
  in software, but the event driver keeps where it is in memory it shares
  with the window server: two 16-bit numbers, x and y, followed by the
  screen's bounds (0, 0, 1120, 832). The memory is searched for that, and of
  what is found, the place whose x and y follow the mouse's motion is taken.

  Knowing where the pointer is, the window can capture the mouse when the
  host's pointer moves onto the screen, and steer the guest's pointer to
  where it came on (or was clicked); and release it when it is pushed
  against an edge of the screen, the host's pointer carrying on from the
  same place.
*/

#include "main.h"
#include "configuration.h"
#include "screen.h"
#include "sdlscreen.h"
#include "sdlpointer.h"
#include "event.h"
#include "m68000.h"
#include "gui.h"

#include <SDL3/SDL.h>


#define MAX_CANDIDATES  8
#define PUSH_TO_LEAVE   48    /* counts pushed against an edge to release */
#define SYNC_FRAMES     180   /* steering gives up after this many frames */
#define PROBE_WAIT      10    /* frames to see a probing step move it */
#define PROBE_STEP      3
#define SCAN_EVERY_NS   3000000000ull

static volatile bool resetSeen;      /* the machine was reset (any thread) */
static uint32_t at;                  /* where the pointer is in RAM, 0: not known */
static uint32_t cand[MAX_CANDIDATES];
static int16_t candX[MAX_CANDIDATES], candY[MAX_CANDIDATES];
static int nCand;
static uint64_t lastScan;
static bool armed = true;            /* moving onto the screen may capture */

/* Steering to (syncX, syncY) after a capture, probing first if needed */
static bool syncing, probing;
static int syncFrames, probeFrames, probeDir;
static float syncX, syncY;
static int sentX, sentY, lastX, lastY;
static float gainX;               /* how far a count of a large step goes */
static bool inFlight;                /* a step sent, its effect not yet seen */
static int waitFrames, stillFrames, fromX, fromY;
static int push[4];                  /* left, right, top, bottom */

static int16_t ram16(uint32_t off) {
	return (int16_t)(NEXTRam[off] << 8 | NEXTRam[off + 1]);
}

static bool inScreen(int x, int y) {
	return x >= 0 && x < NeXT_SCRN_W && y >= 0 && y < NeXT_SCRN_H;
}

void Pointer_Reset(void) {
	resetSeen = true;
}

static void forget(void) {
	at = 0;
	nCand = 0;
	syncing = probing = false;
}

/* Where the pointer is, if known */
static bool where(int* x, int* y) {
	if (resetSeen) {
		resetSeen = false;
		forget();
	}
	if (!at || !NEXTRam || !bEmulationActive) {
		return false;
	}
	*x = ram16(at);
	*y = ram16(at + 2);
	if (!inScreen(*x, *y)) {
		forget();   /* the system that kept it there has gone */
		return false;
	}
	return true;
}

bool Pointer_Known(void) {
	int x, y;
	return where(&x, &y);
}

/* The places that look like it: x and y on the screen, then its bounds */
static void scan(void) {
	uint8_t sig[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	uint32_t off, size = NEXTRamSize;

	nCand = 0;
	lastScan = SDL_GetTicksNS();
	if (!NEXTRam || size < 16) {
		return;
	}
	sig[6] = NeXT_SCRN_W >> 8; sig[7] = NeXT_SCRN_W & 0xff;
	sig[10] = NeXT_SCRN_H >> 8; sig[11] = NeXT_SCRN_H & 0xff;
	for (off = 0; off + 16 <= size && nCand < MAX_CANDIDATES; off += 2) {
		if (NEXTRam[off + 10] != sig[6] || NEXTRam[off + 11] != sig[7] ||
		    memcmp(NEXTRam + off + 4, sig, sizeof(sig)) != 0) {
			continue;
		}
		if (inScreen(ram16(off), ram16(off + 2))) {
			cand[nCand] = off;
			candX[nCand] = ram16(off);
			candY[nCand] = ram16(off + 2);
			nCand++;
		}
	}
}

/* The candidate that moved the way the probe went becomes the pointer */
static bool confirm(int dir) {
	int i;
	for (i = 0; i < nCand; i++) {
		int x = ram16(cand[i]), y = ram16(cand[i] + 2);
		int mx = x - candX[i], my = y - candY[i];
		if (inScreen(x, y) && (mx || my) && mx * dir >= 0 && my * dir >= 0) {
			at = cand[i];
			nCand = 0;
			return true;
		}
	}
	return false;
}

static void startSync(void) {
	int x, y;
	syncing = where(&x, &y);
	syncFrames = 0;
	sentX = sentY = 0;
	gainX = 10.0f;
	inFlight = false;
	lastX = x;
	lastY = y;
}

/* The mouse was captured with the host's pointer at (x, y) in the window:
 * the guest's pointer is steered there */
void Pointer_CaptureAt(float x, float y) {
	Gui_WindowToScreen(&x, &y);
	syncX = x < 0 ? 0 : x > NeXT_SCRN_W - 1 ? NeXT_SCRN_W - 1 : x;
	syncY = y < 0 ? 0 : y > NeXT_SCRN_H - 1 ? NeXT_SCRN_H - 1 : y;
	memset(push, 0, sizeof(push));
	probing = false;
	if (Pointer_Known()) {
		startSync();
		return;
	}
	syncing = false;
	if (!bEmulationActive) {
		return;
	}
	if (!nCand || SDL_GetTicksNS() - lastScan > SCAN_EVERY_NS) {
		scan();
	}
	if (nCand) {
		probing = true;
		probeFrames = 0;
		probeDir = 1;
	}
}

static void capture(float x, float y) {
	bGrabMouse = true;
	Screen_SetMouseGrab(true);
	Pointer_CaptureAt(x, y);
}

/* The host's pointer moved to (x, y) in the window while the mouse was
 * free: onto the screen, that captures it, where the setting allows and
 * the system shows its pointer. Not while a button is held (a drag from
 * outside), the window is in the background or the machine is paused. */
bool Pointer_Enter(float x, float y) {
	float gx = x, gy = y;
	Gui_WindowToScreen(&gx, &gy);
	if (!inScreen((int)gx, (int)gy) || gx < 0 || gy < 0) {
		armed = true;
		return false;
	}
	if (!armed || !ConfigureParams.Mouse.bCaptureOnEntry || !bEmulationActive || bGrabMouse ||
	    Gui_ModalActive() || SDL_GetMouseState(NULL, NULL) ||
	    !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_INPUT_FOCUS)) {
		return false;
	}
	if (!Pointer_Known()) {
		if (SDL_GetTicksNS() - lastScan < SCAN_EVERY_NS) {
			return false;
		}
		scan();
		if (!nCand) {
			return false;
		}
	}
	capture(x, y);
	return true;
}

void Pointer_Leave(void) {
	armed = true;
}

/* The mouse was let go: the host's pointer goes to where the guest's is,
 * and moving onto the screen captures it again only after it has been off */
void Pointer_Released(void) {
	int x, y;
	syncing = probing = false;
	armed = false;
	if (where(&x, &y)) {
		float wx = x + 0.5f, wy = y + 0.5f;
		Gui_ScreenToWindow(&wx, &wy);
		SDL_WarpMouseInWindow(sdlWindow, wx, wy);
		GuiEvent_WarpMouse();
	}
}

/* Leaving at an edge: the host's pointer is put on the screen's edge there */
static void leave(int edge, int x, int y) {
	float wx = x + 0.5f, wy = y + 0.5f, ex = 0, ey = 0;
	float fx = NeXT_SCRN_W, fy = NeXT_SCRN_H;
	Gui_ScreenToWindow(&ex, &ey);
	Gui_ScreenToWindow(&fx, &fy);
	Gui_ScreenToWindow(&wx, &wy);
	if (edge == 0) wx = ex;
	if (edge == 1) wx = fx - 1;
	if (edge == 2) wy = ey;
	if (edge == 3) wy = fy - 1;
	bGrabMouse = false;
	Screen_SetMouseGrab(false);
	syncing = probing = false;
	armed = false;
	SDL_WarpMouseInWindow(sdlWindow, wx, wy);
	GuiEvent_WarpMouse();
}

static void addPush(int edge, int amount, bool stuck) {
	push[edge] = stuck ? push[edge] + amount : 0;
}

/* The user moved the mouse while it is captured: (wx, wy) in the window's
 * units, (dx, dy) the counts that go to the machine. True if the motion is
 * not to go there (it moves where the pointer is steered to instead). */
bool Pointer_Motion(float wx, float wy, int dx, int dy) {
	float ox = 0, oy = 0, sx = wx, sy = wy;
	int x, y, e;

	if (probing || syncing) {
		Gui_WindowToScreen(&ox, &oy);
		Gui_WindowToScreen(&sx, &sy);
		syncX += sx - ox;
		syncY += sy - oy;
		syncX = syncX < 0 ? 0 : syncX > NeXT_SCRN_W - 1 ? NeXT_SCRN_W - 1 : syncX;
		syncY = syncY < 0 ? 0 : syncY > NeXT_SCRN_H - 1 ? NeXT_SCRN_H - 1 : syncY;
		return true;
	}
	if (!where(&x, &y)) {
		return false;
	}
	/* pushing against an edge, with the pointer there and not moving */
	if (dx < 0) addPush(0, -dx, x <= 0 && x <= lastX);
	if (dx > 0) addPush(1, dx, x >= NeXT_SCRN_W - 1 && x >= lastX);
	if (dy < 0) addPush(2, -dy, y <= 0 && y <= lastY);
	if (dy > 0) addPush(3, dy, y >= NeXT_SCRN_H - 1 && y >= lastY);
	lastX = x;
	lastY = y;
	for (e = 0; e < 4; e++) {
		if (push[e] >= PUSH_TO_LEAVE) {
			memset(push, 0, sizeof(push));
			leave(e, x, y);
			return true;
		}
	}
	return false;
}

/* The guest accelerates its pointer by the size of each report, both axes
 * together: OPENSTEP moves the smallest ones as they are, and larger ones
 * up to about ten times as far. So one step is sent at a time, and what it
 * did is seen (the pointer stays where it went) before the next: large
 * steps, whose gain is learnt from what they did, while the pointer is far
 * away, then steps of a few counts, which are not scaled. */
#define SMALL       3
#define SMALL_SUM   4     /* reports this small (x and y) are not scaled */
#define LARGE_MAX   24
#define STEP_WAIT   8     /* frames to see a step's effect */
#define STEP_STILL  1     /* frames the pointer stays put once it moved */

static void learn(int sent, int moved, float* gain) {
	if (abs(sent) > SMALL && moved * sent > 0) {
		float g = (float)moved / (float)sent;
		*gain = g < 1.0f ? 1.0f : g > 20.0f ? 20.0f : g;
	}
}

static void step(int rx, int ry, int* sx, int* sy) {
	int big = abs(rx) > abs(ry) ? abs(rx) : abs(ry);
	float k = 0.95f / gainX;

	if (big * k > SMALL) {
		if (big * k > LARGE_MAX) {
			k = (float)LARGE_MAX / (float)big;
		}
		*sx = (int)lroundf((float)rx * k);
		*sy = (int)lroundf((float)ry * k);
		return;
	}
	/* SMALL_SUM counts across both axes at most, shared out as the rest is */
	{
		int sum = abs(rx) + abs(ry);
		if (sum <= SMALL_SUM) {
			*sx = rx;
			*sy = ry;
		} else {
			*sx = (int)lroundf((float)(rx * SMALL_SUM) / (float)sum);
			*sy = (ry > 0 ? 1 : -1) * (SMALL_SUM - abs(*sx));
			if (!ry) *sy = 0;
		}
	}
}

/* Once a frame (from the main loop) */
void Pointer_Frame(void) {
	int x, y;

	if (!bGrabMouse) {
		syncing = probing = false;
		return;
	}
	if (probing) {
		/* one step, and the other way if it was in a corner */
		if (confirm(probeDir)) {
			probing = false;
			startSync();
		} else if (probeFrames % PROBE_WAIT == 0) {
			if (probeFrames >= 2 * PROBE_WAIT) {
				probing = false;
				return;
			}
			probeDir = probeFrames ? -1 : 1;
			GuiEvent_SendMotion(probeDir * PROBE_STEP, probeDir * PROBE_STEP);
		}
		probeFrames++;
		return;
	}
	if (!syncing) {
		return;
	}
	if (!where(&x, &y) || ++syncFrames > SYNC_FRAMES) {
		syncing = false;
		return;
	}
	if (inFlight) {
		/* until it has moved and stayed there, or long enough */
		if (x != lastX || y != lastY) {
			stillFrames = 0;
		} else if (x != fromX || y != fromY) {
			stillFrames++;
		}
		lastX = x;
		lastY = y;
		if (stillFrames < STEP_STILL && ++waitFrames <= STEP_WAIT) {
			return;
		}
		/* the gain of a large step (small ones are not scaled), from its
		 * larger axis */
		if (abs(sentX) + abs(sentY) > SMALL_SUM) {
			if (abs(sentX) >= abs(sentY)) {
				learn(sentX, x - fromX, &gainX);
			} else {
				learn(sentY, y - fromY, &gainX);
			}
		}
		inFlight = false;
	}
	lastX = x;
	lastY = y;
	step((int)lroundf(syncX) - x, (int)lroundf(syncY) - y, &sentX, &sentY);
	if (!sentX && !sentY) {
		syncing = false;
		return;
	}
	GuiEvent_SendMotion(sentX, sentY);
	inFlight = true;
	waitFrames = stillFrames = 0;
	fromX = x;
	fromY = y;
}

/* How soon Pointer_Frame wants to run again */
int Pointer_WaitMs(void) {
	return (syncing || probing) ? 16 : 1000;
}

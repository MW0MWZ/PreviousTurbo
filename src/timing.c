/*
  Previous - timing.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This file contains functions for managing host and guest times.
*/
const char Timing_fileid[] = "Previous timing.c";

#include "config.h"

#ifdef __MINGW32__
#include <unistd.h>
#define timegm _mkgmtime
#endif
#include <float.h>

#include "main.h"
#include "host.h"
#include "timing.h"
#include "configuration.h"
#include "log.h"
#include "m68000.h"
#include "cycInt.h"


#define NUM_BLANKS 3
static atomic_int  vblCounter[NUM_BLANKS];
static const char* BLANKS[NUM_BLANKS] = {"main", "nd_main", "nd_video"};

static uint64_t     cycleCounterStart;
static uint64_t     cycleDivisor;
static double       turboDivisor;   /* Turbo: cycles per microsecond the host runs */
static double       baseDivisor;    /* what cycleCounterStart was set for */
static uint64_t     startupGuard;   /* Turbo: real time when the guard ends, or 0 */
static uint64_t     perfCounterStart;
static uint64_t     perfFrequency;
static bool         perfCounterFreqInt;
static uint64_t     perfDivisor;
static double       perfMultiplicator;
static uint64_t     pauseTimeStamp;
static bool         enableRealtime;
static bool         osDarkmatter;
static bool         currentIsRealtime;
static uint64_t     hardClockExpected;
static uint64_t     hardClockActual;
static time_t       unixTimeStart;
static time_t       hostTimeStart;
static lock_t       timeLock;
static uint64_t     saveTime;
static unsigned     startUps;       /* Turbo: times the guest has started (the guard has been and gone) */


static inline uint64_t Timing_GetRealTime(void) {
	uint64_t rt = (host_get_counter() - perfCounterStart);
	if (perfCounterFreqInt) {
		rt /= perfDivisor;
	} else {
		rt *= perfMultiplicator;
	}
	return rt;
}

/* Microseconds spent waiting for real time, for the speed shown */
volatile uint64_t Timing_WaitedUs;

/* How late the host's sleep wakes up, in microseconds, as last seen: a short
 * wait sleeps for all but that much and spins the rest */
static int64_t sleepLate = 1000;

static void Timing_Wait(int64_t us, uint64_t until) {
	uint64_t start = Timing_GetRealTime();
	if (us > 10000LL)
		host_sleep_us(us);
	else {
		if (us > 2 * sleepLate + 100) {
			int64_t ask = us - sleepLate - 50, late;
			host_sleep_us(ask);
			late = (int64_t)(Timing_GetRealTime() - start) - ask;
			sleepLate += ((late > 0 ? late : 0) - sleepLate) / 8;
		}
		while (Timing_GetRealTime() < until) {}
	}
	Timing_WaitedUs += Timing_GetRealTime() - start;
}

/* Turbo: the CPU is stopped until the next event, us from now; returns the
 * microseconds slept */
int64_t Timing_Idle(int64_t us) {
	uint64_t start, slept;
	if (us < 200) {
		return 0;
	}
	start = Timing_GetRealTime();
	host_sleep_us(us > 2000 ? 2000 : us);
	slept = Timing_GetRealTime() - start;
	Timing_WaitedUs += slept;
	return (int64_t)slept;
}

/* Cycles per microsecond of the time counted in cycles. In Turbo it is as
 * many as the host runs, so that the guest's clocks keep to real time
 * however fast the CPU goes (Timing_Sync follows it). */
double Timing_CyclesPerUs(void) {
	return ConfigureParams.System.bFullSpeed ? turboDivisor : (double)cycleDivisor;
}

/* Turbo (from Timing_Sync, at 200 Hz): the time counted in cycles runs at
 * half the speed the host runs the CPU at, but no slower than the real CPU
 * and no faster than 12 times it, so it never falls behind real time: it is
 * a little ahead, and waited for. It must not fall behind, as it would then
 * jump forward when real time is used (in user mode), and the guest's kernel
 * reads the 20-bit event counter on the assumption that it does not jump.
 * OPENSTEP 4.2 also stops starting (in its ADB driver) at about 20 times the
 * real CPU. The speed changes slowly, as the guest may time the CPU. */
static void Timing_FollowHost(uint64_t realTime, int64_t offset) {
	static uint64_t lastReal, lastCycles, lastIdle, lastWaited;
	static double rate;
	uint64_t real = realTime - lastReal;
	uint64_t waited = Timing_WaitedUs - lastWaited;
	uint64_t cycles = nCyclesMainCounter - lastCycles;
	uint64_t idle = CycInt_IdleCycles - lastIdle;

	if (lastReal && real > waited && cycles > idle && real - waited > 100) {
		double now = (double)(cycles - idle) / (double)(real - waited);
		rate = rate > 0 ? rate * 0.9 + now * 0.1 : now;
	}
	lastReal = realTime;
	lastWaited = Timing_WaitedUs;
	lastCycles = nCyclesMainCounter;
	lastIdle = CycInt_IdleCycles;

	if (offset < -2000) {
		turboDivisor *= 0.5;           /* behind: at once */
	} else if (rate > 0) {
		double want = rate * 0.5;
		if (want > 12.0 * cycleDivisor) {
			want = 12.0 * cycleDivisor;
		}
		/* towards it by a tenth at a time */
		turboDivisor += (want - turboDivisor) * 0.1;
	}
	if (turboDivisor < cycleDivisor || startupGuard) {
		turboDivisor = cycleDivisor;
	}
	if (startupGuard && realTime > startupGuard) {
		startupGuard = 0;
		startUps++;
	}
}

/* Turbo: while the guest logs in at start-up (from the WindowServer to
 * Workspace, but no longer than STARTUP_GUARD_US), its time is counted in
 * cycles at the real CPU's speed, for programs as for the kernel, as with
 * Turbo and Variable speed off. OPENSTEP 4.2 has a race there: lookupd exits
 * as loginwindow starts, and if the name server gives loginwindow lookupd's
 * port before it has seen that the port is gone, loginwindow waits a minute
 * for an answer. Timed as on the real machine, it does not lose it. */
#define STARTUP_GUARD_US  15000000
void Timing_StartupGuard(bool on) {
	if (!on && startupGuard) {
		startUps++;
	}
	startupGuard = on ? Timing_GetRealTime() + STARTUP_GUARD_US : 0;
	if (on) {
		turboDivisor = cycleDivisor;   /* at once */
	}
}

static uint64_t Timing_GetCycleTime(void) {
	if (!ConfigureParams.System.bFullSpeed && baseDivisor == (double)cycleDivisor) {
		return (nCyclesMainCounter - cycleCounterStart) / cycleDivisor;
	}
	/* the rate changed (Turbo): the time so far stays as it was */
	if (baseDivisor != Timing_CyclesPerUs()) {
		uint64_t now = (uint64_t)((nCyclesMainCounter - cycleCounterStart) / baseDivisor);
		baseDivisor = Timing_CyclesPerUs();
		cycleCounterStart = nCyclesMainCounter - (uint64_t)(now * baseDivisor);
		if (!ConfigureParams.System.bFullSpeed) {
			return (nCyclesMainCounter - cycleCounterStart) / cycleDivisor;
		}
	}
	return (uint64_t)((nCyclesMainCounter - cycleCounterStart) / baseDivisor);
}

void Timing_Pause(bool pause) {
	if (pause) {
		pauseTimeStamp = host_get_counter();
	} else {
		perfCounterStart += host_get_counter() - pauseTimeStamp;
	}
}

/* Return current time as microseconds */
uint64_t Timing_GetTime(void) {
	bool state;
	uint64_t hostTime;
	
	/* switch to realtime if...
	 * 1) ...realtime mode is enabled and...
	 * 2) ...either we are running darkmatter or the m68k CPU is in user mode
	 * (Turbo times as realtime mode does) */
	state = (osDarkmatter || !(regs.s)) && (enableRealtime || (ConfigureParams.System.bFullSpeed && !startupGuard));
	
	if (currentIsRealtime) {
		hostTime = Timing_GetRealTime();
	} else {
		hostTime = Timing_GetCycleTime();
	}
	
	if (currentIsRealtime != state) {
		if (currentIsRealtime) {
			/* switching from real-time to cycle-time */
			baseDivisor = Timing_CyclesPerUs();
			if (ConfigureParams.System.bFullSpeed) {
				cycleCounterStart = nCyclesMainCounter - (uint64_t)(hostTime * baseDivisor);
			} else {
				cycleCounterStart = nCyclesMainCounter - hostTime * cycleDivisor;
			}
		} else {
			/* switching from cycle-time to real-time */
			int64_t realTimeOffset = hostTime - Timing_GetRealTime();
			if (realTimeOffset > 0) {
				/* if hostTime is in the future, wait until realTime is there as well */
				Timing_Wait(realTimeOffset, hostTime);
			}
		}
		currentIsRealtime = state;
	}
	
	return hostTime;
}

/* Is the time read last the host's (not counted in cycles)? */
bool Timing_IsRealtime(void) {
	return currentIsRealtime;
}

void Timing_GetTimes(uint64_t* realTime, uint64_t* hostTime) {
	*hostTime = Timing_GetTime();
	*realTime = Timing_GetRealTime();
}

/* Real time in microseconds, which the time counted (Timing_GetTime) keeps to */
uint64_t Timing_GetRealTimeUs(void) {
	return Timing_GetRealTime();
}

/* Turbo: how many times the guest has started (its start-up guard been
 * and gone) */
unsigned Timing_StartUps(void) {
	return startUps;
}

void Timing_Sync(void) {
	int64_t realTimeOffset;
	uint64_t realTime, hostTime;
	Timing_GetTimes(&realTime, &hostTime);
	host_lock(&timeLock);
	saveTime = hostTime; /* save hostTime to be read by other threads */
	host_unlock(&timeLock);
	realTimeOffset = hostTime - realTime;
	if (ConfigureParams.System.bFullSpeed) {
		Timing_FollowHost(realTime, realTimeOffset);
	}
	if (realTimeOffset > 0) {
		uint64_t start = Timing_GetRealTime();
		host_sleep_us(realTimeOffset);
		Timing_WaitedUs += Timing_GetRealTime() - start;
	}
}

/* This can be used by other threads to read hostTime */
uint64_t Timing_GetSaveTime(void) {
	uint64_t hostTime;
	host_lock(&timeLock);
	hostTime = saveTime;
	host_unlock(&timeLock);
	return hostTime / 1000000ULL;
}

/* The real-time clock keeps time as on battery, by the host's clock: from
 * the host's time at power on, moved only when the guest sets it */
time_t Timing_GetUnixTime(void) {
	return unixTimeStart + (time(NULL) - hostTimeStart);
}

void Timing_SetUnixTime(time_t now) {
	unixTimeStart = now - (time(NULL) - hostTimeStart);
}

struct tm* Timing_GetUnixTimeStruct(void) {
	time_t tmp = Timing_GetUnixTime();
	return gmtime(&tmp);
}

void Timing_SetUnixTimeStruct(struct tm* now) {
	time_t tmp = timegm(now);
	Timing_SetUnixTime(tmp);
}

void Timing_Hardclock(int expected, int actual) {
	if (abs(actual - expected) > 1000) {
		Log_Printf(LOG_WARN, "[Hardclock] Expected: %d us, actual: %d us\n", expected, actual);
	} else {
		hardClockExpected += expected;
		hardClockActual   += actual;
	}
}

static char DARKMATTER[] = "darkmatter";

void Timing_BlankCount(int src, bool state) {
	if (state) {
		host_atomic_add(&vblCounter[src], 1);
	}
	
	/* Check first 4 bytes of version string in darkmatter/daydream kernel */
	osDarkmatter = get_long(0x04000246) == do_get_mem_long((uint8_t*)DARKMATTER);
}

static int Timing_ResetBlankCounter(int src) {
	return host_atomic_set(&vblCounter[src], 0);
}

const char* Timing_Report(uint64_t realTime, uint64_t hostTime) {
	static uint64_t lastVT;
	static char report[512];
	
	int    nBlank    = 0;
	char*  r         = report;
	double dVT       = hostTime - lastVT;
	double hardClock = hardClockExpected;
	
	dVT       /= 1000000.0;
	hardClock /= hardClockActual == 0 ? 1 : hardClockActual;
	
	r += sprintf(r, "[%s] hostTime:%"PRIu64" hardClock:%.3fMHz", enableRealtime ? "Variable" : "CycleTime", hostTime, hardClock);
	
	for(int i = NUM_BLANKS; --i >= 0;) {
		nBlank = Timing_ResetBlankCounter(i);
		r += sprintf(r, " %s:%.1fHz", BLANKS[i], (double)nBlank/dVT);
	}
	
	lastVT = hostTime;
	
	return report;
}

/* Report counter capacity */
#define DAY_TO_US (1000000ULL * 60 * 60 * 24)

static void Timing_ReportLimits(void) {
	uint64_t cycleCounterLimit, perfCounterLimit, perfCounter;
	
	Log_Printf(LOG_WARN, "[Timing] Timing system reset:");
	
	cycleCounterLimit  = UINT64_MAX - nCyclesMainCounter;
	cycleCounterLimit /= cycleDivisor;
	cycleCounterLimit /= DAY_TO_US;
	
	Log_Printf(LOG_WARN, "[Timing] Cycle counter value: %"PRIu64, nCyclesMainCounter);
	Log_Printf(LOG_WARN, "[Timing] Cycle counter frequency: %"PRIu64" MHz", cycleDivisor);
	Log_Printf(LOG_WARN, "[Timing] Cycle counter will overflow in %"PRIu64" days", cycleCounterLimit);
	
	perfCounter        = host_get_counter();
	perfCounterLimit   = UINT64_MAX - perfCounter;
	Log_Printf(LOG_WARN, "[Timing] Realtime counter value: %"PRIu64, perfCounter);
	if (perfCounterFreqInt) {
		perfCounterLimit /= perfDivisor;
		if (perfCounterLimit > INT64_MAX)
			perfCounterLimit = INT64_MAX;
		perfCounterLimit /= DAY_TO_US;
		Log_Printf(LOG_WARN, "[Timing] Realtime counter frequency: %"PRIu64" MHz", perfDivisor);
		Log_Printf(LOG_WARN, "[Timing] Realtime counter will overflow in %"PRIu64" days", perfCounterLimit);
	} else {
		if (perfCounterLimit > (1ULL<<DBL_MANT_DIG)-1)
			perfCounterLimit = (1ULL<<DBL_MANT_DIG)-1;
		if (perfMultiplicator < 1.0)
			perfCounterLimit *= perfMultiplicator;
		else
			Log_Printf(LOG_WARN, "[Timing] Warning: Realtime counter cannot resolve microseconds.");
		perfCounterLimit /= DAY_TO_US;
		Log_Printf(LOG_WARN, "[Timing] Realtime counter frequency: %f MHz", 1.0/perfMultiplicator);
		Log_Printf(LOG_WARN, "[Timing] Realtime timer will start losing precision in %"PRIu64" days", perfCounterLimit);
	}
}

/* Check NeXT specific UNIX time limits and adjust time if needed */
static void Timing_CheckUnixTime(void) {
	static const char* f = "%a %b %d %H:%M:%S %Y";
	struct tm* t;
	char s[32];
	
	t = gmtime(&unixTimeStart);
	if (t) {
		if (strftime(s, sizeof(s), f, t) > 0) {
			Log_Printf(LOG_WARN, "[Timing] Unix time: %s GMT", s);
		}
		if (t->tm_year < NEXT_MIN_YEAR || t->tm_year >= NEXT_LIMIT_YEAR) {
			t->tm_year = NEXT_START_YEAR;
			unixTimeStart = timegm(t);
			Log_Printf(LOG_WARN, "[Timing] Unix time is beyond valid range");
			Log_Printf(LOG_WARN, "[Timing] Unix time lower limit: Thu Jan  1 00:00:00 1970 GMT");
			Log_Printf(LOG_WARN, "[Timing] Unix time upper limit: Thu Dec 31 23:59:59 2037 GMT");
			if (strftime(s, sizeof(s), f, t) > 0) {
				Log_Printf(LOG_WARN, "[Timing] Setting Unix time to %s GMT", s);
			}
		}
	} else if (unixTimeStart < NEXT_MIN_SEC || unixTimeStart >= NEXT_LIMIT_SEC) {
		unixTimeStart = NEXT_START_SEC;
		Log_Printf(LOG_WARN, "[Timing] Unix time value is beyond valid range (%u through %u)", NEXT_MIN_SEC, NEXT_LIMIT_SEC);
		Log_Printf(LOG_WARN, "[Timing] Setting Unix time to Thu Jul  2 12:00:00 1987 GMT");
	}
	Log_Printf(LOG_WARN, "[Timing] Unix time will overflow in %f days", difftime(NEXT_MAX_SEC, unixTimeStart)/(24*60*60));
}

void Timing_Reset(void) {
	int i;
	
	perfCounterStart  = host_get_counter();
	pauseTimeStamp    = perfCounterStart;
	perfFrequency     = host_get_counter_frequency();
	unixTimeStart     = time(NULL);
	hostTimeStart     = time(NULL);
	cycleCounterStart = 0;
	currentIsRealtime = false;
	startupGuard      = 0;
	startUps          = 0;
	hardClockExpected = 0;
	hardClockActual   = 0;
	enableRealtime    = ConfigureParams.System.bRealtime;
	osDarkmatter      = false;
	saveTime          = 0;
	
	for (i = NUM_BLANKS; --i >= 0;) {
		Timing_ResetBlankCounter(i);
	}
	
	cycleDivisor = ConfigureParams.System.nCpuFreq;
	turboDivisor = cycleDivisor;
	baseDivisor  = cycleDivisor;
	
	perfCounterFreqInt = (perfFrequency % 1000000ULL) == 0;
	perfDivisor        = perfFrequency / 1000000ULL;
	perfMultiplicator  = 1000000.0 / perfFrequency;
	
	Timing_ReportLimits();
	Timing_CheckUnixTime();
}

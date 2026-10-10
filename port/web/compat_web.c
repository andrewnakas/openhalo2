/* The few host calls the toolkit's POSIX shim does not cover in a browser. */
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <sys/sysinfo.h>

long long InterlockedExchangeAdd64(volatile long long *addend, long long value)
{
    return __atomic_fetch_add(addend, value, __ATOMIC_SEQ_CST);
}

unsigned int timeBeginPeriod(unsigned int ms) { (void)ms; return 0; }

void YieldProcessor(void) { sched_yield(); }

/* Host memory as the kernel layer reports it; only the totals are used. */
int sysinfo(struct sysinfo *si)
{
    memset(si, 0, sizeof *si);
    si->totalram = 2047u * 1024u * 1024u;
    si->freeram = 1024u * 1024u * 1024u;
    si->mem_unit = 1;
    return 0;
}

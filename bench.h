#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static inline uint64_t read_time(void)
{
    if(sizeof(long)==8)
    {   
        uint64_t a, d;
        asm volatile( "rdtsc\n\t" : "=a" (a), "=d" (d) );
        return (d << 32) | (a & 0xffffffff);
    } else {
        uint64_t l;
        asm volatile( "rdtsc\n\t" : "=A" (l) );
        return l;
    }   
}
 
#define NOP_CYCLES 65 // time measured by an empty timer on Core2
 
#define START_TIMER \
uint64_t tend;\
uint64_t tstart= read_time();

#define STOP_TIMER(id) {\
tend= read_time();\
{\
    static uint64_t tsum=0;\
    static int tcount=0;\
    static int tskip_count=0;\
    if(tskip_count<2)\
        tskip_count++;\
    else{\
    if(tcount<2 || tend - tstart < 8*tsum/tcount){\
        tsum+= tend - tstart;\
        tcount++;\
    }else\
        tskip_count++;\
    if(((tcount+tskip_count) & (tcount+tskip_count-1)) == 0)\
        fprintf(logfile, "%llu dezicycles in %s, %d runs, %d skips\n", tsum*10/tcount-NOP_CYCLES*10, id, tcount, tskip_count);\
}}}

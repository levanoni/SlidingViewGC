\begin{rawcfig}{rcgc\_internal.h}
/*
 * File:    rcblkmgr.h
 * Author:  Mr. Yossi Levanoni
 * Purpose: Header for internal use of the collector/allocator.
 */
#ifndef __RCGC_INTERNAL__
#define __RCGC_INTERNAL__

GCFUNC  bool     gcCompareAndSwap( unsigned *addr, unsigned oldv, unsigned newv);
GCFUNC  void     gcSpinLockEnter(volatile unsigned *p, unsigned id);
GCFUNC  void     gcSpinLockExit(volatile unsigned *p, unsigned id);
GCFUNC  void     gcCheckGC(void);

GCFUNC  void              blkInit( unsigned nMB );
GCFUNC  BlkAllocHdr*      blkAllocBlock( ExecEnv *ee );
GCFUNC  void              blkFreeChunkedBlock( BlkAllocHdr *ph );
GCFUNC  void              blkFreeSomeChunkedBlocks( BlkAllocHdr **pph, int nBlocks );
GCFUNC  void              blkFreeRegion( BlkAllocBigHdr *ph );
GCFUNC  void              blkSweep(void);


GCFUNC    void     chkFlushRecycledListEntry( RLCENTRY *rlce );
GCFUNC    void     chkFlushRecycledListsCache( void );
GCFUNC    void     chkSweepChunkedBlock( BlkAllocHdr *ph, int status);
GCFUNC    void     chkInit(unsigned nMB);

#ifdef RCDEBUG
GCFUNC void chkPreCollect(BLKOBJ* o);
#endif /* RCDEBUG */

#ifdef RCNOINLINE

GCFUNC void H1BIT_Set(byte* entry, unsigned h);
GCFUNC void H1BIT_Clear(byte* entry, unsigned h);
GCFUNC void H1BIT_ClearByte(byte* entry, unsigned h);
GCFUNC void H1BIT_Put(byte* entry, unsigned h, unsigned val);
GCFUNC byte H1BIT_Get(byte* entry, unsigned h);
GCFUNC void H1BIT_Init(H1BIT_BMP* bmp, unsigned* rep_addr, unsigned rep_size );

GCFUNC void H2BIT_Put(byte* entry, unsigned h, unsigned val);
GCFUNC void H2BIT_Clear(byte* entry, unsigned h);
GCFUNC void H2BIT_Stuck(byte* entry, unsigned h);
GCFUNC byte H2BIT_Get(byte* entry, unsigned h);
GCFUNC void H2BIT_Inc(byte* entry, unsigned h);
GCFUNC byte H2BIT_IncRV(byte* entry, unsigned h);
GCFUNC byte H2BIT_Dec(byte* entry, unsigned h);
GCFUNC void H2BIT_Init(H2BIT_BMP* bmp, unsigned* rep_addr, unsigned rep_size );

#endif /*  RCNOINLINE */

GCFUNC   uint  gcGetHandleRC(GCHandle* h);

/***********************************************************
 * System utilities layer (MOK)
 * 
 */
#define mokSleep Sleep

/*
 * Memory 
 */
/* Advanced */
GCFUNC void* mokMemReserve(void *starting_at_hint, unsigned sz );
GCFUNC void  mokMemUnreserve( void *start, unsigned sz );
GCFUNC void* mokMemCommit( void *start, unsigned sz, bool zero_out );
GCFUNC void  mokMemDecommit( void *start, unsigned sz );

/* C style */
GCFUNC void* mokMalloc( unsigned sz, bool zero_out );
GCFUNC void  mokFree( void *);

/* zero out */
GCFUNC void  mokMemZero( void *start, unsigned sz );

#define mokAssert sysAssert
#define gcAssert  sysAssert

#ifdef RCDEBUG
#define Im_used   0x1badbad1
#define Im_free   0x12344321
#endif

int x86CompareAndSwap(unsigned *addr, unsigned oldv, unsigned newv);

#define ___compare_and_swap  x86CompareAndSwap
#define gcCompareAndSwap     x86CompareAndSwap


/*
 * p is a pointer to BlkAllocHdr.  Lock and unlock the page
 */
#pragma optimize( "", off )
static void bhLock(BlkAllocHdr *p)
{
  volatile word *ptr = (volatile word*)&p->StatusLockBinidx;
  for (;;) {
    volatile word oldv, newv;
    oldv =  *ptr;
    oldv = oldv & ~LOCKMASK;
    newv = oldv | LOCKMASK;
    if (gcCompareAndSwap( (word*)ptr, oldv, newv))
      goto ___do_bh_lock_end; 
  }
 ___do_bh_lock_end:;
}

static bhUnlock(BlkAllocHdr* p)
{
  for (;;) {
    volatile word *ptr = (volatile word*)&p->StatusLockBinidx;
    word oldv, newv;
    oldv =  *ptr;
    if (!(oldv & LOCKMASK )) {
      __asm { int 3 }
    }
    newv = oldv & ~LOCKMASK;
    if (gcCompareAndSwap( (word*)ptr, oldv, newv))
      goto ___do_bh_unlock_end;
  }
 ___do_bh_unlock_end:;
}
#pragma optimize( "", on )


#define gcNonNullValidHandle  _isHandle
#define gcValidHandle(h)      ((h)==NULL || _isHandle((h)))

#endif /* __RCGC_INTERNAL__ */
\end{verbatim}
\end{rawcfig}

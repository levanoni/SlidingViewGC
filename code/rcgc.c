/*
 * File:    rcgc.c
 * Author:  Mr. Yossi Levanoni
 * Purpose: implementation of the garbage collector
 */

/* forward declarations */
static void _snoopThreadLocals( sys_thread_t* t );
static void _incrementHandleRC( void * h);
static void _traceSetup(void);
static void _freeHandle(GCHandle* h);

/************** Debug Prints ********************/
static FILE *fDbg;


#ifdef RCDEBUG
static void dbgprn(int level, char *fmt, ...)
{
  char buff[1000];
  if (level <= 2) {
    va_list args;
    
    va_start( args, fmt );
    if (fDbg==NULL) 
      fDbg = fopen("test.txt", "wt" );
    vfprintf( fDbg, fmt, args );
    vsprintf( buff, fmt, args );
    jio_printf( "%s", buff );
    fflush( stdout );
    va_end( args );
  }
}
#endif


/**********************************************************/
/* atomic op */


// int ___compare_and_swap(unsigned *addr, unsigned oldv, unsigned newv);

#pragma optimize( "", off )

GCFUNC void gcSpinLockEnter(volatile unsigned *p, unsigned id)
{
  int i;

  for(i=0; i<N_SPINS; i++) {
    if (*p) continue;
    if (___compare_and_swap((unsigned*)p, 0, id))
      // jio_printf("gcSpinLockEnter ended (1)\n");
      return;
  }
  i = 1;
  for (;;) {
    mokSleep( i/1000 );
    if (___compare_and_swap((unsigned*)p, 0, id)) {
      return;
    }
    i *= 2;
  }
}

GCFUNC void gcSpinLockExit(volatile unsigned *p, unsigned id)
{
#ifdef RCDEBUG
  bool res;
#endif
  mokAssert( *p == id );
#ifdef RCDEBUG
  res =  ___compare_and_swap((unsigned*)p, id, 0);
  mokAssert( res );
#else
  ___compare_and_swap((unsigned*)p, id, 0);
#endif /* RCDEBUG */
}

#pragma optimize( "", on )

/****************  BUFFER MANAGEMENT ***********************/

static uint* buffList = NULL;
static uint  pad_against_false_sharing1[256];
static uint  buffListLock;
static uint  pad_against_false_sharing2[256];

void _buffListLockEnter(uint ee)
{
  gcSpinLockEnter( &buffListLock, (unsigned)ee );
}

void _buffListLockExit(uint ee)
{
  gcSpinLockExit( &buffListLock, (unsigned)ee );
}

static uint* _allocFreshBuff(void)
{
  uint *bf;
  
  bf = (uint*)mokMemReserve( NULL, BUFFSIZE );
  mokMemCommit( bf, BUFFSIZE, false );
  if (!bf) {
    jio_printf("YLRC: out of log buffers space\n");
    fflush( stdout );
    exit(-1);
  }
#ifdef RCDEBUG
  bf[USED_IDX] = Im_used;
#endif
  return bf;
}

static uint* _allocBuff(ExecEnv *ee)
{
  uint *bf;

  if (buffList==NULL) {
    bf = _allocFreshBuff();
    _buffListLockEnter( (unsigned)ee );
    gcvar.nAllocatedChunks++;
    gcvar.nUsedChunks++;
    mokAssert( gcvar.nFreeChunks+gcvar.nUsedChunks == gcvar.nAllocatedChunks );
    _buffListLockExit( (unsigned)ee );
    goto checkout;
  }
  _buffListLockEnter( (unsigned)ee );
  bf = buffList;
  if (!bf) {
    gcvar.nUsedChunks++;
    gcvar.nAllocatedChunks++;
    mokAssert( gcvar.nFreeChunks+gcvar.nUsedChunks == gcvar.nAllocatedChunks );
    _buffListLockExit( (unsigned)ee );
    bf = _allocFreshBuff();
  }
  else {
    gcvar.nUsedChunks++;
    gcvar.nFreeChunks--;
    mokAssert( gcvar.nFreeChunks+gcvar.nUsedChunks == gcvar.nAllocatedChunks );
#ifdef RCDEBUG
    mokAssert( bf[USED_IDX] == Im_free );
    bf[USED_IDX] = Im_used;
#endif // RCDEBUG
    buffList = (unsigned*)bf[LINKED_LIST_IDX];
    _buffListLockExit( (unsigned)ee );
  }
 checkout:
  if (ee != gcvar.ee) {
    gcvar.nChunksAllocatedRecentlyByUser++; // allow inaccuracy due to race condition
    if (gcvar.nChunksAllocatedRecentlyByUser >= gcvar.opt.userBuffTrig 
        && gcvar.initialized
        && !gcvar.gcActive) {
#ifdef RCVERBOSE
      jio_printf("ALLOC BUFF used=%d TRIGERRING ASYNC RC\n", gcvar.nUsedChunks );
      fflush( stdout );
#endif
      gcRequestAsyncGC( );
    }
  }
  return bf;
}

static void _freeBuff( ExecEnv *ee, uint* buff)
{
  mokAssert( ee == gcvar.ee );

#ifdef RCDEBUG
  mokAssert( buff[USED_IDX] == Im_used );
#endif 

  _buffListLockEnter( (unsigned)ee );
  buff[LINKED_LIST_IDX] = (uint)buffList;
  buffList = buff;
  gcvar.nFreeChunks++;
  gcvar.nUsedChunks--;
  mokAssert( gcvar.nFreeChunks+gcvar.nUsedChunks == gcvar.nAllocatedChunks );
#ifdef RCDEBUG
  buff[USED_IDX] = Im_free;
#endif // RCDEBUG
  _buffListLockExit( (unsigned)ee );
}

static void _initBuffReservedSlots( ExecEnv* ee, uint *newbuff )
{
  newbuff[LINKED_LIST_IDX]           = 0;
  newbuff[REINFORCE_LINKED_LIST_IDX] = 0;
  newbuff[NEXT_BUFF_IDX]             = 0;
  newbuff[LAST_POS_IDX]              = 0;
#ifdef RCDEBUG
  newbuff[ALLOCATING_EE]             = (uint)ee;
  newbuff[LOG_CHILDS_IDX]            = 0;
  newbuff[LOG_OBJECTS_IDX]           = 0;
  newbuff[USED_IDX]                  = Im_used;
#endif
}

GCEXPORT void gcBuffAllocAndLink(ExecEnv* ee, BUFFHDR *bh)
{
  uint i;
  uint *newBuff = _allocBuff( ee );

  _initBuffReservedSlots( ee, newBuff );

  /* backword link */
  newBuff[N_RESERVED_SLOTS] = ((uint)bh->pos) | BUFF_LINK_MARK;

  /* forward link */
  /* from the current position to the new chunk */
  *bh->pos = ((uint)&newBuff[N_RESERVED_SLOTS]) | BUFF_LINK_MARK;
  /* from the beginning of the current buffer to the next buffer */
  bh->currBuff[NEXT_BUFF_IDX] = (uint)newBuff;

    
  /* update record */
  bh->pos = &newBuff[N_RESERVED_SLOTS+1];
  bh->limit = newBuff + BUFFSIZE/sizeof(uint);
  bh->currBuff = newBuff;

  /*
   * Reserve place for"
   * 1. the handle and forward pointer (2 words).
   * 2. and a reserved place for a snooped object.
   */
  bh->limit -= 3; 
}

static void buffInit(ExecEnv *ee, BUFFHDR *bh)
{
  int i;
  bh->start = _allocBuff(ee);

  _initBuffReservedSlots( ee, bh->start );

  /* backword link */
  bh->start[N_RESERVED_SLOTS] = ((unsigned)NULL) | BUFF_LINK_MARK;
  bh->pos = &bh->start[N_RESERVED_SLOTS+1];
  bh->limit = bh->start + BUFFSIZE/sizeof(uint);
  bh->limit -= 3; /* for the handle, forward pointer and reserved snoop */
  bh->currBuff = bh->start;
}

#define buffIsModified(bh) ((bh)->pos  != &(bh)->start[N_RESERVED_SLOTS+1])


#pragma optimize( "", off )
GCEXPORT void gcBuffSlowConditionalLogHandle(ExecEnv* ee, GCHandle *h)
{
  int avail;
  GCHandle **objslots;
  GCHandle **p;
  ClassClass *cb;
  BUFFHDR *bh;

#ifdef RCDEBUG
  uint nLoggedChilds = 0;
#endif // RCDEBUG
    
  bh = &ee->gcblk.updateBuffer;
    
    
  if (obj_flags(h)==T_NORMAL_OBJECT) {
    cb = obj_classblock(h);
    mokAssert( cb != classJavaLangClass);
    { /* OK, it's a non-class object */
      unsigned short *offs = cbObjectOffsets(cb);
      int nrefs = unhand(cb)->n_object_offsets;
      objslots = (GCHandle**)(((char*)unhand(h))-1);
            
      mokAssert( objslots && h && bh && ee && offs && nrefs>0);
      p = (GCHandle**)bh->pos;
      avail = bh->limit - (uint*)p;
      if (nrefs > avail) {
        ee->gcblk.cantCoop = false;
        gcBuffAllocAndLink( ee, bh );
        p = (GCHandle**)bh->pos;
#ifdef RCDEBUG
        avail = bh->limit - bh->pos;
        mokAssert( nrefs <= avail );
#endif /* RCDEBUG */
        ee->gcblk.cantCoop = true;
      }
      for (;;) {
        unsigned short slot = *offs;
        GCHandle *child;
        
        if (slot==0) break;
        
        child = *(GCHandle**)(slot + (char*)objslots);
        if (child) {
          *p = child;
          p++;
#ifdef RCDEBUG
          nLoggedChilds++; // increment counter of logged slots
          mokAssert( nrefs > 0 );
          nrefs--;
#endif // RCDEBUG
        }
        offs++;
      }
    }
  }
  else {
    register long n = obj_length(h);
    GCHandle **body = (GCHandle**)(((ArrayOfObject*)gcUnhand(h))->body);

    mokAssert( obj_flags(h) == T_CLASS);     /* an array of classes */
    mokAssert( n > 0 );

    p = (GCHandle**)bh->pos;
    avail = bh->limit - (uint*)p;
    if (n > avail) {
      ee->gcblk.cantCoop = false;
      gcBuffAllocAndLink( ee, bh );
      p = (GCHandle**)bh->pos;
#ifdef RCDEBUG
      avail = bh->limit - bh->pos;
      mokAssert( n <= avail );
#endif /* RCDEBUG */
      ee->gcblk.cantCoop = true;
    }
    while (--n >= 0) {
      GCHandle *child = *body;
      body++;
      if (child) {
        *p = child;
        p++;
#ifdef RCDEBUG
        nLoggedChilds++; // increment counter of logged slots
#endif // RCDEBUG
      }
    }
  }

  /* commit ? or discard ? */
  if (!h->logPos) { /* commit */
    *p = (GCHandle*)(BUFF_HANDLE_MARK | (unsigned)h);
    /*
     * actually the order of instructions here
     * should be reversed in order to enable
     * async reading of buffers.
     */
    h->logPos = (uint*)p;
    bh->pos = (unsigned*)(p+1);
#ifdef RCDEBUG
    // increment counters of logged slots
    bh->start[LOG_CHILDS_IDX] += nLoggedChilds;
    bh->start[LOG_OBJECTS_IDX] ++;
#endif // RCDEBUG
  }
}
#pragma optimize( "", on )


#ifdef RCNOINLINE

GCEXPORT void gcBuffConditionalLogHandle(ExecEnv* ee, GCHandle *h)
{
  if (!h->logPos)
    gcBuffSlowConditionalLogHandle( ee, h);
}


GCEXPORT void gcBuffLogWordUnchecked(ExecEnv *ee, BUFFHDR *bh, uint w)
{
  *bh->pos = w;
  bh->pos++;
#ifdef RCDEBUG
  // increment counter of logged objects
  bh->start[LOG_OBJECTS_IDX] ++;
#endif // RCDEBUG
}

GCEXPORT void gcBuffReserveWord(ExecEnv *ee, BUFFHDR *bh)
{
  mokAssert( bh && ee );
  if ( bh->pos >= bh->limit) {
    gcBuffAllocAndLink( ee, bh );
  }
}

GCEXPORT void gcBuffLogWord(ExecEnv *ee, BUFFHDR *bh, uint w)
{
  mokAssert( w && bh && ee );

  gcBuffReserveWord( ee, bh );
  gcBuffLogWordUnchecked( ee, bh, w );
}

GCEXPORT void gcBuffLogNewHandle(ExecEnv *ee, GCHandle *h)
{
  BUFFHDR *bh;

  mokAssert( ee );

  bh = &ee->gcblk.createBuffer;

  ee->gcblk.cantCoop = true;
  *bh->pos = (uint)h;
  h->logPos = bh->pos;
  bh->pos++;
#ifdef RCDEBUG
  // increment counter of logged objects
  bh->start[LOG_OBJECTS_IDX] ++;
#endif // RCDEBUG
  mokAssert( gcGetHandleRC(h)==0 );
  ee->gcblk.cantCoop = false;
  gcBuffReserveWord( ee, bh );

  mokAssert( gcNonNullValidHandle(h) );
}

#endif /* RCNOINLINE */



/******************** VALIDATION *****************************/

GCFUNC bool _isHandle(void *h)
{
  BlkAllocHdr *bah;
  int status;

  if ((byte*)(h) <blkvar.heapStart) return false;
  if ((byte*)(h) >= blkvar.heapTop) return false;
  if ((((unsigned)h) & OBJMASK) != (unsigned)h) return false;
  if ((byte*)unhand((JHandle*)h) != (byte*)gcUnhand((JHandle*)h)) return false;
#ifdef RCDEBUG
  if (((GCHandle*)h)->status != Im_used) return false;
#endif

  bah = OBJBLOCKHDR(h);
  status = bhGet_status( bah );

  if (status==ALLOCBIG) {
    if ( ((uint)h & BLOCKMASK) == 0) 
      return true;
    return false;
  }
  
  if (status<OWNED || status>PARTIAL)
    return false;

#ifdef RCDEBUG
  {
    int bin_idx = bhGet_bin_idx( bah );
    mokAssert( (((uint)h &  BLOCKMASK) % chkconv.binSize[bin_idx]) == 0);
  }
#endif
  
  /* check if on same page or ALLOC_LIST terminator */
  if ((uint)((GCHandle*)h)->logPos == (uint)ALLOC_LIST_NULL) return false;
  if ( ((uint)h ^ (uint)((GCHandle*)h)->logPos) < BLOCKSIZE )
    return false;

#ifdef RCDEBUG
  {
    uint val;
    uint *pos = ((GCHandle*)h)->logPos;
    if (pos) {
      val = *pos;
      if ( (val & ~3) != (uint)h ) {
        /*
         * This is a problem only if we're the collctor,
         * this means that someone has garbaled the log, the
         * logPos pointer or both.
         *
         * If we're a mutator then this is not an error since
         * the log could have already been freed by the collector.
         */
        if (gcvar.ee == EE()) {
          mokAssert(0);
        }
      }
    }
  }
#endif

  return true;
}


/*****************************************************************/

/***************** ZCT + RC **************************************/

#ifndef RCDEBUG
#define _putInNextZCT(h)\
do { \
  gcBuffLogWord( gcvar.ee, (&gcvar.nextZctBuff), (uint)h );\
} while(0)
#else
static void _putInNextZCT(void *h)
{ 
  gcBuffLogWord( gcvar.ee, (&gcvar.nextZctBuff), (uint)h );
  gcvar.dbgpersist.nPendInCycle++;
}
#endif


#define _markInZCT(h) H1BIT_Set( gcvar.zctBmp.entry, (unsigned)h )

#define _markNotInZCT(h)  H1BIT_Clear( gcvar.zctBmp.entry, (unsigned)h )

static bool _isInZCT(GCHandle *h)
{
  bool res;
  H1BIT_GetInlined( gcvar.zctBmp.entry, (unsigned)h, res );
  return res;
}


GCFUNC uint gcGetHandleRC( GCHandle *h)
{
  uint res;
  H2BIT_GetInlined( gcvar.rcBmp.entry, (unsigned)h, res  );
  return res;
}

static void _incrementHandleRC( void  * h)
{
  H2BIT_Inc( gcvar.rcBmp.entry, (unsigned)h );
}

static uint _incrementHandleRCWithReturnValue( void * h)
{
  uint res;
  H2BIT_IncRVInlined( gcvar.rcBmp.entry, (unsigned)h, res );
  return res;
}

static void _decrementHandleRCInUpdate( void * h)
{
  uint prevRC;
  H2BIT_DecInlined( gcvar.rcBmp.entry, (unsigned)h, prevRC );
  if (prevRC==1 && !_isInZCT(h)) {
    _markInZCT( h );
    gcBuffLogWord( gcvar.ee, &gcvar.zctBuff, (uint)h );
#ifdef RCDEBUG
    gcvar.dbg.nInZct++;
    gcvar.dbg.nUpdate2ZCT++;
#endif // RCDEBUG
  }
}


static void _enlargeZctStack(void)
{
  GCHandle **p;
  uint sz = ((char*)gcvar.zctStackTop)-((char*)gcvar.zctStack);

  mokAssert( gcvar.zctStackSp == gcvar.zctStackTop );
  p = (GCHandle**)mokMemReserve( gcvar.zctStack, sz );
  if (p) {
    mokAssert( p == gcvar.zctStack );
    mokMemCommit( p, sz, false );
    gcvar.zctStackTop = (GCHandle**)(sz + (char*)gcvar.zctStackTop);
  }
  else {
    uint newsz = sz*2;
    GCHandle **oldstack = gcvar.zctStack;
    gcvar.zctStack = (GCHandle**)mokMemReserve( NULL, newsz );
    gcvar.zctStackTop = (GCHandle**)(newsz + (char*)gcvar.zctStack);
    gcvar.zctStackSp = (GCHandle**)( sz + (char*)gcvar.zctStack );
    mokMemCommit( (char*)gcvar.zctStack, newsz, false );
    CopyMemory( gcvar.zctStack, oldstack, sz );
    mokMemDecommit( (char*)oldstack, sz );
    mokMemUnreserve( (char*)oldstack, sz );
  }
}


static void _decrementHandleRCInDeletion(void *child)
{
  uint prevRC;
  H2BIT_DecInlined( gcvar.rcBmp.entry, (unsigned)child, prevRC );
  mokAssert( !_isInZCT(child) );
  mokAssert( prevRC > 0 );
  if (prevRC==1) {
#ifdef RCDEBUG
    gcvar.dbg.nRecursiveDel++;
    _freeHandle( child );
#else
    if (gcvar.zctStackSp == gcvar.zctStackTop) {
      _enlargeZctStack();
    }
    *gcvar.zctStackSp++ = child;
#endif // RCDEBUG
  }
}

static void _putInMarkStack(void *h)
{
  if (gcvar.zctStackSp == gcvar.zctStackTop) {
    _enlargeZctStack();
  }
  *gcvar.zctStackSp++ = (GCHandle*)h;
}


static void _decrementLocalHandleRC(void *h)
{
  uint prevRC;
  H2BIT_DecInlined( gcvar.rcBmp.entry, (unsigned)h, prevRC );
  mokAssert( !_isInZCT(h) );
  mokAssert( prevRC > 0 );
  if (prevRC==1) {
    _markInZCT(h);
    _putInNextZCT( h );
  }
}

/**********************  Local Marks **************************/

static bool _isLocal(void *h)
{
  uint res;
  H1BIT_GetInlined( gcvar.localsBmp.entry, (unsigned)h, res );
  return res;
}

static void _setLocal(void *h)
{
  if (!_isLocal(h)) {
    H1BIT_Set( gcvar.localsBmp.entry, (unsigned)h);
    _incrementHandleRC(h);
    gcBuffLogWord( gcvar.ee, (&gcvar.uniqueLocalsBuff), (uint)h );
#ifdef RCDEBUG
    gcvar.dbg.nLocals++;
#endif
  }
}


static void _unsetLocal(void *h)
{
  /* This also resets the local mark of near by objects,
   * but we don't care since we're turning everybody
   * off.
   */
  H1BIT_ClearByte( gcvar.localsBmp.entry, (unsigned)h);
}


/******************** COLLECTION !!!!! ***********************/


/******************* HS1 ************************************/
         

static int _setSnoopFlagHelper(sys_thread_t * thrd, void *dummy)
{
  ExecEnv *ee = SysThread2EE( thrd );

  mokAssert( ee != gcvar.ee );
  ee->gcblk.snoop = true;
  return SYS_OK;
}

static int _HS1Helper(sys_thread_t *thrd, bool *allOK)
{
  ExecEnv *ee;

  ee = SysThread2EE( thrd );

  mokAssert( ee != gcvar.ee );

  if (ee->gcblk.stage == GCHS1) return SYS_OK;
  if (ee->gcblk.cantCoop) {
    *allOK = false;
    return SYS_OK;
  }

  while( gcvar.nPreAllocatedBuffers < 2) {
    buffInit( gcvar.ee, &gcvar.preAllocatedBuffers[gcvar.nPreAllocatedBuffers] );
    gcvar.nPreAllocatedBuffers++;
  }

  mokThreadSuspendForGC( thrd );
  mokAssert(ee->gcblk.stage==GCHS4);
  if (ee->gcblk.cantCoop) {
    mokThreadResumeForGC( thrd );
    *allOK = false;
    return SYS_OK;
  }
#ifdef RCDEBUG
  gcvar.dbg.nHS1Threads++;
  gcvar.dbg.nUpdateObjects += ee->gcblk.updateBuffer.start[LOG_OBJECTS_IDX];
  gcvar.dbg.nUpdateChilds += ee->gcblk.updateBuffer.start[LOG_CHILDS_IDX];
  gcvar.dbg.nCreateObjects += ee->gcblk.createBuffer.start[LOG_OBJECTS_IDX];
#endif // RCDEBUG

  /* now steal the buffers (if they were modified) */

  if (buffIsModified(&ee->gcblk.createBuffer)) {
    /* make sure that the last word in the buffer is NULL */
    *ee->gcblk.createBuffer.pos = 0;
    /* make sure the second entry in the buffer points to
     * the last entry
     */
    ee->gcblk.createBuffer.start[LAST_POS_IDX] = (uint)ee->gcblk.createBuffer.pos;
    /* the first entry is the linked list pointer */
    ee->gcblk.createBuffer.start[LINKED_LIST_IDX] = (uint)gcvar.createBuffList;
    gcvar.createBuffList = ee->gcblk.createBuffer.start;
    /* give the thread new buffers to play with */
    gcvar.nPreAllocatedBuffers--;
    ee->gcblk.createBuffer = gcvar.preAllocatedBuffers[gcvar.nPreAllocatedBuffers];
  }
#ifdef RCDEBUG
  else {
    mokAssert( ee->gcblk.dbg.nBytesAllocatedInCycle==0 );
    mokAssert( ee->gcblk.dbg.nRefsAllocatedInCycle==0 );
  }
#endif

  if (buffIsModified(&ee->gcblk.updateBuffer)) {
    /* do the same for the update buffer */
    *ee->gcblk.updateBuffer.pos = 0;
    ee->gcblk.updateBuffer.start[LAST_POS_IDX] = (uint)ee->gcblk.updateBuffer.pos;
    ee->gcblk.updateBuffer.start[LINKED_LIST_IDX] = (uint)gcvar.updateBuffList;
    gcvar.updateBuffList = ee->gcblk.updateBuffer.start;
    gcvar.nPreAllocatedBuffers--;
    ee->gcblk.updateBuffer = gcvar.preAllocatedBuffers[gcvar.nPreAllocatedBuffers];
  }

#ifdef RCDEBUG
  gcvar.dbg.nBytesAllocatedInCycle += ee->gcblk.dbg.nBytesAllocatedInCycle;
  gcvar.dbg.nRefsAllocatedInCycle += ee->gcblk.dbg.nRefsAllocatedInCycle;
  gcvar.dbg.nNewObjectUpdatesInCycle += ee->gcblk.dbg.nNewObjectUpdatesInCycle;
  gcvar.dbg.nOldObjectUpdatesInCycle += ee->gcblk.dbg.nOldObjectUpdatesInCycle;

  ee->gcblk.dbg.nBytesAllocatedInCycle = 0;
  ee->gcblk.dbg.nRefsAllocatedInCycle = 0;
  ee->gcblk.dbg.nNewObjectUpdatesInCycle = 0;
  ee->gcblk.dbg.nOldObjectUpdatesInCycle = 0;
#endif

  /* restart the thread */
  ee->gcblk.stage = GCHS1;
  mokThreadResumeForGC( thrd );
#if 0
  ee->gcblk.gcSuspended = true;
#endif
  return SYS_OK;
}

#pragma optimize( "", off )
static void _Initiate_Collection_Cycle(void)
{
  bool allOK;

  mokAssert( gcvar.stage == GCHS4);

  // if (gcvar.
  /* raise snoop flags */
  QUEUE_LOCK( gcvar.sys_thread );
  mokThreadEnumerateOver( _setSnoopFlagHelper, NULL );
  QUEUE_UNLOCK( gcvar.sys_thread );
        
#ifdef RCDEBUG
  memset( &gcvar.dbg, 0, sizeof(gcvar.dbg) );
  gcvar.dbg.nInZct = gcvar.dbgpersist.nPendInCycle;
  gcvar.dbgpersist.nPendInCycle =0;
#endif // RCDEBUG

  /* do first handshake */
  QUEUE_LOCK( gcvar.sys_thread );

  gcvar.stage = GCHS1;
  mokAssert( gcvar.createBuffList == NULL );
  mokAssert( gcvar.updateBuffList == NULL );

  gcvar.createBuffList = gcvar.deadThreadsCreateBuffList;
  gcvar.deadThreadsCreateBuffList = NULL;
  gcvar.updateBuffList = gcvar.deadThreadsUpdateBuffList;
  gcvar.deadThreadsUpdateBuffList = NULL;

#ifdef RCDEBUG
  gcvar.dbg.nUpdateObjects = gcvar.dbgpersist.nDeadUpdateObjects;
  gcvar.dbgpersist.nDeadUpdateObjects = 0;

  gcvar.dbg.nUpdateChilds = gcvar.dbgpersist.nDeadUpdateChilds;
  gcvar.dbgpersist.nDeadUpdateChilds = 0;

  gcvar.dbg.nCreateObjects = gcvar.dbgpersist.nDeadCreateObjects;
  gcvar.dbgpersist.nDeadCreateObjects = 0;
#endif

  for(;;) {
    allOK = true;
    mokThreadEnumerateOver( _HS1Helper, &allOK );
    if (allOK) break;
    mokSleep( 10 );
  }

  QUEUE_UNLOCK( gcvar.sys_thread );
}
#pragma optimize( "", on  )

/*********************  HS2 & HS3 **************************/

static void _clearFlagsInUpdateBuffer(uint *p)
{
  uint *ptr;
  uint type;
#ifdef RCDEBUG
  uint *first_entry = p+N_RESERVED_SLOTS;
#endif

  mokAssert( p );


  p = (uint*)p[LAST_POS_IDX];
  mokAssert( ! *p );
  p--;
  mokAssert( *p );

  for (;;) {
    type = *p & 3;
  next_entry:
    ptr = (uint*)(*p & ~3);
#ifdef RCDEBUG
    /* 
     *the one and only entry which
     * is supposed to be NULL is the
     * last one.
     */
    if (p==first_entry) 
      mokAssert( *p == BUFF_LINK_MARK );
    if (*p == BUFF_LINK_MARK) 
      mokAssert(p == first_entry );
#endif
    switch (type) {

    case BUFF_DUP_HANDLE_MARK: {
#ifdef RCDEBUG
      gcvar.dbg.nActualCyclesBroken++;
      gcvar.dbg.nActualUpdateObjects++;
      /*
       * can happen becuase of deletion
       * cycle breaking.
       */
      dbgprn( 3, "\t\tclear:up:broken %x\n", ptr );
#endif
      for (;;) {
        p--;
        type = *p & 3;
        if (type) goto next_entry;
#ifdef RCDEBUG
        gcvar.dbg.nActualUpdateChilds++;
#endif
      }
    }

    case 0: {/* Logged slot entry */
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( gcNonNullValidHandle(h) );
      p--;
#ifdef RCDEBUG
      dbgprn( 4, "\t\tclear:up:slot %x\n", ptr );
      gcvar.dbg.nActualUpdateChilds++;
#endif
      break;
    }

    case BUFF_LINK_MARK: {
      if (!ptr) {
#ifdef RCDEBUG
        mokAssert(p==first_entry);
#endif
        return;
      }
      mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
      p = ptr-1; // skip forward pointer
      break;
    }

    case BUFF_HANDLE_MARK: { /* Containing object entry */
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( h );
#ifdef RCDEBUG
      dbgprn( 4, "\t\tclear:up:hand %x\n", ptr );
      /* is this entry cycle closing ?
       * we assume that the striking majority
       * of entries are, so we modify
       * only those which are duplicates.
       */
      gcvar.dbg.nActualUpdateObjects++;
#endif
      if (h->logPos == p) { /* yep */
        mokAssert( gcNonNullValidHandle(h) );
        h->logPos = NULL; /* clear dirty flag */
      } else {
        *p = BUFF_DUP_HANDLE_MARK | (uint)h;
#ifdef RCDEBUG
        gcvar.dbg.nUpdateDuplicates++;
#endif
      }
      p--;
      break;
    }
    }
  }
}

static void _clearFlagsInUpdateBufferList(void)
{
  uint *buffList = gcvar.updateBuffList;
  while (buffList) {
    _clearFlagsInUpdateBuffer( buffList );
    buffList = (uint*)buffList[LINKED_LIST_IDX];
  }
}


static void _clearFlagsInCreateBuffer(uint *p)
{
#ifdef RCDEBUG
  uint *last_entry = (uint*)p[LAST_POS_IDX];
#endif

  mokAssert( p );

  p += N_RESERVED_SLOTS;
  p++; /* skip the first back pointer */

  for (;;) {
    uint *ptr = (uint*)(*p & ~3);
    uint type = *p & 3;
    mokAssert( type != BUFF_HANDLE_MARK);
    mokAssert( type != BUFF_DUP_HANDLE_MARK);
#ifdef RCDEBUG
    /* 
     * the one and only entry which
     * is supposed to be NULL is the
     * last one.
     */
    if (p==last_entry) 
      mokAssert( *p == 0 );
    if (!*p) 
      mokAssert(p == last_entry );
#endif
    if (type==0) {
      GCHandle *h = (GCHandle*)ptr;
#ifdef RCDEBUG
      dbgprn( 4, "\t\tclear:cr: %x\n", ptr );
#endif
      if (!h) return;
      mokAssert( gcValidHandle(h)  );
      /* In the create buffer all entries
       * are cycle closing since there is
       * no contention for these objects.
       */
      mokAssert( h->logPos == p );
      h->logPos = NULL; /* clear dirty mark */
#ifdef RCDEBUG
      gcvar.dbg.nActualCreateObjects++;
#endif
      p++;
    }
    else { /*type==BUFF_LINK_MARK*/
      mokAssert( ptr );
      mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
      mokAssert( (LOWBUFFMASK & (uint)ptr) == N_RESERVED_SLOTS*sizeof(uint));
      p = ptr+1;
    }
  }
}



static void _clearFlagsInCreateBufferList( void )
{
  uint *buffList = gcvar.createBuffList;
  while (buffList) {
    _clearFlagsInCreateBuffer( buffList );
    buffList = (uint*)buffList[LINKED_LIST_IDX];
  }
}


static void _Clear_Dirty_Marks(void)
{
#ifdef RCDEBUG
  DWORD start, end;

  start = GetTickCount();
  dbgprn( 0, "_Clear_Dirty_Marks(begin) time=%d\n", start);
#endif

  _clearFlagsInCreateBufferList(  );
  _clearFlagsInUpdateBufferList(  );

#ifdef RCDEBUG
  end = GetTickCount();

  dbgprn( 2, "\tnHS1Threads=%d\n", gcvar.dbg.nHS1Threads );
  dbgprn( 2, "\tnUpdateObjects=%d\n",  gcvar.dbg.nUpdateObjects );
  dbgprn( 2, "\tnUpdatdChilds=%d\n", gcvar.dbg.nUpdateChilds );
  dbgprn( 2, "\tnActualUpdateObjects=%d\n", gcvar.dbg.nActualUpdateObjects );
  dbgprn( 2, "\tnActualUpdateChilds=%d\n", gcvar.dbg.nActualUpdateChilds );
  dbgprn( 2, "\tnFreeCyclesBroken=%d\n", gcvar.dbgpersist.nFreeCyclesBroken );
  dbgprn( 2, "\tnCreateObjects=%d\n",  gcvar.dbg.nCreateObjects );
  dbgprn( 2, "\tnActualCreateObjects=%d\n",  gcvar.dbg.nActualCreateObjects );

  if (gcvar.dbg.nUpdateDuplicates) {
    dbgprn( 1, "\tnUpdateDuplicates=%d\n", gcvar.dbg.nUpdateDuplicates );
  }

  if (gcvar.dbgpersist.nFreeCyclesBroken) {
    dbgprn( 1, "\tnFreeCyclesBroken=%d\n", gcvar.dbgpersist.nFreeCyclesBroken );
  }

  mokAssert( gcvar.dbg.nActualUpdateObjects == gcvar.dbg.nUpdateObjects );
  mokAssert( gcvar.dbg.nActualUpdateChilds == gcvar.dbg.nUpdateChilds );
  mokAssert( gcvar.dbg.nActualCyclesBroken == gcvar.dbgpersist.nFreeCyclesBroken );
  mokAssert( gcvar.dbg.nActualCreateObjects == gcvar.dbg.nCreateObjects );

  gcvar.dbgpersist.nFreeCyclesBroken = 0;
  dbgprn( 0, "_Clear_Dirty_Marks(end) time=%d delta=%d\n", end, end-start );
#endif // RCDEBUG
}


static int _HS2Helper(sys_thread_t *thrd, bool *allOK)
{
  ExecEnv *ee;

  ee = SysThread2EE( thrd );

  mokAssert( gcvar.ee != ee );

  if (ee->gcblk.stage == GCHS2) return SYS_OK;
  if (ee->gcblk.cantCoop) {
    *allOK = false;
    return SYS_OK;
  }

  mokThreadSuspendForGC( thrd );
  mokAssert( ee->gcblk.stage == GCHS1 );
  if (ee->gcblk.cantCoop) {
    mokThreadResumeForGC( thrd );
    *allOK = false;
    return SYS_OK;
  }

  /* mark current position in the buffer */
  ee->gcblk.updateBuffer.start[LAST_POS_IDX] = (uint)ee->gcblk.updateBuffer.pos;
  /* 
   * link the buffer into the reinforce buff
   * list.  Note that the buffer stays at the
   * mutator.
   *
   * We link the buffers instead of going again
   * through the thread ring in order not to
   * lock it when we really do the reinforce
   * stage.
   */
  ee->gcblk.updateBuffer.start[REINFORCE_LINKED_LIST_IDX] = 
    (uint)gcvar.reinforceBuffList;
  gcvar.reinforceBuffList = ee->gcblk.updateBuffer.start;

#ifdef RCDEBUG
  {
    uint *pos = ee->gcblk.updateBuffer.pos;
    /*
     * i.e., we never point to the reserved area:
     */
    mokAssert( (((uint)pos)&LOWBUFFMASK) >= N_RESERVED_SLOTS );
    /*
     * If there is something in the current chunk, then
     * the last entry is a containing handle entry.
     * i.e., we don't see partial entries.
     */
    mokAssert( (((uint)pos)&LOWBUFFMASK) >= (N_RESERVED_SLOTS+1)*sizeof(int) );
    if ( (((uint)pos)&LOWBUFFMASK) > (N_RESERVED_SLOTS+1)*sizeof(int)) {
      mokAssert( (((uint)*(pos-1))&3) == BUFF_HANDLE_MARK );
    }
    /*
     * Otherwise, this should be a back-pointer to the
     * previous chunk.
     */
    else {
      mokAssert( (((uint)*(pos-1))) == BUFF_LINK_MARK );
    }
    gcvar.dbg.nHS2Threads++;
    gcvar.dbg.nReinforceObjects += ee->gcblk.updateBuffer.start[LOG_OBJECTS_IDX];
    gcvar.dbg.nReinforceChilds += ee->gcblk.updateBuffer.start[LOG_CHILDS_IDX];
  }
#endif /* RCDEBUG */

  /* restart the thread */
  ee->gcblk.stage = GCHS2;
  mokThreadResumeForGC( thrd );
  return SYS_OK;
}

static void _reinforceUpdateBuffer( uint *p, uint *limit )
{
  mokAssert( p );

  p += N_RESERVED_SLOTS;
  p++; /* skip the first back pointer */

  for (;;) {
    uint *ptr = (uint*)(*p & ~3);
    uint type = *p & 3;
#ifdef DEBUG
    if (!ptr)
      mokAssert( p ==  limit);
#endif DEBUG
    if (p==limit)
      return;
    mokAssert( type != BUFF_DUP_HANDLE_MARK );
    switch (type) {
    case BUFF_LINK_MARK: {
      mokAssert( ptr );
      mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
      mokAssert( (LOWBUFFMASK & (uint)ptr) == N_RESERVED_SLOTS*sizeof(uint));
      p = ptr+1; /* skip backward pointer */
      break;
    }

    case BUFF_HANDLE_MARK: {
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( h );
      mokAssert( gcNonNullValidHandle(h) );
                                /* reinforce, if needed */
      if (!h->logPos)
        h->logPos = p;
      p++;
#ifdef RCDEBUG
      gcvar.dbg.nActualReinforceObjects ++;
#endif // RCDEBUG
      break;
    }
                
    case 0: {
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( h );
      mokAssert( gcNonNullValidHandle(h) );
#ifdef RCDEBUG
      gcvar.dbg.nActualReinforceChilds ++;
#endif // RCDEBUG
      p++;
      break;
    }
    }
  }
}

static void _HS3Cooperate(ExecEnv *ee)
{
  bool res = gcCompareAndSwap( &ee->gcblk.stageCooperated, GCHSNONE, GCHS3 );
  mokAssert( res );
#ifdef RCDEBUG
  gcvar.dbg.nHS3CoopThreads++;
#endif /* RCDEBUG */
}

static int _HS3Helper(sys_thread_t *thrd, bool *allOK)
{
  ExecEnv *ee;
  bool res;

  ee = SysThread2EE( thrd );

  mokAssert( gcvar.ee != ee );

  /* already moved to the next state? */
  if (ee->gcblk.stage == GCHS3) return SYS_OK;

  /* only the collector advances the stage field */
  mokAssert( ee->gcblk.stage == GCHS2 );

  /* did the thread cooperate voluntarily? */
  res = gcCompareAndSwap( &ee->gcblk.stageCooperated, GCHS3, GCHSNONE);
  if (res) {
    ee->gcblk.stage = GCHS3;
#ifdef RCDEBUG
    gcvar.dbg.nHS3Threads += 100;
#endif /* RCDEBUG */
    return SYS_OK;
  }

  /* OK, we will suspend the thread, but only
   * if it's in cooperative mode.
   *
   * Pesimistic check:
   */
  if (ee->gcblk.cantCoop) {
    *allOK = false; /* try later */
    return SYS_OK;
  }
        
  /* Suspend the thread */
  mokThreadSuspendForGC( thrd );

  /* 
   * Now we have to check cantCoop again.
   */
  if (ee->gcblk.cantCoop) {
    mokThreadResumeForGC( thrd );
    *allOK = false; /* try later */
    return SYS_OK;
  }

  mokAssert( ee->gcblk.stageCooperated == GCHS3 ||
             ee->gcblk.stageCooperated == GCHSNONE );
  
  ee->gcblk.stageCooperated = GCHSNONE;
  ee->gcblk.stage = GCHS3;
  mokThreadResumeForGC( thrd );

#ifdef RCDEBUG
  gcvar.dbg.nHS3Threads++;
#endif /* RCDEBUG */

  return SYS_OK;
}

static void _Reinforce_Clearing_Conflict_Set(void)
{
  bool allOK;

#ifdef RCDEBUG
  uint start, end;

  start = GetTickCount();
  dbgprn( 0, "_Reinforce_Clearing_Conflict_Set(begin) time=%d\n", start);
#endif

  /* do second handshake */
  mokAssert( gcvar.reinforceBuffList == NULL );

  QUEUE_LOCK( gcvar.sys_thread );  
  gcvar.stage = GCHS2;
  /* 
   * Link for reinforcemenr buffers of threads who
   * died between HS1 and HS2
   */
  gcvar.reinforceBuffList = gcvar.deadThreadsReinforceBuffList;
  gcvar.deadThreadsReinforceBuffList = NULL;

#ifdef RCDEBUG
  gcvar.dbg.nReinforceObjects = gcvar.dbgpersist.nDeadReinforceObjects;
  gcvar.dbgpersist.nDeadReinforceObjects = 0;

  gcvar.dbg.nReinforceChilds  = gcvar.dbgpersist.nDeadReinforceChilds;
  gcvar.dbgpersist.nDeadReinforceChilds = 0;
#endif

  /*
   * Link update buffers of live threads
   */
  for(;;) {
    allOK = true;
    mokThreadEnumerateOver( _HS2Helper, &allOK );
    if (allOK) break;
    mokSleep( 10 );
  }
  QUEUE_UNLOCK( gcvar.sys_thread );

  while ( gcvar.reinforceBuffList ) {
    uint *p = gcvar.reinforceBuffList;
    uint *limit = (uint*)gcvar.reinforceBuffList[LAST_POS_IDX];
    _reinforceUpdateBuffer( p, limit );
    gcvar.reinforceBuffList = (uint*)p[REINFORCE_LINKED_LIST_IDX];
  }

  /* do third handshake */
  QUEUE_LOCK( gcvar.sys_thread );
  gcvar.stage = GCHS3;
  for(;;) {
    allOK = true;
    mokSleep( 10 );
    mokThreadEnumerateOver( _HS3Helper, &allOK );
    if (allOK) break;
  }
  QUEUE_UNLOCK( gcvar.sys_thread );

#ifdef RCDEBUG
  end = GetTickCount();

  dbgprn( 2, "\tnHS2Threads=%d\n", gcvar.dbg.nHS2Threads );
  dbgprn( 2, "\tnHS3Threads=%d\n", gcvar.dbg.nHS3Threads );
  dbgprn( 2, "\tnHS3CoopThreads=%d\n", gcvar.dbg.nHS3CoopThreads );

  if (gcvar.dbg.nReinforceObjects || gcvar.dbg.nReinforceChilds) {
    dbgprn( 1, "\tnReinforceChilds=%d\n", gcvar.dbg.nReinforceChilds );
    dbgprn( 1, "\tnReinforceObjects=%d\n",  gcvar.dbg.nReinforceObjects );
  }

  mokAssert( gcvar.dbg.nActualReinforceObjects == gcvar.dbg.nReinforceObjects );
  mokAssert( gcvar.dbg.nActualReinforceChilds == gcvar.dbg.nReinforceChilds );

  dbgprn( 
         0, 
         "_Reinforce_Clearing_Conflict_Set(end) time=%d delta=%d\n", 
         end, 
         end-start );
#endif // RCDEBUG
}


static void _markHandlesInSnoopBufferAsLocal(uint *buff)
{
  uint *ptr, type, *p;

  mokAssert( buff );

  /* go backwards */
  p = (uint*)buff[LAST_POS_IDX];
  mokAssert( p );
  mokAssert( *p==0 );
  p--;
  mokAssert( *p );


  for (;;) {
    ptr = (uint*)(*p & ~3);
    type = *p & 3;
    mokAssert( type != BUFF_HANDLE_MARK );
    mokAssert( type != BUFF_DUP_HANDLE_MARK );
#ifdef DEBUG
    if (!ptr)
      mokAssert( buff+N_RESERVED_SLOTS == p);
    if ( buff+N_RESERVED_SLOTS == p) 
      mokAssert( *p == BUFF_LINK_MARK);
#endif
    if (type==0) {
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( h );
      mokAssert( gcNonNullValidHandle(h) );
      _setLocal( h );
#ifdef RCDEBUG
      gcvar.dbg.nActualSnooped++;
#endif // RCDEBUG
      p--;
    }
    else { /*type==BUFF_LINK_MARK*/
      mokAssert( (LOWBUFFMASK & (uint)p) == N_RESERVED_SLOTS*sizeof(uint));
      /* free the more recent buffer */
      _freeBuff( gcvar.ee, p - N_RESERVED_SLOTS);
      if (!ptr)
        return;
      mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
      p = ptr-1; /* skip forward pointer */
    }

  }
}

static void _markSnoopedAsLocal(void)
{
  uint *buff = gcvar.snoopBuffList;
  while (buff) {
    uint *nextBuff = (uint*)buff[0];
    _markHandlesInSnoopBufferAsLocal(buff);
    buff = nextBuff;
  }
  gcvar.snoopBuffList = NULL;
}


/**************************** HS4 *****************************************/

#define SAFETY_MARGINE 20

static void _snoopExactHandle(JHandle *h)
{
  if (!h) return;
  mokAssert( _isHandle(h) );
  _setLocal( h );
}

static void _snoopHandleOrScalar(JHandle *h)
{
  if (_isHandle(h))
    _setLocal(h);
}

static void _snoopHandleOrObjectOrScalar(JHandle *h)
{
  if (_isHandle(h))
    _setLocal(h);
  else {
    JHandle *obj = gcRehand(h);
    if (_isHandle(obj)) {
      _setLocal( obj );
    }
  }
}

static void _snoopJavaFrame(JavaFrame *frame, stack_item *top_top_stack)
{
  stack_item *ssc, *limit;
  JHandle *ptr;
  JavaStack *javastack;
  struct methodblock *mb = frame->current_method;
  
  limit = top_top_stack;
  javastack = frame->javastack;
  
  /* Scan the operand stack. */
  /*CONSTCOND*/
  while (1) {
    int is_first_chunk = IN_JAVASTACK((stack_item *)frame, javastack);
    for (ssc = is_first_chunk ? frame->ostack : javastack->data;
         ssc < limit; ssc++) {
      ptr = ssc->h;
      _snoopHandleOrScalar( (JHandle*)ptr ); /* Never an object pointer */
    }
    if (is_first_chunk)
      break;
    javastack = javastack->prev;
    limit = javastack->end_data;
  }

  /* Nothing more to do for pseudo and JIT frames. */
  if (mb == 0 || IS_JIT_FRAME(frame)) {
    mokAssert( !IS_JIT_FRAME(frame) ); /* YLRC -- don't support JIT ... */
    return;
  }
  
  if (mb->fb.access & ACC_NATIVE) {
    /* For native frames, we scan the arguments stored at the top 
       of the previous frame. */
    JavaFrame *prev_frame = frame->prev;
    if (prev_frame == 0)
      return;
    ssc = prev_frame->optop;
    limit = ssc + mb->args_size;
  } else {
    /* Scan local variables in Java frame */
    ssc = frame->vars;
    if (ssc == 0)
      return;
    limit = (stack_item *)frame;
  }
  
  for (; ssc < limit; ssc++) {
    ptr = ssc->h;
    _snoopHandleOrScalar(ptr); /* Never an object pointer */
  }
}

static void _snoopThreadLocals( sys_thread_t *t )
{
  ExecEnv *ee = SysThread2EE(t);
  JHandle *tobj = ee->thread;
  unsigned char **ssc, **limit;
  void *base;
  
  mokAssert( EE2SysThread(ee) != sysThreadSelf());

  if (ee->initial_stack == NULL) {
    /* EE already destroyed. */
    return;
  }
  
  /* Mark thread object */
  if (tobj) {
    mokAssert( gcNonNullValidHandle((GCHandle*)tobj) );
    _snoopExactHandle( tobj );
  }
  {
    long *regs;
    int nregs;
    
    /* Scan the saved registers */
    regs = sysThreadRegs(t, &nregs);
    for (nregs--; nregs >= 0; nregs--) {
      _snoopHandleOrObjectOrScalar( (JHandle*)regs[nregs] );
    }
    
    base = ee->stack_base;
    ssc  = sysThreadStackPointer(t);
  }

  if (ssc == 0 || base == 0 || (ssc == base)) {
    /*
     * If the stack does not have a top of stack pointer or a base
     * pointer then it hasn't run yet and we don't need to scan 
     * its stack.  When exactly each of these data becomes available
     * may be system-dependent, but we need both to bother scanning.
     */
    goto ScanJavaStack;
  }

  /* Align stack top, important on Windows 95. */
  if ((long)ssc % sizeof(void *)) {
    ssc = (unsigned char **)((long)(ssc) & ~(sizeof(void *) - 1));
  }

  limit = (unsigned char **) base;

  mokAssert(ssc != limit);

  /*
   * The code that scans the C stack is assuming that the current
   * stack pointer is at a lower address than the limit of the stack.
   * Obvioulsy, this is only true for downward growing stacks.  For
   * upward growing stack, we exchange ssc and limit before we start
   * to scan the stack.
   */
  
#if defined(STACK_GROWS_UP)
  {
    unsigned char  **tmp;
    
    tmp = limit;
    limit = ssc;
    ssc = tmp;
  }
#endif /* STACK_GROWS_UP */


  while (ssc < limit) {
    register unsigned char *ptr = *ssc;
    _snoopHandleOrObjectOrScalar( (JHandle*)ptr );
    ssc++;
  }

  /*
   * Whether or not we scan the thread stack, we decide independently
   * whether to scan the Java stack.  Doing so should be more robust
   * in the face of partially-initialized or partially-zeroed threads
   * during thread creation or exit, or changes to any of that code.
   */
 ScanJavaStack:
  {
    JavaFrame *frame;
    
    /*
     * Because of the Invocation API, the EE may not be on the C
     * stack anymore.
     */
    _snoopExactHandle( ee->exception.exc );

    _snoopExactHandle(ee->pending_async_exc);

    if ((frame = ee->current_frame) != 0) {
      struct methodblock *prev_current_method = 0;
      while (frame) {
        struct methodblock *current_method = frame->current_method;
        /*
         * If the previous frame was a transition frame from C back 
         * to Java (indicated by prev_current_method == NULL), then 
         * this new frame might not have set its optop.  We must be 
         * conservative.   Otherwise, we can use the optop value.
         *
         * Also permit two consecutive frames with NULL current
         * methods, in support of JITs.  See bug 4022856.
         */
        stack_item *top_top_stack = 
          (prev_current_method == 0 && current_method != NULL &&
           ((current_method->fb.access & ACC_NATIVE) == 0))
          ? &frame->ostack[frame->current_method->maxstack] 
          : frame->optop;
        _snoopJavaFrame(frame, top_top_stack);
        frame = frame->prev;
        prev_current_method = current_method;
      }
    }
  }
}



static int _HS4Helper( sys_thread_t *thrd, bool *allOK )
{
  ExecEnv *ee;

  ee = SysThread2EE( thrd );
  
  mokAssert( gcvar.ee != ee );

  if (ee->gcblk.stage == GCHS4) return SYS_OK;
  if (ee->gcblk.cantCoop) {
    *allOK = false;
    return SYS_OK;
  }
  
  while(gcvar.nPreAllocatedBuffers < 1) {
    buffInit( gcvar.ee, &gcvar.preAllocatedBuffers[gcvar.nPreAllocatedBuffers] );
    gcvar.nPreAllocatedBuffers++;
  }

  mokThreadSuspendForGC( thrd );
  mokAssert( ee->gcblk.stage == GCHS3 );
  if (ee->gcblk.cantCoop) {
    mokThreadResumeForGC( thrd );
    *allOK = false;
    return SYS_OK;
  }
  
  ee->gcblk.snoop = false;
  
  /* put into the snooped object set 
   * all of the locally reachable objects
   */
  _snoopThreadLocals( thrd );
  
  /* now steal the snooped objects set */
  if (buffIsModified(&ee->gcblk.snoopBuffer)) {
    *ee->gcblk.snoopBuffer.pos = 0;
    ee->gcblk.snoopBuffer.start[LAST_POS_IDX] = (uint)ee->gcblk.snoopBuffer.pos;
    ee->gcblk.snoopBuffer.start[LINKED_LIST_IDX] = (uint)gcvar.snoopBuffList;
    gcvar.snoopBuffList = ee->gcblk.snoopBuffer.start;
    
#ifdef RCDEBUG
    gcvar.dbg.nSnooped += ee->gcblk.snoopBuffer.start[LOG_OBJECTS_IDX];
#endif // RCDEBUG
  
    /* give the thread a new snoop buffer to play with */
    gcvar.nPreAllocatedBuffers--;
    ee->gcblk.snoopBuffer = gcvar.preAllocatedBuffers[gcvar.nPreAllocatedBuffers];
  }
  
#ifdef RCDEBUG
  gcvar.dbg.nHS4Threads++;
#endif // RCDEBUG

  /* restart the thread */
  ee->gcblk.stage = GCHS4;
  
  mokThreadResumeForGC( thrd );
  return SYS_OK;
}

static void _snoopClass(ClassClass *cb) 
{
  /* We must be extra careful in scanning the internals of a class
   * structure, because this routine may be called when a class
   * is only partially loaded (in createInternalClass).
   */
  /*
   * YLRC --
   *
   * No need to recursively trace super classes as we mark all
   * classes anyway.  This also holds for classes referred
   * to from the constant pool.
   *
   */
  JHandle *h;

  if (cbConstantPool(cb) &&
      cbConstantPool(cb)[CONSTANT_POOL_TYPE_TABLE_INDEX].type) {
    union cp_item_type *constant_pool = cbConstantPool(cb);
    union cp_item_type *cpp = 
      constant_pool+ CONSTANT_POOL_UNUSED_INDEX;
    union cp_item_type *end_cpp = 
      &constant_pool[cbConstantPoolCount(cb)];
    unsigned char *type_tab =
      constant_pool[CONSTANT_POOL_TYPE_TABLE_INDEX].type;
    unsigned char *this_type = 
      &type_tab[CONSTANT_POOL_UNUSED_INDEX];

    for ( ; cpp < end_cpp; cpp++, this_type++) {
      if (*this_type == (CONSTANT_String|CONSTANT_POOL_ENTRY_RESOLVED)) {
        _snoopExactHandle( (JHandle*)(*cpp).p );
      }
    } /* loop over constant pool*/
  }

  /* Scan class definitions looking for statics */
  if (cbFields(cb) && 
      (cbFieldsCount(cb) > 0)) { /* defensive check */
    int i;
    struct fieldblock *fb;
    for (i = cbFieldsCount(cb), fb = cbFields(cb); --i >= 0; fb++) {
      if (fieldsig(fb) &&  /* Extra defensive */
          (fieldIsArray(fb) || fieldIsClass(fb)) && (fb->access & ACC_STATIC)) {
        JHandle *sub = *(JHandle **)normal_static_address(fb);
        _snoopExactHandle( sub );
      }
    }
  }

  h = (JHandle *)cbClassname(cb);
  _snoopExactHandle( h );

  h = (JHandle *)cbLoader(cb);
  _snoopExactHandle( h );
  
  h = (JHandle *)cbSigners(cb);
  _snoopExactHandle( h );
  
  h = (JHandle *)cbProtectionDomain(cb);
  _snoopExactHandle( h );
}

static void _snoopBinClasses(void)
{
  ClassClass **pcb;
  int i;

  BINCLASS_LOCK( sysThreadSelf() /*gcvar.sys_thread*/ );
  pcb = binclasses;
  for (i = nbinclasses; --i >= 0; pcb++) {
    ClassClass *cb = *pcb;
    _snoopExactHandle( (JHandle*)cb );
    _snoopClass( cb );
  }
  BINCLASS_UNLOCK( sysThreadSelf() /*gcvar.sys_thread*/ );
}

static void _snoopPrimitiveClasses(void)
{
  static ClassClass **primitive_classes[] = {
    &class_void, &class_boolean, &class_byte, &class_char, &class_short,
    &class_int, &class_long, &class_float, &class_double, NULL
  };
  ClassClass ***cbpp = primitive_classes;

  while (*cbpp) {
    ClassClass *cb = **cbpp;
    _snoopExactHandle( (JHandle*)cb );
    _snoopClass( cb );
    cbpp++;
  }
}
  
static void _snoopMonitorCacheHelper(monitor_t *mid, void *cookie)
{
  JHandle *h = (JHandle*) mid->key;
  if (_isHandle(h) && sysMonitorInUse(sysmon(mid)) ) {
    _snoopExactHandle( h );
  }
}

static void _snoopMonitorCache(void)
{
  CACHE_LOCK( sysThreadSelf() /*gcvar.sys_thread*/ );
  monitorEnumerate( _snoopMonitorCacheHelper, 0);
  CACHE_UNLOCK( sysThreadSelf() /*gcvar.sys_thread*/ );
}

static void  _snoopJNIGlobalsRefs( void )
{
  _snoopJavaFrame(globalRefFrame, globalRefFrame->optop);
}

static void _snoopInternedStrings(void);
  
static void _snoopGlobals(void)
{
  _snoopBinClasses(  );
  _snoopPrimitiveClasses(  );
  _snoopMonitorCache(  );
  _snoopInternedStrings( );
  _snoopJNIGlobalsRefs(  );
}


static void _Consolidate( void )
{
  bool allOK;
#ifdef RCDEBUG
  uint start, end;
  
  start = GetTickCount();
  dbgprn( 0, "_Consolidate(begin) time=%d\n", start);
#endif

  if (gcvar.collectionType == GCT_TRACING)
    _traceSetup();

  /* init buffer of local objects */
  buffInit( gcvar.ee, &gcvar.uniqueLocalsBuff );

  /* snoop global objects */
  _snoopGlobals( );

#ifdef RCDEBUG
  gcvar.dbg.nGlobals = gcvar.dbg.nLocals;
  gcvar.dbg.nLocals = 0;
#endif

  /* do fourth handshake */
  QUEUE_LOCK( gcvar.sys_thread );

  gcvar.stage = GCHS4;
  mokAssert( gcvar.snoopBuffList == NULL );

  /* add snoop buffers of dead threads and
   * clear the list
   */
  gcvar.snoopBuffList = gcvar.deadThreadsSnoopBuffList;
  gcvar.deadThreadsSnoopBuffList = NULL;


#ifdef RCDEBUG
  gcvar.dbg.nSnooped = gcvar.dbgpersist.nDeadSnooped;
  gcvar.dbgpersist.nDeadSnooped = 0;
#endif

  /* now add the threads buffers */
  for(;;) {
    allOK = true;
    mokThreadEnumerateOver( _HS4Helper, &allOK );
    if (allOK) break;
    mokSleep( 10 );
  }
  
  QUEUE_UNLOCK( gcvar.sys_thread );

  /* process thread buffers */
  _markSnoopedAsLocal();


#ifdef RCDEBUG
  end = GetTickCount();

  dbgprn( 2, "\tnHS4Threads=%d\n", gcvar.dbg.nHS4Threads );
  dbgprn( 2, "\tnSnooped=%d\n",  gcvar.dbg.nSnooped );
  dbgprn( 4, "\tnActualSnooped=%d\n", gcvar.dbg.nActualSnooped );
  dbgprn( 2, "\tnLocals=%d\n", gcvar.dbg.nLocals );
  dbgprn( 2, "\tnGlobals=%d\n", gcvar.dbg.nGlobals );

  mokAssert( gcvar.dbg.nActualSnooped == gcvar.dbg.nSnooped );
  dbgprn( 0, "_Consolidate(end) time=%d delta=%d\n", end, end-start);

#endif // RCDEBUG
}


/*****************************************************************/
/************************  UPDATE PHASE  *************************/
/*****************************************************************/

/************************  Updating Counters *********************/

static void _determineHandleContents(GCHandle *h)
{
  uint *p;
  
 start:
  p = h->logPos;
  
  if (p) {
    mokAssert( h == (GCHandle*)(*p^BUFF_HANDLE_MARK) );
#ifdef RCDEBUG
    gcvar.dbg.nUndetermined++;
#endif // RCDEBUG
    p--;
    while (1) {
      GCHandle *hSon = (GCHandle*)*p;
      uint type = 3 & *p;
      mokAssert( hSon );
      if (type) return;
      _incrementHandleRC( hSon );
      p--;
    }
  }
  
  {
    GCHandle **tempbuff = gcvar.tempReplicaSpace;
    register GCHandle *child;
    register GCHandle **objslots;

    switch (obj_flags(h)) {
    case T_NORMAL_OBJECT:{
      register ClassClass *cb = obj_classblock(h);
      register unsigned short  offset;
      register unsigned short *object_offsets ;

      if (cb == classJavaLangClass || unhand(cb)->n_object_offsets==0) {
#ifdef RCDEBUG
        gcvar.dbg.nDetermined++;
#endif
        return;
      }
        
      object_offsets = cbObjectOffsets(cb);
      objslots = (GCHandle **)(((char *)unhand(h)) - 1);
      while ((offset = *object_offsets++)) {
        child = *(GCHandle **) ((char *) objslots + offset);
        if (child) {
          tempbuff++;
          *tempbuff = child;
        }
      }
      break;
    }

    case T_CLASS: {     /* an array of classes */
      register long n = obj_length(h);
      GCHandle **body = (GCHandle**)(((ArrayOfObject*)gcUnhand(h))->body);
      while (--n >= 0) {
        child  = body[n];
        if (child) {
          tempbuff++;
          *tempbuff = child;
        }
      }
      break;
    }
    }
    if (h->logPos) {
      goto start;
    }
    /* OK, the replica we have at this point is valid
     * so use it as the reference to the objects'
     * contents.
     */
#ifdef RCDEBUG
    gcvar.dbg.nDetermined++;
#endif // RCDEBUG
    while( tempbuff > gcvar.tempReplicaSpace) {
      child = *tempbuff;
      _incrementHandleRC( child );
      tempbuff--;
    }
  }
}

static void _updateRCofSingleUpdateLog(uint *buff)
{
  uint *ptr, type, *p;

  mokAssert( buff );

  /* 
   * go backwards since its better to
   * first increment and only then decrement
   * (it will cause less entries in the ZCT)
   * so we want to first see the handle and
   * only then its contents.
   */
  p = (uint*)buff[LAST_POS_IDX];

  mokAssert( p );
  mokAssert( *p==0 );
  p--;
  mokAssert( *p );


  for (;;) {
    type = *p & 3;
  next_round:
    ptr = (uint*)(*p & ~3);
    mokAssert( type != 0 );
    switch (type) {
    case BUFF_LINK_MARK: {
      mokAssert( (LOWBUFFMASK & (uint)p) == N_RESERVED_SLOTS*sizeof(uint));
                                /* free the more recent buffer */
      _freeBuff( gcvar.ee, p - N_RESERVED_SLOTS);
      if (!ptr) {
        mokAssert( buff+N_RESERVED_SLOTS == p);
        return;
      }
      mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
      p = ptr-1; /* skip forward pointer */
      break;
    }

    case BUFF_HANDLE_MARK: {
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( h );
      mokAssert( gcNonNullValidHandle(h) );
      _determineHandleContents( h );
#ifdef RCDEBUG
      gcvar.dbg.nUpdateRCObjects++;
#endif // RCDEBUG
      for(;;) {
        GCHandle *h;
        p--;
        h = (GCHandle*)*p;
        type = ((uint)h) &3;
        if (type) goto next_round;
        mokAssert( gcNonNullValidHandle(h) );
        _decrementHandleRCInUpdate( h );
#ifdef RCDEBUG
        gcvar.dbg.nUpdateRCChilds++;
#endif // RCDEBUG
      }
    }

    case BUFF_DUP_HANDLE_MARK: {                                
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( h );
#ifdef RCDEBUG
      gcvar.dbg.nUpdateRCObjects++;
      gcvar.dbg.nUpdateRCDuplicates++;
#endif // RCDEBUG
      for(;;) {
        p--;
        type = *p & 3;
        if (type) goto next_round;
#ifdef RCDEBUG
        gcvar.dbg.nUpdateRCChilds++;
#endif // RCDEBUG
      }
    }
    }
  }
}


static void _updateRCofSingleCreateLog(uint *buff)
{
  uint *ptr, type, *p;

  mokAssert( buff );

  p = (uint*)buff[LAST_POS_IDX];
  mokAssert( p );
  mokAssert( *p == 0 );
  p--;
  mokAssert( *p );

  for (;;) {
    ptr = (uint*)(*p & ~3);
    type = *p & 3;
    mokAssert( type != BUFF_HANDLE_MARK );
    mokAssert( type != BUFF_DUP_HANDLE_MARK );

    if (type==0) {
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( h );
      mokAssert( gcNonNullValidHandle(h) );
      _determineHandleContents( h );
#ifdef RCDEBUG
      gcvar.dbg.nCreateRCObjects++;
#endif // RCDEBUG
      p--;
    }
    else { /* type==BUFF_LINK_MARK*/
      mokAssert( (LOWBUFFMASK & (uint)p) == N_RESERVED_SLOTS*sizeof(uint));
      if (!ptr) {
        mokAssert( buff+N_RESERVED_SLOTS == p);
        return;
      }
      mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
      p = ptr-1; /* skip forward pointer */
    }
  }
}

static void _updateRCofUpdateLog( void )
{
  uint *log = gcvar.updateBuffList;
  while (log) {
    uint *nextLog = (uint*)log[0];
    _updateRCofSingleUpdateLog( log );
    log = nextLog;
  }
  gcvar.updateBuffList = NULL;
}

static void _updateRCofCreateLog( void )
{
  uint *log = gcvar.createBuffList;
  while (log) {
    _updateRCofSingleCreateLog( log );
    log = (uint*)log[0];
  }
}


static void _Update_Reference_Counters( void )
{
#ifdef RCDEBUG
  uint start, end;

  mokAssert( gcvar.zctBuff.start[LOG_OBJECTS_IDX] == gcvar.dbg.nInZct );

  start = GetTickCount();

  dbgprn( 0, "__Update_Reference_Counters(begin) time=%d\n", start);
#endif // RCDEBUG

  _updateRCofUpdateLog();
  _updateRCofCreateLog();

#ifdef RCDEBUG
  end = GetTickCount();

  dbgprn( 3, "\tnUpdateRCObjects=%d\n",  gcvar.dbg.nUpdateRCObjects );
  dbgprn( 3, "\tnUpdateRCChilds=%d\n",  gcvar.dbg.nUpdateRCChilds );
  dbgprn( 3, "\tnUpdateRCDuplicates=%d\n",  gcvar.dbg.nUpdateRCDuplicates );
  dbgprn( 3, "\tnCreateRCObjects=%d\n",  gcvar.dbg.nCreateRCObjects );
  dbgprn( 2, "\tnDetermined=%d\n", gcvar.dbg.nDetermined );
  dbgprn( 2, "\tnUndetermined=%d\n", gcvar.dbg.nUndetermined );
  dbgprn( 2, "\tnInZct=%d\n", gcvar.dbg.nInZct );

  mokAssert( gcvar.dbg.nDetermined+gcvar.dbg.nUndetermined == 
             gcvar.dbg.nUpdateObjects + gcvar.dbg.nCreateObjects -
             (gcvar.dbg.nUpdateDuplicates + gcvar.dbg.nActualCyclesBroken) );
  mokAssert( gcvar.dbg.nUpdateRCObjects == gcvar.dbg.nUpdateObjects);
  mokAssert( gcvar.dbg.nUpdateRCChilds == gcvar.dbg.nUpdateChilds);
  mokAssert( gcvar.dbg.nUpdateRCDuplicates == 
             gcvar.dbg.nUpdateDuplicates +gcvar.dbg.nActualCyclesBroken);
  mokAssert( gcvar.dbg.nCreateRCObjects == gcvar.dbg.nCreateObjects );
  mokAssert( gcvar.zctBuff.start[LOG_OBJECTS_IDX] == gcvar.dbg.nInZct );

  dbgprn( 0, "_Update_Reference_Counters(end) time=%d delta=%d\n", end, end-start );
#endif // RCDEBUG
}

/**********************  Reclamation ************************************/

static void _throwNonZerosFromCurrentZCT( BUFFHDR *tmpZCT )
{
  uint *ptr, type, *p, *buff;
  
#ifdef RCDEBUG
  uint nOld = 0, nDel = 0, nThrown =0, nPend=0;
  uint start, end;

  start = GetTickCount();
  dbgprn( 0, "_throwNonZerosFromCurrentZCT(start) time=%d\n", start );
#endif 

  buff = gcvar.zctBuff.start;
  
  mokAssert( (((uint)buff) & LOWBUFFMASK) == 0);
  mokAssert( buff );
  
  p = gcvar.zctBuff.pos-1;
  
  mokAssert( p );
  mokAssert( *p );
  
  for (;;) {
    ptr = (uint*)(*p & ~3);
    type = *p & 3;
    mokAssert( type != BUFF_HANDLE_MARK );
    mokAssert( type != BUFF_DUP_HANDLE_MARK );
    
    if (type==0) {
      GCHandle *h = (GCHandle*)ptr;
#ifdef RCDEBUG
      nOld++;
#endif 
      mokAssert( h );
      mokAssert( gcNonNullValidHandle(h) );
      mokAssert( _isInZCT(h) );
      if (gcGetHandleRC(h) > 0) {
        _markNotInZCT(h);
#ifdef RCDEBUG
        nThrown++;
#endif // RCDEBUG
      }
      else {
#ifdef RCDEBUG
        nDel++;
#endif 
        gcBuffLogWord( gcvar.ee, tmpZCT, (unsigned)h );
      }
      p--;
    }
    else { /*type==BUFF_LINK_MARK*/
      mokAssert( (LOWBUFFMASK & (uint)p) == N_RESERVED_SLOTS*sizeof(uint));
      /* free the more recent buffer */
      _freeBuff( gcvar.ee, p - N_RESERVED_SLOTS);
      if (!ptr) {
        mokAssert( buff+N_RESERVED_SLOTS == p);
        goto __end;
      }
      mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
      p = ptr-1; /* skip forward pointer */
    }
  }
 __end:;
#ifdef RCDEBUG
  end = GetTickCount();

  mokAssert( gcvar.tmpZctBuff.start[LOG_OBJECTS_IDX] == nDel );
  mokAssert( nThrown+nDel+nPend == nOld );

  gcvar.dbg.nInZct = gcvar.tmpZctBuff.start[LOG_OBJECTS_IDX];
  gcvar.dbgpersist.nPendInCycle= nPend;
  
  dbgprn( 2, "\tnOld=%d\n",  nOld  );
  dbgprn( 2, "\tnDel=%d\n",  nDel  );
  dbgprn( 2, "\tnPend=%d\n", nPend );
  dbgprn( 2, "\tnThrown=%d\n", nThrown );
  dbgprn( 2, "\tnInZct=%d\n", gcvar.dbg.nInZct );
  dbgprn( 2, "\tnInNextZct=%d\n", gcvar.dbgpersist.nPendInCycle );
  dbgprn( 2, "_throwNonZerosFromCurrentZCT(end) time=%d delta=%d\n", end, end-start );
#endif 
}


static void _processCreateBuffsIntoZCT( void )
{
#ifdef RCDEBUG
  uint nCreate = 0, nDel = 0, nThrown=0, nPend=0;
  uint nAlreadyInZct=0;
  uint start, end;
#endif 

  uint *ptr, type, *p;
  uint *buff = gcvar.createBuffList, *nextBuff;
  BUFFHDR *tmpZCT = &gcvar.tmpZctBuff;


#ifdef RCDEBUG
  start = GetTickCount();
  dbgprn( 0, "_processCreateBuffIntoZCT(start)  time=%d\n", start );
#endif

  while (buff) {
    nextBuff = (uint*)buff[0];

    mokAssert( (((uint)buff) & LOWBUFFMASK) == 0);
    mokAssert( buff );

    p = (uint*)buff[LAST_POS_IDX];
    mokAssert( p );
    mokAssert( *p == 0 );
    p--;
    mokAssert( *p );

    for        (;;) {
      ptr = (uint*)(*p & ~3);
      type = *p & 3;
      mokAssert( type != BUFF_HANDLE_MARK );
      mokAssert( type != BUFF_DUP_HANDLE_MARK );
      if (type==0) {
        GCHandle *h = (GCHandle*)ptr;
#ifdef RCDEBUG
        nCreate++;
#endif 
        mokAssert( h );
        mokAssert( gcNonNullValidHandle(h) );
        if (gcGetHandleRC(h) == 0) {
          if (!_isInZCT(h)) {
            _markInZCT( h );
#ifdef RCDEBUG
            nDel++;
#endif 
            gcBuffLogWord( gcvar.ee, tmpZCT, (unsigned)h );
          }
#ifdef RCDEBUG
          else {
            nAlreadyInZct++;
          }
#endif // RCDEBUG
        }
#ifdef RCDEBUG
        else {
          nThrown++;
        }
#endif // RCDEBUG
        p--;
      }
      else { /* type==BUFF_LINK_MARK*/
        mokAssert( (LOWBUFFMASK & (uint)p) == N_RESERVED_SLOTS*sizeof(uint));
                                /* free the more recent buffer */
        _freeBuff( gcvar.ee, p - N_RESERVED_SLOTS);
        if (!ptr) {
          mokAssert( buff+N_RESERVED_SLOTS == p);
          goto __end_chunk;
        }
        mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
        p = ptr-1; /* skip forward pointer */
      }
    }
  __end_chunk:
    buff = nextBuff;
  }
  gcvar.createBuffList = NULL;

#ifdef RCDEBUG
  end = GetTickCount();

  mokAssert( gcvar.tmpZctBuff.start[LOG_OBJECTS_IDX] == nDel + gcvar.dbg.nInZct );
  gcvar.dbg.nInZct = gcvar.tmpZctBuff.start[LOG_OBJECTS_IDX] ;

  gcvar.dbgpersist.nPendInCycle += nPend;

  mokAssert( gcvar.dbg.nCreateObjects == nCreate );
  mokAssert( nThrown+nDel+nPend+nAlreadyInZct == nCreate );

  gcvar.dbg.nCreateDel = nDel;

  dbgprn( 2, "\tnCreate=%d\n", nCreate  );
  dbgprn( 2, "\tnDel=%d\n",  nDel  );
  dbgprn( 2, "\tnPend=%d\n", nPend );
  dbgprn( 2, "\tnThrown=%d\n", nThrown );
  dbgprn( 2, "\tnInZct=%d\n", gcvar.dbg.nInZct );
  dbgprn( 2, "\tnInNextZct=%d\n",  gcvar.dbgpersist.nPendInCycle );
  dbgprn( 0, "_processCreateBuffIntoZCT(end) time=%d delta=%d\n", start, end-start );
#endif 
}

#pragma optimize( "", off )
static void _freeHandle(GCHandle* h)
{
  for (;;) {
    unsigned *p;
    BlkAllocBigHdr *bh;
    int status;

    mokAssert( h );
    mokAssert( gcNonNullValidHandle(h) );
    mokAssert( gcGetHandleRC(h)==0 );
    
#ifdef RCDEBUG
    {
      unsigned obj_type = obj_flags(h);
      if (obj_type == T_NORMAL_OBJECT) {
        register ClassClass *cb = obj_classblock(h);
        gcvar.dbg.nRefsFreedInCycle += unhand(cb)->n_object_offsets;
      }
      else if (obj_type == T_CLASS) { /* an array of references */
        long n = obj_length(h);
        gcvar.dbg.nRefsFreedInCycle += n;
      }
    }
#endif // RCDEBUG

    p = h->logPos;
    if (p) {
#ifdef RCDEBUG
      dbgprn( 1, "\t\tfree:dirty: %x\n", h);
      mokAssert( h == (GCHandle*)(*p^BUFF_HANDLE_MARK) );
      h->logPos = NULL;
      gcvar.dbgpersist.nFreeCyclesBroken++;
#endif 
      *p = *p | BUFF_DUP_HANDLE_MARK;
      p--;
      while (1) {
        GCHandle *child = (GCHandle*)*p;
        uint type = 3 & *p;
        mokAssert( child );
        if (type) break;
#ifdef RCDEBUG
        dbgprn( 3, "\t\tfree:dirty:dec %x\n", child);
#endif
        _decrementHandleRCInDeletion( child );
        p--;
      }
    }
    else {
      register GCHandle  *child;
      register char      *objslots;
      unsigned obj_type = obj_flags(h);

      if (obj_type == T_NORMAL_OBJECT) {
        register ClassClass *cb = obj_classblock(h);
        unsigned short *object_offsets;
        int offset;
        
        mokAssert( cb != classJavaLangClass);
        
        object_offsets = cbObjectOffsets(cb);
        if (object_offsets) {
          objslots = ((char *)gcUnhand(h)) - 1;
          while ((offset = *object_offsets++)) {
            child =  *((GCHandle **) (((char *)objslots) + offset));
            if (child) {
              mokAssert( gcNonNullValidHandle(child) );
              _decrementHandleRCInDeletion( child );
            }
          }
        }
      }
      else if (obj_type == T_CLASS) { /* an array of references */
        register long n = obj_length(h);
        GCHandle **body;

        body = (GCHandle**)(((ArrayOfObject *)gcUnhand(h))->body);
        while (--n >= 0) {
          child = body[n];
          if (child) {
            _decrementHandleRCInDeletion( child );
          }
        }
      }
    }
#ifdef RCDEBUG
    gcvar.dbg.nFreedInCycle++;
    h->status = Im_free;
#endif
    bh = (BlkAllocBigHdr *)OBJBLOCKHDR(h);
    status = bhGet_status( bh );

    mokAssert( status==ALLOCBIG || 
               status==VOIDBLK || 
               status==PARTIAL || 
               status==OWNED );
    mokAssert( ALLOCBIG < OWNED );
    mokAssert( OWNED < VOIDBLK );
    mokAssert( VOIDBLK < PARTIAL );

    if (status == ALLOCBIG) {
#ifdef RCDEBUG
      gcvar.dbg.nBytesFreedInCycle +=  
        ((BlkAllocBigHdr *)OBJBLOCKHDR(h))->blobSize * BLOCKSIZE;
#endif
      blkFreeRegion( (BlkAllocBigHdr *)OBJBLOCKHDR(h) );
    }
    else {
#ifdef RCDEBUG
      gcvar.dbg.nBytesFreedInCycle += 
        chkconv.binSize[ bhGet_bin_idx( (BlkAllocHdr*)bh ) ];
#endif
      chkPreCollect( (BLKOBJ*)h );
    }
    if (gcvar.zctStackSp == gcvar.zctStack)
      return;
    gcvar.zctStackSp--;
    h = *gcvar.zctStackSp;
  }
}
#pragma optimize( "", on )

static void _freeHandlesOnTempZCT(BUFFHDR *tmpZCT)
{
  uint *buff = tmpZCT->start;
  uint *ptr, type, *p;
  
#ifdef RCDEBUG
  uint start, end;
  uint nInZCT = 0;

  start = GetTickCount();
  dbgprn( 0, "_freeHandlesOnTempZCT(start) time=%d\n", start );
#endif // RCDEBUG
  

  mokAssert( (((uint)buff) & LOWBUFFMASK) == 0);
  mokAssert( buff );
  
  p = tmpZCT->pos - 1;
  mokAssert( p );
  mokAssert( *p );

  for (;;) {
    ptr = (uint*)(*p & ~3);
    type = *p & 3;
    mokAssert( type != BUFF_DUP_HANDLE_MARK );
    mokAssert( type != BUFF_HANDLE_MARK );
    
    if (type==0) {
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( h );
      mokAssert( _isInZCT(h) );
      mokAssert( gcNonNullValidHandle(h) );
      mokAssert( gcGetHandleRC(h)==0 );
      _freeHandle( h );
      _markNotInZCT(h);
#ifdef RCDEBUG
      nInZCT++;
#endif // RCDEBUG
      p--;
    }
    else { /* type==BUFF_LINK_MARK*/
      mokAssert( (LOWBUFFMASK & (uint)p) == N_RESERVED_SLOTS*sizeof(uint));
      /* free the more recent buffer */
      _freeBuff( gcvar.ee, p - N_RESERVED_SLOTS);
      if (!ptr) {
        mokAssert( buff+N_RESERVED_SLOTS == p);
#ifdef RCDEBUG
        mokAssert( nInZCT == gcvar.tmpZctBuff.start[LOG_OBJECTS_IDX] );
#endif // RCDEBUG
        goto __end;
      }
      mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
      p = ptr-1; /* skip forward pointer */
    }
  }
 __end:;
#ifdef RCDEBUG
  end = GetTickCount();
  dbgprn( 2, "\tnFreedInCycle=%d\n", gcvar.dbg.nFreedInCycle );
  dbgprn( 2, "\tnRecursiveDel=%d\n", gcvar.dbg.nRecursiveDel );
  dbgprn( 2, "\tnRecursivePend=%d\n", gcvar.dbg.nRecursivePend );
  dbgprn( 0, "_freeHandlesOnTempZCT(start) delta=%d\n", end-start );
#endif
}

static void _processLocalsIntoNextZCT( void)
{
  uint *buff = gcvar.uniqueLocalsBuff.start;
  uint *ptr, type, *p;

#ifdef RCDEBUG
  uint start, end;
  start = GetTickCount();
  dbgprn( 0, "_processLocalsIntoNextZCT(start) time=%d\n", start );
#endif // RCDEBUG
  

  mokAssert( (((uint)buff) & LOWBUFFMASK) == 0);
  mokAssert( buff );

  /* allocate buffer for next ZCT */
  buffInit( gcvar.ee, &gcvar.nextZctBuff );

  p = gcvar.uniqueLocalsBuff.pos - 1;
  mokAssert( p );
  mokAssert( *p );

  for (;;) {
    ptr = (uint*)(*p & ~3);
    type = *p & 3;
    mokAssert( type != BUFF_DUP_HANDLE_MARK );
    mokAssert( type != BUFF_HANDLE_MARK );

    if (type==0) {
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( h );
      mokAssert( _isHandle( h ) );
      mokAssert( !_isInZCT(h) );

      _unsetLocal(h);
      _decrementLocalHandleRC( h );

      p--;
    }
    else { /* type==BUFF_LINK_MARK*/
      mokAssert( (LOWBUFFMASK & (uint)p) == N_RESERVED_SLOTS*sizeof(uint));
      /* free the more recent buffer */
      _freeBuff( gcvar.ee, p - N_RESERVED_SLOTS);
      if (!ptr) {
        mokAssert( buff+N_RESERVED_SLOTS == p);
#ifdef RCDEBUG
        gcvar.uniqueLocalsBuff.pos = NULL;
#endif
        goto checkout;
      }
      mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
      p = ptr-1; /* skip forward pointer */
    }
  }
 checkout:;
#ifdef RCDEBUG
  end = GetTickCount();
  dbgprn( 2, "\tnPendInCycle=%d\n", gcvar.dbgpersist.nPendInCycle );
  dbgprn( 0, "_processLocalsIntoNextZCT(start) delta=%d\n", end-start );
#endif
}

static void _Reclaim_Garbage(void)
{
  buffInit( gcvar.ee, &gcvar.tmpZctBuff );

  _throwNonZerosFromCurrentZCT( &gcvar.tmpZctBuff );

  _processCreateBuffsIntoZCT( );

  _freeHandlesOnTempZCT( &gcvar.tmpZctBuff );

  chkFlushRecycledListsCache( );
}


/************************************************************
************* Tracing Cycle Stuff ***************************
************************************************************/

static void _freeListOfBuffers( uint* buff )
{
  while (buff) {
    uint *next;
    next = (uint*)buff[NEXT_BUFF_IDX];
    _freeBuff( gcvar.ee, buff );
    buff = next;
  }
}


static void _freeListOfListsOfBuffers( uint *buff)
{
  while (buff) {
    uint *next;
    next = (uint*)buff[LINKED_LIST_IDX];
    _freeListOfBuffers( buff );
    buff = next;
  }
}


static void _traceSetup( void )
{
  _freeListOfListsOfBuffers( gcvar.createBuffList );
  gcvar.createBuffList = NULL;

  _freeListOfListsOfBuffers( gcvar.updateBuffList );
  gcvar.updateBuffList = NULL;

  *gcvar.zctBuff.pos = 0;
  gcvar.zctBuff.start[ LAST_POS_IDX ] = (int)gcvar.zctBuff.pos;
  _freeListOfBuffers( gcvar.zctBuff.start );

  /* Decommit the "zct" bmp */
  mokMemDecommit( gcvar.zctBmp.bmp, gcvar.zctBmp.bmp_size );

  /*  Clear the "rc" bmp */
  mokMemDecommit( gcvar.rcBmp.bmp, gcvar.rcBmp.bmp_size );
  mokMemCommit( gcvar.rcBmp.bmp, gcvar.rcBmp.bmp_size, true );
}

static void _scanHandle(GCHandle *h)
{
  int prevRC = _incrementHandleRCWithReturnValue( h );
  if (prevRC == 0)
    _putInMarkStack( h );
}


static void _markHandleSons(GCHandle *h)
{
  uint *p;
  
 start:
  p = h->logPos;
  
#ifdef RCDEBUG
  gcvar.dbg.nTracedInCycle++;
#endif // RCDEBUG
  if (p) {
#ifdef RCDEBUG
    gcvar.dbg.nUndetermined++;
#endif // RCDEBUG
    if ( ((*p) & 3) == 0) { /* newly created object */
      /* 
       * must be called directly from _traceFromLocals
       */
      mokAssert( _isLocal(h) );
      return;
    }
    mokAssert( h == (GCHandle*)(*p^BUFF_HANDLE_MARK) );
    p--;
    while (1) {
      GCHandle *hSon = (GCHandle*)*p;
      uint type = 3 & *p;
      mokAssert( hSon );
      if (type) return;
      _scanHandle( hSon );
      p--;
    }
  }
  
  {
    GCHandle **tempbuff = gcvar.tempReplicaSpace;
    register GCHandle *child;
    register GCHandle **objslots;

    switch (obj_flags(h)) {
    case T_NORMAL_OBJECT:{
      register ClassClass *cb = obj_classblock(h);
      register unsigned short  offset;
      register unsigned short *object_offsets ;

      if (cb == classJavaLangClass || unhand(cb)->n_object_offsets==0) {
#ifdef RCDEBUG
        gcvar.dbg.nDetermined++;
#endif
        return;
      }
        
      object_offsets = cbObjectOffsets(cb);
      objslots = (GCHandle **)(((char *)unhand(h)) - 1);
      while ((offset = *object_offsets++)) {
        child = *(GCHandle **) ((char *) objslots + offset);
        if (child) {
          tempbuff++;
          *tempbuff = child;
        }
      }
      break;
    }

    case T_CLASS: {     /* an array of classes */
      register long n = obj_length(h);
      GCHandle **body = (GCHandle**)(((ArrayOfObject*)gcUnhand(h))->body);
      while (--n >= 0) {
        child  = body[n];
        if (child) {
          tempbuff++;
          *tempbuff = child;
        }
      }
      break;
    }
    }
    if (h->logPos) {
      goto start;
    }
    /* OK, the replica we have at this point is valid
     * so use it as the reference to the objects'
     * contents.
     */
#ifdef RCDEBUG
    gcvar.dbg.nDetermined++;
#endif // RCDEBUG
    while( tempbuff > gcvar.tempReplicaSpace) {
      child = *tempbuff;
      _scanHandle( child );
      tempbuff--;
    }
  }
}


static void _emptyMarkStack( void )
{
  for (;;) {
    GCHandle *h;

    if (gcvar.zctStackSp == gcvar.zctStack)
      return;
    gcvar.zctStackSp--;
    h = *gcvar.zctStackSp;
        
#ifdef RCDEBUG
    mokAssert( _isHandle(h) );
    mokAssert( gcGetHandleRC(h) > 0);
    {
      /*
       * Check that if we see an object nested in 
       * another one then this object cannot be
       * a one created since the beginning of the
       * cycle.
       */
      uint *p = h->logPos;
      if (p) {
        mokAssert( h == (GCHandle*)(*p^BUFF_HANDLE_MARK) );
      }
    }
#endif
    _markHandleSons( h );
  }
}
        

static void _traceFromLocals( void)
{
  uint *buff = gcvar.uniqueLocalsBuff.start;
  uint *ptr, type, *p;

  mokAssert( (((uint)buff) & LOWBUFFMASK) == 0);
  mokAssert( buff );

  p = gcvar.uniqueLocalsBuff.pos - 1;
  mokAssert( p );
  mokAssert( *p );

  for (;;) {
    ptr = (uint*)(*p & ~3);
    type = *p & 3;
    mokAssert( type != BUFF_DUP_HANDLE_MARK );
    mokAssert( type != BUFF_HANDLE_MARK );

    if (type==0) {
      GCHandle *h = (GCHandle*)ptr;
      mokAssert( _isHandle(h) );
#ifdef RCDEBUG
      {
        int rc = gcGetHandleRC( h );
        mokAssert( rc >= 1 );
      }
#endif 
      _markHandleSons( h );
      _emptyMarkStack();
      p--;
    }
    else { /* type==BUFF_LINK_MARK*/
      mokAssert( (LOWBUFFMASK & (uint)p) == N_RESERVED_SLOTS*sizeof(uint));
      if (!ptr) {
        mokAssert( buff+N_RESERVED_SLOTS == p);
        return;
      }
      mokAssert( *ptr == BUFF_LINK_MARK|(uint)p );
      p = ptr-1; /* skip forward pointer */
    }
  }
}


static void _Trace( void )
{
#ifdef RCDEBUG
  uint start, end;
  start = GetTickCount();
  dbgprn( 0, "_Trace(start) time=%d\n", start );
#endif

  _traceFromLocals();

#ifdef RCDEBUG
  end = GetTickCount();
  dbgprn( 2, "\tnTracedInCycle=%d\n", gcvar.dbg.nTracedInCycle );
  dbgprn( 0, "_Trace(end) delta=%d\n", end-start );
#endif
}



static void _Sweep( void )
{
#ifdef RCDEBUG
  uint start, end;
  start = GetTickCount();
  dbgprn( 0, "_Sweep(start) time=%d\n", start );
#endif

  blkSweep();

#ifdef RCDEBUG
  end = GetTickCount();
  dbgprn( 2, "\tnFreedInCycle=%d\n", gcvar.dbg.nFreedInCycle );
  dbgprn( 0, "_Sweep(end) delta=%d\n", end-start );
#endif
}

/****************** GC Driver Func ***************/
#if 0
static int _ResumeHelper( sys_thread_t *thrd, bool *allOK )
{
  ExecEnv *ee;

  mokAssert( gcvar.sys_thread != thrd );
  ee = SysThread2EE( thrd );
  if (ee->gcblk.gcSuspended)
    mokThreadResumeForGC( thrd );
  return SYS_OK;
}
#endif /* 0 */

#ifdef RCDEBUG
static void _printStats(void)
{
  float avg, avgs;
  dbgprn( 1, " __________  THIS CYCLE STATS _______________:\n");
  dbgprn( 1, "STORE: new=%d old=%d\n", 
          gcvar.dbg.nNewObjectUpdatesInCycle,
          gcvar.dbg.nOldObjectUpdatesInCycle );
  dbgprn( 1, "UPDATE: updated=%d logged-slots=%d\n", 
          gcvar.dbg.nUpdateObjects, gcvar.dbg.nUpdateChilds );
  if (gcvar.dbg.nCreateObjects) {
    avg = (float)gcvar.dbg.nBytesAllocatedInCycle/gcvar.dbg.nCreateObjects;
    avgs = (float)gcvar.dbg.nRefsAllocatedInCycle/gcvar.dbg.nCreateObjects;
  }
  else {
    avg =-1;
    avgs = -1;
  }

  dbgprn( 1, "CREATE: objects=%d bytes=%d avg=%f refs=%d avg=%f\n", 
          gcvar.dbg.nCreateObjects, gcvar.dbg.nBytesAllocatedInCycle, avg,
          gcvar.dbg.nRefsAllocatedInCycle, avgs);
  dbgprn( 1, 
          "RECLAIM: objects=%d   bytes=%d\n", 
          gcvar.dbg.nFreedInCycle, 
          gcvar.dbg.nBytesFreedInCycle );
  dbgprn( 1, "STUCK: %d\n", gcvar.dbg.nStuckCountersInCycle );


  gcvar.dbgpersist.nLoggedUpdates += gcvar.dbg.nUpdateObjects;
  gcvar.dbgpersist.nLoggedSlots += gcvar.dbg.nUpdateChilds;

  gcvar.dbgpersist.nObjectsAllocated += gcvar.dbg.nCreateObjects;
  gcvar.dbgpersist.nBytesAllocated += gcvar.dbg.nBytesAllocatedInCycle;
  gcvar.dbgpersist.nRefsAllocated += gcvar.dbg.nRefsAllocatedInCycle;
  gcvar.dbgpersist.nObjectsFreed +=  gcvar.dbg.nFreedInCycle;
  gcvar.dbgpersist.nBytesFreed += gcvar.dbg.nBytesFreedInCycle;
  gcvar.dbgpersist.nRefsFreed += gcvar.dbg.nRefsFreedInCycle;
  gcvar.dbgpersist.nNewObjectUpdates += gcvar.dbg.nNewObjectUpdatesInCycle;
  gcvar.dbgpersist.nOldObjectUpdates += gcvar.dbg.nOldObjectUpdatesInCycle;
  gcvar.dbgpersist.nStuckCounters += gcvar.dbg.nStuckCountersInCycle;



  dbgprn( 1, " __________  ACCUMULATING STATS _______________:\n");
  dbgprn( 1, "STORE: new=%d old=%d\n", 
          gcvar.dbgpersist.nNewObjectUpdates,
          gcvar.dbgpersist.nOldObjectUpdates );
  dbgprn( 1, "UPDATE: updated=%d logged-slots=%d\n", 
          gcvar.dbgpersist.nLoggedUpdates, gcvar.dbgpersist.nLoggedSlots );
  if (gcvar.dbgpersist.nObjectsAllocated) {
    avg = (float)gcvar.dbgpersist.nBytesAllocated / gcvar.dbgpersist.nObjectsAllocated;
    avgs = (float)gcvar.dbgpersist.nRefsAllocated / gcvar.dbgpersist.nObjectsAllocated;
  }
  else {
    avg = -1;
    avgs = -1;
  }
  dbgprn( 1, "CREATE: objects=%d bytes=%d avg=%f refs=%d avg=%f\n", 
          gcvar.dbgpersist.nObjectsAllocated, 
          gcvar.dbgpersist.nBytesAllocated,
          avg,
          gcvar.dbgpersist.nRefsAllocated,
          avgs );
  dbgprn( 
         1, 
         "RECLAIM: objects=%d bytes=%d\n", 
         gcvar.dbgpersist.nObjectsFreed, 
         gcvar.dbgpersist.nBytesFreed );
  dbgprn( 1, "STUCK: %d\n", gcvar.dbgpersist.nStuckCounters );
  {
    int nAllocated =  gcvar.dbgpersist.nBytesAllocated - gcvar.dbgpersist.nBytesFreed;
    int nFree = blkvar.heapSz - nAllocated;
    dbgprn( 1, "USAGE:   free=%10d   used= %10d\n", nFree, nAllocated );
  }
  blkPrintStats();
  dbgprn( 1, "PARTIAL: %d\n", chkCountPartialBlocks() );
}
#endif /* RCDEBUG */

GCFUNC void gcCheckGC(void)
{
  int nFreeBlocks = FREE_BLOCKS();
  if (nFreeBlocks < gcvar.gcTrigHigh)
    gcRequestAsyncGC();
}

static int _recommendCollectionMethod(void)
{
  int    nSamples, i, t, m;
  float  norm, avg[2], prob[2], r;
  
  if (gcvar.opt.recommendOnlyRCGC)
    return GCT_RCING;

  for (t=0; t<2; t++) {
    nSamples = 0;
    avg[t] = 0;
    for (i=0; i<N_SAMPLES; i++) {
      if (gcvar.runHist[t][i]) {
        avg[t] += gcvar.runHist[t][i];
        nSamples++;
      }
      else break;
    }
    avg[t] = nSamples ? avg[t]/nSamples : 0;
  }
  
  printf( "***  _recommendCollectionMethod trace=%f rc=%f\n",
          avg[GCT_TRACING], avg[GCT_RCING] );

  if (avg[GCT_TRACING] < 0.001) return GCT_TRACING;
  if (avg[GCT_RCING] < 0.001) return GCT_RCING;

  /* 
   * Normalize so that prob ~ 1/avg
   * and prob[0]+prob[1] == 1
   */
  norm = (avg[0] * avg[1]) / ( avg[0] + avg[1] );
  prob[0] = norm / avg[0];
  prob[1] = norm / avg[1];

  printf( "p[0]=%f p[1]=%f sum=%f\n", prob[0], prob[1], prob[0]+prob[1] );
  r = (float)rand() / (float)RAND_MAX;

  if (r < prob[0]) m = 0;
  else m = 1;

  printf("r=%f --> m=%d\n", r , m );
  return m;
}

static void _updateRunHist(int runTime)
{
  int i;
  int t = gcvar.collectionType;

  for (i=N_SAMPLES-2; i>=0; i--)
    gcvar.runHist[t][i+1] =  gcvar.runHist[t][i];

  gcvar.runHist[t][0] = runTime;
}

static void _gc(void)
{
  uint  delta, end, start;
  int   nWasFree;

  start = GetTickCount();
  gcvar.gcActive = true;
  gcvar.collectionType = gcvar.nextCollectionType;
  gcvar.nextCollectionType = GCT_RCING;
  if (gcvar.usrSyncGC) {
    gcvar.collectionType = GCT_TRACING;
    gcvar.usrSyncGC = false;
  }
  if (gcvar.memStress) {
    gcvar.memStress = false;
    gcvar.collectionType = GCT_TRACING;
  }
  if (gcvar.opt.useOnlyTracingGC)
    gcvar.collectionType = GCT_TRACING;
  if (gcvar.opt.useOnlyRCGC)
    gcvar.collectionType = GCT_RCING;
    

  nWasFree = FREE_BLOCKS();

#ifdef RCVERBOSE
  jio_printf("----------------- start gc(%d--%s)  time=%d  -----\n", 
             gcvar.iCollection,  
             gcvar.collectionType == GCT_TRACING ? "TRACING" : "RC",
             start );
  fflush( stdout );
#endif
  _Initiate_Collection_Cycle();
  _Clear_Dirty_Marks();
  _Reinforce_Clearing_Conflict_Set();
  _Consolidate();
  if (gcvar.collectionType == GCT_RCING) {
    _Update_Reference_Counters( );
    _Reclaim_Garbage( );
  }
  else {
    _Trace();
    _Sweep();
    /* re-commit the "zct" bmp */
    mokMemCommit( gcvar.zctBmp.bmp, gcvar.zctBmp.bmp_size, true );
  }

  _processLocalsIntoNextZCT();
  gcvar.zctBuff = gcvar.nextZctBuff;
  gcvar.nextZctBuff.pos = NULL;

  end = GetTickCount();
  delta = end - start;

  _updateRunHist( delta );

#ifdef RCDEBUG
  if (gcvar.collectionType == GCT_RCING) {
    gcvar.dbgpersist.nPendInCycle = gcvar.nextZctBuff.start[LOG_OBJECTS_IDX];
    mokAssert( gcvar.dbg.nFreedInCycle == gcvar.dbg.nInZct + gcvar.dbg.nRecursiveDel );
  }
#endif //RCDEBUG

  /*
   * OK, now see where we stand and set the strategy for the
   * next cycle.
   */
  {
    int nNowFree, nLowMark;
    int prevTrig;
    bool  failed, gotIntoSync;

    nNowFree = FREE_BLOCKS();
    nLowMark = gcvar.gcTrigHigh + (gcvar.opt.lowTrigDelta * blkvar.nBlocks)/100;
    
    failed = nNowFree < nLowMark;
    gotIntoSync = gcvar.memStress;

    jio_printf("**** high=%d low=%d free=%d was=%d failed=%d sync=%d\n",
               gcvar.gcTrigHigh,
               nLowMark,
               nNowFree,
               nWasFree,
               failed,
               gotIntoSync
               );
    fflush( stdout );

    prevTrig = gcvar.gcTrigHigh;

    if (gcvar.collectionType == GCT_TRACING) {
      if (gotIntoSync && failed) {
        gcvar.nextCollectionType =  GCT_TRACING;
        gcvar.gcTrigHigh -= (gcvar.opt.raiseTrigInc * blkvar.nBlocks)/100;
      }
      else if (gotIntoSync && !failed) {
        gcvar.nextCollectionType =  GCT_TRACING;
        gcvar.gcTrigHigh += (gcvar.opt.lowerTrigDec * blkvar.nBlocks)/100;
      }
      else if (!gotIntoSync && failed) {
        gcvar.nextCollectionType =  GCT_TRACING;
        gcvar.gcTrigHigh -= (gcvar.opt.raiseTrigInc * blkvar.nBlocks)/100;
      }
      else /* (!gotIntoSync && !failed) */ {
        gcvar.nextCollectionType =  _recommendCollectionMethod();
      }
    }
    else /*(gcvar.collectionType == GCT_RCING)*/ {
      if (gotIntoSync && failed) {
        gcvar.nextCollectionType =  GCT_TRACING;
      }
      else if (gotIntoSync && !failed) {
        gcvar.nextCollectionType =  GCT_TRACING;
      }
      else if (!gotIntoSync && failed) {
        gcvar.nextCollectionType =  GCT_TRACING;
      }
      else /* (!gotIntoSync && !failed) */ {
        gcvar.nextCollectionType =  _recommendCollectionMethod();
      }
    }
      
    jio_printf("**** prevTrig=%d currTrig=%d curCycle=%s nextCycle=%s\n",
               prevTrig,
               gcvar.gcTrigHigh,
               gcvar.collectionType == GCT_RCING ? "RC" : "TRACING",
               gcvar.nextCollectionType == GCT_RCING ? "RC" : "TRACING"
               );
    fflush( stdout );
  }


#ifdef RCDEBUG
  _printStats();
#endif


  gcvar.gcActive = false;

#ifdef RCVERBOSE
  jio_printf( 
             "----------------- end gc(%d)  delta=%d ---------\n", 
             gcvar.iCollection, 
             end-start );
  fflush( stdout );
#endif
}

HANDLE hGCEvent, hMutEvent;

void gcThreadFunc(void *param)
{
  gcvar.ee = EE();
  gcvar.sys_thread = EE2SysThread ( gcvar.ee );

#ifdef RCDEBUG
  dbgprn( 
         0, 
         "GC Thread starting ... ee=%x sys_thread=%x\n", 
         gcvar.ee, 
         gcvar.sys_thread );
#endif
  gcvar.initialized = true;

  for(;;) {
    PulseEvent( hMutEvent );
#ifdef RCDEBUG
    dbgprn( 0, " *************** GC -- sleeping (%d)\n", gcvar.iCollection );
#endif
    WaitForSingleObject( hGCEvent, INFINITE );
#ifdef RCDEBUG
    jio_printf( " *************** GC -- wokeup (%d)\n", gcvar.iCollection );
    fflush( stdout );
#endif
    gcvar.nChunksAllocatedRecentlyByUser = 0;
    _gc();
#ifdef RCDEBUG
    dbgprn( 0, " *************** GC -- done (%d)\n", gcvar.iCollection );
#endif
    gcvar.iCollection++;
  }
}

/*************************************************/
/**************** USER REQUESTS ******************/
/*************************************************/

GCEXPORT void gcRequestSyncGC(void)
{
  sys_thread_t *self = sysThreadSelf();
  int wasPhase = gcvar.iCollection;
  int waitT = 100;

#ifdef RCVERBOSE
  jio_printf("SYNC GC thread=%x (iCollection=%d) stress=%d\n", 
             self, 
             wasPhase,
             gcvar.memStress);
  fflush( stdout );
#endif
  gcvar.usrSyncGC = true;
  SetEvent( hGCEvent );
  while (wasPhase == gcvar.iCollection) {
    WaitForSingleObject( hMutEvent, waitT );
    waitT *= 2;
#ifdef RCDEBUG
    dbgprn( 0, 
            "SYNC GC thread=%x GOT GC LOCK (iCollect=%d)\n", 
            self,  
            gcvar.iCollection );
#endif
  }
#ifdef RCDEBUG
  dbgprn( 0, "SYNC GC thread=%x DONE (iCollect=%d)\n", self,  gcvar.iCollection );
#endif
}

GCEXPORT void gcRequestAsyncGC(void)
{
  if (!gcvar.gcActive) {
    SetEvent( hGCEvent );
  }
}

/*------------------------ Init ----------------------------*/

static void gcInit(int __nMegs)
{
  DWORD HEAP_SIZE = __nMegs << 20;
  DWORD  ZCT_SIZE = HEAP_SIZE/0x100;

  FILE *f;

  DWORD TimeAdjustment;       // size of time adjustment
  DWORD TimeIncrement;        // time between adjustments
  BOOL  TimeAdjustmentDisabled; // disable option

  hGCEvent  = CreateEvent( NULL, FALSE, FALSE, NULL );
  hMutEvent = CreateEvent( NULL, FALSE, FALSE, NULL );

  GetSystemTimeAdjustment(
                          &TimeAdjustment,       // size of time adjustment
                          &TimeIncrement,        // time between adjustments
                          &TimeAdjustmentDisabled // disable option
                          );
#ifdef RCDEBUG
  dbgprn( 0, "TimeAdjustment=%d, TimeIncrement=%d, TimeAdjustmentDisabled=%d\n",
          TimeAdjustment,       // size of time adjustment
          TimeIncrement,        // time between adjustments
          TimeAdjustmentDisabled // disable option
          );
#endif

  f = fopen( "gcopt.txt", "r" );
  if (!f) {
    jio_printf( "GCOPT.txt could not be opened\n");
    exit(-1);
  }
  for (;;) {
    char buff[200];
    char opt[100];
    int  val;
    
    if (! fgets( buff, sizeof(buff), f) ) break;
    if (buff[0]=='#') continue; /* remark line */
    if (2 != sscanf( buff, "%s %d", opt, &val )) {
      jio_printf("Error reading GCOPT.TXT\n");
      exit(-1);
    }
#define CHECKGCOPT(optname) if (strcmp(opt, #optname)==0) {\
                               gcvar.opt. optname = val;\
                               jio_printf("GCOPT set: %s = %d\n", #optname, val);\
                               continue;\
                            } else do {} while(0)
    CHECKGCOPT(recommendOnlyRCGC);
    CHECKGCOPT(useOnlyTracingGC);
    CHECKGCOPT(useOnlyRCGC);
    CHECKGCOPT(listBlkWorth);
    CHECKGCOPT(userBuffTrig);
    CHECKGCOPT(initialHighTrigMark);
    CHECKGCOPT(lowTrigDelta);
    CHECKGCOPT(raiseTrigInc);
    CHECKGCOPT(lowerTrigDec);
    CHECKGCOPT(uniPrio);
    CHECKGCOPT(multiPrio);
    jio_printf("GCOPT unknown option %s\n", opt );
    exit(-1);
  }
  fclose( f );

  /* Init blocks manager */
  blkInit( HEAP_SIZE >> 20 );
  
  /* Init chunks manager */
  chkInit( HEAP_SIZE >> 20 );

  gcvar.stage = GCHS4;
  gcvar.createBuffList = NULL;
  gcvar.updateBuffList = NULL;
  gcvar.snoopBuffList = NULL;
  gcvar.deadThreadsCreateBuffList = NULL;
  gcvar.deadThreadsUpdateBuffList = NULL;
  gcvar.deadThreadsSnoopBuffList = NULL;
  gcvar.reinforceBuffList = NULL;

  gcvar.tempReplicaSpace = (GCHandle**)mokMemReserve( NULL, BUFFSIZE );
  mokMemCommit( (char*)gcvar.tempReplicaSpace, BUFFSIZE, false );

  gcvar.zctStack = (GCHandle**)mokMemReserve( NULL, ZCT_SIZE );
  mokMemCommit( (char*)gcvar.zctStack, ZCT_SIZE, false );
  gcvar.zctStackTop = (GCHandle**)(ZCT_SIZE + (char*)gcvar.zctStack);
  gcvar.zctStackSp = gcvar.zctStack;

  H1BIT_Init( &gcvar.localsBmp, (uint*)blkvar.heapStart, HEAP_SIZE );
  H2BIT_Init( &gcvar.rcBmp, (uint*)blkvar.heapStart, HEAP_SIZE );
  H1BIT_Init( &gcvar.zctBmp, (uint*)blkvar.heapStart, HEAP_SIZE );

  buffInit( gcvar.ee, &gcvar.zctBuff );

  gcvar.gcMon = (sys_mon_t*)sysMalloc(sysMonitorSizeof());        
  gcvar.requesterMon = (sys_mon_t*)sysMalloc(sysMonitorSizeof());        

  sysMonitorInit(  gcvar.gcMon );
  sysMonitorInit(  gcvar.requesterMon );
        
  gcvar.collectionType = GCT_RCING;

  gcvar.gcTrigHigh = (gcvar.opt.initialHighTrigMark * blkvar.nBlocks)/100;
}

GCEXPORT void gcStartGCThread(void)
{
  int priority;

  /*
   * If we're on an MP then the GC thread should be alloted a processor
   * of its own when it needs it.  So we select the priority to be
   * 10 which is translated in threads_md.c into win32 time critical
   * priority.
   *
   * Otherwise, we choose priority==9 which translates into win32
   * "highest priority"
   */
  if (sysGetSysInfo()->isMP)
    priority = gcvar.opt.multiPrio;
  else
    priority = gcvar.opt.uniPrio;
  createSystemThread("YLRC Garbage Collector (YEH!)", 9, 10*1024, gcThreadFunc, NULL);
}

GCEXPORT void gcThreadCooperate(ExecEnv *ee)
{
  int gcStage;

  mokAssert( !ee->gcblk.cantCoop );

  ee->gcblk.cantCoop = true;
  gcStage = gcvar.stage;
  if (ee->gcblk.stage == gcStage) goto __exit;
  if (ee->gcblk.stageCooperated == gcStage) goto __exit;
  mokAssert( ee->gcblk.stageCooperated == GCHSNONE );
  switch (gcStage) {
  case GCHS1:
    mokAssert( ee->gcblk.stage == GCHS4 );
    goto __exit;

  case GCHS2:
    mokAssert( ee->gcblk.stage == GCHS1 );
    goto __exit;

  case GCHS3:
    mokAssert( ee->gcblk.stage == GCHS2 );
    _HS3Cooperate( ee );
    goto __exit;

  case GCHS4:
    mokAssert( ee->gcblk.stage == GCHS3 );
    goto __exit;
  }  
  
 __exit:
  ee->gcblk.cantCoop = false;
}


GCEXPORT void gcThreadAttach(ExecEnv* ee)
{
  int i, stage;
  sys_thread_t *self = EE2SysThread( ee );

#ifdef RCDEBUG
  dbgprn( 0, "gcThreadAttach starting for ee=%x thread=%x\n", ee, self);
#endif

  ee->gcblk.cantCoop = false;

  buffInit( ee, &ee->gcblk.updateBuffer );
  buffInit( ee, &ee->gcblk.createBuffer );
  buffInit( ee, &ee->gcblk.snoopBuffer );

#ifdef RCDEBUG
  dbgprn( 2, "QUEUE_LOCK %x\n", self );
#endif
  QUEUE_LOCK( self );
#ifdef RCDEBUG
  dbgprn( 2, "QUEUE_LOCK %x took the lock\n", self );
#endif

  {
    SAVEDALLOCLISTS *sal = gcvar.pListOfSavedAllocLists;

    if (sal) {
      gcvar.pListOfSavedAllocLists = sal->pNext;
      memcpy( ee->gcblk.allocLists, sal->allocLists, sizeof(sal->allocLists) );
      sysFree( sal );
    }
    else {
      for (i=0; i<N_BINS; i++) {
        ee->gcblk.allocLists[i].binIdx = i;
        ee->gcblk.allocLists[i].head = ALLOC_LIST_NULL;
      }
    }
  }

  stage = gcvar.stage;
  ee->gcblk.stageCooperated = GCHSNONE;
  ee->gcblk.stage = stage;
  if (ee->gcblk.stage != GCHS4)
    ee->gcblk.snoop = true;
  else
    ee->gcblk.snoop = false;
  ee->gcblk.gcInited = true;
  QUEUE_UNLOCK( self );
#ifdef RCDEBUG
  dbgprn( 0, "gcThreadAttach ee=%x stage=%d\n", ee, stage);
  dbgprn( 0, "gcThreadAttach ended for ee=%x self=%x\n", ee, self);
#endif
}

GCEXPORT void gcThreadDetach(ExecEnv* ee)
{
  sys_thread_t *self = EE2SysThread( ee );
  SAVEDALLOCLISTS *sal;

  sal = (SAVEDALLOCLISTS*)sysMalloc(  sizeof(SAVEDALLOCLISTS) );

  mokAssert( sizeof(sal->allocLists) == sizeof(ee->gcblk.allocLists) );
  mokAssert( sizeof(sal->allocLists) == sizeof(ALLOCLIST)*N_BINS );

  memcpy( sal->allocLists, ee->gcblk.allocLists, sizeof( ee->gcblk.allocLists) );

  QUEUE_LOCK( self );

  sal->pNext = gcvar.pListOfSavedAllocLists;
  gcvar.pListOfSavedAllocLists = sal;
  
#ifdef RCDEBUG
  gcvar.dbgpersist.nDeadUpdateObjects += 
    ee->gcblk.updateBuffer.start[LOG_OBJECTS_IDX];
  gcvar.dbgpersist.nDeadUpdateChilds  +=  
    ee->gcblk.updateBuffer.start[LOG_CHILDS_IDX];
  gcvar.dbgpersist.nDeadCreateObjects +=  
    ee->gcblk.createBuffer.start[LOG_OBJECTS_IDX];
  gcvar.dbgpersist.nDeadSnooped       +=  
    ee->gcblk.snoopBuffer.start[LOG_OBJECTS_IDX];
#endif

  /* link the create buffer into a list for dead threads */
  *ee->gcblk.createBuffer.pos = 0;
  ee->gcblk.createBuffer.start[LAST_POS_IDX] = (uint)ee->gcblk.createBuffer.pos;
  ee->gcblk.createBuffer.start[LINKED_LIST_IDX] = 
    (uint)gcvar.deadThreadsCreateBuffList;
  gcvar.deadThreadsCreateBuffList = ee->gcblk.createBuffer.start;

  /* do the same for the update buffer */
  *ee->gcblk.updateBuffer.pos = 0;
  ee->gcblk.updateBuffer.start[LAST_POS_IDX] = (uint)ee->gcblk.updateBuffer.pos;
  ee->gcblk.updateBuffer.start[LINKED_LIST_IDX] = 
    (uint)gcvar.deadThreadsUpdateBuffList;
  gcvar.deadThreadsUpdateBuffList = ee->gcblk.updateBuffer.start;

  /* do the same for the snoop buffer */
  *ee->gcblk.snoopBuffer.pos = 0;
  ee->gcblk.snoopBuffer.start[LAST_POS_IDX] = (uint)ee->gcblk.snoopBuffer.pos;
  ee->gcblk.snoopBuffer.start[LINKED_LIST_IDX] = (uint)gcvar.deadThreadsSnoopBuffList;
  gcvar.deadThreadsSnoopBuffList = ee->gcblk.snoopBuffer.start;


  /* If we're between HS1 & HS2 then also link the update buffer
   * into the dead threads reinforce list
   */
  if (ee->gcblk.stage == GCHS1) {
#ifdef RCDEBUG
    gcvar.dbgpersist.nDeadReinforceObjects +=  
      ee->gcblk.updateBuffer.start[LOG_OBJECTS_IDX];
    gcvar.dbgpersist.nDeadReinforceChilds  +=  
      ee->gcblk.updateBuffer.start[LOG_CHILDS_IDX];
#endif
    ee->gcblk.updateBuffer.start[REINFORCE_LINKED_LIST_IDX] = 
      (uint)gcvar.deadThreadsReinforceBuffList;
    gcvar.deadThreadsReinforceBuffList = ee->gcblk.updateBuffer.start;
  }

  ee->gcblk.gcInited = false;

  QUEUE_UNLOCK( self );
}


void gcDo_gcupdate(ExecEnv *ee, void *_h, void *_slot, void *_newval )
{
#ifdef RCDEBUG
  static int deltaMax = -1;
  int delta = GetTickCount();
#endif

  GCHandle *h = (GCHandle*)_h;
  GCHandle **slot = (GCHandle**)_slot;
  GCHandle *newval = (GCHandle*)_newval;

#ifdef RCDEBUG
  sysAssert( h );
  sysAssert( ValidHandle(h) );
  sysAssert( !*slot || ValidHandle(*slot) );
  sysAssert( !newval || ValidHandle(newval) );

  {
    uint *p = h->logPos;
    if (p) {
      uint val = *p;
      uint type = val&3;
      sysAssert( (val&~3) == (uint)h );
      if (type==0) { // create log
        ee->gcblk.dbg.nNewObjectUpdatesInCycle++;
      }
      else {
        ee->gcblk.dbg.nOldObjectUpdatesInCycle++;
      }
    }
  }
#endif // RCDEBUG

  ee->gcblk.cantCoop = true;
  if (!h->logPos) {
    gcBuffSlowConditionalLogHandle( ee, (GCHandle*)h );
  }
  *slot = newval;
  if (newval && ee->gcblk.snoop) {
    BUFFHDR *bh = &ee->gcblk.snoopBuffer;
    gcBuffLogWordUnchecked( ee, bh, (uint)newval );
    ee->gcblk.cantCoop = false;
    gcBuffReserveWord( ee, bh );
  }
  else {
    ee->gcblk.cantCoop = false;
  }

#ifdef RCDEBUG
  delta = GetTickCount() - delta;
  if (delta > deltaMax) {
    deltaMax = delta;
    dbgprn( 0, " *** UPDATE(offset=%d) delta=%d\n", (char*)slot - (char*)h, delta );
  }
#endif
}

void gcDo_gcupdate_array(ExecEnv *ee, void *_arrayh, void* _slot, void *_newval )
{
  gcupdate( ee, _arrayh, _slot, _newval );
}

void gcDo_gcupdate_jvmglobal(ExecEnv* ee, void* _global, void *_newval )
{
#ifdef RCDEBUG
  static int deltaMax = -1;
  int delta = GetTickCount();
#endif

  GCHandle **slot = (GCHandle**)_global;
  GCHandle *newval = (GCHandle*)_newval;
  sysAssert( !newval || ValidHandle(newval) );

  ee->gcblk.cantCoop = true;
  *slot = newval;
  if (newval && ee->gcblk.snoop) {
    BUFFHDR *bh = &ee->gcblk.snoopBuffer;
    gcBuffLogWordUnchecked( ee, bh, (uint)newval );
    ee->gcblk.cantCoop = false;
    gcBuffReserveWord( ee, bh );
  }
  else {
    ee->gcblk.cantCoop = false;
  }

#ifdef RCDEBUG
  delta = GetTickCount() - delta;
  if (delta > deltaMax) {
    deltaMax = delta;
    dbgprn( 0, " *** UPD_GLOBAL delta=%d\n", delta );
  }
#endif
}

void gcDo_gcupdate_class(ExecEnv*  ee, ClassClass* cb, void *_slot, void *_newval )
{
  GCHandle **slot = (GCHandle**)_slot;

  sysAssert( ValidHandle(cb) );
  sysAssert( !*slot || ValidHandle(*slot) );

  gcupdate_jvmglobal( ee, slot, _newval );
}

void gcDo_gcupdate_static( 
  ExecEnv* ee,
  struct fieldblock* fb, 
  void *_slot, 
  void* _newval 
)
{
  GCHandle **slot = (GCHandle**)_slot;
  char isig = fieldsig(fb)[0];
  if (isig == SIGNATURE_CLASS || isig == SIGNATURE_ARRAY) {
    sysAssert( !*slot || ValidHandle(*slot) );
    gcupdate_jvmglobal( ee, slot, _newval );
  }
  else {
    *slot = (GCHandle*)_newval;
  }
}

GCEXPORT void gcPutstatic(ExecEnv *ee, struct fieldblock *fb, JHandle *val)
{
  sysAssert( fb );
  sysAssert( ValidHandle(fb->clazz) );
  gcupdate_static( ee, fb, &fb->u.static_value, val );
}

GCEXPORT void gcPutfield(ExecEnv *ee, JHandle *h, int offset, JHandle *val)
{
  Classjava_lang_Class *ucb;
  JHandle **slot;
  GCHandle *_h;

#ifdef RCDEBUG
  {
    Classjava_lang_Class *ucb;

    mokAssert( h );
    mokAssert( isHandle(h) );

    ucb = unhand(obj_classblock(h));

    mokAssert( ucb->is_reference[offset] );
    mokAssert( !val || isHandle(val) );
  }
#endif

  slot = (JHandle**)(((uint*)unhand(h)) + offset);
  gcupdate( ee, h, slot, val );
}


GCEXPORT void gcAastore(ExecEnv *ee, ClassArrayOfObject *arr, int offset, JHandle *val)
{
  JHandle **slot;
  JHandle *arrh;
#ifdef RCDEBUG
  ClassClass *cb;
  long n;
#endif


  arrh = gcRehand( arr );

#ifdef RCDEBUG
  mokAssert( arr );
  mokAssert( arrh );
  mokAssert( isHandle(arrh) );
#endif

  slot = &arr->body[offset];

#ifdef RCDEBUG
  mokAssert( !*slot || isHandle(*slot) );
  mokAssert( !val || isHandle(val) );

  mokAssert( obj_flags(arrh) == T_CLASS );

  n = obj_length(arrh);
  
  mokAssert( offset < n );
  mokAssert( offset >=0 );

  cb = (ClassClass*)arr->body[n];

  mokAssert( cb );
  mokAssert( isHandle(cb) );
#endif
  
  gcupdate_array( ee, arrh, slot, val );
}

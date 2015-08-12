\begin{rawcfig}{rcchunkmgr.c}
/*
 * File:    rcchunkmgr.c
 * Author:  Mr. Yossi Levanoni
 * Purpose: implementation of the chunk manager
 */
/************************************************
*
* Lock a partial list.  Implemented by a spin
* lock which is imbedded in the list header.
*/
#define _lockPartialList(pList, ee)\
do {\
        mokAssert( ee );\
        gcSpinLockEnter( &pList->lock, (unsigned)ee );\
} while(0)

/************************************************
*
* Unlock a partial list
*/
#define _unlockPartialList(pList, ee)\
do {\
        mokAssert( ee );\
        gcSpinLockExit( &pList->lock, (unsigned)(ee) );\
} while(0)


static void  _getPartialListStats( int iList, 
                                   int *pFreeBlocks, 
                                   int *pFreeBytes )
{
  ExecEnv *ee = EE();
  PARTIALLIST *pList = &chunkvar.partialLists[ iList ];
  int objSz  = chkconv.binSize[ iList ];
  int maxObj = chkconv.binToObjectsPerBlock[ iList ];
  int status, count;
  BlkAllocHdr *ph, *nextPh;
  BLKOBJ *freeList;
  
  *pFreeBlocks = 0;
  *pFreeBytes = 0;
  
  _lockPartialList( pList, ee);
  
  ph = pList->firstBlock;
  while (ph) {
    (*pFreeBlocks)++;
    status = bhGet_status( ph );
    mokAssert( status == PARTIAL );
    freeList = (BLKOBJ*)ph->freeList;
    if (freeList) {
      mokAssert( OBJBLOCKHDR(freeList) == ph );
      count = (int)freeList->count;
      mokAssert( count<=maxObj && count>0 );
      *pFreeBytes += count;
    }
    ph = ph->nextPartial;
  }
  _unlockPartialList( pList, ee );
  *pFreeBytes *= objSz;
}

GCEXPORT void chkGetPartialBlocksStats( int freeBlocks[], int freeBytes[])
{
  int i;
  for (i=0; i<N_BINS; i++)
    _getPartialListStats( i, &freeBlocks[i], &freeBytes[i] );
}


GCEXPORT int chkCountPartialBlocks(void)
{
  int n=0, i;
  for (i=0; i<N_BINS;i++)
    n += chunkvar.nBlocksInPartialList[i];
  return n;
}

/****************************************************************
************** Mutual Services **********************************
****************************************************************/

/********************************************
*
* Initialize conversion tables.
*
*/
static void _initChunkConv( void )
{
  int target,i, j;

  i=0;

  chkconv.binSize[ i++ ] = 8;
  chkconv.binSize[ i++ ] = 16;
  chkconv.binSize[ i++ ] = 24;
  chkconv.binSize[ i++ ] = 32;
  chkconv.binSize[ i++ ] = 40;
  chkconv.binSize[ i++ ] = 48;
  chkconv.binSize[ i++ ] = 56;
  chkconv.binSize[ i++ ] = 64;
  chkconv.binSize[ i++ ] = 80;
  chkconv.binSize[ i++ ] = 96;
  chkconv.binSize[ i++ ] = 112;
  chkconv.binSize[ i++ ] = 128;
  chkconv.binSize[ i++ ] = 160;
  chkconv.binSize[ i++ ] = 192;
  chkconv.binSize[ i++ ] = 224;
  chkconv.binSize[ i++ ] = 256;
  chkconv.binSize[ i++ ] = 320;
  chkconv.binSize[ i++ ] = 384;
  chkconv.binSize[ i++ ] = 448;
  chkconv.binSize[ i++ ] = 512;
  chkconv.binSize[ i++ ] = 640;
  chkconv.binSize[ i++ ] = 768;
  chkconv.binSize[ i++ ] = 1024;
  chkconv.binSize[ i++ ] = 1280;
  chkconv.binSize[ i++ ] = 2048;
  chkconv.binSize[ i++ ] = 4096;
  chkconv.binSize[ i++ ] = 8192;

  mokAssert( i == N_BINS );

  j = 0;
  for (i=0; i<=N_BINS; i++) {
    target = chkconv.binSize[i];
    for (; j<=target; j++) {
      chkconv.szToBinIdx[ j ] = i;
      chkconv.szToBinSize[ j ] = target;
    }
  }

  for (i=0; i<N_BINS; i++) {
    chkconv.binToObjectsPerBlock[i] = BLOCKSIZE / chkconv.binSize[i];
#ifdef RCDEBUG
    chunkvar.nBlocksInPartialList[i] = 0;
#endif /* RCDEBUG */
  }
}


/***************************************************************/
/******************* COLLECTION ********************************/
/***************************************************************/


/*************************************************
*
* Adds a block to a partial list.
*
* A block is added to the partial list by a
* collector when it finds that it's in the
* VOIDBLK state.
*
* The state is changed and the block is added to
* the appropriate list.
*
*
* Locks taken: 
*       the partial list lock
*
* Competing operations: 
*       mutators executing _getPartialBlock
*
* state changes:
*       VOIDBLK ---> PARTIAL.  No contention.
*/
static void _addPageToPartialList( BlkAllocHdr* ph )
{
  BlkAllocHdr *head;
  int idx = bhGet_bin_idx(ph);
  PARTIALLIST *pList = &chunkvar.partialLists[ idx ];

  mokAssert( bhGet_status(ph) == VOIDBLK );
  bhSet_status(ph, PARTIAL );

  _lockPartialList( pList, gcvar.ee );
  head = pList->firstBlock;
  ph->nextPartial = head;
  ph->prevPartial = (BlkAllocHdr*)pList;
  if (head)
    head->prevPartial = ph;
  pList->firstBlock = ph;
#ifdef RCDEBUG
  chunkvar.nBlocksInPartialList[ idx ] ++;
#endif /* RCDEBUG */
  _unlockPartialList( pList, gcvar.ee );
}


/*************************************************
*
* Flush the buffers that contains block headers
* which have observed to be full.
*
* Each partial list is locked and the buffer 
* corresponding to it is examined.
*
* Each element has been already observed to be
* entirely free may have undergone many changes
* since:
*
* 1. It could have been reallocated and now
*     it is either OWNED or VOIDBLK.
*
* 2. If it turned into VOPIDBLK then the collector
*    could have already freed it.
*
* We protect against each of these possibilities
* by checking that the block is indeed full, and
* in the original partial list where it was observed.
*
* Additionally, we mark such a block as DUMMYBLK in
* order not to free it twice.
*
* When the candidates for freeing are verifired, the
* array of truly deletable blocks is passed to the
* block manager.
*
* Locks taken: 
*     1. the partial list lock.  Each at a time.
*     2. Afterwards, the block manager lock.
*
* Competing operations: 
*     mutators executing _getPartialBlock.
*
* State changes:
*     PARTIAL ---> Block Mgr states.  Contention resolved
*     by block mgr lock.
*/
static void _flushObservedFull(void)
{
  int  listIdx, status, count, maxObj, currentListIdx;
  int   blockIdx;
  PARTIALLIST *pList;
  BlkAllocHdr *ph;

  chunkvar.nTrulyFull = 0;


  for (listIdx = 0; listIdx<N_BINS; listIdx++) {
    pList = &chunkvar.partialLists[ listIdx ];
    maxObj = chkconv.binToObjectsPerBlock[listIdx] ;

    _lockPartialList( pList, gcvar.ee);

    for (blockIdx=0; blockIdx<pList->nObservedFull; blockIdx++) {
      ph = pList->observedFull[ blockIdx ];
                
      /* Did some mutator took it ? */
      status = bhGet_status(ph);
      if (status != PARTIAL) { /* yep */
        continue;
      }
        
      /* 
       * Is it in the original partial list
       * where it was observed to be full ?
       */
      currentListIdx = bhGet_bin_idx(ph);
      if (currentListIdx != listIdx ) /* nop */
        continue;

      /**
       * Is it still fully free ?
       */
      if (!ph->freeList) /* nop */
        continue;

      count = ph->freeList->count;

      mokAssert( count>=0 && count<=maxObj );
                        
      if (count < maxObj) /* nop */
        continue;
                        
      /*
       * Protect against extracting a single block
       * mutiple times.
       */
      bhSet_status( ph, DUMMYBLK );
                        
      /* extract the page */
      ph->prevPartial->nextPartial = ph->nextPartial;
      if (ph->nextPartial)
        ph->nextPartial->prevPartial = ph->prevPartial;
#ifdef RCDEBUG
      chunkvar.nBlocksInPartialList[ listIdx ] --;
#endif /* RCDEBUG */

      chunkvar.trulyFull[ chunkvar.nTrulyFull++ ] = ph;
    }

    _unlockPartialList( pList, gcvar.ee );

    pList->nObservedFull = 0; /* reset the list specific counter */
  }
  /* reset global counter */
  chunkvar.nObservedFull = 0;        

  /* return blocks to the block manager */
  blkFreeSomeChunkedBlocks( chunkvar.trulyFull, chunkvar.nTrulyFull );
}

/*************************************************************************
*
* Take a note that a block has been observed to be fully free.
*
* For each partial list we keep a buffer and a counter of blocks that
* were observed as full.  Additonally, we keep a global counter of
* all the blocks in all the partial lists that were observed to be full.
*
* If either the list specific counter or the global counter crosses a
* threshold, the lists are flushed using _flushObservedFull()
*
*
* Locks taken:
*       the call to _flushObservedFull() may lock partial lists and/or
*       the block manager (one at a time).
*/
static void _handleFullPartialBlock( PARTIALLIST *pList, BlkAllocHdr* ph )
{
  pList->observedFull[ pList->nObservedFull++ ] = ph;
  chunkvar.nObservedFull++;
  if (pList->nObservedFull >= MAX_OBSERVED_FULL_PER_LIST || 
      chunkvar.nObservedFull >= MAX_OBSERVED_FULL)
    _flushObservedFull();
}






/*************************************************
***************  Allocation **********************
**************************************************/


/**************************************************
*
* Moves all the items in a page's free list into
* the allocation list passed as a parameter.
*
* This function is called by a mutator which is
* the owner of this block.  It is invoked for 
* a page which has just been extracted from a
* partial list so it's clear that the free
* list is non-empty.
*
* Locks taken:
*    The page's lock
*
* Competing operations:
*    _flushRecycledListEntry().  Contention is
*    resolved by the page's lock.
*/
static void _stealFreeList( ALLOCLIST *allocList )
{
  BlkAllocHdr *ph = allocList->allocBlock;
  BLKOBJ *prev, *head;

  mokAssert( allocList->binIdx == bhGet_bin_idx( ph ) );
  mokAssert( bhGet_status(ph) == OWNED );

  bhLock( ph );
  (volatile BLKOBJ*)prev = ph->freeList;
  ph->freeList = NULL;
  bhUnlock(ph);

  mokAssert( prev );

  head = prev->next;

  prev->next = ALLOC_LIST_NULL;

  allocList->head = head;
}

/***************************************************
*
* Tries extracting a block from a partial list.
*
* If the partial list corresponding to the allocation
* list is non-empty then the first element is extracted.
*
* While the partial list lock is held, the state of the
* block is changed to OWNED.  This protects against
* freeing the block by the collector back to the block
* manager.
*
* The partial list lock is then released.
*
* Then the blocks free list is stolen (i.e., moved onto the
* allocation list) which entails locking the block.
*/
static BOOL _getPartialBlock( ALLOCLIST *allocList, ExecEnv *ee )
{
#ifdef RCDEBUG
  static int deltaMax = -1;
  int delta = GetTickCount();
#endif

  BlkAllocHdr *ph;
  PARTIALLIST *pList = &chunkvar.partialLists[ allocList->binIdx ];
        
  _lockPartialList( pList, ee );
  ph = pList->firstBlock;
  if ( !ph ) {
    _unlockPartialList( pList, ee );
#ifdef RCDEBUG
    delta = GetTickCount() - delta;
    if (delta > deltaMax) {
      deltaMax = delta;
      jio_printf(" ***1 ALLOC_PARTIAL delta=%d\n", delta );
      fflush( stdout );
    }
#endif
    return FALSE;
  }
  else {
    BlkAllocHdr *next = ph->nextPartial;
    pList->firstBlock = next;
    if (next) 
      next->prevPartial = (BlkAllocHdr*)pList;
  }
  bhSet_status( ph, OWNED );
#ifdef RCDEBUG
  chunkvar.nBlocksInPartialList[ allocList->binIdx ] --;
#endif /* RCDEBUG */
  _unlockPartialList( pList, ee );

  allocList->allocBlock = ph;

  _stealFreeList(allocList);
        
  mokAssert( allocList->head );
  mokAssert( allocList->head->count );

#ifdef RCDEBUG
  delta = GetTickCount() - delta;
  if (delta > deltaMax) {
    deltaMax = delta;
    jio_printf(" ***2 ALLOC_PARTIAL delta=%d\n", delta );
    fflush( stdout );
  }
#endif
  return TRUE;
}    


/**********************************************************
*
* Tries allocating object from the allocation list or from
* the block which is currently owned by it.
*
* If the allocation list is non-empty, then the first element
* is extracted and returned (no locking required).
*
* Otherwise, if the allocation list has no allocation block
* associated with it, then the function fails.
*
* Othetwise, the page is locked and its free list is probed.
* If the free list is empty then the page is transformed into
* a VOIDBLK block, the block is disassociated with the 
* allocation list and the fucntion fails.
*
* Otherwise, the free list is stolen and merged into the 
* allocation list.  The first element is extracted and
* returned.
*/
static BLKOBJ *_allocFromOwnedBlock( ALLOCLIST* allocList )
{
  BLKOBJ *head = allocList->head;

  if (head != ALLOC_LIST_NULL) {

#ifdef RCDEBUG
    {
      BLKOBJ *firstObj;

      mokAssert( allocList->allocBlock );
      mokAssert( bhGet_status( allocList->allocBlock) == OWNED );
      firstObj = BLOCKHDROBJ(allocList->allocBlock);
      if ((char*)firstObj <  blkvar.heapStart || 
          (char*)firstObj >= blkvar.heapTop ||
          (char*)head < blkvar.heapStart ||
          (char*)head >= blkvar.heapTop ) {
        jio_printf(
                   "Blk=%x first=%x head=%x\n", 
                   allocList->allocBlock, 
                   firstObj, 
                   head );
        fflush( stdout );
        mokAssert( 0 );
      }
      mokAssert( (((word)head) & ((word)firstObj)) == ((word)firstObj));
      if (allocList->head) 
        mokAssert( 
                  ((((int)allocList->head) - 
                    ((int)head)) % chkconv.binSize[ allocList->binIdx ]) == 0  );
    }
#endif
    allocList->head = head->next;
    return head;
  }

  {
#ifdef RCDEBUG
    static int deltaMax = -1;
    int delta = GetTickCount();
#endif

    BlkAllocHdr *ph = allocList->allocBlock;
    if (!ph) return NULL;

    /* see if there is something on the free list */
    bhLock( ph );
    (volatile BLKOBJ*)head = ph->freeList;
    if (head) { 
      /* copy and clear */
      ph->freeList = NULL;
      bhUnlock(ph);
      {
        BLKOBJ *ret = head->next;

        head->next = ALLOC_LIST_NULL;
        allocList->head = ret->next;
        return ret;
      }        
    }

    /* OK, we have to abandon the page, i.e.,
     * transfrom it into a VOIDPG page
     */
    bhSet_status(ph, VOIDBLK );
    bhUnlock( ph );
    allocList->allocBlock = NULL;

#ifdef RCDEBUG
    delta = GetTickCount() - delta;
    if (delta > deltaMax) {
      deltaMax = delta;
      jio_printf(" ***3 ALLOC_OWNED delta=%d\n", delta );
      fflush( stdout );
    }
#endif
  }
  return NULL;
}

/********************************************************
*
* Allocate a single block from the block manager and
* chunk it into the given allocation list.
*/
static bool _getBlkMgrBlock( ALLOCLIST* allocList, ExecEnv *ee )
{
#ifdef RCDEBUG
  static int deltaMax = -1;
  int delta = GetTickCount();
#endif

  BlkAllocHdr *ph = blkAllocBlock( ee );
  int sz;
  int count;
  BLKOBJ *start, *curr, *next;
        
  if (!ph) {
#ifdef RCDEBUG
    delta = GetTickCount() - delta;
    if (delta > deltaMax) {
      deltaMax = delta;
      jio_printf(" ***4 ALLOC_BLK delta=%d\n", delta );
      fflush( stdout );
    }
#endif
    return false;
  }
        
  sz = chkconv.binSize[ allocList->binIdx ];
  count = chkconv.binToObjectsPerBlock[ allocList->binIdx ];

  mokAssert( count >= 2 );

  count--;

  start = curr = BLOCKHDROBJ(ph);
        
  for ( ;count>0; count--) {
    next = (BLKOBJ*)(((word)curr) + sz );
    curr->next = next;
    curr = next;
  }
  curr->next = ALLOC_LIST_NULL;

  allocList->head = start;
  allocList->allocBlock = ph;
  ph->nextPartial = ph->prevPartial = NULL;
  ph->freeList = NULL;
  ph->StatusLockBinidx = (OWNED << 24) | allocList->binIdx;

#ifdef RCDEBUG
  delta = GetTickCount() - delta;
  if (delta > deltaMax) {
    deltaMax = delta;
    jio_printf(" ***5 ALLOC_BLK delta=%d\n", delta );
    fflush( stdout );
  }
#endif
  return true;
}



/********************************************************************
*********************************************************************
*******************  Exported Functions  ****************************
*********************************************************************
********************************************************************/

/*********************** Collection ********************************/

#ifdef RCDEBUG
GCFUNC void chkPreCollect(BLKOBJ *o)
{
  word blockid;
  RLCENTRY *rlce;
  BLKOBJ *head;

  blockid = OBJBLOCKID(o);
  rlce = &chunkvar.rlCache[blockid % chunkvar.nCacheEntries];
  head = rlce->recycledList;
        
  /**
   * Is the cache entry currently owned by this block ?
   */
  if ((((word)head) ^ ((word)o)) < BLOCKSIZE) {
        
    mokAssert( OBJBLOCKID(head)==blockid );
    {
      int binIdx = bhGet_bin_idx( OBJBLOCKHDR(o) );
      int objSize = chkconv.binSize[ binIdx ];
      int maxObjs = chkconv.binToObjectsPerBlock[ binIdx ];

      /**
       * since some but not all BLKBOJs of the block are linked
       * the following should hold.
       */
      mokAssert( head->count>0 && head->count<maxObjs );
    }
    o->next  = head->next;
    head->next = o;
    head->count ++;
    return;
  }

  if (head) 
    chkFlushRecycledListEntry( rlce );
        
  /* now the entry is vacant and we can use it */
  o->count = 1;
  o->next  = o;
  rlce->recycledList = o;
}
#endif /* RCDEBUG */

/***********************************************************************
*
* Flush an entry in the recycled lists cache.
*
*
* First of, the block is locked then its state is read, the free list
* is merged with the recycled list and then the lock is released.
*
* -- If the block is in the VOIDBLK state:
*
* a. The free list must be empty.
* b. If the free list now contains all elements in the block then the
*    block is returned directly to the block manager (without going 
*    through the "observed full" set).  Otherwise, the state is changed 
*    to PARTIAL  (no lock is taken).  Then the corresponding partial list
*    is locked and the block is added to it.
*
* -- Additional action for PARTIAL
* a. If the block is now fully freed, then it is marked as "observed full"
*    which may lead to the flushing of the "observed full" set.
*
* Note: free lists and recycled lists are circular.
*
*/
GCFUNC void chkFlushRecycledListEntry(RLCENTRY *rlce)
{
  BlkAllocHdr *ph;
  int nFree, nRecycled;
  BLKOBJ *recycledList, *freeList;
  unsigned status;
        
  recycledList = rlce->recycledList;
  ph = OBJBLOCKHDR( recycledList );

  mokAssert( recycledList ); /* or else it woudn't be in the cache */
  mokAssert( recycledList->next ); /* it's a circular list */

  nRecycled = recycledList->count;

  mokAssert( nRecycled ); /* or else it woudn't be in the cache */
        
  bhLock( ph );

  status = bhGet_status(ph);
  mokAssert( status==PARTIAL || status==OWNED || status==VOIDBLK);

  (volatile BLKOBJ*)freeList = ph->freeList;

  if (freeList) {
    BLKOBJ *t;
    mokAssert( freeList->count );
    nFree = freeList->count + recycledList->count;
    t = recycledList->next;
    recycledList->next = freeList->next;
    freeList->next = t;
  }
  else {
    nFree = recycledList->count;
    freeList = recycledList;
  }

  freeList->count = nFree;
  ph->freeList = freeList;

  bhUnlock( ph );

  if (status == PARTIAL) {
    /*
     * Have we freed all chunks on a
     * partial page ?
     */
    int binIdx = bhGet_bin_idx( ph );
    PARTIALLIST *pList = &chunkvar.partialLists[ binIdx ];
    int maxChunks = chkconv.binToObjectsPerBlock[ binIdx ];
    if (maxChunks == nFree)
      _handleFullPartialBlock( pList, ph );
  } 
  else if (status == VOIDBLK) {
    /** 
     * either put the VOIDBLK page into the partial list or
     * return it to the block manager.
     */
    int binIdx = bhGet_bin_idx( ph );
    int maxChunks = chkconv.binToObjectsPerBlock[ binIdx ];
    if (maxChunks==nFree) {
      blkFreeChunkedBlock(ph);
    }
    else {
      _addPageToPartialList(ph);
    }
  }
  rlce->recycledList = NULL;
}

GCFUNC void chkFlushRecycledListsCache( void )
{
  int i;
  RLCENTRY *rlce = chunkvar.rlCache;
  for (i=chunkvar.nCacheEntries; i>0; i--, rlce++) 
    if (rlce->recycledList) 
      chkFlushRecycledListEntry( rlce );
}

GCFUNC void chkSweepChunkedBlock( BlkAllocHdr *ph, int status)
{
  int binidx = bhGet_bin_idx( ph );
  int objsz = chkconv.binSize[ binidx ];
  int nobj = chkconv.binToObjectsPerBlock[ binidx ];
  GCHandle *h = (GCHandle*)BLOCKHDROBJ(ph);
  RLCENTRY rlce;
  int count = 0;

  while (nobj>0) {
    nobj--;
    if (gcGetHandleRC(h)==0 && !h->logPos) {
      BLKOBJ *o = (BLKOBJ*)h;
      o->next = o;
      rlce.recycledList = o;
      count = 1;
      goto __scan_with_list;
    }
    h = (GCHandle*)(objsz + (char*)h);
  }

  return; /* found nothing */

 __scan_with_list:
  /* here recycled list is non-empty */

  h = (GCHandle*)(objsz + (char*)h);
  while (nobj>0) {
    nobj--;
    if (gcGetHandleRC(h)==0 && !h->logPos) {
      BLKOBJ *o = (BLKOBJ*)h;
      count++;
      o->next = rlce.recycledList->next;
      rlce.recycledList->next = o;
      goto __scan_with_list;
    }
    h = (GCHandle*)(objsz + (char*)h);
  }

#ifdef RCDEBUG
  gcvar.dbg.nFreedInCycle += count;
  gcvar.dbg.nBytesFreedInCycle += count*objsz;
#endif
  rlce.recycledList->count = count;
  chkFlushRecycledListEntry( &rlce );
}

/******************* Allocation ********************************/

GCEXPORT BLKOBJ *chkAllocSmall(ExecEnv* ee, unsigned binIdx )
{
  int retries;
  ALLOCLIST *allocList = & ee->gcblk.allocLists[ binIdx ];
  BLKOBJ* ores;

  ores = _allocFromOwnedBlock( allocList );
  if (ores) {
    return ores;
  }
  
  /* now is a good time to cooperate ! */
  //  if (ee->gcblk.stage != gcvar.stage)
  // gcThreadCooperate(ee);

  for (retries=0; retries<3; retries++) {
    if (_getPartialBlock( allocList, ee )) {
      ores = _allocFromOwnedBlock( allocList );
      mokAssert( ores );
      return ores;
    }
    if (_getBlkMgrBlock( allocList, ee )) {
      ores = _allocFromOwnedBlock( allocList );
      mokAssert( ores );
      return ores;
    }
    /* Sync GC */
    if (gcvar.initialized) {
      gcvar.memStress = true;
      gcRequestSyncGC();
    }
    else
      break;
  }
  OutOfMemory();
  return NULL;
}

/******************* Initialization ********************************/
GCFUNC void chkInit(unsigned nMB)
{
  unsigned      sz;
  unsigned      nPages;
 
  /* init conversion tables */
  _initChunkConv();

  /* Allocate page headers cache, ZEROED OUT */
  nPages = nMB << (20 - BLOCKBITS); 
  chunkvar.nCacheEntries = nPages / RLCACHE_RATIO;
  if (chunkvar.nCacheEntries < 117)
    chunkvar.nCacheEntries = 117;
  sz = chunkvar.nCacheEntries * sizeof(RLCENTRY);
  chunkvar.rlCache = (RLCENTRY*)mokMemReserve( NULL, sz );
  mokMemCommit( chunkvar.rlCache, sz, true );
}
\end{verbatim}
\end{rawcfig}



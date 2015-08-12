\begin{rawcfig}{rcblkmgr.c}
/*
 * File:    rcblkmgr.c
 * Author:  Mr. Yossi Levanoni
 * Purpose: implementation of the block manager
 */
/******************* Initialization ********************************/
GCFUNC void blkInit(unsigned nMB)
{
  unsigned      sz;
   
  /* Zero out all vars */
  memset( &blkvar, 0, sizeof(blkvar) );

  /* Allocate the heap */
  mokAssert( nMB < (1<<BLOCKBITS) && nMB>0);
  blkvar.heapSz = nMB << 20;
  blkvar.heapStart = (byte*)mokMemReserve( NULL, blkvar.heapSz );
  blkvar.heapTop = blkvar.heapStart + blkvar.heapSz;
  mokMemCommit( blkvar.heapStart, blkvar.heapSz, false );

#ifdef RCVERBOSE
  jio_printf(
         "heap[%x<-->%x]\n", 
         (unsigned)blkvar.heapStart, 
         blkvar.heapSz + (unsigned)blkvar.heapStart);
  fflush( stdout );
#endif 

  /* Allocate block headers table */
  blkvar.nWildernessBlocks = blkvar.nBlocks = blkvar.heapSz >> BLOCKBITS;
  sz = sizeof( BlkAllocHdr ) * (blkvar.nBlocks + 3);
  blkvar.allocatedBlockHeaders  = (BlkAllocHdr*)mokMemReserve( NULL, sz );
  mokMemCommit( blkvar.allocatedBlockHeaders, sz, true );

  blkvar.allocatedBlockHeaders ++;
	
  blkvar.pRegionLists = 
    (BlkListHdr*)blkvar.allocatedBlockHeaders + blkvar.nBlocks + 1;


  bhSet_status( (blkvar.allocatedBlockHeaders-1) , DUMMYBLK );
  bhSet_status( (blkvar.allocatedBlockHeaders+blkvar.nBlocks) , DUMMYBLK );

  blkvar.blockHeaders = 
    blkvar.allocatedBlockHeaders - ((unsigned)blkvar.heapStart>>BLOCKBITS);
	
  blkvar.heapTopRegion = (BlkRegionHdr*)OBJBLOCKHDR( blkvar.heapTop );
  blkvar.wildernessRegion = (BlkRegionHdr*)OBJBLOCKHDR( blkvar.heapStart );
	
  /* Allocate mutex */
  blkvar.blkMgrMon = sysMalloc(sysMonitorSizeof());
  sysMonitorInit( blkvar.blkMgrMon );

#ifdef RCDEBUG
  jio_printf("headers1[%x<-->%x]\n", 
	 (unsigned)blkvar.allocatedBlockHeaders, 
	 sz + (unsigned)blkvar.allocatedBlockHeaders);
  jio_printf("headers2[%x<-->%x]\n",
	 OBJBLOCKHDR(blkvar.heapStart), 
	 OBJBLOCKHDR( (((byte*)blkvar.heapStart)+(nMB<<20)) ) );
#endif 
}



/*******************************************************
*                   LOCKING                            *
********************************************************/

static void _LockBlkMgr(sys_thread_t *thrd)
{
  sysMonitorEnter( thrd, blkvar.blkMgrMon );
}

static void _UnlockBlkMgr(sys_thread_t* thrd )
{
  sysMonitorExit( thrd, blkvar.blkMgrMon );
}

  
/*******************************************************
* Allocate nBlocks from the part of the heap that
* hasn't been touched thus far.
********************************************************/
static 	BlkAllocHdr* _allocFromWilderness( int nBlocks )
{
  BlkRegionHdr* base = blkvar.wildernessRegion;
  BlkRegionHdr* target =  base + nBlocks;
  if (target > blkvar.heapTopRegion)
    return NULL;
  blkvar.wildernessRegion = target;

  return (BlkAllocHdr*)base;
}

/*******************************************************
 *
 * Insert this block, with the specified size, into the 
 * respective quick list.
 *
 * No merging with neighboring regions is attempted nor
 * should be applicable.
 *
 * The limitting blocks have their "regionSize" set.
 *******************************************************/
static void _insertRegionIntoQuickLists( BlkRegionHdr *brh, int sz )
{
  BlkRegionHdr *lastBlk = brh + (sz-1);

  brh->StatusUnused = BLK << 24;
  brh->regionSize = sz;

  if (lastBlk != brh) {
    lastBlk->StatusUnused = BLK << 24;
    lastBlk->regionSize = -sz;
  }

  brh->nextRegion = blkvar.quickLists[sz];
  if (brh->nextRegion)
    brh->nextRegion->prevRegion = brh;
  brh->prevRegion = (BlkRegionHdr *)&blkvar.quickLists[sz];
  blkvar.quickLists[sz] = brh;
}

/*****************************************************************
* Insert a region into the list of lists of regions.  If a list
* for the region size exists then it is added to it.  Otherwise,
* a new list is inserted to the list of lists for holding regions
* of "sz" blocks.
* 
* If the region becomes an element in a list of regions than its
* "regionSize" field is updated to "sz".  The last block in the
* region has its size updated to "-sz" at any rate.
******************************************************************/
static void _insertRegionIntoRegionLists( BlkRegionHdr *brh, int sz )
{
  int regionSize = -1;
  BlkListHdr *pPrevList, *pList;
  BlkListHdr *blh = (BlkListHdr *)brh;

  BlkRegionHdr *lastBlk = brh + (sz-1);

  lastBlk->StatusUnused = BLK << 24;
  lastBlk->regionSize = -sz;

  mokAssert( sz > 1 );

  pList = blkvar.pRegionLists->nextList;
  pPrevList = blkvar.pRegionLists;
  for (; pList; pPrevList = pList, pList = pList->nextList) {
    regionSize = pList->listRegionSize;
    if (sz <= regionSize)
      break;
  }


  /**
   * Perfect match
   */
  if (regionSize == sz ) {

    brh->StatusUnused = BLK<<24;
    brh->regionSize = sz;

    brh->nextRegion = pList->nextRegion;
    brh->prevRegion = (BlkRegionHdr *)pList;
    if (pList->nextRegion)
      pList->nextRegion->prevRegion = brh;
    pList->nextRegion = brh;

    return;
  }

  /**
   * Create new empty list.
   */
  blh->nextRegion = NULL;

  blh->StatusPrevListID = BLKLIST << 24;
  blh->listRegionSize = sz;

  /** 
   * we want to insert after pPrevList and before
   * pList.
   */
  bhSet_prev_region_list( blh, pPrevList );
  blh->nextList = pList;
  pPrevList->nextList = blh;
  if (pList) {
    bhSet_prev_region_list( pList, blh);
  }
}

/*****************************************************
* Extract the argument region from the list it's
* in.  Assumes that the region is not a list header.
******************************************************/
static void _extractFromRegionList( BlkRegionHdr *ph )
{
  ph->prevRegion->nextRegion = ph->nextRegion;
  if (ph->nextRegion)
    ph->nextRegion->prevRegion = ph->prevRegion;
}

/*****************************************************
* Extract the argument region, which is a list header,
* from the list of lists.
******************************************************/
static void _extractFromListOfLists( BlkListHdr *ph )
{
  BlkListHdr *newHeader = (BlkListHdr *)ph->nextRegion;
  BlkListHdr *prevList = bhGet_prev_region_list( ph );

  /**
   * Change list header to the next element in the
   * list
   */
  if (newHeader) {
    int sz = ((BlkRegionHdr *)newHeader)->regionSize;
    bhSet_prev_region_list( newHeader, prevList );
    newHeader->nextList = ph->nextList;

    prevList->nextList = newHeader;
    if (newHeader->nextList) {
      bhSet_prev_region_list( newHeader->nextList, newHeader );
    }		
    bhSet_status( newHeader, BLKLIST );
    newHeader->listRegionSize = sz;
    return;
  }
  /**
   * Eliminate the list.
   */
  prevList->nextList = ph->nextList;
  if (ph->nextList) {
    BlkListHdr *prevList = bhGet_prev_region_list( ph );
    bhSet_prev_region_list( ph->nextList, prevList );
  }
}

/*****************************************************
* See if the region adjacent to the argument region 
* from the right (i.e., with higher address) is in
* the hands of the block manager.
*
* If so, extract it from wherever it is.
*******************************************************/
static void _tryExtractRightNbr( BlkRegionHdr **pph, int *pSz)
{
  BlkRegionHdr *nbr = *pph + *pSz;
  int status = bhGet_status( nbr );
  int size = nbr->regionSize; // conicides with the size field of BLKLIST

#ifdef RCDEBUG
  if (status==BLK || status==BLKLIST) {
    BlkRegionHdr *lastBlock = nbr + size - 1;
    mokAssert( size > 0 );
    mokAssert( bhGet_status( lastBlock ) == BLK );
    mokAssert( lastBlock==nbr || lastBlock->regionSize == -size );
  }
#endif

  if (status == BLK) {
    _extractFromRegionList( nbr );
    blkvar.nListsBlocks -= size;
    *pSz += size;
  }
  else if (status == BLKLIST) {
    BlkListHdr *blh = (BlkListHdr *)nbr;
    _extractFromListOfLists( blh );
    blkvar.nListsBlocks -= size;
    *pSz += size;
  }
}

/*****************************************************
* See if the region adjacent to the argument region 
* from the left (i.e., with lower address) is in
* the hands of the block manager.
*
* If so, extract it from wherever it is.
*******************************************************/
static void _tryExtractLeftNbr( BlkRegionHdr **pph, int *pSz)
{
  BlkRegionHdr *nbr = *pph - 1;
  int status = bhGet_status( nbr );
  int size = nbr->regionSize==1 ? 1 : -nbr->regionSize;

  /** 
   * That's because items in the list are
   * bigger than a single block and their
   * final block is marked with BLK.
   */
  mokAssert( status != BLKLIST );
	
  if (status == BLK) {
    mokAssert( size > 0 );
    nbr = nbr + 1 - size;

    status = bhGet_status( nbr );
    mokAssert( nbr->regionSize == size );

    if (status == BLK) {
      _extractFromRegionList( nbr );
    }
    else {
      mokAssert( status == BLKLIST );
      _extractFromListOfLists( (BlkListHdr *)nbr );
    }

    blkvar.nListsBlocks -= size;

    *pSz += size;
    *pph = nbr;
  }
}

/***********************************************************
* Free the specified region:
*
* 1. see if it can be added to the wilderness.
* 2. if not, try coalescing from the left and right.
* 3. finally, add the resulting block to either the
*    quick lists or the list of lists, depending on its
*    size.
*************************************************************/
static void _blkFreeRegion_locked( BlkRegionHdr *ph, int sz )
{
  blkvar.nAllocatedBlocks -= sz;

  _tryExtractLeftNbr( &ph, &sz );

  if (ph + sz == blkvar.wildernessRegion) {
    blkvar.wildernessRegion = ph;
    blkvar.nWildernessBlocks += sz;
    return;
  }

  _tryExtractRightNbr( &ph, &sz );

  blkvar.nListsBlocks += sz;

  if (sz<N_QUICK_BLK_MGR_LISTS ) 
    _insertRegionIntoQuickLists( ph, sz );
  else
    _insertRegionIntoRegionLists( ph, sz );
}
	
/*******************************************************
 * Find the first non-empty list with size at least
 * "sz".  Then take the first element out.
 * If there is leftover, put it in the respective list.
 *******************************************************/
static BlkAllocHdr* _allocFromQuickLists( unsigned sz )
{
  BlkRegionHdr** pList = &blkvar.quickLists[sz];
  BlkRegionHdr*  brh, *nextB;
  unsigned i;

  for (i=sz; i<N_QUICK_BLK_MGR_LISTS; i++, pList++) {
    brh = *pList;
    if (brh)
      goto __found_list;
  }
  return NULL;

 __found_list:

  nextB = brh->nextRegion;
  if (nextB) 
    nextB->prevRegion = (BlkRegionHdr *)pList;
  (BlkRegionHdr*)*pList = nextB;

  if (sz != i) {
    BlkRegionHdr *leftover = brh + sz;
    int newSz = i - sz;
    _insertRegionIntoQuickLists( leftover, newSz );
  }
  return (BlkAllocHdr*)brh;
}

/*************************************************
* 
* Allocates "sz" blocks from the lists of regions.
* Try finding a list with elements at list of size
* "sz".
*
* If the found list contains additional elements
* besides the header, then the element after the
* header is extracted from the list.
*
* Otherwise, the list header itself is extracted 
* from the list of lists.
*
* Finally, if the list is not an exact match, the
* leftover is returned to the system.
**************************************************/                     
static BlkAllocHdr* _allocFromRegionLists( int sz )
{
  BlkRegionHdr *brh;
  BlkListHdr *pList = blkvar.pRegionLists->nextList;
  int regionSize, leftover;

  for (; pList; pList = (BlkListHdr *)pList->nextList) {
    regionSize = pList->listRegionSize;
    if (sz <= regionSize)
      goto __found_list;
  }
  return NULL;

 __found_list:

  brh = pList->nextRegion;
  if (brh) { // extract next element in the list
    BlkRegionHdr *nextB = brh->nextRegion;
    if (nextB)
      nextB->prevRegion = (BlkRegionHdr*)pList;
    pList->nextRegion = nextB;
  }
  else { // extract list header itself
    BlkListHdr *prevList = bhGet_prev_region_list( pList );
    if (pList->nextList) {
      bhSet_prev_region_list( pList->nextList, prevList);
    }
    /**
     * the next assignment may update *blkvar.pRegionLists
     * itself since the first element in the list
     * has its prevList pointer pointing at this
     * variable.
     */
    prevList->nextList = pList->nextList;
    brh = (BlkRegionHdr*)pList;
  }

  // do we have leftover
  leftover = regionSize - sz;

  if (leftover >= N_QUICK_BLK_MGR_LISTS) {
    _insertRegionIntoRegionLists( brh + sz, leftover );
  }
  else if (leftover >= 1) {
    _insertRegionIntoQuickLists( brh + sz, leftover );
  }
  return (BlkAllocHdr*)brh;
}

static void _sweepBig(BlkAllocBigHdr *ph)
{
  GCHandle *h;
  uint *p;

  if (ph->allocInProgress) return;

  h = (GCHandle*)BLOCKHDROBJ( (BlkAllocHdr*)ph );
  
  if (gcGetHandleRC(h)>0) return;

  p = h->logPos;
 
  if (p) {
    mokAssert( ((*p)&~3) == (uint)h );
    mokAssert( ((*p)&3) == 0 || ((*p)&3) == BUFF_HANDLE_MARK);
    /* leave it for next cycle */
    return;
  }
#ifdef RCDEBUG
  gcvar.dbg.nFreedInCycle++;
  gcvar.dbg.nBytesFreedInCycle += ph->blobSize * BLOCKSIZE;
#endif
  blkFreeRegion( ph );
}

/**********************************************************
* Allocate "nBlocks" of memory. Self explaining.
*
***********************************************************/
static BlkAllocHdr* _blkAllocRegion_locked( int nBlocks )
{
  BlkAllocHdr *res;

  if (nBlocks < N_QUICK_BLK_MGR_LISTS) {
    res = _allocFromQuickLists( nBlocks );
    if (res) {
      blkvar.nAllocatedBlocks += nBlocks;
      blkvar.nListsBlocks -= nBlocks;
      goto __checkout;
    }
  }
  res = _allocFromRegionLists( nBlocks );
  if (res) {
    blkvar.nAllocatedBlocks += nBlocks;
    blkvar.nListsBlocks -= nBlocks;
    goto __checkout;
  }

  res = _allocFromWilderness( nBlocks );
  if (!res) return NULL;
  blkvar.nAllocatedBlocks += nBlocks;
  blkvar.nWildernessBlocks -= nBlocks;

 __checkout:
  return res;
}


static int _calcAllocSize(int nBytes) 
{
  int blocks = nBytes / BLOCKSIZE;
  if (blocks==0 || nBytes%BLOCKSIZE)
    blocks++;
  return blocks;
}


/**** Exported Functions ***************/
GCFUNC BlkAllocHdr* blkAllocBlock( ExecEnv *ee )
{
  BlkAllocHdr *ph;
  sys_thread_t *self = EE2SysThread( ee );

  _LockBlkMgr( self );

  ph = (BlkAllocHdr *)_blkAllocRegion_locked( 1 );
  if (ph) {
    bhSet_status( ph, CHUNKING );
  }
  _UnlockBlkMgr( self );

  gcCheckGC();

  return ph;
}

GCEXPORT BlkAllocBigHdr* blkAllocRegion( unsigned nBytes, ExecEnv *ee )
{
  sys_thread_t *self = EE2SysThread( ee );

#ifdef RCDEBUG
  BlkAllocInternalHdr *inter;
  unsigned i;
#endif

  unsigned nBlocks;
  BlkAllocBigHdr *ph;
  BlkAllocBigHdr *lastBlk;
	
  nBlocks = _calcAllocSize( nBytes );

  _LockBlkMgr( self );
  ph = (BlkAllocBigHdr *)_blkAllocRegion_locked( nBlocks );

  if (!ph) {
    _UnlockBlkMgr( self );
    return NULL;
  }


  lastBlk = ph + (nBlocks-1);
  lastBlk->StatusUnused = ALLOCBIG << 24;
  lastBlk->blobSize = nBlocks;

  ph->allocInProgress = 1;
  ph->StatusUnused = ALLOCBIG << 24;
  ph->blobSize = nBlocks;

  _UnlockBlkMgr( self );

#ifdef RCDEBUG
  inter = (BlkAllocInternalHdr *)(ph+1);
  for (; inter < (BlkAllocInternalHdr *)lastBlk; inter++) {
    inter->startBlock = ph;
    bhSet_status( inter, INTERNALBIG );
  }
#endif

  gcCheckGC();

  return ph;
}

GCFUNC void blkFreeSomeChunkedBlocks( BlkAllocHdr **pph, int n )
{
  int i, status;
  BlkAllocHdr *ph;

  _LockBlkMgr( gcvar.sys_thread );

  for (i=0; i<n; i++) {
    ph = pph[i];
    status = bhGet_status(ph);
    mokAssert( status == DUMMYBLK );
    _blkFreeRegion_locked( (BlkRegionHdr*)ph, 1 );
  }

  _UnlockBlkMgr( gcvar.sys_thread );
}

GCFUNC void blkFreeChunkedBlock( BlkAllocHdr *ph )
{
#ifdef RCDEBUG
  int status = bhGet_status( ph );
  mokAssert ( status==VOIDBLK || status==PARTIAL );
#endif

  _LockBlkMgr( gcvar.sys_thread );
  _blkFreeRegion_locked( (BlkRegionHdr*)ph, 1 );
  _UnlockBlkMgr( gcvar.sys_thread );
}

GCFUNC void blkFreeRegion( BlkAllocBigHdr *ph )
{
  unsigned sz = ph->blobSize;

#ifdef RCDEBUG
  {
    BlkAllocBigHdr *lastBlk;
    BlkAllocInternalHdr *inter;
    unsigned i;
    
    lastBlk = ph + (sz-1);

    mokAssert( ph->StatusUnused = ALLOCBIG << 24 );
    mokAssert( lastBlk->StatusUnused = ALLOCBIG << 24 );
    mokAssert( lastBlk->blobSize == sz );
    mokAssert( ! ph->allocInProgress  );

    inter = (BlkAllocInternalHdr *)(ph+1);
    for (; inter < (BlkAllocInternalHdr *)lastBlk; inter++) {
      uint status = bhGet_status( inter );
      mokAssert( status == INTERNALBIG );
      mokAssert( inter->startBlock == ph );
    }
  }
#endif

  _LockBlkMgr( gcvar.sys_thread );
  _blkFreeRegion_locked( (BlkRegionHdr *)ph, sz );
  _UnlockBlkMgr( gcvar.sys_thread );

}

#ifdef RCDEBUG
GCFUNC void blkPrintStats(void)
{
  jio_printf("_______________ BLK STATS _______________\n" );
  jio_printf("wild=%d list=%d used=%d\n",
         blkvar.nWildernessBlocks, blkvar.nListsBlocks, blkvar.nAllocatedBlocks );
}
#endif

#pragma optimize( "", off )
GCFUNC void blkSweep(void)
{
  BlkRegionHdr *wildernessHdr = blkvar.wildernessRegion;
  BlkRegionHdr *brh = (BlkRegionHdr*)blkvar.allocatedBlockHeaders;
  volatile int *volatile p;

  while (brh < wildernessHdr) {
    volatile int size, status;

    p = (volatile int *volatile)&brh->regionSize;
    size = *p;
    p++;
    status = (*p) >> 24;

  __next_round:
    switch (status) {
    case BLK:
    case BLKLIST:
      mokAssert( size >= 1 );
      brh += size;
      break;

    case ALLOCBIG:
      _sweepBig( (BlkAllocBigHdr*)brh );
      mokAssert( size >= 1 );
      brh += size;
      break;

    case OWNED:
    case VOIDBLK:
    case PARTIAL: {
      int nextStatus;
      BlkRegionHdr *nextBrh = brh + 1;
      p = (volatile int *volatile)&nextBrh->regionSize;
      size = *p;
      p++;
      nextStatus = (*p) >> 24;
      chkSweepChunkedBlock( (BlkAllocHdr*)brh, status );
      brh = nextBrh;
      status = nextStatus;
      if (nextBrh >= wildernessHdr) return;
      goto __next_round;
    }

    default:
      mokAssert( status == CHUNKING );
      brh++;
      break;
    }
  }
}
#pragma optimize( "", on )
\end{verbatim}
\end{rawcfig}

#include "rchub.c"
.
.
.
/*****************************************************************
 *************                                  ******************
 *************  Allocation Cache (degenerated)  ******************
 *************                                  ******************
 ****************************************************************/

HObject * cacheAlloc(ExecEnv *ee, struct methodtable *mptr, long size)
{
#ifdef RCDEBUG
  static int deltaMax = -1;
  int delta = GetTickCount();
#endif

  GCHandle *h;
  JHandle  *_h;
  uint     *obj;
  int      bin;
 
  uint nbytes = sizeof(GCHandle) + size;

  if (nbytes <= MAX_CHUNK_ALLOC) {
    bin = chkconv.szToBinIdx[ nbytes ];

    chkAllocSmallInlined( ee, bin, _h );

    if (!_h) return NULL;

#ifdef RCDEBUG
    ee->gcblk.dbg.nBytesAllocatedInCycle += chkconv.binSize[ bin ];
#endif

    h = (GCHandle*)_h;
    obj = (uint *)(h + 1);
    if (size > 0) 
      memset( obj, 0, size );

#ifdef RCDEBUG
    h->status = Im_used;
#endif
    h->methods = mptr;
    h->obj     = obj;

    gcBuffLogNewHandle(ee, h);

#ifdef RCDEBUG
    delta = GetTickCount() - delta;
    if (delta > deltaMax) {
      deltaMax = delta;
      printf( " *** CACHE(small, nbytes=%d) delta=%d\n", nbytes, delta );
    }
#endif
  }
  else {
    BlkAllocBigHdr *ph;
    int i;
    for(i=0; i<3; i++) {
      ph = blkAllocRegion( nbytes, ee  );
      if (ph) goto __good;
      gcvar.memStress = true;
      gcRequestSyncGC();
    }
    
    return NULL;

  __good:
    h = (GCHandle*)BLOCKHDROBJ((BlkAllocHdr*)ph);

#ifdef RCDEBUG
    ee->gcblk.dbg.nBytesAllocatedInCycle += ph->blobSize * BLOCKSIZE;
#endif


    obj = (uint *)(h+1);
    ZeroMemory( obj, size );
#ifdef RCDEBUG
    h->status = Im_used;
#endif
    h->methods =  mptr;
    h->obj     =  obj;

    gcBuffLogNewHandle(ee, h);
    
    ph->allocInProgress = 0;

#ifdef RCDEBUG
    delta = GetTickCount() - delta;
    if (delta > deltaMax) {
      deltaMax = delta;
      printf( " *** CACHE(big, nbytes=%d) delta=%d\n", nbytes, delta );
    }
#endif
  }
  sysAssert( h );

  return (HObject*)h;
}



/*****************************************************************
 *************                                  ******************
 *************           Heap Meters            ******************
 *************                                  ******************
 ****************************************************************/
.
.
.
int64_t
TotalObjectMemory(void)
{
  return blkvar.heapSz;
}

int64_t
FreeObjectMemory(void)
{
  int freePartialBytes[N_BINS], freePartialBlocks[N_BINS];

  int nBlockBlocks = blkvar.nWildernessBlocks + blkvar.nListsBlocks;
  int nBlockBytes, nPartialBytes, nPartialBlocks, nBytes, i;
  float avgRes;

  printf("****************** FreeObjectMemory statistics(begin)\n");
  nBlockBytes = nBlockBlocks*BLOCKSIZE;
  printf("BlkMgr blocks=%d MB=%d\n", nBlockBlocks, nBlockBytes>>20 );


  chkGetPartialBlocksStats( freePartialBlocks, freePartialBytes );
  printf("Partial:\n");
  printf("binsz\tblocks\tMB\n");

  nPartialBytes = 0;
  nPartialBlocks = 0;

  for (i=0; i<N_BINS; i++) {
    printf("%d\t%d\t%d\n", 
           chkconv.binSize[i],
           freePartialBlocks[i],
           freePartialBytes[i]>>20 );
    nPartialBlocks += freePartialBlocks[i];
    nPartialBytes += freePartialBytes[i];
  }

  if (nPartialBlocks) 
    avgRes =  (float)nPartialBytes /((float)BLOCKSIZE*(float)nPartialBlocks);
  else 
    avgRes = -1;

  printf("Total partial: blocks=%d MB=%d avg-res=%f\n",
         nPartialBlocks,
         nPartialBytes>>20,
         avgRes
         );
  nBytes = nBlockBytes + nPartialBytes;
  printf("Total free MB=%d\n", nBytes>>20 );
  printf("****************** FreeObjectMemory statistics(end)\n");
  
  return nBytes;
}

int64_t
TotalHandleMemory(void)
{
  return 0;
}

int64_t
FreeHandleMemory(void)
{
  return 0;
}
.
.
.
/*
 * User interface to synchronous garbage collection.  This is called
 * by an explicit call to GC.
 */
void 
gc(unsigned int free_space_goal)
{
  gcRequestSyncGC();
}
.
.
.
bool_t isHandle(void *p)
{
  return _isHandle(p);
}

bool_t isObject(void *p)
{
  GCHandle *h = (GCHandle* )(((char*)p)-sizeof(GCHandle));
  return _isHandle(h);
}
.
.
.
bool_t isValidHandle(JHandle *h) 
{
  return _isHandle(h);
}

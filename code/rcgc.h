/*
 * File:    rcgc.h
 * Author:  Mr. Yossi Levanoni
 * Purpose: Publicly visible interface to garbage collection and allocation.
 */
/******************* Initialization ********************************/
#ifndef __RCGC__
#define __RCGC__

#include <assert.h>
#include <stdio.h>
#include <windows.h>

#include "monitor.h"

//#ifdef DEBUG
#define RCDEBUG
//#endif

#define RCVERBOSE

#define RCNOINLINE

#define GCEXPORT
#define GCFUNC static

#ifdef RCDEBUG
#define RCDEBUGVAR 1
#else
#define RCDEBUGVAR 0
#endif

/***********************************************************************************
*
* Forward declarations for external structures
*/
#define DECSTRUCT(T) struct T; typedef struct T T;

DECSTRUCT(BUFFHDR);
struct execenv;
typedef struct execenv ExecEnv;
typedef bool_t bool;

typedef struct GCHandle { 
  unsigned  *obj; 
  struct methodtable *methods; 
  unsigned *logPos;
#ifdef RCDEBUG
  unsigned status;
#endif 
} GCHandle ;

#define false FALSE
#define true  TRUE

/***********************************************************************************
*
* Atomic operatrions
*
*
*/
#define N_SPINS 4000

/******************************************************************
* 
* Some primitive data structures.
*/
typedef unsigned         word;
typedef unsigned         uint;
typedef unsigned char    byte;
typedef unsigned short   PAGEID;
typedef unsigned short   PAGECNT;

/*******************************************************************
*
* An object (chunk of memory) as the chunk manager sees it.
*/

typedef struct BLKOBJtag BLKOBJ;

struct BLKOBJtag {
  int      count;
  int      unused;
  BLKOBJ   *next;
};

/*******************************************************************
*
* Object and page sizes.
*
* We assume that objects are at least 8 bytes aligned.  This leaves 3 
* bits for playing.
*
* The minimal object size is 16 bytes because we have (at least) two
* words overhead per object: class pointer and log pointer.  In the
* "handled" JVM we have a third extra poiner.
*/
#define OBJGRAIN  8
#define OBJBITS   3
#define MINOBJ    16
#define OBJMASK   (~(OBJGRAIN-1))
/*
 * Minimal size of a page for the design to work: 256 bytes.
 * The reason for this is that we sometimes (in BLKLIST blocks) keep 
 * a block identifier as a 24 bit entity.  Thus, a block has to be at 
 * least 8 bits wide in order to allow 4GB regions.
 *
 * Since in pracrice we use blocks which are at least 4KB big, this is 
 * not a problem.
 *
 * Additionally, we store the size of chunks in a block on a 16 bit 
 * entity.  Thus, a block cannot be much bigger than 64KB or we'll
 * have to encode this field etc.
 */
#define MINBLOCKBITS        8
#define MAXNONBLOCKBITS     (32-MINBLOCKBITDS)

#define MAXBLOCKBITS        16


#define MAXOBJPERBLOCK      (BLOCKSIZE/MINOBJ)

/*
 * Actual block size.  This coincides with the PC page size.
 */
#define BLOCKBITS           (14)
#define NONBLOCKBITS        (32-BLOCKBITS)
#define BLOCKSIZE           (1<<BLOCKBITS)
#define BLOCKMASK           ((1<<BLOCKBITS)-1)

/*
* Size of maximal chunk.  Allocations larger than this size 
* are given full blocks.
*/
#define MAX_CHUNK_ALLOC     (BLOCKSIZE/2)

/* address of first object on the block */
#define OBJPAGE(o)       ((OBJECT*)(((unsigned)o) & (~BLOCKMASK)))

/* offset of object in the block */
#define OBJOFFSET(o)     (((unsigned)(o)) & BLOCKMASK)

/* number of block relative to address 0 */
#define OBJBLOCKID(o)    (((unsigned)(o))>>BLOCKBITS)

/* Object's block header */
#define OBJBLOCKHDR(o)   (&blkvar.blockHeaders[ OBJBLOCKID(o)])

/* convert from block header to the block's address */
#define BLOCKHDROBJ(ph)  ((BLKOBJ*)(((ph)-blkvar.blockHeaders)<<BLOCKBITS))


/************************************************************************/
/************************************************************************/
/******                                                             *****/
/******                        BLOCK MANAGER                        *****/
/******                                                             *****/
/************************************************************************/
/************************************************************************/
/************************************************************************
 */

/*
* Page States
*/

#define   BLK           1 /* In the block manager */
#define   BLKLIST       2 /* ---   " " -----------*/
#define   CHUNKING      3 /* Just out of the block manager, going to be OWNED */
#define   ALLOCBIG      4 /* Multiple-blocks object */
#define   INTERNALBIG   5 /* In the middle of ALLOCBIG, only in DEBUG */
#define   OWNED         6 /* Chunked block which is owned by some thread */
#define   VOIDBLK       7 /* Chunked block, allocation exhausted. */
#define   PARTIAL       8 /* Chunked block, sitting in a partial blocks list */
#define   DUMMYBLK      9 /* Temporary state */

#define   LASTMGRSTATE  BLKLIST

/*
Page header format for: OWNED, VOIDPG, PARTIAL.

Word 0:  <-------------------- nextPartial(32) ------------------------->
Word 1:  <-------------------- prevPartal(32) -------------------------->
Word 2:  <-------------------- freeList(32) ---------------------------->
Word 3:  <-- status(8) --><-- lock(8) --><-------- binidx(16)  --------->

In this case, the second word in the object pointed by "freeList"
contains the number of objects in the list.  recycledList is cached
(see below), the number of elements is held in the same manner at the
second word of the first element of the list.


&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&

Page header format for ALLOCBIG:

Word 0:  <---------------------- AllocInProgress(32) ------------------->
Word 1:  <---------------------- unused(32) ---------------------------->
Word 2:  <---------------------- size(32) ------------------------------>
Word 3:  <-- status(8) --><---------------- unused(24) ----------------->

"AllocInProgress" is true in the interval between the changing of the
state from BLKxxx to ALLOCBIG till the object is logged in the allocating
thread create log.  This prevents sweep from reclaiming such an object
just after it has been allocated.

"size" is the size of this large object, in blocks.


&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&

Page header format for INTERNALBIG:

Word 0:  <---------------------- startBlock(32) ------------------------>
Word 1:  <---------------------- unused(32) ---------------------------->
Word 2:  <---------------------- unused(32) ---------------------------->
Word 3:  <-- status(8) --><--------------unused(24) -------------------->

Where "start page" is the address where this large object begins.

THIS FORMAT IS GUARANTEED ONLY IN DEBUG MODE.


&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&

  Page header format for BLK:

Word 0:  <---------------------- nextRegion(32) ------------------------>
Word 1:  <---------------------- prevRegion(32) ------------------------>
Word 2:  <---------------------- size(32) ------------------------------>
Word 3:  <-- status(8) --><--------------unused(24) -------------------->



Next and prev are linked list pointers.  size is the size in pages of the
regions.


&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&


  Page header format for BLKLIST:

Word 0:  <---------------------- firstRegion --------------------------->
Word 1:  <---------------------- nextList (32) ------------------------->
Word 2:  <---------------------- size (32) ----------------------------->
Word 3:  <-- status(8) --><------------ prevListIDX (24)  -------------->


"firstRegion" is a pointer to a BLK block, the first on a linked list
of regions with the same size.

"nextList" points to the next list header (of type BLKLIST).  The pointer
to the previous list is encoded in the field "prevListIDX" as an index
into the allocatedPageHeaders array.

"size" is the size of the region.  Each element in the list has this size.

*************************************************************************/


/*
* Field selectors
*/
#define STATUSMAK     0xff000000
#define LOCKMASK      0x00ff0000
#define BINIDXMASK    0x0000ffff
#define PREVLISTMASK  0x00ffffff


typedef struct BlkAllocHdrTAG          BlkAllocHdr;
typedef struct BlkAllocBigHdrTAG       BlkAllocBigHdr;
typedef struct BlkAllocInternalHdrTAG  BlkAllocInternalHdr;
typedef struct BlkRegionHdrTAG         BlkRegionHdr;
typedef struct BlkListHdrTAG           BlkListHdr;
typedef struct BlkAnyHdrTAG            BlkAnyHdr;



struct BlkAllocHdrTAG {
  BlkAllocHdr   *nextPartial;
  BlkAllocHdr   *prevPartial;
  volatile  BLKOBJ        *freeList;
  volatile  word          StatusLockBinidx;
};

struct BlkAllocBigHdrTAG {
  volatile  word   allocInProgress;
  word   unused2;
  volatile  int    blobSize;
  volatile  word   StatusUnused;
};

struct BlkAllocInternalHdrTAG {
  BlkAllocBigHdr   *startBlock;
  word             unused1;
  word             unused2;
  volatile  word             StatusUnused;
};

struct BlkListHdrTAG {
  BlkRegionHdr    *nextRegion;
  BlkListHdr      *nextList;
  volatile  int             listRegionSize;
  volatile  word            StatusPrevListID;
};

struct BlkRegionHdrTAG {
  BlkRegionHdr   *nextRegion;
  BlkRegionHdr   *prevRegion;
  volatile  int            regionSize;
  volatile  word           StatusUnused;
};

struct BlkAnyHdrTAG {
  volatile word w0;
  volatile word w1;
  volatile word w2;
  volatile union {
    volatile byte b[4];
    volatile unsigned short s[2];
    volatile word w;
  } u;
};


/*
 * Utility macros
 */

/*
 * p is a pointer to AllocPgHdr.  Set and get the chunk size
 */
#define bhGet_bin_idx(p)      ((int)(((p)->StatusLockBinidx)&BINIDXMASK))
#define bhSet_bin_idx(p,idx)  do {\
  word v; \
  mokAssert( (idx)< N_BINS ); \
  v = p->StatusLockBinidx; \
  v = v & ~BINIDXMASK; \
  v = v | idx; \
  p->StatusLockBinidx = v; \
} while(0)


/*
 * p is a pointer to BlkRegionHdr. Set and get the previous list IS.
 */
#define bhGet_prev_region_list(p)  \
    ((BlkListHdr*)&blkvar.allocatedBlockHeaders[(p)->StatusPrevListID & PREVLISTMASK])

#define bhSet_prev_region_list(p,pBlkListHeader)  \
do {\
  word idx; \
  word v; \
  idx = (pBlkListHeader) - (BlkListHdr*)blkvar.allocatedBlockHeaders; \
  mokAssert (idx < (word)(blkvar.nBlocks+2)); \
  v = p->StatusPrevListID; \
  v = v & ~PREVLISTMASK; \
  v = v | idx; \
  (p)->StatusPrevListID = v; \
} while(0)



/*
 * Set and get the status of any page
 */
#define bhGet_status(p)          (((BlkAnyHdr*)p)->u.b[3])
#define bhSet_status(p,s)        do{ bhGet_status(p)=(s); }while(0)


/***************************************************************************
*
* Block manager structure
*
*/
#define N_QUICK_BLK_MGR_LISTS    5

struct BLKVAR {
  BlkListHdr*    pRegionLists;
  BlkRegionHdr*  quickLists[ N_QUICK_BLK_MGR_LISTS ];
  byte*          heapStart;
  byte*          heapTop;
  BlkRegionHdr*  heapTopRegion;
  BlkRegionHdr*  wildernessRegion;
  word           heapSz;
  word           nBlocks;
  BlkAllocHdr    *blockHeaders;
  BlkAllocHdr*   allocatedBlockHeaders;
  sys_mon_t*     blkMgrMon;
  int            nWildernessBlocks;
  int            nListsBlocks;
  int            nAllocatedBlocks;
};


#define FREE_BLOCKS() \
  (((blkvar.nListsBlocks*gcvar.opt.listBlkWorth)/100)+blkvar.nWildernessBlocks)

/***************************************************************************
 * Block manager exports 
 */
GCEXPORT  BlkAllocBigHdr*   blkAllocRegion( unsigned nBytes, ExecEnv *ee );


/************************************************************************/
/************************************************************************/
/******                                                             *****/
/******                   CHUNK MANAGEMENT                          *****/
/******                                                             *****/
/************************************************************************/
/************************************************************************/
/************************************************************************/

/************************************************************************
* 
* Recycled lists cache.
*
* The cache is simply an array of pointers to blocks.  The blocks are
* linked in a circular list with the first element holding the number
* of elements in the list.
*
* Collisions are treated by flushing an entry.  Meaning: adding the
* list to the block's free list.
*/

/* 
* this ration defines the number of blocks per recycled lists cache
* entry.
*/
#define RLCACHE_RATIO               10

typedef struct RLCacheEnteryTAG RLCENTRY;

struct RLCacheEnteryTAG {
  BLKOBJ       *recycledList;
};

/*************************************************************************
*
* Partial Lists to Block Manager evacuation thresholds.
*
*/
#define MAX_OBSERVED_FULL_PER_LIST  2
#define MAX_OBSERVED_FULL           4

/**************************************************************************
*
* Allocation lists
*
* These structures are embedded in the threads EE for fast allocation.
* Each thread has an allocation list per bin size.
*
*/

typedef struct AllocListTAG ALLOCLIST;

#define ALLOC_LIST_NULL ((BLKOBJ*)0x12baab21)

struct AllocListTAG {
  BLKOBJ*        head;
  BlkAllocHdr*   allocBlock;
  int            binIdx;
};


#define OutOfMemory()   mokAssert(0)
#define ALLOC_RETRY     (20)


/***************************************************************************
*
* Bins conversion tables.
*
*/
#define N_BINS  (27)

struct CHKCONV {
  int szToBinIdx[ BLOCKSIZE ];
  int szToBinSize[ BLOCKSIZE ];
  int binSize[ N_BINS ];
  int binToObjectsPerBlock[ N_BINS ];
};


/*****************************************************************************
*
* Partial lists.
*
* A partial list is a list of blocks which have some free chunks on them.  The
* pages are linked in a doubly linked list whose head is in this structure.
*
* There is a list per each bin size.
*
* The list also contains a remembered set of blocks which have been observed to
* be full.
*
* Finally the list contains a lock and therefore it is padded to a total size
* of 256 bytes (assuming this is bigger or equal to the contention granule) 
* in order to prevent false sharing with other partial lists.
*/
struct PARTIALLISTtag {
  BlkAllocHdr  *firstBlock;
  word         lock;
  int          nObservedFull;
  BlkAllocHdr  *observedFull[ MAX_OBSERVED_FULL_PER_LIST ];
  word         pad[64 - (MAX_OBSERVED_FULL_PER_LIST +3) ];
};


typedef struct PARTIALLISTtag PARTIALLIST;


/******************************************************************************
*
* Chunk manager structure.
*
*/
struct CHUNKVAR {
  PARTIALLIST   partialLists[ N_BINS ];
  int           nBlocksInPartialList[ N_BINS ];
  int           nCacheEntries;
  RLCENTRY      *rlCache;
  int           nObservedFull;
  int           nTrulyFull;
  BlkAllocHdr*  trulyFull[ MAX_OBSERVED_FULL ];
};


/******************************************************************************
*
* Chunk Manager exports
*
*/
GCEXPORT  int      chkCountPartialBlocks(void);
GCEXPORT  BLKOBJ*  chkAllocSmall(ExecEnv* ee, unsigned binIdx);
GCEXPORT  void     chkReleaseAllocLists( ExecEnv *ee);

#ifndef RCDEBUG

#define chkPreCollect(__o) \
do{\
  word blockid;\
  RLCENTRY *rlce;\
  BLKOBJ *head;\
  BLKOBJ *o = (BLKOBJ*)(__o);\
\
  blockid = OBJBLOCKID(o);\
  rlce = &chunkvar.rlCache[blockid % chunkvar.nCacheEntries];\
  head = rlce->recycledList;\
        \
  if ((((word)head) ^ ((word)o)) < BLOCKSIZE) {\
    o->next  = head->next;\
    head->next = o;\
    head->count ++;\
    goto __chkPreCollect_done_;\
  }\
  if (head) \
    chkFlushRecycledListEntry( rlce );\
        \
  o->count = 1;\
  o->next  = o;\
  rlce->recycledList = o;\
__chkPreCollect_done_:;\
} while(0)

#define _allocFromOwnedBlockInlined( allocList, __res )\
do {\
   BLKOBJ *head = allocList->head;\
   if (head != ALLOC_LIST_NULL) {\
      allocList->head = head->next;\
      (BLKOBJ*)__res = head;\
   }\
   else {\
      __res = NULL;\
   } \
} while (0)


#define chkAllocSmallInlined(ee, binIdx, __res)\
do {\
   ALLOCLIST *allocList = & (ee)->gcblk.allocLists[ (binIdx) ];\
   _allocFromOwnedBlockInlined( allocList, __res);\
   if (!__res) {\
      (BLKOBJ*)__res = chkAllocSmall( ee, binIdx);\
   }\
} while (0)

#else /* RCDEBUG */

#define chkAllocSmallInlined( ee, binIdx, __res)\
do {\
   (BLKOBJ*)__res = chkAllocSmall( ee, binIdx );\
} while(0)

#endif /* ! RCDEBUG */


/************************************************************************/
/************************************************************************/
/******                                                             *****/
/******                  BITMAPS                                    *****/
/******                                                             *****/
/************************************************************************/
/************************************************************************/
/************************************************************************/


/************************************************************************
*
* 1 Bit per handle BMP
*/
typedef struct H1BIT_BMP H1BIT_BMP;
struct H1BIT_BMP {
  byte *entry;
  byte *bmp;
  byte *rep_addr;
  unsigned bmp_size;
};

/************************************************************************
*
* 2 Bits per handle BMP
*/
typedef struct H2BIT_BMP H2BIT_BMP;
struct H2BIT_BMP {
  byte *entry;
  byte *bmp;
  byte *rep_addr;
  unsigned bmp_size;
};

/*
* 
* Include inline vertions of bmp functions:

void H1BIT_Set(byte* entry, unsigned h);
void H1BIT_Clear(byte* entry, unsigned h);
void H1BIT_Put(byte* entry, unsigned h, unsigned val);
byte H1BIT_Get(byte* entry, unsigned h);
void H1BIT_Init(H1BIT_BMP* bmp, unsigned* rep_addr, unsigned rep_size );

void H2BIT_Put(byte* entry, unsigned h, unsigned val);
void H2BIT_Clear(byte* entry, unsigned h);
void H2BIT_Stuck(byte* entry, unsigned h);
byte H2BIT_Get(byte* entry, unsigned h);
void H2BIT_Inc(byte* entry, unsigned h);
byte H2BIT_IncRV(byte* entry, unsigned h);
byte H2BIT_Dec(byte* entry, unsigned h);
void H2BIT_Init(H2BIT_BMP* bmp, unsigned* rep_addr, unsigned rep_size );

Functions that have a return value have "Inlined" appended to their name
e.g H1BIT_GetInlined( entry, h, __res_var) where __res_var is the *name*
of the variable onto which the result should be stored.
*/

#ifdef RCNOINLINE

#define H1BIT_GetInlined( entry, h, __res_var)\
do {\
        __res_var = H1BIT_Get(entry, h );\
} while (0)

#define H2BIT_GetInlined( entry, h, __res_var)\
do {\
        __res_var = H2BIT_Get(entry, h );\
} while (0)

#define H2BIT_IncRVInlined( entry, h, __res_var)\
do {\
        __res_var = H2BIT_IncRV(entry, h );\
} while (0)

#define H2BIT_DecInlined( entry, h, __res_var)\
do {\
        __res_var = H2BIT_Dec(entry, h );\
} while (0)


#else /* ! RCNOINLINE */

#include "rcbmp_inline.h"

#endif /*  RCNOINLINE */


/************************************************************************/
/************************************************************************/
/******                                                             *****/
/******                  GC Data Structures                         *****/
/******                                                             *****/
/************************************************************************/
/************************************************************************/
/************************************************************************/


/************************************************************************
*
* Buffer mgmnt.
*
*/
#define BUFFBITS   18
#define BUFFSIZE   (1<<BUFFBITS)
#define BUFFMASK   (BUFFSIZE-1)
#define LOWBUFFMASK ((1<<16)-1)

#define BUFF_LINK_MARK            1U
#define BUFF_HANDLE_MARK          2U
#define BUFF_DUP_HANDLE_MARK      3U
        
#ifdef RCDEBUG
#define N_RESERVED_SLOTS 8
#else
#define N_RESERVED_SLOTS 4
#endif //RCDEBUG

#define LINKED_LIST_IDX            0
#define REINFORCE_LINKED_LIST_IDX  1
#define NEXT_BUFF_IDX              2
#define LAST_POS_IDX               3
#ifdef RCDEBUG
#define ALLOCATING_EE              4
#define LOG_CHILDS_IDX             5
#define LOG_OBJECTS_IDX            6
#define USED_IDX                   7
#endif

typedef struct BUFFHDR BUFFHDR;
struct BUFFHDR {
  uint *pos;
  uint *limit;
  uint *start;
  uint *currBuff;
};

GCEXPORT void gcBuffConditionalLogHandle(ExecEnv *ee, GCHandle *h);
GCEXPORT void gcBuffLogWord(ExecEnv *ee, BUFFHDR *bh, uint w);
GCEXPORT void gcBuffLogNewHandle(ExecEnv *ee, GCHandle *h);


/*******************************************************************************
*
* Thread specific GC block
*
* It conatains the create, uodate and snoop buffers.
*
* Also it contains the thread GC state and allocation lists.
*/
struct GCTHREADBLK {
  bool      gcInited;
  bool      gcSuspended;
  bool      cantCoop;
  bool      snoop;
  int       stage;
  int       stageCooperated;
  BUFFHDR   updateBuffer;
  BUFFHDR   createBuffer;
  BUFFHDR   snoopBuffer;

  ALLOCLIST allocLists[ N_BINS ];
#ifdef RCDEBUG
  struct {
    int nBytesAllocatedInCycle;
    int nRefsAllocatedInCycle;
    int nNewObjectUpdatesInCycle;
    int nOldObjectUpdatesInCycle;
  } dbg;
#endif // RCDEBUG
};

typedef struct SAVEDALLOCLISTS {
  struct SAVEDALLOCLISTS *pNext;
  ALLOCLIST allocLists[ N_BINS ];
} SAVEDALLOCLISTS;

/***********************************************************************************
*
* Global GC block
*
* GCHS4 is defined as zero so that the GC is in this state when the system
* is initialized.
*/
enum GCSTAGE { GCHS1=1, GCHS2=2, GCHS3=3, GCHS4=0, GCHSNONE=0x12345678};

#define N_GC_STAGES 4

enum GCTYPE { GCT_TRACING=0, GCT_RCING=1 };

#define N_SAMPLES 4

struct GCVAR {
  bool           initialized;
  bool           gcActive;
  int            iCollection;
  int            requestPhase;
  int            collectionType;
  int            nextCollectionType;

  // triggering
  bool           memStress;
  bool           usrSyncGC;
  int            gcTrigHigh;
  int            runHist[2][N_SAMPLES];
  
  ExecEnv*       ee;
  sys_thread_t*  sys_thread; 
  int            stage;
  uint*          createBuffList;
  uint*          updateBuffList;
  uint*          snoopBuffList;
  uint*          deadThreadsCreateBuffList;
  uint*          deadThreadsUpdateBuffList;
  uint*          deadThreadsSnoopBuffList;
  uint*          deadThreadsReinforceBuffList;
  uint*          reinforceBuffList;
  GCHandle**     tempReplicaSpace;
  H1BIT_BMP      localsBmp;
  H2BIT_BMP      rcBmp;
  H1BIT_BMP      zctBmp;
  BUFFHDR        zctBuff;
  BUFFHDR        nextZctBuff;
  BUFFHDR        tmpZctBuff;
  BUFFHDR        uniqueLocalsBuff;
  BUFFHDR        preAllocatedBuffers[2];
  int            nPreAllocatedBuffers;
  GCHandle**     zctStack;
  GCHandle**     zctStackSp;
  GCHandle**     zctStackTop;
  sys_mon_t*     gcMon;
  sys_mon_t*     requesterMon;
  SAVEDALLOCLISTS *pListOfSavedAllocLists;

  // chunk mgmt
  uint nAllocatedChunks;
  uint nChunksAllocatedRecentlyByUser;
  uint nUsedChunks;
  uint nFreeChunks;

  // settable options
  struct {
    int recommendOnlyRCGC;
    int useOnlyRCGC;
    int useOnlyTracingGC;
    int listBlkWorth;
    int userBuffTrig;
    int initialHighTrigMark;
    int lowTrigDelta;
    int raiseTrigInc;
    int lowerTrigDec;
    int uniPrio;
    int multiPrio;
  } opt;

#ifdef RCDEBUG

  struct {
    // running totals
    uint nObjectsAllocated;
    uint nObjectsFreed;

    uint nBytesAllocated;
    uint nBytesFreed;

    uint nRefsAllocated;
    uint nRefsFreed;

    uint nOldObjectUpdates;
    uint nNewObjectUpdates;
    uint nLoggedUpdates;
    uint nLoggedSlots;
    uint nStuckCounters;

    // from prev to curr cycle
    uint nPendInCycle;
    uint nFreeCyclesBroken;
    uint nDeadUpdateObjects;
    uint nDeadUpdateChilds;
    uint nDeadCreateObjects;
    uint nDeadReinforceObjects;
    uint nDeadReinforceChilds;
    uint nDeadSnooped;
  } dbgpersist;

  struct {
    uint nHS1Threads;
    uint nHS2Threads;
    uint nHS3Threads;
    uint nHS4Threads;

    uint nHS1CoopThreads;
    uint nHS2CoopThreads;
    uint nHS3CoopThreads;
    uint nHS4CoopThreads;

    // update logs
    uint nUpdateObjects;
    uint nUpdateChilds;
    uint nActualUpdateObjects;
    uint nActualUpdateChilds;
    uint nUpdateDuplicates;
    uint nUpdate2ZCT;
    uint nActualCyclesBroken;

    // update logs, for reinforcement
    uint nReinforceObjects;
    uint nReinforceChilds;
    uint nActualReinforceObjects;
    uint nActualReinforceChilds;

    // create logs
    uint nCreateObjects;
    uint nActualCreateObjects;
    uint nCreateDel;

    // same checks, during RC updating
    uint nUpdateRCObjects;
    uint nUpdateRCChilds;
    uint nUpdateRCDuplicates;
    uint nCreateRCObjects;

    // more RC updating...
    uint nDetermined;
    uint nUndetermined;

    // roots
    uint nLocals;
    uint nGlobals;
    uint nSnooped;
    uint nActualSnooped;

    // freeing
    uint nInZct;
    uint nRecursiveDel;
    uint nFreedInCycle;
    uint nRecursivePend;
    uint nBytesAllocatedInCycle;
    uint nBytesFreedInCycle;
    uint nRefsAllocatedInCycle;
    uint nRefsFreedInCycle;

    // tracing stuff
    uint nTracedInCycle;

    // counters
    uint nStuckCountersInCycle;

    // updates
    int nNewObjectUpdatesInCycle;
    int nOldObjectUpdatesInCycle;
  } dbg;
#endif // RCDEBUG
};


/***********************************************************************************
*
* GC Exports
*/
GCEXPORT void  gcGetInfo( uint *pUc, uint *pFc, uint *pAc, int *iGc );
GCEXPORT void  gcBuffSlowConditionalLogHandle( ExecEnv *ee, GCHandle *h);
GCEXPORT void  gcBuffAllocAndLink( ExecEnv *ee, BUFFHDR *bh);
GCEXPORT void  gcRequestSyncGC(void);
GCEXPORT void  gcRequestAsyncGC();
GCEXPORT void  gcInit(int nMegs);
GCEXPORT void  gcInstallBlk(ExecEnv* ee);
GCEXPORT void  gcUninstallBlk(ExecEnv* ee);
GCEXPORT bool  gcNonNullValidHandle( GCHandle *h);
GCEXPORT bool  gcValidHandle( GCHandle *h);
GCEXPORT void  gcThreadAttach(ExecEnv *ee);
GCEXPORT void  gcThreadDetach(ExecEnv *ee);
GCEXPORT void  gcThreadCooperate(ExecEnv *ee);

extern struct BLKVAR     blkvar;
extern struct CHKCONV    chkconv;


#endif /* __RCGC__ */

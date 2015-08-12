\begin{rawcfig}{rchub.c}
/*
 * File:    rcbmp.c
 * Aurhor:  Yossi Levanoni
 * Purpose: Includes all of the allocator and collector into a single
 *          translation unit.
 */
#define GCINTERNAL

#define gcUnhand(h)   ((JHandle**)(((char*)h)+sizeof(GCHandle)))
#define gcRehand(obj) ((JHandle*)(((char*)obj)-sizeof(GCHandle)))

#include "rcgc.h"
#include "rcgc_internal.h"
#include "../../../win32/hpi/include/threads_md.h"

struct BLKVAR   blkvar;
struct CHKCONV  chkconv;

static struct CHUNKVAR   chunkvar;
static struct GCVAR      gcvar; 

#include "mok_win32.c"
#include "rcbmp.c"
#include "rcblkmgr.c"
#include "rcchunkmgr.c"
#include "rcgc.c"

\end{verbatim}
\end{rawcfig}

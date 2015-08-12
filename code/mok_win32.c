/* File name: mok_win32.c
 * Author:    Yossi Levaoni
 * Purpose:   Win32 abstraction layer
 */

/*
 * Memory 
 *
 */
/* Advanced */

#define WIN32PGGRANULE (64*1024)

void* mokMemReserve(void *starting_at_hint, unsigned sz )
{
  void *p = VirtualAlloc( starting_at_hint, sz, MEM_RESERVE, PAGE_READWRITE );
  sysAssert( sz );
  sysAssert( p );
  return p;
}

void mokMemUnreserve( void *start, unsigned sz )
{
  BOOL res;
  mokMemDecommit( start, sz );
  res = VirtualFree( start, 0, MEM_RELEASE );
  sysAssert( res );
  
}

void* mokMemCommit( void *start, unsigned sz, bool zero_out )
{
  void *p = VirtualAlloc( start, sz, MEM_COMMIT, PAGE_READWRITE );
  sysAssert( start );
  sysAssert( sz );
  sysAssert( p );
  return p;
}

void mokMemDecommit( void *start, unsigned sz )
{
  BOOL res;
  sysAssert( start );
  sysAssert( sz );
  res = VirtualFree( start, sz, MEM_DECOMMIT );
  sysAssert( res );
}

/* C style */
void* mokMalloc( unsigned sz, bool zero_out )
{
  void *p;
  sysAssert( sz );
  p = malloc( sz );
  sysAssert( p );
  if (zero_out)
    memset( p, 0, sz );
  return p;
}

void mokFree( void * p)
{
  sysAssert( p );
  free( p );
}

/* zero out */
void mokMemZero( void *start, unsigned sz )
{
  mokMemDecommit( start, sz );
  mokMemCommit( start, sz, TRUE );
}


/*
 * YLRC --
 * 
 * The functions:
 *   
 *   mokThreadSuspendForGC
 *   mokThreadResumeForGC
 *
 * are needed for on the fly garbage collection
 *
 */
void mokThreadSuspendForGC(sys_thread_t *tid)
{
  sysAssert( tid != sysThreadSelf() );

  if (SuspendThread(tid->handle) == 0xffffffffUL) {
    jio_printf( "sysThreadSuspendForGC: SuspendThread failed" );
    __asm { int 3 }
  }
  {
    CONTEXT context;
    DWORD *esp = (DWORD *)tid->regs;

    context.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
    if (!GetThreadContext(tid->handle, &context)) {
      jio_printf( "sysThreadSuspendForGC: GetThreadContext failed" );
      __asm { int 3 }
    }
    *esp++ = context.Eax;
    *esp++ = context.Ebx;
    *esp++ = context.Ecx;
    *esp++ = context.Edx;
    *esp++ = context.Esi;
    *esp++ = context.Edi;
    *esp   = context.Ebp;
  }
}

void mokThreadResumeForGC(sys_thread_t *tid)
{
  sysAssert( tid != sysThreadSelf() );

  if (ResumeThread(tid->handle) == 0xffffffffUL) {
    printf( "sysThreadResumeForGC: ResumeThread failed" );
    __asm { int 3 }
  }
}


typedef struct xxpair {
  int (*func)(sys_thread_t*, void*);
  void *param;
} xxpair;

static int  _mokThreadEnumerateOverHelper( sys_thread_t *thrd, xxpair* xx)
{
  int res;
  ExecEnv *ee;
  if (thrd == gcvar.sys_thread) return SYS_OK;
  ee = SysThread2EE( thrd );
  if (!ee->gcblk.gcInited) return SYS_OK;
  res = xx->func( thrd, xx->param );
  return res;
}

int mokThreadEnumerateOver( int(*f)(sys_thread_t *, void*), void *param)
{
  xxpair xx;
  int ret;

  xx.func = f;
  xx.param = param;

#ifdef RCDEBUG
  {
    sys_thread_t* self = sysThreadSelf();
    mokAssert( self == gcvar.sys_thread );
  }
#endif
  ret = sysThreadEnumerateOver( _mokThreadEnumerateOverHelper, &xx );
  return ret;
}




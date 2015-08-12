/*
 * File:    rcbmp.c
 * Aurhor:  Yossi Levanoni
 * Purpose: 1 bit per word and 2 bit per word bitmap implementation.
 */
#ifdef RCNOINLINE

#include <stdio.h>

#include "rcgc.h"


#define PAGE_SIZE 4096
#define ROUND_PAGE(u) (((u)&~(PAGE_SIZE-1))+PAGE_SIZE)

/*
 * BIT FIELD MANIPULATION
 */
#define MAKE_MASK(shift,length)           (((1<<(length))-1)<<(shift))
#define GET_BIT_FIELD(w,shift,length)     (((w)&MAKE_MASK(shift,length))>>shift)

#define OR_BIT_FIELD(w,v,shift)           do{ (w) = (w) | ((v)<<(shift)); \
                                          }while(0)

#define CLEAR_BIT_FIELD(w,shift,length)   do{ (w) = (w) & (~MAKE_MASK(shift,length)); \
                                          }while(0)

#define SET_BIT_FIELD(w,v,shift,length)   do{CLEAR_BIT_FIELD(w,shift,length);\
                                          OR_BIT_FIELD(w,v,shift);\
                                          }while(0)

/*
 * Specify (log) allignment of handles.
 */
#define H_GRAIN_BITS      3
/*
 * Field selector bits.  The next 3 bits select
 * the bit inside the bmp word.  there
 * are 8 options.
 */
#define H1B_FS_BITS       3
/*
 * The rest of the bits handle selects
 * the bmp byte inside the bitmap.
 */
#define H1B_BS_BITS       (32-(H_GRAIN_BITS+H1B_FS_BITS))
#define H1B_NON_BS_BITS   (H_GRAIN_BITS+H1B_FS_BITS)


#define H1BIT_BYTE(entry,h)   (byte*)(((uint)h>>H1B_NON_BS_BITS) + (byte*)entry)

void H1BIT_Set(byte* entry, unsigned h)
{
  /* entry address into the bitmap.*/
  byte *bbmp = H1BIT_BYTE(entry, h);
  byte v = *bbmp;
  uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS, H1B_FS_BITS );
  OR_BIT_FIELD(v, 1, field_selector );
  *bbmp = v;
}


void H1BIT_Clear(byte* entry, unsigned h)
{
  /* entry address into the bitmap.*/
  byte *bbmp = H1BIT_BYTE(entry, h);
  byte v = *bbmp;
  uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS, H1B_FS_BITS );
  CLEAR_BIT_FIELD(v, field_selector, 1 );
  *bbmp = v;
}

void H1BIT_ClearByte(byte* entry, unsigned h)
{
  byte *bbmp = H1BIT_BYTE(entry, h);
  *bbmp = 0;
}

void H1BIT_Put(byte* entry, unsigned h, unsigned val)
{
  mokAssert( val <= 1);
  if (val==0)
    H1BIT_Clear(entry, h);
  else
    H1BIT_Set(entry, h);
}

byte H1BIT_Get(byte* entry, unsigned h)
{
  /* entry address into the bitmap.*/
  byte  *bbmp = H1BIT_BYTE(entry, h);
  byte  v = *bbmp;
  uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS, H1B_FS_BITS );
  uint res = GET_BIT_FIELD(v, field_selector, 1 );
  return res;
}

/*
 * Create a new 1-bit per handle BMP with the handles starting
 * at address `rep_addr' and the handles area being `rep_size'
 * bytes long.
 */
void H1BIT_Init(H1BIT_BMP* bmp, unsigned* rep_addr, unsigned rep_size )
{
  /* each bit in the bimtap represents a handle, which
   * takes 2^H_GRAIN_BITS bytes.  So a byte in the
   * bitmap repreesnts 2^(H_GRAIN_BITS+3) bytes in the
   * handle space.
        */
  bmp->bmp_size = rep_size >> (H_GRAIN_BITS+3);
  bmp->bmp_size = ROUND_PAGE( bmp->bmp_size );
  bmp->bmp = (byte*)mokMemReserve( NULL, bmp->bmp_size );
  mokMemCommit( bmp->bmp, bmp->bmp_size, true );
  bmp->rep_addr = (byte*)rep_addr;
  bmp->entry = bmp->bmp - (((unsigned)rep_addr)>>H1B_NON_BS_BITS);
}


/******************************************************************** 

Implementation of a 2 bit per handle BMP.

Layout of a handle:

| 31 --------- 5 | 4 -- 3 | 2 - 0 |
|      BS        |   FS   |  Z    |

Where:

-- Z: these bits are always zero (because handles are 8-byte aligned).
-- FS: Field Select.  Selects a 2-bit field in a byte of the
       bitmap.  The selector is 4 bits wide cause there are 4
       possibilies.
-- BS: Word selector, relatively to the beginning of the heap, this is
       the bitmap word selector.

**********************************************************************/

/*
 * Field selector bits.  There are 16 options.  If the
 * selector value is s (with 0<=s<=15), then the field
 * begins at bit s*2.
 */
#define H2B_FS_BITS       2
/*
 * The rest of the handle selects
 * the bmp word inside the bitmap.
 */
#define H2B_BS_BITS       (32-(H_GRAIN_BITS+H2B_FS_BITS))
#define H2B_NON_BS_BITS   (32-H2B_BS_BITS)

#define H2BIT_BYTE(entry,h)   ((((uint)h)>>H2B_NON_BS_BITS) + entry)


void H2BIT_Put(byte* entry, unsigned h, unsigned val)
{
  /* entry address into the bitmap.*/
  byte *bbmp = H2BIT_BYTE(entry, h);
  byte v = *bbmp;
  /* we include the third least bit in the selector (it is always zero). */
  /* to get selection of 0,2,4,...,30, and not 0,1,...15.                */
  uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );

  mokAssert( field_selector%2 == 0);
  mokAssert( field_selector <= 30 );
  mokAssert( val <= 3);

  SET_BIT_FIELD(v, val, field_selector, 2);
  *bbmp = v;
}


void H2BIT_Clear(byte* entry, unsigned h)
{
  /* entry address into the bitmap.*/
  byte *bbmp = H2BIT_BYTE(entry, h);
  byte v = *bbmp;
  /* we indlude the upper zero in the selector */
  uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );

  mokAssert( field_selector%2 == 0);
  mokAssert( field_selector <= 30 );

  CLEAR_BIT_FIELD(v, field_selector, 2);
  *bbmp = v;
}


void H2BIT_Stuck(byte* entry, unsigned h)
{
  /* entry address into the bitmap.*/
  byte *bbmp = H2BIT_BYTE(entry, h);
  byte v = *bbmp;
  /* we indlude the upper zero in the selector */
  uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );

  mokAssert( field_selector%2 == 0);
  mokAssert( field_selector <= 30 );

  OR_BIT_FIELD(v, 3, field_selector );
  *bbmp = v;
}

byte  H2BIT_Get(byte* entry, unsigned h)
{
  /* entry address into the bitmap.*/
  byte *bbmp = H2BIT_BYTE(entry, h);
  byte v = *bbmp;
  byte  res;
  /* we indlude the upper zero in the selector */
  uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );

  mokAssert( field_selector%2 == 0);
  mokAssert( field_selector <= 30 );

  res = GET_BIT_FIELD(v, field_selector, 2 );
  return res;
}

#ifdef RCDEBUG
#pragma optimize( "", off )
void _forceIncSanityCheck(byte *entry, unsigned h, int f)
{
  int nextF = (f==3) ? 3 : f+1;

  mokAssert( H2BIT_Get(entry,h) == nextF );
  if (f==2) {
    gcvar.dbg.nStuckCountersInCycle++;
  }
}
#pragma optimize( "", on )
#endif

void H2BIT_Inc(byte* entry, unsigned h)
{
  /* entry address into the bitmap.*/
  byte *bbmp = H2BIT_BYTE(entry, h);
  byte val = *bbmp;
  uint f;
  /* we indlude the upper zero in the selector */
  uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );

  mokAssert( field_selector%2 == 0);
  mokAssert( field_selector <= 30 );

  f = GET_BIT_FIELD(val, field_selector, 2);
  mokAssert( f<= 3 );
  if (f<3) { /* STUCK remains STUCK */
    SET_BIT_FIELD( val, f+1, field_selector, 2); 
    *bbmp = val;
  }
#ifdef RCDEBUG
  _forceIncSanityCheck( entry, h, f);
#endif
}


byte H2BIT_IncRV(byte* entry, unsigned h)
{
  /* entry address into the bitmap.*/
  byte *bbmp = H2BIT_BYTE(entry, h);
  byte val = *bbmp;
  uint f;
  /* we indlude the upper zero in the selector */
  uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );

  mokAssert( field_selector%2 == 0);
  mokAssert( field_selector <= 30 );

  f = GET_BIT_FIELD(val, field_selector, 2);
  mokAssert( f<= 3 );
  if (f<3) { /* STUCK remains STUCK */
    SET_BIT_FIELD( val, f+1, field_selector, 2); 
    *bbmp = val;
  }
#ifdef RCDEBUG
  _forceIncSanityCheck( entry, h, f);
#endif
  return f;
}

byte H2BIT_Dec(byte* entry, unsigned h)
{
  /* entry address into the bitmap.*/
  byte *bbmp = H2BIT_BYTE(entry, h);
  byte val = *bbmp;
  uint f;
  /* we include the upper zero in the selector */
  uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );

  mokAssert( field_selector%2 == 0);
  mokAssert( field_selector <= 30 );

  f = GET_BIT_FIELD(val, field_selector, 2);
  mokAssert( f<= 3 );
  mokAssert( f>= 1 ); /* we should never go below zero */
  if (f<3) { /* STUCK remains STUCK */
    SET_BIT_FIELD( val, f-1, field_selector, 2);
    *bbmp = val;
    mokAssert( H2BIT_Get(entry,h)== f-1 );
  }
  return f;
}

/*
 * Create a new 2-bit per handle BMP with the handles starting
 * at address `rep_addr' and the handles area being `rep_size'
 * bytes long.
 */
void H2BIT_Init(H2BIT_BMP* bmp, unsigned* rep_addr, unsigned rep_size )
{
  /* each 2 bits in the bimtap represents a handle, which
   * takes 2^H_GRAIN_BITS bytes.  So a byte in the
   * bitmap repreesnts 2^(H_GRAIN_BITS+2) bytes in the
   * handle space.
        */
  bmp->bmp_size = rep_size >> (H_GRAIN_BITS+2);
  bmp->bmp_size = ROUND_PAGE( bmp->bmp_size );
  bmp->bmp = (byte*)mokMemReserve( NULL, bmp->bmp_size );
  mokMemCommit( bmp->bmp, bmp->bmp_size, true );
  bmp->rep_addr = (byte*)rep_addr;
  bmp->entry = bmp->bmp - (((unsigned)rep_addr)>>H2B_NON_BS_BITS);
}

char * write_bits(unsigned x)
{
  char *s = (char *)mokMalloc(33, false);
  unsigned i = 1<<31;
  int j=0;
  for (;j<32;j++) {
    s[j] = x&i ? '1' : '0';
    i >>= 1;
  }
  s[j] = '\0';
  return s;
}

void testBitFields(void)
{
  int shift, length;
  unsigned m=0, val;

  while (1) {
    jio_printf("Enter shift length val, please: ");
    scanf("%d %d %x", &shift, &length, &val );
    SET_BIT_FIELD(m, val, shift, length);
    jio_printf("m=(%x)%s field=(%x)%s\n", m, write_bits(m),
           GET_BIT_FIELD(m, shift, length), write_bits(GET_BIT_FIELD(m, shift, length)) );
  }
}


typedef struct HandleTAG { unsigned h1, h2; } Handle;

H2BIT_BMP Bmp;

#define N_HANDLES 10000

void test2BitBmp(void)
{
  int i,j;
  Handle* handleSpace = (Handle*)mokMalloc( N_HANDLES*sizeof(Handle), false );
  H2BIT_BMP *bmp = &Bmp;
        
  H2BIT_Init( bmp, (unsigned*)handleSpace, N_HANDLES*sizeof(Handle) );
  for (i=0; i<2 ;i++) {
    for (j=0; j<N_HANDLES; j++) {
      uint v = H2BIT_Get( bmp->entry, (unsigned)&handleSpace[j] );
      if (v != (uint)i) 
        jio_printf("Bad RC for j=%d, val=%x\n", j, v );
      else
        jio_printf("Good RC for j=%d, val=%x\n", j, v );
      H2BIT_Inc(  bmp->entry, (unsigned)&handleSpace[j] );
    }
  }
  for (i=2; i>=0 ;i--) {
    for (j=0; j<N_HANDLES; j++) {
      uint v = H2BIT_Get( bmp->entry, (unsigned)&handleSpace[j] );
      if (v != (uint)i ) 
        jio_printf("Bad RC for j=%d, val=%x exoect=%i\n", j, v, i );
      else
        jio_printf("Good RC for j=%d, val=%x\n", j, v );
      H2BIT_Dec(  bmp->entry, (unsigned)&handleSpace[j] );
    }
  }
}


#endif /* RCNOINLINE */
/**/

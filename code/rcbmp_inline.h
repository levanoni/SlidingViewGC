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


#define H1BIT_BYTE(entry,h)   (byte*)(((uint)(h)>>H1B_NON_BS_BITS) + (byte*)entry)

#define H1BIT_Set(entry,h)\
do {\
	/* entry address into the bitmap.*/\
	byte *bbmp = H1BIT_BYTE((entry), (h));\
	byte v = *bbmp;\
	uint field_selector = GET_BIT_FIELD( (h), H_GRAIN_BITS, H1B_FS_BITS );\
	OR_BIT_FIELD(v, 1, field_selector );\
	*bbmp = v;\
} while(0)



#define H1BIT_Clear(entry,h)\
do {\
	/* entry address into the bitmap.*/\
	byte *bbmp = H1BIT_BYTE((entry), (h));\
	byte v = *bbmp;\
	uint field_selector = GET_BIT_FIELD( (h), H_GRAIN_BITS, H1B_FS_BITS );\
	CLEAR_BIT_FIELD(v, field_selector, 1 );\
	*bbmp = v;\
} while(0)

#define H1BIT_Put(entry, h, val)\
do {\
	mokAssert( val <= 1);\
	if (val==0)\
		H1BIT_Clear(entry, h);\
	else\
		H1BIT_Set(entry, h);\
} while(0)


#define H1BIT_GetInlined(entry, h, __res_var__)\
do {\
	/* entry address into the bitmap.*/\
	byte  *bbmp = H1BIT_BYTE(entry, h);\
	byte  v = *bbmp;\
	uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS, H1B_FS_BITS );\
	uint res = GET_BIT_FIELD(v, field_selector, 1 );\
	return res;\
} while(0)

/*
 * Create a new 1-bit per handle BMP with the handles starting
 * at address `rep_addr' and the handles area being `rep_size'
 * bytes long.
 */
	/* each bit in the bimtap represents a handle, which
	* takes 2^H_GRAIN_BITS bytes.  So a byte in the
	* bitmap repreesnts 2^(H_GRAIN_BITS+3) bytes in the
	* handle space.
	*/
#define H1BIT_Init( __bmp, __rep_addr, __rep_size )\
do {\
	(__bmp)->bmp_size = (__rep_size) >> (H_GRAIN_BITS+3);\
	(__bmp)->bmp_size = ROUND_PAGE( (__bmp)->bmp_size );\
	(__bmp)->bmp = (byte*)mokMemReserve( NULL, (__bmp)->bmp_size );\
	mokMemCommit( (__bmp)->bmp, (__bmp)->bmp_size, true );\
	(__bmp)->rep_addr = (byte*)(__rep_addr);\
	(__bmp)->entry = (__bmp)->bmp - (((unsigned)(__rep_addr))>>H1B_NON_BS_BITS);\
} while (0) 


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


#define H2BIT_Put(entry,h,val)\
do {\
	/* entry address into the bitmap.*/\
	byte *bbmp = H2BIT_BYTE(entry, h);\
	byte v = *bbmp;\
	/* we include the third least bit in the selector (it is always zero). */\
	/* to get selection of 0,2,4,...,30, and not 0,1,...15.                */\
	uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );\
\
	mokAssert( field_selector%2 == 0);\
	mokAssert( field_selector <= 30 );\
	mokAssert( val <= 3);\
\
	SET_BIT_FIELD(v, val, field_selector, 2);\
	*bbmp = v;\
} while(0)


#define H2BIT_Clear(entry,h)\
do {\
	/* entry address into the bitmap.*/\
	byte *bbmp = H2BIT_BYTE(entry, h);\
	byte v = *bbmp;\
	/* we indlude the upper zero in the selector */\
	uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );\
\
	mokAssert( field_selector%2 == 0);\
	mokAssert( field_selector <= 30 );\
\
	CLEAR_BIT_FIELD(v, field_selector, 2);\
	*bbmp = v;\
} while (0)


#define H2BIT_Stuck(entry, h)\
do {\
	/* entry address into the bitmap.*/\
	byte *bbmp = H2BIT_BYTE(entry, h);\
	byte v = *bbmp;\
	/* we indlude the upper zero in the selector */\
	uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );\
\
	mokAssert( field_selector%2 == 0);\
	mokAssert( field_selector <= 30 );\
\
	OR_BIT_FIELD(v, 3, field_selector );\
	*bbmp = v;\
} while (0)

#define H2BIT_GetInlined( entry, h, __res_var__)\
do {\
	/* entry address into the bitmap.*/\
	byte *bbmp = H2BIT_BYTE(entry, h);\
	byte v = *bbmp;\
	byte  res;\
	/* we indlude the upper zero in the selector */\
	uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );\
\
	mokAssert( field_selector%2 == 0);\
	mokAssert( field_selector <= 30 );\
\
	__res_var__ = GET_BIT_FIELD(v, field_selector, 2 );\
} while(0)

#define H2BIT_Inc(entry, h)\
do {\
	/* entry address into the bitmap.*/\
	byte *bbmp = H2BIT_BYTE(entry, h);\
	byte val = *bbmp;\
	uint f;\
	/* we indlude the upper zero in the selector */\
	uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );\
\
	mokAssert( field_selector%2 == 0);\
	mokAssert( field_selector <= 30 );\
	f = GET_BIT_FIELD(val, field_selector, 2);\
	mokAssert( f<= 3 );\
	if (f<3) { /* STUCK remains STUCK */\
		SET_BIT_FIELD( val, f+1, field_selector, 2); \
		*bbmp = val;\
	}\
} while (0)


#define H2BIT_IncRVInlined( __entry, __h, __res_var__)\
do {\
	/* entry address into the bitmap.*/\
	byte *bbmp = H2BIT_BYTE(__entry, __h);\
	byte val = *bbmp;\
	uint f;\
	/* we indlude the upper zero in the selector */\
	uint field_selector = GET_BIT_FIELD( __h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );\
\
	mokAssert( field_selector%2 == 0);\
	mokAssert( field_selector <= 30 );\
\
	f = GET_BIT_FIELD(val, field_selector, 2);\
	mokAssert( f<= 3 );\
	if (f<3) { /* STUCK remains STUCK */\
		SET_BIT_FIELD( val, f+1, field_selector, 2); \
		*bbmp = val;\
	}\
	__res_var__ = f;\
} while(0)

#define H2BIT_Dec(entry,h)\
{\
	/* entry address into the bitmap.*/\
	byte *bbmp = H2BIT_BYTE(entry, h);\
	byte val = *bbmp;\
	uint f;\
	/* we include the upper zero in the selector */\
	uint field_selector = GET_BIT_FIELD( h, H_GRAIN_BITS-1, H2B_FS_BITS+1 );\
\
	mokAssert( field_selector%2 == 0);\
	mokAssert( field_selector <= 30 );\
\
	f = GET_BIT_FIELD(val, field_selector, 2);\
	mokAssert( f<= 3 );\
	mokAssert( f>= 1 ); /* we should never go below zero */\
	if (f<3) { /* STUCK remains STUCK */\
		SET_BIT_FIELD( val, f-1, field_selector, 2);\
		*bbmp = val;\
	}\
	return f;\
}

/*
 * Create a new 2-bit per handle BMP with the handles starting
 * at address `rep_addr' and the handles area being `rep_size'
 * bytes long.
 */
#define H2BIT_Init( __bmp, __rep_addr, __rep_size )\
do {\
	/* each 2 bits in the bimtap represents a handle, which\
	* takes 2^H_GRAIN_BITS bytes.  So a byte in the\
	* bitmap repreesnts 2^(H_GRAIN_BITS+2) bytes in the\
	* handle space.\
	*/\
	(__bmp)->bmp_size = (__rep_size) >> (H_GRAIN_BITS+2);\
	(__bmp)->bmp_size = ROUND_PAGE( (__bmp)->bmp_size );\
	(__bmp)->bmp = (byte*)mokMemReserve( NULL, (__bmp)->bmp_size );\
	mokMemCommit( (__bmp)->bmp, (__bmp)->bmp_size, true );\
	(__bmp)->rep_addr = (byte*)(__rep_addr);\
	(__bmp)->entry = (__bmp)->bmp - (((unsigned)(__rep_addr))>>H2B_NON_BS_BITS);\
} while(0);


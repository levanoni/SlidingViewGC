/*
 * File:    ylrc_protocol.h
 * Author:  Mr. Yossi Levanoni
 * Purpose: Definition of the write barrier
 */
#ifndef YLRC

#define YLRC

struct execenv;
typedef struct execenv ExecEnv;

void gcDo_gcupdate(ExecEnv *ee, void *_h, void *_slot, void *_newval );
void gcDo_gcupdate_array(ExecEnv *ee, void *_arrayh, void* _slot, void *newval);
void gcDo_gcupdate_class(ExecEnv*  ee, ClassClass* cb, void *_slot, void *_newval );
void gcDo_gcupdate_jvmglobal(ExecEnv* ee, void* _global, void *_newval );
void gcDo_gcupdate_static( ExecEnv* ee, struct fieldblock* fb, void* slot, void* _newval );


#define gcupdate(ee,_h,_slot,_newval ) \
  gcDo_gcupdate(ee,_h, _slot,_newval )

#define gcupdate_array(ee,_arrayh,_slot,newval) \
  gcDo_gcupdate_array(ee,_arrayh, _slot,newval)
#define gcupdate_class(ee,cb,_slot,_newval ) \
  gcDo_gcupdate_class(ee,cb,_slot,_newval )
#define gcupdate_jvmglobal(ee,_global,_newval ) \
  gcDo_gcupdate_jvmglobal(ee, _global,_newval )
#define gcupdate_static(ee,fb,slot,_newval ) \
  gcDo_gcupdate_static(ee,fb,slot,_newval )

#endif /* ! YLRC */

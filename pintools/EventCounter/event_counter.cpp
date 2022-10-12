// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: BSD-3-Clause
/*BEGIN_LEGAL 
BSD License 

Copyright (c)2022 Intel Corporation. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
#include <iostream>
#include <string>
#include <sstream>

#include "pin.H"
#include "dcfg_pin_api.H"
#include "control_manager.H"
#if defined(SDE_INIT)
#  include "sde-init.H"
#  include "sde-control.H"
#endif
#if defined(PINPLAY)
#if defined(SDE_INIT)
#include "sde-pinplay-supp.H"
#endif
#include "pinplay.H"
static PINPLAY_ENGINE *pinplay_engine;
#endif
#include "instlib.H"
#include "ialarm.H"
#include "atomic.hpp"
static PINPLAY_ENGINE pp_pinplay_engine;

// This tool demonstrates how to register a handler for various
// events reported by SDE's controller module.
// At every event, global (all threads) instruction count is
// printed along with the triggering thread number and PC.

// Optional feature:
// Monitor occurrences of up to three PCs. These PC can be provided
// using the three Knob*PC knobs below.
// A global (all threads) count of the monitored PCs is output on
// certain events.

using namespace INSTLIB;

using namespace CONTROLLER;

static CACHELINE_COUNTER global_ins_counter  = {0,0};

static PIN_LOCK output_lock;

static FILE  **out_fp=NULL;
static UINT64  *thread_icount;

struct AddrData {
  ADDRINT addr;
  CACHELINE_COUNTER global_addr_counter  = {0,0};
  UINT64  *thread_addrcount;
};

struct AddrData *AddrDataArray;

CONTROL_MANAGER * control_manager = NULL;
static CONTROLLER::CONTROL_MANAGER control("pinplay:");

#if defined(SDE_INIT)
  CONTROL_ARGS args("","pintool:control:pinplay");
#else
  CONTROL_ARGS args("pinplay:","pintool:control:pinplay");
#endif

#if ! defined(SDE_INIT)
#if defined(PINPLAY)
#define KNOB_LOG_NAME  "log"
#define KNOB_REPLAY_NAME "replay"
#define KNOB_FAMILY "pintool:pinplay-driver"

KNOB_COMMENT pinplay_driver_knob_family(KNOB_FAMILY, "PinPlay Driver Knobs");

KNOB<BOOL>KnobPinPlayReplayer(KNOB_MODE_WRITEONCE, KNOB_FAMILY,
                       KNOB_REPLAY_NAME, "0", "Replay a pinball");
KNOB<BOOL>KnobPinPlayLogger(KNOB_MODE_WRITEONCE,  KNOB_FAMILY,
                     KNOB_LOG_NAME, "0", "Create a pinball");
KNOB<BOOL> KnobHandleXsave(KNOB_MODE_WRITEONCE, "pintool", "handle_xsave", "1",
        "Handle xsave to be like SNB architecture");
#endif
#endif
KNOB<UINT32> KnobThreadCount(KNOB_MODE_WRITEONCE, "pintool", "thread_count",
         "0", "Number of threads");
KNOB<string> KnobPrefix(KNOB_MODE_WRITEONCE, "pintool", "prefix", "",
        "Prefix for output event_icount.tid.txt files");
KNOB<ADDRINT> KnobWatchAddr(KNOB_MODE_APPEND, "pintool", "watch_addr", "",
                  "Address to watch");
KNOB<BOOL>KnobExitOnStop(KNOB_MODE_WRITEONCE,"pintool", "exit_on_stop", "0",
                       "Exit on Sim-End");


static void PrintAddrcount(THREADID tid, string eventstr)
{
 UINT32 numAddrs = KnobWatchAddr.NumberOfValues();
 for (UINT32 a = 0; a < numAddrs ; a++)
 {
   struct AddrData *ad = &AddrDataArray[a];
   cerr << "addr 0x" << hex << ad->addr << " global count " << dec << ad->global_addr_counter._count <<   endl;
  UINT32 tcount = KnobThreadCount; 
    for (UINT32 i = 0; i < tcount; i++)
    {
      UINT64 taddrcount = ad->thread_addrcount[i];
#ifdef TARGET_IA32E
      fprintf(out_fp[i], "%s %lx  global_addrcount: %ld", eventstr.c_str(), ad->addr, ad->global_addr_counter._count);
#else
      fprintf(out_fp[i], "%s %x  global_addrcount: %lld", eventstr.c_str(), ad->addr, ad->global_addr_counter._count);
#endif
    fprintf(out_fp[i], "\n");
#ifdef TARGET_IA32E
      fprintf(out_fp[i], "%s %lx tid: %d addrcount %ld", eventstr.c_str(), ad->addr, i, taddrcount) ;
#else
      fprintf(out_fp[i], "%s %x tid: %d addrcount %lld", eventstr.c_str(), ad->addr, i, taddrcount) ;
#endif
      fprintf(out_fp[i], "\n");
      fflush(out_fp[i]);
    }
 }
}

static void PrintIcount(THREADID tid, string eventstr, UINT32 rid=0)
{
  UINT32 tcount = KnobThreadCount;
  {
    for (UINT32 i = 0; i < tcount; i++)
    {
      UINT64 ticount = thread_icount[i];
      //if(KnobReplayer)
      //  ticount = pinplay_engine.ReplayerGetICount(i);
#ifdef TARGET_IA32E
      fprintf(out_fp[i], "%s global_icount: %ld", eventstr.c_str(), global_ins_counter._count);
#else
      fprintf(out_fp[i], "%s global_icount: %lld", eventstr.c_str(), global_ins_counter._count);
#endif
      if(rid)
      {
        fprintf(out_fp[i], " rid: %d \n", rid);
      }
      else
      {
        fprintf(out_fp[i], "\n");
      }
#ifdef TARGET_IA32E
      fprintf(out_fp[i], "%s tid: %d icount %ld", eventstr.c_str(), i, ticount) ;
#else
      fprintf(out_fp[i], "%s tid: %d icount %lld", eventstr.c_str(), i, ticount) ;
#endif
      if(rid)
      {
        fprintf(out_fp[i], " rid: %d \n", rid);
      }
      else
      {
        fprintf(out_fp[i], "\n");
      }
      fflush(out_fp[i]);
    }
  }
}

VOID Handler(EVENT_TYPE ev, VOID * v, CONTEXT * ctxt, VOID * ip,
             THREADID tid, BOOL bcast)
{
    PIN_GetLock(&output_lock, 1);

    string eventstr;

    switch(ev)
    {
      case EVENT_START:
        eventstr="Sim-Start";
        break;

      case EVENT_WARMUP_START:
        eventstr="Warmup-Start";
        break;

      case EVENT_STOP:
        eventstr="Sim-End";
        break;

     case EVENT_WARMUP_STOP:
        eventstr="Warmup-Stop";
        break;

      case EVENT_THREADID:
        std::cerr << "ThreadID" << endl;
        eventstr="ThreadID";
        break;

      case EVENT_STATS_EMIT:
        std::cerr << "STATS_EMIT" << endl;
        eventstr="STATS_EMIT";
        break;

      default:
        ASSERTX(false);
        break;
    }
    cerr << " global icount " << dec << global_ins_counter._count;
    cerr <<  " " << eventstr << endl;

    PrintIcount(tid, eventstr);
    PrintAddrcount(tid, "\tMarker ");
    if((ev == EVENT_STOP) && KnobExitOnStop)
      PIN_ExitProcess(0);
    PIN_ReleaseLock(&output_lock);
}

VOID LateHandler(EVENT_TYPE ev, VOID * v, CONTEXT * ctxt, VOID * ip,
             THREADID tid, BOOL bcast)
{
    PIN_GetLock(&output_lock, 1);

    string eventstr;

    switch(ev)
    {
      case EVENT_START:
        eventstr="Late-Sim-Start";
        break;

      case EVENT_WARMUP_START:
        eventstr="Late-Warmup-Start";
        break;

      case EVENT_STOP:
        eventstr="Late-Sim-End";
        break;

     case EVENT_WARMUP_STOP:
        eventstr="Late-Warmup-Stop";
        break;

      case EVENT_THREADID:
        std::cerr << "ThreadID" << endl;
        eventstr="Late-ThreadID";
        break;

      case EVENT_STATS_EMIT:
        std::cerr << "STATS_EMIT" << endl;
        eventstr="Late-STATS_EMIT";
        break;

      default:
        ASSERTX(false);
        break;
    }
    cerr << " global icount " << dec << global_ins_counter._count;
    cerr <<  " " << eventstr << endl;
    PIN_ReleaseLock(&output_lock);
}

static VOID ProcessFini(INT32 code, VOID *v)
{
  PrintIcount(0, "Fini ");
  PrintAddrcount(0, "Addr ");
}

// This function is called before every block
// Use the fast linkage for calls
VOID docount(THREADID tid, ADDRINT c)
{
    ATOMIC::OPS::Increment<UINT64>(&global_ins_counter._count, c);
    //global_ins_counter._count+= c;
    thread_icount[tid]+=c;
}


#if ! defined(SDE_INIT)
#if defined(PINPLAY)
static void ChangeEAX(ADDRINT *eax)
{
    // turn off all bits beyond x87/SSE/AVX
    *eax = *eax & 0x7;
}
#endif
#endif

static VOID  AddrBefore(THREADID tid, VOID * v)
{
    struct AddrData * ad = (struct AddrData *) v;
    ATOMIC::OPS::Increment<UINT64>(&(ad->global_addr_counter._count), 1);
    ad->thread_addrcount[tid]+=1;
}


static VOID ThreadStart(THREADID tid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
  if(tid == KnobThreadCount) 
  {
    cerr << "ERROR: Unexpected tid " << tid <<  " check '-thread_count' value " << KnobThreadCount << endl;
    cerr << " (if using replay, provide '-xyzzy -replay:deadlock_timeout 0') " << endl;
    ASSERTX(FALSE);
  }
}

VOID Instruction(INS ins, VOID *v)
{
  UINT32 numAddrs = KnobWatchAddr.NumberOfValues();
  for (UINT32 a = 0; a < numAddrs ; a++)
  {
    if(INS_Address(ins) == AddrDataArray[a].addr)
    {
      INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(AddrBefore),
                       IARG_THREAD_ID,
                       IARG_PTR, (VOID *)&AddrDataArray[a],
                       IARG_END);
    }
  }
}

VOID Trace(TRACE trace, VOID *v)
{
    // Visit every basic block  in the trace
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        // Insert a call to docount for every bbl, passing
        // the number of instructions.
        // IPOINT_ANYWHERE allows Pin to schedule the call
        //  anywhere in the bbl
        // to obtain best performance.
        // Use a fast linkage for the call.
        BBL_InsertCall(bbl, IPOINT_BEFORE, AFUNPTR(docount),
                       IARG_THREAD_ID,
                       IARG_UINT32, BBL_NumIns(bbl),
                       IARG_END);
#if ! defined(SDE_INIT)
#if defined(PINPLAY)
        if (KnobHandleXsave)
        {
          for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
          {
            if ((INS_Category(ins) == XED_CATEGORY_XSAVE) || 
                INS_Category(ins) == XED_CATEGORY_XSAVEOPT ||
                INS_Opcode(ins) == XED_ICLASS_XRSTOR)
            {
                  INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(ChangeEAX),
                    IARG_REG_REFERENCE, REG_GAX,
                    IARG_END);
            }
          }
        }
#endif
#endif
     for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
     {
      Instruction(ins, v);
     }
    }
}

static void OpenThreadFiles()
{
 if(!KnobThreadCount)
 {
   cerr << "ERROR: must use '-thread_count N'\n";
   PIN_ExitProcess(1);
 }

 if(KnobPrefix.Value().empty())
 {
   cerr << "ERROR: must use '-prefix prefix'\n";
   PIN_ExitProcess(1);
 }
 UINT32 tcount = KnobThreadCount;
 thread_icount = new UINT64 [tcount]();
 out_fp = new FILE *[tcount]();
 for (UINT32 i = 0; i < tcount; i++)
 {
    char  fname[100];
    sprintf(fname, "%s.event_icount.%d.txt", KnobPrefix.Value().c_str(), i);
    out_fp[i] = fopen(fname, "w");
    ASSERTX(out_fp[i]);
 }

 UINT32 numAddrs = KnobWatchAddr.NumberOfValues();

 AddrDataArray = new struct AddrData [numAddrs]();
 for (UINT32 a = 0; a < numAddrs ; a++)
 {
   AddrDataArray[a].addr = KnobWatchAddr.Value(a);
   cerr << "Watching addr 0x" << hex << AddrDataArray[a].addr << endl;
   AddrDataArray[a].thread_addrcount = new UINT64 [tcount]();
 }

}
 
INT32 Usage()
{
    cerr << "Tool to output global/per-thread counts at controller events." << endl;
    cerr << "Usage examples:" << endl;
    cerr << "... -thread_count 8 -prefix lbm  -pinplay:pcregions:in lbm-s.1_15218.Data/lbm-s.1_15218.global.pinpoints.csv ..." << endl << endl;
    cerr << "... -thread_count 8 -prefix lbm  -pinplay:control start:address:0x00054099f:count10078754:global,stop:address:0x00054099f:count3727356:global -pinplay:controller_log ..." << endl << endl;

    cerr << KNOB_BASE::StringKnobSummary() << endl;

    return -1;
}
// argc, argv are the entire command line, including pin -t <toolname> -- ...
int main(int argc, char * argv[])
{
#if defined(SDE_INIT)
    sde_pin_init(argc,argv);
    sde_init();
#else
    if( PIN_Init(argc,argv) )
    {
        return Usage();
    }
#endif
    PIN_InitSymbols();

    OpenThreadFiles();

#if defined(SDE_INIT)
    // This is a replay-only tool (for now)
    pinplay_engine = sde_tracing_get_pinplay_engine();
#else
    pinplay_engine = &pp_pinplay_engine;
    pinplay_engine->Activate(argc, argv, KnobPinPlayLogger, KnobPinPlayReplayer);
#endif

#if defined(PINPLAY)
#if defined(SDE_INIT)
    control_manager = SDE_CONTROLLER::sde_controller_get();
#else

    //if(KnobPinPlayLogger)
     // control_manager = pinplay_engine->LoggerGetController();
   control.Activate();
   control_manager = &control;
#endif
#else //not PinPlay
#if defined(SDE_INIT)
    control_manager = SDE_CONTROLLER::sde_controller_get();
#else
   control.Activate();
   control_manager = &control;
#endif
#endif


    PIN_InitLock(&output_lock);
    control_manager->RegisterHandler(Handler, 0, 0, LateHandler);

    PIN_AddThreadStartFunction(ThreadStart, 0);
    TRACE_AddInstrumentFunction(Trace, 0);
    PIN_AddFiniFunction(ProcessFini, 0);

    // Start the program, never returns
    PIN_StartProgram();

    return 0;
}

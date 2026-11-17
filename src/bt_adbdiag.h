/*
 *  bt_adbdiag.h  --  adbhook2: the wake hook's record, ONE layout shared by the driver
 *  (src/bt_inject.c fills it) and BTCheck (bt-check/bt_check.c includes this file by
 *  relative path and reads it).
 *
 *  WHY IT EXISTS. adbhook1 ran on the FW800 on 2026-11-16 (DBT-120, Apple's USB stack,
 *  Sleep Assistant mode A) and the A1015 still clicked but did not move after the wake:
 *  BTCheck read `cursor device created 1` and no row marked ours, so the JADBProc hook
 *  never replaced the record ADBReInit deleted. The hook's own counters were never
 *  published, so three causes fit the same numbers:
 *      the hook was never chained;
 *      something replaced JADBProc after we chained (cut us out of the chain);
 *      our island is in the chain but ADBReInit's call never reached our code (a stale
 *      68K translation of a reused island address: Apple's own HIDEmulation.c says
 *      "there does not seem to be any way to do this from PPC code" except BlockMove,
 *      and this driver's BlockMove is DriverServicesLib's).
 *  Every word below exists to tell those three apart in one run.
 *
 *  HOW A READER FINDS IT. The counter block word kWAdbDiagPtr (740, a hole retired in
 *  v16.1 that no reader prints) holds this record's address. The reader checks that the
 *  address lies inside the system heap, then kAdMagic, kAdSelf and kAdEnd, and refuses the
 *  record otherwise.
 *
 *  WORD INDICES, NOT A STRUCT, for the reason the counter block uses them: two binaries
 *  built separately must agree on a layout, and one enum in one header cannot drift
 *  between them.
 */
#ifndef OS9BT_ADBDIAG_H
#define OS9BT_ADBDIAG_H

#define kAdMagicValue  0x42544148UL   /* 'BTAH' */
#define kAdEndValue    0x454E4453UL   /* 'ENDS' */
#define kAdVersion     2UL            /* adbhook2 */

enum {
    kAdMagic = 0,       /* 'BTAH', stamped at run time, never a static initialiser */
    kAdSelf,            /* the record's own address: a reader checks it            */
    kAdVersionW,        /* kAdVersion                                              */

    /* the hook */
    kAdHooked,          /* 1 = chained into JADBProc                               */
    kAdIsland,          /* our 68K island (0 = never allocated)                    */
    kAdRD,              /* the routine descriptor the island JSRs to               */
    kAdOld,             /* the JADBProc value we chained to                        */
    kAdInstallTick,     /* Ticks at the install                                    */
    kAdInstallRead,     /* JADBProc read straight back after we wrote it           */
    kAdFlushErr,        /* 0x10000 | FlushCodeCacheRange's OSErr, first only       */
    kAdPreN,            /* ADBReInit's JADBProc calls that reached us: before ...  */
    kAdPostN,           /* ... and after the reinit                                */
    kAdPreTick,         /* Ticks at the last of each                               */
    kAdPostTick,

    /* the Expert's notifications to this driver */
    kAdDemandN,         /* kNotifySystemSleepDemand: count, Ticks, JADBProc then   */
    kAdDemandTick,
    kAdDemandJadb,
    kAdWakeN,           /* kNotifySystemSleepWakeUp: the same                      */
    kAdWakeTick,
    kAdWakeJadb,
    kAdPostAtDemand,    /* kAdPostN at the last demand; the wake compares to it    */

    /* the cursor device */
    kAdForgetHook,      /* forgets that found a record: by the hook's pre-call ... */
    kAdForgetDemand,    /* ... at the sleep demand ...                             */
    kAdForgetWake,      /* ... and at the wake                                     */
    kAdRebindHook,      /* rebinds attempted inside ADBReInit (the hook's post)    */
    kAdRebindTask,      /* rebinds attempted at task level (a dropped report)      */
    kAdKept,            /* outcomes: the old record was still listed and ours ...  */
    kAdMade,            /* ... a new one was made ...                              */
    kAdFail,            /* ... or CursorDeviceNewDevice failed (retried)           */
    kAdRebindTick,      /* Ticks at the last rebind that published a record        */
    kAdAccelSaved,      /* Fixed: the acceleration kept at the last forget         */
    kAdAccelReset,      /* rebinds whose kept acceleration was out of range        */

    /* live, refreshed with the counter block */
    kAdJadbNow,
    kAdCurDev,
    kAdCurDevPrev,
    kAdTickNow,

    kAdEnd,             /* 'ENDS'                                                  */
    kAdWords            /* the record's size in words                              */
};

#endif /* OS9BT_ADBDIAG_H */

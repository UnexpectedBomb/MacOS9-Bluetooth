/*
 *  bt_keyfile.h  --  the link key store's file half. ⚠⚠ TASK LEVEL ONLY.
 *
 *  Legal callers: ProbeInitialize (load), the NM response in bt_defer.c (flush),
 *  ProbeFinalize (flush). Nothing else. See bt_keyfile.c.
 */
#ifndef OS9BT_KEYFILE_H
#define OS9BT_KEYFILE_H

/* ⚠ TASK LEVEL. Call from ProbeInitialize BEFORE hci_init, so a key is in RAM
 * before BTstack can be asked for one. */
void BT_KeyFileLoad(void);

/* ⚠ TASK LEVEL. Writes only if the RAM store is dirty. Temp file then swap. */
void BT_KeyFileFlushIfDirty(void);

/* ⚠ TASK LEVEL ONLY. Deletes the switcher's "Bluetooth Switch Attempted" marker,
 * which is how this driver tells USBBluetoothSwitch v1.3 that it came up. If it
 * never runs the switcher falls back to leaving the card with Mac OS on every
 * later boot -- safe, but permanent, so the counters matter. */
void BT_ClearSwitchMarker(void);
extern unsigned long gMarkCleared, gMarkErr, gMarkRuns;

extern unsigned long gKfLoads;     /* load attempts                            */
extern unsigned long gKfLoaded;    /* ★ records read back in -- the reboot proof */
extern unsigned long gKfFlushes;   /* flushes that found the store dirty        */
extern unsigned long gKfWritten;   /* records written                           */
extern unsigned long gKfErr;       /* File Manager / FindFolder failures        */
extern unsigned long gKfRejected;  /* file discarded: bad magic or WRONG DONGLE */

/* ---- v14.2: the battery publish -------------------------------------------------
 *
 * BT_BatteryNote records a reading keyed by address and does PLAIN STORES ONLY, so it is
 * safe from the GET_REPORT response handler at interrupt level. BT_BatteryFlushIfDirty
 * does the File Manager work and is TASK LEVEL ONLY -- call it beside
 * BT_KeyFileFlushIfDirty and nowhere else [[reference_os9_no_filemgr_at_interrupt]].
 *
 * ⚠ `len` is the payload length as the device reported it, and the VALUE IS THE LAST
 * BYTE: the A1016 answers Feature 71 with two bytes (the report ID, then the level)
 * although Tiger's ioreg declares one. Pass the payload whole and let it decide. */
void BT_BatteryNote(unsigned long addrHi, unsigned long addrLo,
                    const unsigned char *payload, unsigned short len,
                    unsigned long nowTicks);
void BT_BatteryFlushIfDirty(void);

/* ⚠ TASK LEVEL ONLY, from ProbeInitialize. Sets gShowAllDevices from the existence of
 * `Preferences:Bluetooth Show All Devices`, which turns the scanner's drivable-only
 * filter off for bench testing -- the phone is this project's only re-pairable test
 * device. The inquiry result reads the flag at interrupt level and must never touch the
 * File Manager itself. */
void BT_LoadShowAllFlag(void);

/* ★ Post any owed low-battery warning. TASK LEVEL, from the defer response beside
 * BT_BatteryFlushIfDirty. The threshold is latched at interrupt level in
 * BT_BatteryNote with hysteresis, so this only ever has work when a level has newly
 * crossed downward. */
void BT_BatteryWarnIfPending(void);

/* ★ The device-names file, 'BTNM', so the CSM can show a real name instead of a
 * class-of-device label. BT_NamesNote is INTERRUPT-SAFE (a flag plus BT_DeferRequest);
 * BT_NamesFlushIfDirty is TASK LEVEL, from the defer response. The driver owns this
 * file because it is what issued Remote_Name_Request. */
void BT_NamesNote(void);
void BT_NamesFlushIfDirty(void);

extern unsigned long gBatFlushes;    /* flushes that found a reading to write     */
extern unsigned long gBatFlushErr;   /* File Manager failures on the way out      */
extern unsigned long gBatPublished;  /* ★ readings accepted -- the proof it works */

#endif /* OS9BT_KEYFILE_H */

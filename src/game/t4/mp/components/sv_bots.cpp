// ===========================================================================
// PATCH for sv_bots.cpp — r299 angles-from-lastUsercmd
// ===========================================================================
//
// FINDING: CoD4x's SV_BotUserMove (which T4 mirrors) does:
//
//     VectorCopy(ent->client->sess.cmd.angles, ucmd.angles);
//
// to preserve the bot's current view angles in the synthesized usercmd.
//
// Our T4 SV_BotUserMove_Stub never sets cmd.angles[] outside the mirror
// branch. cmd was memset to 0, so angles stay (0, 0, 0). Every server
// tick we ship (0, 0, 0) to SV_ClientThink, which writes those into
// ps.viewangles, snapping the bot to face world-north-flat regardless
// of what GSC's setplayerangles set on r.currentAngles.
//
// Since clientBW_t exposes lastUsercmd directly (verified offset 0x20EF4
// in structs_bw_ext.h), we copy from cl->lastUsercmd.angles. That's the
// bot's most recent processed usercmd angles, in the same packed-int
// format (verified via CoD4x q_shared.h: `int angles[3]` PACKED_ANGLE).
//
// VERIFIED via CoD4x's usercmd_s definition:
//     typedef struct usercmd_s
//     {
//         int serverTime;
//         int buttons;
//         int angles[3];        // <-- int, PACKED_ANGLE format
//         byte weapon;
//         ...
//     } usercmd_t;
//
// Plus CoD4x's SV_BotUserMove explicitly wraps angles to [0, 0xFFFF]
// confirming the packed-16-bit-in-int convention.
//
// ===========================================================================
//
// HOW TO APPLY:
//
// 1. Open sv_bots.cpp
//
// 2. In the toggle defines block near "r293 DIAGNOSTIC TOGGLES", ADD:
//
//        #define BW_R299_FIX_BOT_ANGLES       1
//        #define BW_R299_DIAG_LOG_ENTRY       1
//
// 3. In SV_BotUserMove_Stub, locate this line:
//
//        cmd.buttons = static_cast<button_mask>(g_botai[clientNum].buttons);
//
//    INSERT immediately AFTER it:
//
// ---------------- BEGIN INSERT 1 (angle fix) ----------------
#if BW_R299_FIX_BOT_ANGLES
    // [r299] Preserve the bot's current usercmd angles. Without this,
    // memset()-zeroed angles snap the bot's viewangles to (0,0,0) every
    // tick when SV_ClientThink processes the synthesized cmd, breaking
    // aim. CoD4x SV_BotUserMove does the equivalent via
    // ent->client->sess.cmd.angles; we use the same source from a
    // different struct path (clientBW_t.lastUsercmd at +0x20EF4).
    cmd.angles[0] = cl->lastUsercmd.angles[0];
    cmd.angles[1] = cl->lastUsercmd.angles[1];
    cmd.angles[2] = cl->lastUsercmd.angles[2];
#endif
// ----------------  END INSERT 1  ----------------
//
// 4. ALSO insert the diagnostic logging block. Locate where you have:
//
//        const int clientNum = static_cast<int>(cl - reinterpret_cast<clientBW_t *>(svsHeader->clients));
//        if (clientNum < 0 || clientNum >= MAX_CLIENTS_BW)
//        {
//            SV_BotUserMove_Detour.GetOriginal<SV_BotUserMove_t>()(cl);
//            return;
//        }
//
//    INSERT immediately AFTER the closing brace of that if, BEFORE the
//    "Defense in depth" guards:
//
// ---------------- BEGIN INSERT 2 (diag) ----------------
#if BW_R299_DIAG_LOG_ENTRY
    {
        static int s_lastEntryLogMs[MAX_CLIENTS_BW] = {0};
        const int now = svsHeader->time;
        if ((now - s_lastEntryLogMs[clientNum]) > 2000)
        {
            s_lastEntryLogMs[clientNum] = now;
            DbgPrint("sv_bots: BUM_Stub entry cn=%d remAdr.type=%d isTest=%d state=%d\n",
                     clientNum,
                     static_cast<int>(cl->header.netchan.remoteAddress.type),
                     static_cast<int>(cl->isTestClient),
                     static_cast<int>(cl->header.state));
        }
    }
#endif
// ----------------  END INSERT 2  ----------------
//
// 5. Build (Run workflow on GitHub Actions, NOT Re-run all jobs).
//
// 6. Boot a match, add bots, observe.
//
// ===========================================================================
//
// WHAT TO LOOK FOR AFTER DEPLOYING:
//
// A) On-screen behavior:
//    - Bots should aim more naturally. Look for bots that previously
//      stood still while staring north now tracking enemies.
//    - Bot bullet KILLS should appear in the killfeed (not just knife).
//    - Critical test: bot-vs-bot ADS should now register damage.
//
//    NOTE: this fix is about BOT aim. If the player-ADS-no-damage bug
//    is the SAME root cause (host slipping into bot code path), this
//    fix alone WON'T address it. The diag log answers whether host
//    is in the path.
//
// B) Log lines from BW_R299_DIAG_LOG_ENTRY:
//
//    Look for "sv_bots: BUM_Stub entry" lines in xenia.log.
//
//    EXPECTED (guards work, host safe):
//       BUM_Stub entry cn=1 remAdr.type=0 isTest=1 state=4
//       BUM_Stub entry cn=2 remAdr.type=0 isTest=1 state=4
//       ... no cn=0
//
//    BUG CONFIRMED (host leaks as bot):
//       BUM_Stub entry cn=0 remAdr.type=0 isTest=0 state=4
//       (host clientNum=0 appears; guards catch it via isTest=0 check
//        and early-return, so host view should NOT be corrupted by our
//        stub. If cn=0 appears with isTest=1, that's a real problem.)
//
// ===========================================================================
//
// IF BOT-VS-BOT ADS WORKS BUT PLAYER ADS STILL BROKEN:
//
// Two separate bugs. The angle fix solves bot-side. Player-side is a
// different mechanism — possibly:
//   - Engine bullet trace using wrong origin/direction for ADS shots
//   - Codxe binary patch affecting player usercmd path
//   - default_mp.ff weapon data (silenced variants specifically)
//
// IF NEITHER CHANGES: angles weren't the issue and we need to look at
// the engine bullet trace path itself.
//
// ===========================================================================

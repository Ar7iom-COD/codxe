// ===========================================================================
// DIAGNOSTIC patch for sv_bots.cpp — r299-diag
// ===========================================================================
//
// PURPOSE: gather data about who enters SV_BotUserMove_Stub. Two theories
// in play, this patch tests THEORY B without applying any behavior fix.
//
//   THEORY B (test target): host slipping through guards on Xenia.
//     On XLive-offline, host's netchan.remoteAddress.type may default to
//     0 (NA_BOT). If so, the host enters the stub. Our guards check
//     remAdr.type != NA_BOT AND isTestClient == 0 to skip — but if EITHER
//     is wrong for the host on Xenia, the stub runs ClientThink for host
//     with a synthesized cmd, breaking host ADS damage.
//
// HOW TO READ THE LOG (xenia.log):
//
//   Look for lines like:
//     sv_bots: BUM_Stub entry cn=N remAdr.type=X isTest=Y state=Z
//
//   Expected if guards work (host safe):
//     cn=1..18 (bots), remAdr.type=0 (NA_BOT), isTest=1, state=3 (CS_ACTIVE)
//     NO line for cn=0 (host).
//
//   Bug confirmed if you see:
//     cn=0 ANYTHING                     <- host entered stub at all
//   especially:
//     cn=0 remAdr.type=0 isTest=0       <- host's remAdr.type leaks NA_BOT,
//                                          isTest guard saved us THIS TICK
//                                          but the OUTER caller still
//                                          fires for the host => check
//                                          state and tick frequency
//
// TWO HOSTS scenario (split-screen): cn=0 and cn=1 are both human, the
// second human shows up via the same mechanism.
//
// ===========================================================================
//
// HOW TO APPLY:
//
// 1. Open sv_bots.cpp
// 2. Find the toggle defines block near "r293 DIAGNOSTIC TOGGLES":
//
//        #define CODXE_DIAG_ENABLE_WEAPON_HOOK         0
//        #define CODXE_DIAG_ENABLE_USERINFO_HOOK       1
//        #define CODXE_DIAG_ENABLE_BOTUSERMOVE         1
//
//    Add BELOW these:
//
//        #define BW_R299_DIAG_LOG_ENTRY                1
//
// 3. Find SV_BotUserMove_Stub function. After this block near the top:
//
//        const int clientNum = static_cast<int>(cl - reinterpret_cast<clientBW_t *>(svsHeader->clients));
//        if (clientNum < 0 || clientNum >= MAX_CLIENTS_BW)
//        {
//            SV_BotUserMove_Detour.GetOriginal<SV_BotUserMove_t>()(cl);
//            return;
//        }
//
//    INSERT the diagnostic block BEFORE the "Defense in depth" comment:
//
// ---------------- BEGIN INSERT ----------------
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
// ----------------  END INSERT  ----------------
//
// 4. Build (Run workflow on GitHub Actions, NOT Re-run all jobs)
// 5. Boot a match. Add a few bots. Play for ~30 seconds.
// 6. Inspect xenia.log for "BUM_Stub entry" lines
//
// ===========================================================================

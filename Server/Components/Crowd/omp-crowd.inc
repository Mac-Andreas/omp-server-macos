/* omp-crowd - the server's control of the launcher's Local crowd (component omp-crowd relay).
 * The crowd is off ("crowd: no" in the server info) until a script turns it on. */
#if defined _omp_crowd_included
    #endinput
#endif
#define _omp_crowd_included

native bool:Crowd_Enable(bool:enabled);                     // on/off for everyone ("crowd: yes/no")
native bool:Crowd_IsEnabled();
native bool:Crowd_SetDensity(Float:peds, Float:cars);       // 0.0 - 1.0 each (defaults 0.6, 0.5)
native bool:Crowd_SetLimits(maxPeds, maxCars);              // up to 40 peds, 20 cars around each player (defaults 20, 8)
native bool:Crowd_SetPlayerEnabled(playerid, bool:enabled); // one player off (an event, an interior...) or back on
native bool:Crowd_IsPlayerLinked(playerid);                 // their game runs the crowd (the launcher's DLL is linked)

#ifndef NEVERLOSE_INTERNAL_FIXES_H
#define NEVERLOSE_INTERNAL_FIXES_H

void fix_mem_dispatcher();
void fix_defensive();
void fix_doubletap();
void fix_resolver();
void fix_sha256();
void hijack_requestor();
void fix_lua();
void fix_cfg();

// Resolver modes - defined here so extern "C" functions can use them
enum ResolverMode : int
{
    MODE_OFF = 0,
    MODE_LEFT = 1,
    MODE_RIGHT = 2,
    MODE_CENTER = 3,
    MODE_SMART = 4,
    MODE_BRUTE_LEFT = 5,
    MODE_BRUTE_RIGHT = 6,
    MODE_MAX
};

// Resolver API - can be called from other modules (aimbot, etc.)
extern "C" void __cdecl resolver_report_hit(int player_index);
extern "C" void __cdecl resolver_report_miss(int player_index);
extern "C" int __cdecl resolver_get_mode(int player_index);
extern "C" void __cdecl resolver_set_mode(int player_index, int mode);

#endif // NEVERLOSE_INTERNAL_FIXES_H

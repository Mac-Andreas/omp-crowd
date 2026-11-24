// omp-crowd.dll - the open.mp launcher's Local crowd (Settings -> Beta): GTA's own pedestrians
// back on, locally, the way GTA Online does it - each player's game makes its own crowd, which
// only that player sees. Loaded with samp.dll and omp-client.dll when the switch is on (the
// launcher then writes omp-crowd.ini next to gta_sa.exe; without it this does nothing).
//
// SA-MP turns the game's population off. Rather than guess how, at start this compares the
// game's code in memory with gta_sa.exe on disk, logs what was changed (omp-crowd.log), and
// puts back only what the crowd needs. Peds only for now: no ambient cars (getting into a car
// the server doesn't know about would upset SA-MP's sync).
//
// GTA SA 1.0 US addresses from gta-reversed (github.com/gta-reversed/gta-reversed).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <vector>

namespace addr {
    const uintptr_t GameState            = 0xC8D4C0; // int32, 9 = in game
    const uintptr_t PedDensityMultiplier = 0x8D2530; // float
    const uintptr_t MaxNumberOfPedsInUse = 0x8D2538; // uint32
    const uintptr_t CarDensityMultiplier = 0x8A5B20; // float
    const uintptr_t MaxNumberOfCarsInUse = 0x8A5B24; // uint32
    const uintptr_t GameProcess          = 0x53BEE0; // CGame::Process
    const uintptr_t PopulationUpdate     = 0x616650; // CPopulation::Update(bool)
    const uintptr_t ManagePopulation     = 0x616190; // CPopulation::ManagePopulation
    const uintptr_t GenerateRandomCars   = 0x4341C0; // CCarCtrl::GenerateRandomCars
    const uintptr_t AddPed               = 0x612710; // CPopulation::AddPed - every crowd ped is made here
    const uintptr_t TotalPeds            = 0xC0EC28; // CPopulation::ms_nTotalPeds
    const uintptr_t TotalCivPeds         = 0xC0EC30; // CPopulation::ms_nTotalCivPeds
    const uintptr_t CountDownToPeds      = 0xC0EC23; // uint8, CPopulation::m_CountDownToPedsAtStart
    const uintptr_t NumOtherPeds         = 0xC0BC40; // float, CPopCycle::m_NumOther_Peds (how many the plan wants)
    const uintptr_t CurrentZoneType      = 0xC0BC6C; // int32, CPopCycle::m_nCurrentZoneType
    const uintptr_t CurrZoneInfo         = 0xC0BC68; // CZoneInfo*
    const uintptr_t NumPedsLoaded        = 0x8E4BB0; // uint32, CStreaming::ms_numPedsLoaded
    const uintptr_t PedsLoaded           = 0x8E4C00; // int32[8], CStreaming::ms_pedsLoaded
    const uintptr_t DisableStreaming     = 0x9654B0; // bool, CStreaming::ms_disableStreaming
    const uintptr_t AddPedInCarSeat      = 0x613BA7; // AddPedInCar's call that seats the driver (SA-MP takes it out)
    const uintptr_t NumRandomCars        = 0x969094; // int32, CCarCtrl::NumRandomCars
    const uintptr_t VehiclePool          = 0xB74494; // CPools::ms_pVehiclePool
    const size_t    VehicleSize          = 0xA18;    // a vehicle pool slot (sizeof CHeli)
    const size_t    VehCreatedBy         = 0x4A4;    // uint8: 1 random traffic, 2 script (SA-MP's), 3 parked, 4 permanent
    const size_t    VehDoorLock          = 0x4F8;    // uint32: 1 unlocked, 2 locked
}
static bool g_cars = false;
// ---- Sharing the crowd (the relay's groups). Everything here runs on the game's own thread
// (popUpdateStub, once a frame), except the link thread's inbox/outbox hand-offs.
// Crowd peds are known exactly: every one GTA makes goes through addPedStub.
struct CrowdPed { uint8_t* ped; uint8_t pedType; uint16_t model; };
static CrowdPed g_crowd[64];
static volatile LONG g_role = 0, g_ownerId = -1;
// The server decides (its relay's CONFIG): off until it says on; then its density and limits.
static volatile LONG g_serverOn = 0;
static volatile float g_srvPedDensity = 0.6f, g_srvCarDensity = 0.5f;
static volatile LONG g_srvMaxPeds = 20, g_srvMaxCars = 8;          // 0 alone, 1 owner, 2 viewer (the relay says)
static CRITICAL_SECTION g_ioLock;
static uint8_t g_outbox[1200]; static int g_outboxLen = 0; // the owner's next crowd packet (payload)
static uint8_t g_inbox[1200]; static int g_inboxLen = 0; static DWORD g_inboxAt = 0; static bool g_inboxNew = false;
struct Copy { uint16_t netId; uint8_t kind, pedType; uint16_t model; uint8_t* ent; float x, y, z, fx, fy, fz, ux, uy, uz, vx, vy, vz; DWORD seen; bool fresh; };
static Copy g_copies[64];
static volatile LONG g_copiesMade = 0, g_copiesGone = 0;
static bool isCopy(const void* e) { for (auto& c : g_copies) if (c.ent && c.ent == e) return true; return false; }
// The crowd's memory: GTA deletes a ped that's out of view and over 25 m away for 4 s (and cars
// much the same), so turning round wiped the street. Anything still within these of the player
// stays; further off, GTA's own removal carries on. Dead peds and wrecks always go.
static float g_keepPedsM = 60.0f, g_keepCarsM = 80.0f;
static volatile LONG g_keptPeds = 0, g_keptCars = 0;
static const uintptr_t kManagePedRemove = 0x61222C;   // CPopulation::ManagePed's RemovePed call returns here
static const uintptr_t kPossiblyRemoveVehicle = 0x424F80, kRemoveDistantCars = 0x42CD10;
static bool entityPos(const uint8_t* e, float& x, float& y)
{
    if (!e) return false;
    const uint8_t* m = *(const uint8_t* const*)(e + 0x14);   // CPlaceable::m_matrix
    const float* p = m ? (const float*)(m + 0x30) : (const float*)(e + 0x4);
    x = p[0]; y = p[1];
    return true;
}
static bool nearPlayer(const uint8_t* e, float radius)
{
    const uint8_t* player = *(const uint8_t* const*)0xB7CD98;   // CWorld::Players[0].m_pPed
    float px, py, ex, ey;
    if (!entityPos(player, px, py) || !entityPos(e, ex, ey)) return false;
    float dx = ex - px, dy = ey - py;
    return dx * dx + dy * dy < radius * radius;
}
static void __cdecl possiblyRemoveVehicleStub(uint8_t* veh)
{
    if (g_serverOn && g_role != 2 && veh && veh[0x4A4] == 1 && *(const float*)(veh + 0x4C0) > 250.0f && nearPlayer(veh, g_keepCarsM)) { InterlockedIncrement(&g_keptCars); return; }
    ((void(__cdecl*)(uint8_t*))kPossiblyRemoveVehicle)(veh);
}

// The two calls to CPopulation::Update in CGame::Process go through here, so the log can say
// whether GTA's crowd update runs at all.
static volatile LONG g_popUpdates = 0;
// And the crowd's own steps: AddToPopulation (tried / said yes) and AddPed (asked / made a ped).
static volatile LONG g_a2pCalls = 0, g_a2pYes = 0, g_addPedCalls = 0, g_addPedMade = 0;
static volatile float g_a2pArgs[4];
static uintptr_t g_addPedReal = 0;
static bool __cdecl addToPopulationStub(float a, float b, float c, float d)
{
    InterlockedIncrement(&g_a2pCalls);
    g_a2pArgs[0] = a; g_a2pArgs[1] = b; g_a2pArgs[2] = c; g_a2pArgs[3] = d;
    bool r = ((bool(__cdecl*)(float, float, float, float))0x614720)(a, b, c, d);
    if (r) InterlockedIncrement(&g_a2pYes);
    return r;
}
static void logf(const char* fmt, ...);
static bool writeCode(uintptr_t a, const uint8_t* bytes, size_t n);
// Who removes crowd peds: CPopulation::RemovePed (0x610F20) is hooked - its first two
// instructions (push esi / mov esi, [esp+8], 5 bytes) run in a trampoline - and each caller's
// address is counted (GTA's code is 0x4xxxxx-0x8xxxxx; anything else is a DLL's).
static const uintptr_t kRemovePed = 0x610F20;
static void* g_removePedTramp = nullptr;
static volatile LONG g_removed = 0;
static struct { uintptr_t ret; LONG n; } g_removers[16];
static CRITICAL_SECTION g_removersLock;
static void __cdecl removePedHook(void* ped)
{
    uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    if (ped && isCopy(ped)) return;   // a copy goes only when the owner's crowd says so
    // GTA's "not needed any more" removal of a live ped that's still near: kept (not as a viewer:
    // its own crowd makes way for the owner's).
    if (g_serverOn && g_role != 2 && ret == kManagePedRemove && ped && *(const float*)((const uint8_t*)ped + 0x540) > 0.0f && nearPlayer((const uint8_t*)ped, g_keepPedsM))
    { InterlockedIncrement(&g_keptPeds); return; }
    InterlockedIncrement(&g_removed);
    EnterCriticalSection(&g_removersLock);
    for (auto& r : g_removers) { if (r.ret == ret) { r.n++; break; } if (!r.ret) { r.ret = ret; r.n = 1; break; } }
    LeaveCriticalSection(&g_removersLock);
    for (auto& c : g_crowd) if (c.ped == ped) c.ped = nullptr;
    ((void(__cdecl*)(void*))g_removePedTramp)(ped);
}
static void hookRemovePed(const uint8_t* disk)
{
    static const uint8_t expect[5] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };
    if (memcmp((const void*)kRemovePed, expect, 5) != 0 || (disk && memcmp(disk, expect, 5) != 0)) { logf("RemovePed isn't as expected - not counted"); return; }
    uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, expect, 5);
    t[5] = 0xE9; int32_t back = (int32_t)((kRemovePed + 5) - ((uintptr_t)t + 10)); memcpy(t + 6, &back, 4);
    g_removePedTramp = t;
    uint8_t jmp[5] = { 0xE9 }; int32_t rel = (int32_t)((uintptr_t)&removePedHook - (kRemovePed + 5)); memcpy(jmp + 1, &rel, 4);
    writeCode(kRemovePed, jmp, 5);
    logf("RemovePed: counted (who calls it)");
}
// Random traffic is locked: getting into a car the server doesn't know about would upset
// SA-MP's sync (until it can be handed to the server). The created-by byte is checked first:
// if the pool shows anything but 1-4 there, the offsets are wrong and nothing is touched.
static void lockTraffic(int& total, int& random, int& locked, bool& sane)
{
    total = random = locked = 0; sane = true;
    uint8_t* pool = *(uint8_t**)addr::VehiclePool;
    if (!pool) return;
    uint8_t* objects = *(uint8_t**)pool;
    uint8_t* bytemap = *(uint8_t**)(pool + 4);
    int size = *(int32_t*)(pool + 8);
    for (int i = 0; i < size; i++)
    {
        if (bytemap[i] & 0x80) continue;
        uint8_t by = objects[i * addr::VehicleSize + addr::VehCreatedBy];
        total++;
        if (by < 1 || by > 4) sane = false;
        if (by == 1) random++;
    }
    if (!sane) return;
    for (int i = 0; i < size; i++)
    {
        if (bytemap[i] & 0x80) continue;
        uint8_t* v = objects + i * addr::VehicleSize;
        if (v[addr::VehCreatedBy] != 1) continue;
        uint32_t* lock = (uint32_t*)(v + addr::VehDoorLock);
        if (*lock != 2) *lock = 2;
        locked++;
    }
}
// The ped pool: how many slots, how many free (GTA removes the nearest ped when fewer than 8 are).
static void pedPool(int& size, int& freeSlots)
{
    size = freeSlots = -1;
    uint8_t* pool = *(uint8_t**)0xB74490;   // CPools::ms_pPedPool
    if (!pool) return;
    uint8_t* bytemap = *(uint8_t**)(pool + 4);
    size = *(int32_t*)(pool + 8);
    freeSlots = 0;
    for (int i = 0; i < size; i++) if (bytemap[i] & 0x80) freeSlots++;
}
static void* __cdecl addPedStub(int pedType, unsigned model, const void* pos, bool b)
{
    InterlockedIncrement(&g_addPedCalls);
    void* ped = ((void*(__cdecl*)(int, unsigned, const void*, bool))g_addPedReal)(pedType, model, pos, b);
    if (ped)
    {
        InterlockedIncrement(&g_addPedMade);
        for (auto& c : g_crowd) if (!c.ped) { c.ped = (uint8_t*)ped; c.pedType = (uint8_t)pedType; c.model = (uint16_t)model; break; }
    }
    return ped;
}
// GTA only makes new peds on a frame where loading models took under 4 ms (CGame::Process
// passes generatePeds = that); under Wine, with SA-MP's bigger streaming, that's never - the
// crowd ran but was never told to make anyone. So it's always told to (still capped by
// max_peds). force_generate=0 in the ini puts GTA's own rule back.
static bool g_forceGenerate = true;
static void crowdFrame();
static void __cdecl popUpdateStub(bool generate)
{
    InterlockedIncrement(&g_popUpdates);
    ((void(__cdecl*)(bool))addr::PopulationUpdate)(g_forceGenerate ? true : generate);
    // Traffic: SA-MP took CGame::Process's call to GenerateRandomCars out (and it sits behind the
    // same 4 ms rule), so it's made here, once a frame, on the game's own thread.
    if (g_cars && g_serverOn) ((void(__cdecl*)())addr::GenerateRandomCars)();
    crowdFrame();
}

static char g_dir[MAX_PATH];
static FILE* g_log = nullptr;

static void logf(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

struct Config {
    bool enabled = false, diag = true, peds = true;
    float pedDensity = 0.6f;
    int maxPeds = 20;
    bool cars = true;
    float carDensity = 0.5f;
    int maxCars = 8;
};

static Config readConfig()
{
    Config c;
    char ini[MAX_PATH]; snprintf(ini, sizeof(ini), "%s\\omp-crowd.ini", g_dir);
    // Testing on another server (a local one): omp-crowd-dev.ini, which the launcher leaves alone.
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) snprintf(ini, sizeof(ini), "%s\\omp-crowd-dev.ini", g_dir);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return c;   // not our server: nothing at all
    c.enabled = GetPrivateProfileIntA("world", "enabled", 1, ini) != 0;
    c.diag = GetPrivateProfileIntA("world", "diag", 1, ini) != 0;
    c.peds = GetPrivateProfileIntA("world", "peds", 1, ini) != 0;
    c.maxPeds = GetPrivateProfileIntA("world", "max_peds", 20, ini);
    g_forceGenerate = GetPrivateProfileIntA("world", "force_generate", 1, ini) != 0;
    c.cars = GetPrivateProfileIntA("world", "cars", 1, ini) != 0;
    c.maxCars = GetPrivateProfileIntA("world", "max_cars", 8, ini);
    g_keepPedsM = (float)GetPrivateProfileIntA("world", "keep_peds_m", 60, ini);
    g_keepCarsM = (float)GetPrivateProfileIntA("world", "keep_cars_m", 80, ini);
    char cbuf[32]; GetPrivateProfileStringA("world", "car_density", "0.5", cbuf, sizeof(cbuf), ini);
    c.carDensity = (float)atof(cbuf);
    if (c.maxCars < 0) c.maxCars = 0;
    if (c.maxCars > 20) c.maxCars = 20;
    char buf[32]; GetPrivateProfileStringA("world", "ped_density", "0.6", buf, sizeof(buf), ini);
    c.pedDensity = (float)atof(buf);
    if (c.maxPeds < 0) c.maxPeds = 0;
    if (c.maxPeds > 40) c.maxPeds = 40;   // SA-MP keeps remote players in the same ped pool (140): leave them room
    if (c.pedDensity < 0.0f) c.pedDensity = 0.0f;
    if (c.pedDensity > 1.0f) c.pedDensity = 1.0f;
    return c;
}

// gta_sa.exe's .text as it is on disk, and where it sits in memory.
struct TextImage { std::vector<uint8_t> disk; uintptr_t va = 0; size_t size = 0; };

static bool loadText(TextImage& t)
{
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, sizeof(exe));
    FILE* f = fopen(exe, "rb");
    if (!f) { logf("can't open %s", exe); return false; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file(len > 0 ? len : 0);
    if (len <= 0 || fread(file.data(), 1, len, f) != (size_t)len) { fclose(f); return false; }
    fclose(f);
    auto* dos = (IMAGE_DOS_HEADER*)file.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = (IMAGE_NT_HEADERS32*)(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
    {
        if (strncmp((const char*)sec->Name, ".text", 5) != 0) continue;
        size_t n = sec->SizeOfRawData < sec->Misc.VirtualSize ? sec->SizeOfRawData : sec->Misc.VirtualSize;
        if (sec->PointerToRawData + n > file.size()) return false;
        t.disk.assign(file.begin() + sec->PointerToRawData, file.begin() + sec->PointerToRawData + n);
        t.va = base + sec->VirtualAddress;
        t.size = n;
        logf("gta_sa.exe .text: %08X, %u bytes (base %08X)", (unsigned)t.va, (unsigned)n, (unsigned)base);
        return true;
    }
    return false;
}

static const uint8_t* diskAt(const TextImage& t, uintptr_t a)
{
    return (a >= t.va && a < t.va + t.size) ? &t.disk[a - t.va] : nullptr;
}

// Every stretch where the game's code in memory isn't what's on disk (SA-MP's patches and
// hooks, and anyone else's), to omp-crowd.log.
static void logPatches(const TextImage& t)
{
    const uint8_t* mem = (const uint8_t*)t.va;
    size_t i = 0, ranges = 0, bytes = 0;
    while (i < t.size)
    {
        if (mem[i] == t.disk[i]) { i++; continue; }
        size_t s = i, last = i;
        while (i < t.size && i - last <= 8) { if (mem[i] != t.disk[i]) last = i; i++; }
        size_t n = last - s + 1;
        bytes += n;
        if (ranges++ < 600)
        {
            char was[64] = "", now[64] = "";
            for (size_t k = 0; k < n && k < 16; k++) { sprintf(was + k * 3, "%02X ", t.disk[s + k]); sprintf(now + k * 3, "%02X ", mem[s + k]); }
            logf("  patched %08X +%u  disk: %s  mem: %s", (unsigned)(t.va + s), (unsigned)n, was, now);
        }
        i = last + 1;
    }
    logf("%u patched stretches, %u bytes, in the game's code", (unsigned)ranges, (unsigned)bytes);
}

static bool writeCode(uintptr_t a, const uint8_t* bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void*)a, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)a, bytes, n);
    VirtualProtect((void*)a, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)a, n);
    return true;
}

// The calls to CPopulation::Update in CGame::Process (found in the code on disk): back as they
// were if they've been taken out. And the function's own start, if it's been turned into a
// plain return (a hook - a jump somewhere - is left alone, and logged).
static void restorePopulation(const TextImage& t)
{
    int found = 0, restored = 0;
    for (uintptr_t a = addr::GameProcess; a < addr::GameProcess + 0x1400; a++)
    {
        const uint8_t* d = diskAt(t, a);
        if (!d || d[0] != 0xE8) continue;
        int32_t rel; memcpy(&rel, d + 1, 4);
        uintptr_t target = a + 5 + rel;
        if (target != addr::PopulationUpdate) continue;
        found++;
        if (memcmp((const void*)a, d, 5) != 0)
        {
            const uint8_t* m = (const uint8_t*)a;
            logf("CGame::Process calls CPopulation::Update at %08X: taken out (%02X %02X %02X %02X %02X) - putting it back", (unsigned)a, m[0], m[1], m[2], m[3], m[4]);
            if (writeCode(a, d, 5)) restored++;
        }
        else logf("CGame::Process calls CPopulation::Update at %08X: in place", (unsigned)a);
        uint8_t call[5] = { 0xE8 };
        int32_t rel2 = (int32_t)((uintptr_t)&popUpdateStub - (a + 5));
        memcpy(call + 1, &rel2, 4);
        writeCode(a, call, 5);
    }
    logf("population calls: %d found, %d put back", found, restored);
    // CPopulation::Update's call to AddToPopulation, through the counting stub.
    for (uintptr_t a = addr::PopulationUpdate; a < addr::PopulationUpdate + 0x400; a++)
    {
        const uint8_t* d = diskAt(t, a);
        if (!d || d[0] != 0xE8) continue;
        int32_t rel; memcpy(&rel, d + 1, 4);
        if (a + 5 + rel != 0x614720) continue;
        uint8_t call[5] = { 0xE8 };
        int32_t r2 = (int32_t)((uintptr_t)&addToPopulationStub - (a + 5));
        memcpy(call + 1, &r2, 4);
        writeCode(a, call, 5);
        logf("CPopulation::Update calls AddToPopulation at %08X: counted", (unsigned)a);
    }
    // SA-MP's block (0.1.0's log showed it): CPopulation::AddPed made to return nothing at once
    // (xor eax, eax / ret) - the crowd ran, but no ped could be made.
    const uintptr_t fns[] = { addr::PopulationUpdate, addr::ManagePopulation, addr::AddPed };
    for (uintptr_t f : fns)
    {
        const uint8_t* d = diskAt(t, f);
        const uint8_t* m = (const uint8_t*)f;
        if (!d) continue;
        if (memcmp(m, d, 8) == 0) { logf("%08X starts as on disk", (unsigned)f); continue; }
        size_t n = 0;
        for (size_t k = 0; k < 16; k++) if (m[k] != d[k]) n = k + 1;   // up to the last byte that differs
        bool returnsAtOnce = m[0] == 0xC3 || m[0] == 0xC2 || ((m[0] == 0x33 || m[0] == 0x31) && m[1] == 0xC0 && (m[2] == 0xC3 || m[2] == 0xC2));
        if (returnsAtOnce) { logf("%08X was made to return at once - putting its first %u bytes back", (unsigned)f, (unsigned)n); writeCode(f, d, n); }
        // AddPed is "nop; jmp <body>": go round the counting stub to the body.
        if (f == addr::AddPed && d[0] == 0x90 && d[1] == 0xE9)
        {
            int32_t rel; memcpy(&rel, d + 2, 4);
            g_addPedReal = f + 6 + rel;
            uint8_t jmp[5] = { 0xE9 };
            int32_t r2 = (int32_t)((uintptr_t)&addPedStub - (f + 5));
            memcpy(jmp + 1, &r2, 4);
            writeCode(f, jmp, 5);
            logf("AddPed (body at %08X): counted", (unsigned)g_addPedReal);
        }
        else logf("%08X is hooked (%02X %02X %02X %02X %02X) - left alone", (unsigned)f, m[0], m[1], m[2], m[3], m[4]);
    }
}

// ---- The link to the server's omp-crowd relay (its UDP port: the game's + 1 unless the ini
// says relay_port). Who we are and where the game went come from gta_sa.exe's command line
// (the launcher starts it with -n name -h host -p port).
namespace link {
    const uint8_t kMagic[4] = { 'O', 'C', 'R', 'W' };
    enum : uint8_t { HELLO = 1, WELCOME = 2, REFUSED = 3, PING = 4, PONG = 5 };
    SOCKET sock = INVALID_SOCKET;
    sockaddr_in relay {};
    char name[32] = "";
    bool linked = false;
    uint32_t session = 0;
    int playerId = -1;
}

static bool argAfter(const char* cmd, const char* flag, char* out, size_t size)
{
    const char* p = strstr(cmd, flag);
    if (!p) return false;
    p += strlen(flag);
    while (*p == ' ') p++;
    size_t n = 0;
    if (*p == '"') { p++; while (*p && *p != '"' && n + 1 < size) out[n++] = *p++; }
    else while (*p && *p != ' ' && n + 1 < size) out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

static void linkSend(uint8_t type, const uint8_t* body, size_t n)
{
    uint8_t buf[1220];
    if (n > sizeof(buf) - 6) return;
    memcpy(buf, link::kMagic, 4); buf[4] = 1; buf[5] = type;
    if (n) memcpy(buf + 6, body, n);
    sendto(link::sock, (const char*)buf, (int)(6 + n), 0, (const sockaddr*)&link::relay, sizeof(link::relay));
}

static DWORD WINAPI linkThread(LPVOID param)
{
    int relayPort = (int)(intptr_t)param;
    const char* cmd = GetCommandLineA();
    char host[128] = "", port[16] = "";
    if (!argAfter(cmd, " -n ", link::name, sizeof(link::name)) || !argAfter(cmd, " -h ", host, sizeof(host)) || !argAfter(cmd, " -p ", port, sizeof(port)))
    { logf("link: no -n/-h/-p on the command line - no relay"); return 0; }
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) { logf("link: can't resolve %s", host); return 0; }
    link::relay = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    int rp = relayPort > 0 ? relayPort : atoi(port) + 1;
    link::relay.sin_port = htons((u_short)rp);
    link::sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1; ioctlsocket(link::sock, FIONBIO, &nb);
    logf("link: %s -> relay %s:%d", link::name, host, rp);
    DWORD lastHello = 0, lastPing = 0, lastPong = 0, lastRefusedLog = 0;
    for (;;)
    {
        DWORD now = GetTickCount();
        uint8_t buf[1300];
        for (;;)
        {
            int n = recv(link::sock, (char*)buf, sizeof(buf), 0);
            if (n < 6 || memcmp(buf, link::kMagic, 4) != 0) break;
            switch (buf[5])
            {
            case link::WELCOME:
                if (n >= 12)
                {
                    uint16_t id; memcpy(&id, buf + 6, 2); memcpy(&link::session, buf + 8, 4);
                    if (!link::linked) logf("link: linked as player %u (session %08X)", (unsigned)id, (unsigned)link::session);
                    link::linked = true; link::playerId = id; lastPong = now;
                }
                break;
            case link::REFUSED:
                if (now - lastRefusedLog > 10000) { logf("link: refused (%s)", n > 6 && buf[6] == 2 ? "not from this player's address" : n > 6 && buf[6] == 1 ? "no such player yet" : "bad packet"); lastRefusedLog = now; }
                link::linked = false;
                break;
            case link::PONG:
                lastPong = now;
                break;
            case 6:   // ROLE
                if (n >= 10)
                {
                    LONG role = buf[6], owner = buf[7] | (buf[8] << 8);
                    if (role != g_role || owner != g_ownerId) logf("link: %s (owner %ld, %u in the group)", role == 1 ? "owner of the crowd here" : role == 2 ? "viewer - the owner's crowd is copied" : "alone", owner, (unsigned)buf[9]);
                    g_ownerId = owner; g_role = role;
                }
                break;
            case 9:   // CONFIG: the server's say
                if (n >= 17)
                {
                    LONG on = buf[6]; float pd, cd; memcpy(&pd, buf + 7, 4); memcpy(&cd, buf + 11, 4);
                    if (on != g_serverOn) logf("server: the crowd is %s here (peds %.2f, cars %.2f, at most %u / %u)", on ? "ON" : "OFF", pd, cd, (unsigned)buf[15], (unsigned)buf[16]);
                    g_srvPedDensity = pd; g_srvCarDensity = cd; g_srvMaxPeds = buf[15]; g_srvMaxCars = buf[16]; g_serverOn = on;
                }
                break;
            case 7:   // CROWD from the owner
                if (n > 8 && n - 6 <= (int)sizeof(g_inbox))
                {
                    EnterCriticalSection(&g_ioLock);
                    g_inboxLen = n - 6; memcpy(g_inbox, buf + 6, g_inboxLen); g_inboxAt = now; g_inboxNew = true;
                    LeaveCriticalSection(&g_ioLock);
                }
                break;
            }
        }
        if (!link::linked && now - lastHello > 2000)
        {
            uint8_t b[40]; size_t nl = strlen(link::name); if (nl > 24) nl = 24;
            b[0] = (uint8_t)nl; memcpy(b + 1, link::name, nl);
            linkSend(link::HELLO, b, 1 + nl);
            lastHello = now;
        }
        if (link::linked && now - lastPing > 2000) { uint8_t pb[6]; memcpy(pb, &link::session, 4); uint16_t fps = 0; memcpy(pb + 4, &fps, 2); linkSend(link::PING, pb, 6); lastPing = now; }
        if (link::linked && g_role == 1)
        {
            uint8_t pkt[1210]; int len = 0;
            EnterCriticalSection(&g_ioLock);
            if (g_outboxLen) { memcpy(pkt, &link::session, 4); memcpy(pkt + 4, g_outbox, g_outboxLen); len = 4 + g_outboxLen; g_outboxLen = 0; }
            LeaveCriticalSection(&g_ioLock);
            if (len) linkSend(7, pkt, len);
        }
        if (link::linked && now - lastPong > 10000) { logf("link: relay silent for 10 s - linking again"); link::linked = false; }
        Sleep(g_role == 1 ? 50 : 100);
    }
}


// ---- once a frame, on the game's thread ------------------------------------------------------
static inline int16_t q16(float v, float scale) { float f = v * scale; if (f > 32767.f) f = 32767.f; if (f < -32767.f) f = -32767.f; return (int16_t)f; }
static bool entityFrame(const uint8_t* e, const float*& m) { if (!e) return false; m = *(const float* const*)(e + 0x14); return m != nullptr; }
static void put(uint8_t*& w, const void* v, size_t n) { memcpy(w, v, n); w += n; }

// The owner: its crowd (peds it made, random cars), nearest first, at most 28, five times a second.
static void ownerSnapshot()
{
    static DWORD last = 0; static uint16_t seq = 0;
    DWORD now = GetTickCount();
    if (now - last < 200) return;
    last = now;
    const uint8_t* player = *(const uint8_t* const*)0xB7CD98;
    const float* pm; if (!entityFrame(player, pm)) return;
    struct Cand { const uint8_t* e; uint16_t netId; uint8_t kind, pedType; uint16_t model; float d; };
    Cand c[96]; int nc = 0;
    for (int i = 0; i < 64; i++)
    {
        const uint8_t* e = g_crowd[i].ped; const float* m;
        if (!e || !entityFrame(e, m) || nc >= 96) continue;
        float dx = m[12] - pm[12], dy = m[13] - pm[13];
        c[nc++] = { e, (uint16_t)i, 0, g_crowd[i].pedType, g_crowd[i].model, dx * dx + dy * dy };
    }
    uint8_t* pool = *(uint8_t**)addr::VehiclePool;
    if (g_cars && pool)
    {
        uint8_t* objects = *(uint8_t**)pool; uint8_t* bytemap = *(uint8_t**)(pool + 4); int size = *(int32_t*)(pool + 8);
        for (int i = 0; i < size && nc < 96; i++)
        {
            if (bytemap[i] & 0x80) continue;
            const uint8_t* v = objects + i * addr::VehicleSize; const float* m;
            if (v[addr::VehCreatedBy] != 1 || !entityFrame(v, m)) continue;
            float dx = m[12] - pm[12], dy = m[13] - pm[13];
            c[nc++] = { v, (uint16_t)(0x100 + i), 1, 0, *(const uint16_t*)(v + 0x22), dx * dx + dy * dy };
        }
    }
    for (int i = 1; i < nc; i++) for (int j = i; j > 0 && c[j].d < c[j - 1].d; j--) { Cand t = c[j]; c[j] = c[j - 1]; c[j - 1] = t; }
    if (nc > 28) nc = 28;
    uint8_t buf[1200]; uint8_t* w = buf;
    seq++; put(w, &seq, 2); uint8_t cnt = (uint8_t)nc; put(w, &cnt, 1);
    for (int i = 0; i < nc; i++)
    {
        const float* m; entityFrame(c[i].e, m);
        put(w, &c[i].netId, 2); put(w, &c[i].kind, 1); put(w, &c[i].pedType, 1); put(w, &c[i].model, 2);
        put(w, m + 12, 12);                                                     // position
        int16_t o[6] = { q16(m[4], 32767.f), q16(m[5], 32767.f), q16(m[6], 32767.f), q16(m[8], 32767.f), q16(m[9], 32767.f), q16(m[10], 32767.f) };
        put(w, o, 12);                                                          // forward, up
        const float* sp = (const float*)(c[i].e + 0x44);                        // CPhysical::m_vecMoveSpeed
        int16_t v[3] = { q16(sp[0], 1000.f), q16(sp[1], 1000.f), q16(sp[2], 1000.f) };
        put(w, v, 6);
        float hp = *(const float*)(c[i].e + (c[i].kind ? 0x4C0 : 0x540));
        uint8_t h = (uint8_t)(hp < 0 ? 0 : hp > 1000 ? 255 : (c[i].kind ? hp / 4 : hp)); put(w, &h, 1);
        uint8_t flags = 0; put(w, &flags, 1);
    }
    EnterCriticalSection(&g_ioLock);
    g_outboxLen = (int)(w - buf); memcpy(g_outbox, buf, g_outboxLen);
    LeaveCriticalSection(&g_ioLock);
}

typedef void (__thiscall* TeleportFn)(void* self, float x, float y, float z, bool resetRotation);
static void teleport(uint8_t* e, float x, float y, float z) { void** vt = *(void***)e; ((TeleportFn)vt[14])(e, x, y, z, false); }
static bool modelReady(uint16_t model)
{
    if (*(const uint8_t*)(0x8E4CC0 + model * 0x14 + 0x10) == 1) return true;   // CStreaming::ms_aInfoForModel[model].m_nLoadState == LOADED
    ((void(__cdecl*)(int, int))0x4087E0)(model, 2);                              // CStreaming::RequestModel (kept in memory)
    return false;
}
static void removeCopy(Copy& cp)
{
    if (cp.ent)
    {
        if (cp.kind == 0) { uint8_t* e = cp.ent; cp.ent = nullptr; ((void(__cdecl*)(void*))g_removePedTramp)(e); }
        else { uint8_t* e = cp.ent; cp.ent = nullptr; ((void(__cdecl*)(void*))0x563280)(e); void** vt = *(void***)e; ((void(__thiscall*)(void*, int))vt[0])(e, 1); }   // CWorld::Remove, then delete
        InterlockedIncrement(&g_copiesGone);
    }
    cp.netId = 0xFFFF;
}
static void setOrientation(uint8_t* e, const Copy& cp)
{
    float* m = *(float**)(e + 0x14); if (!m) return;
    float fx = cp.fx, fy = cp.fy, fz = cp.fz, ux = cp.ux, uy = cp.uy, uz = cp.uz;
    m[0] = fy * uz - fz * uy; m[1] = fz * ux - fx * uz; m[2] = fx * uy - fy * ux;   // right = forward x up
    m[4] = fx; m[5] = fy; m[6] = fz; m[8] = ux; m[9] = uy; m[10] = uz;
}

// A viewer: the owner's crowd as copies - made once their model's loaded, moved by GTA's own
// Teleport and given the owner's speed (GTA's physics carries them between updates), removed
// when the owner stops sending them.
static void viewerCopies()
{
    DWORD now = GetTickCount();
    uint8_t buf[1200]; int len = 0;
    EnterCriticalSection(&g_ioLock);
    if (g_inboxNew) { len = g_inboxLen; memcpy(buf, g_inbox, len); g_inboxNew = false; }
    LeaveCriticalSection(&g_ioLock);
    if (len >= 5)
    {
        const uint8_t* r = buf + 2;   // after the owner's id
        uint8_t cnt = r[2]; r += 3;
        for (int i = 0; i < cnt && r + 38 <= buf + len; i++, r += 38)
        {
            uint16_t netId; memcpy(&netId, r, 2);
            Copy* cp = nullptr;
            for (auto& c : g_copies) if (c.netId == netId && (c.ent || c.model)) { cp = &c; break; }
            if (!cp) for (auto& c : g_copies) if (!c.ent && !c.model) { cp = &c; cp->netId = netId; cp->ent = nullptr; break; }
            if (!cp) continue;
            cp->kind = r[2]; cp->pedType = r[3]; memcpy(&cp->model, r + 4, 2);
            float p3[3]; memcpy(p3, r + 6, 12); cp->x = p3[0]; cp->y = p3[1]; cp->z = p3[2];
            int16_t o[6]; memcpy(o, r + 18, 12);
            cp->fx = o[0] / 32767.f; cp->fy = o[1] / 32767.f; cp->fz = o[2] / 32767.f; cp->ux = o[3] / 32767.f; cp->uy = o[4] / 32767.f; cp->uz = o[5] / 32767.f;
            int16_t v[3]; memcpy(v, r + 30, 6); cp->vx = v[0] / 1000.f; cp->vy = v[1] / 1000.f; cp->vz = v[2] / 1000.f;
            cp->seen = now; cp->fresh = true;
        }
    }
    for (auto& cp : g_copies)
    {
        if (!cp.model) continue;
        if (now - cp.seen > 2000) { removeCopy(cp); cp.model = 0; continue; }
        if (!cp.ent)
        {
            if (!modelReady(cp.model)) continue;
            float pos[3] = { cp.x, cp.y, cp.z };
            if (cp.kind == 0) cp.ent = (uint8_t*)((void*(__cdecl*)(int, unsigned, const void*, bool))g_addPedReal)(cp.pedType ? cp.pedType : 4, cp.model, pos, false);
            else
            {
                cp.ent = (uint8_t*)((void*(__cdecl*)(int, float, float, float, bool))0x431F80)(cp.model, cp.x, cp.y, cp.z, false);   // CCarCtrl::CreateCarForScript
                if (cp.ent) *(uint32_t*)(cp.ent + addr::VehDoorLock) = 2;
            }
            if (cp.ent) InterlockedIncrement(&g_copiesMade);
            cp.fresh = true;
        }
        if (cp.ent && cp.fresh)
        {
            const float* m = *(const float* const*)(cp.ent + 0x14);
            float dx = m ? m[12] - cp.x : 99.f, dy = m ? m[13] - cp.y : 99.f, dz = m ? m[14] - cp.z : 99.f;
            if (dx * dx + dy * dy + dz * dz > 1.5f * 1.5f) teleport(cp.ent, cp.x, cp.y, cp.z);
            setOrientation(cp.ent, cp);
            float* sp = (float*)(cp.ent + 0x44); sp[0] = cp.vx; sp[1] = cp.vy; sp[2] = cp.vz;
            cp.fresh = false;
        }
    }
}

static void crowdFrame()
{
    if (g_role == 1) ownerSnapshot();
    if (g_role == 2) viewerCopies();
    else { static bool cleared = true; if (!cleared) { for (auto& cp : g_copies) if (cp.model) { removeCopy(cp); cp.model = 0; } } cleared = true; }
}

static DWORD WINAPI worker(LPVOID)
{
    Config cfg = readConfig();
    if (!cfg.enabled) return 0;
    char logPath[MAX_PATH]; snprintf(logPath, sizeof(logPath), "%s\\omp-crowd.log", g_dir);
    // Appended, one session after another (a crash leaves its trail); started afresh past 2 MB.
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool big = GetFileAttributesExA(logPath, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 2u * 1024 * 1024);
    g_log = fopen(logPath, big ? "w" : "a");
    if (g_log) fputs("\n----------------------------------------------------------------\n", g_log);
    logf("omp-crowd 0.6.0 (the server decides; local fallback) peds %s, density %.2f, at most %d; cars %s, density %.2f, at most %d", cfg.peds ? "on" : "off", cfg.pedDensity, cfg.maxPeds, cfg.cars ? "on" : "off", cfg.carDensity, cfg.maxCars);
    // In the game (and SA-MP has had its say) before anything.
    while (*(volatile int32_t*)addr::GameState != 9) Sleep(500);
    Sleep(8000);
    logf("in game");
    {
        char ini[MAX_PATH]; snprintf(ini, sizeof(ini), "%s\\omp-crowd.ini", g_dir);
        if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) snprintf(ini, sizeof(ini), "%s\\omp-crowd-dev.ini", g_dir);
        int relayPort = GetPrivateProfileIntA("world", "relay_port", 0, ini);
        if (GetPrivateProfileIntA("world", "relay", 1, ini)) CreateThread(nullptr, 0, linkThread, (LPVOID)(intptr_t)relayPort, 0, nullptr);
    }
    TextImage t;
    bool haveText = loadText(t);
    if (haveText && cfg.diag) logPatches(t);
    if (cfg.peds && haveText) restorePopulation(t);
    InitializeCriticalSection(&g_removersLock);
    InitializeCriticalSection(&g_ioLock);
    for (auto& cp : g_copies) { cp.netId = 0xFFFF; cp.model = 0; cp.ent = nullptr; }
    if (cfg.cars && haveText)
    {
        const uint8_t* d = diskAt(t, addr::AddPedInCarSeat);
        if (d && d[0] == 0xE8 && memcmp((const void*)addr::AddPedInCarSeat, d, 5) != 0)
        {
            writeCode(addr::AddPedInCarSeat, d, 5);
            logf("AddPedInCar: the driver's seat call put back (traffic has drivers)");
        }
        g_cars = true;
        // RemoveDistantCars' calls to PossiblyRemoveVehicle go through the keeping stub.
        int redirected = 0;
        for (uintptr_t a = kRemoveDistantCars; a < kRemoveDistantCars + 0x300; a++)
        {
            const uint8_t* dd = diskAt(t, a);
            if (!dd || dd[0] != 0xE8) continue;
            int32_t rel; memcpy(&rel, dd + 1, 4);
            if (a + 5 + rel != kPossiblyRemoveVehicle || memcmp((const void*)a, dd, 5) != 0) continue;
            uint8_t call[5] = { 0xE8 }; int32_t r2 = (int32_t)((uintptr_t)&possiblyRemoveVehicleStub - (a + 5)); memcpy(call + 1, &r2, 4);
            writeCode(a, call, 5); redirected++;
        }
        logf("cars kept near you: %d call(s) to PossiblyRemoveVehicle go through the keeper", redirected);
    }
    if (cfg.peds && haveText) hookRemovePed(diskAt(t, kRemovePed));
    // Keep the crowd's numbers where we want them (SA-MP, a mission or a cutscene may reset them).
    for (int tick = 0;; tick++)
    {
        if (tick % 15 == 0)
        {
            const int32_t* pl = (const int32_t*)addr::PedsLoaded;
            logf("tries: AddToPopulation %ld (yes %ld, distances %.1f %.1f %.1f %.1f) | AddPed %ld (made %ld)",
                (long)g_a2pCalls, (long)g_a2pYes, g_a2pArgs[0], g_a2pArgs[1], g_a2pArgs[2], g_a2pArgs[3], (long)g_addPedCalls, (long)g_addPedMade);
            int ps, pf; pedPool(ps, pf);
            char who[200] = ""; size_t w = 0;
            EnterCriticalSection(&g_removersLock);
            for (auto& r : g_removers) if (r.ret && w < sizeof(who) - 24) w += snprintf(who + w, sizeof(who) - w, " %08X x%ld", (unsigned)r.ret, (long)r.n);
            LeaveCriticalSection(&g_removersLock);
            logf("removed %ld (by%s) | kept near you: %ld peds, %ld cars | ped pool %d, %d free", (long)g_removed, who[0] ? who : " nobody", (long)g_keptPeds, (long)g_keptCars, ps, pf);
            logf("crowd: %u peds (%u civilians) | updates %ld | plan wants %.1f others | zone type %d (info %p) | countdown %u | ped models loaded %u [%d %d %d %d %d %d %d %d] | streaming %s",
                *(volatile uint32_t*)addr::TotalPeds, *(volatile uint32_t*)addr::TotalCivPeds, (long)g_popUpdates,
                *(volatile float*)addr::NumOtherPeds, *(volatile int32_t*)addr::CurrentZoneType, *(void* volatile*)addr::CurrZoneInfo,
                (unsigned)*(volatile uint8_t*)addr::CountDownToPeds, *(volatile uint32_t*)addr::NumPedsLoaded,
                pl[0], pl[1], pl[2], pl[3], pl[4], pl[5], pl[6], pl[7], *(volatile bool*)addr::DisableStreaming ? "off" : "on");
        }
        if (cfg.peds)
        {
            *(volatile float*)addr::PedDensityMultiplier = (!g_serverOn || g_role == 2) ? 0.0f : g_srvPedDensity;
            *(volatile uint32_t*)addr::MaxNumberOfPedsInUse = g_serverOn ? (uint32_t)g_srvMaxPeds : 0;
        }
        *(volatile float*)addr::CarDensityMultiplier = (g_cars && g_serverOn && g_role != 2) ? g_srvCarDensity : 0.0f;
        *(volatile uint32_t*)addr::MaxNumberOfCarsInUse = (g_cars && g_serverOn) ? (uint32_t)g_srvMaxCars : 0;
        int vt = 0, vr = 0, vl = 0; bool vs = true;
        if (g_cars) lockTraffic(vt, vr, vl, vs);
        if (g_cars && !vs) { g_cars = false; logf("traffic OFF: the vehicle pool doesn't look as expected (created-by byte out of 1-4)"); }
        if (tick % 15 == 0 && g_role) logf("sharing: %s (owner %ld) | copies made %ld, gone %ld", g_role == 1 ? "owner" : "viewer", (long)g_ownerId, (long)g_copiesMade, (long)g_copiesGone);
        if (tick % 15 == 0 && g_cars) logf("traffic: %d vehicles, %d random (%d locked), NumRandomCars %d", vt, vr, vl, *(volatile int32_t*)addr::NumRandomCars);
        Sleep(1000);
    }
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(mod);
        char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, sizeof(exe));
        strncpy(g_dir, exe, sizeof(g_dir)); g_dir[sizeof(g_dir) - 1] = 0;
        char* slash = strrchr(g_dir, '\\'); if (slash) *slash = 0;
        // A newer build dropped in as omp-crowd-dev.dll (testing without a new launcher) runs instead.
        char self[MAX_PATH]; GetModuleFileNameA(mod, self, sizeof(self));
        char dev[MAX_PATH]; snprintf(dev, sizeof(dev), "%s\\omp-crowd-dev.dll", g_dir);
        const char* base = strrchr(self, '\\');
        if (GetFileAttributesA(dev) != INVALID_FILE_ATTRIBUTES && !(base && _stricmp(base + 1, "omp-crowd-dev.dll") == 0))
        {
            LoadLibraryA(dev);
            return TRUE;
        }
        CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    }
    return TRUE;
}

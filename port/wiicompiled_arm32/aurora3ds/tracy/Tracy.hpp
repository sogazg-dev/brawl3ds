#pragma once
// 3DS stand-in for Tracy's <tracy/Tracy.hpp>, found before the real one by b3_aurora's include path: Aurora's
// ZoneScoped markers become cheap per-site timers (inclusive system ticks and a call count), so the match summary can
// say where Aurora's GX front end spends its time on the console. TracyPlot is dropped.
// Brawl3dsZonesReset()/Brawl3dsZonesWrite() are in port/wiicompiled_arm32/aurora3ds/backend_3ds.cpp.

#include <cstdint>
#include <cstdio>

extern "C" {
#include <3ds/svc.h>  // svcGetSystemTick; libctru headers have C linkage only through <3ds.h>
}

namespace brawl3ds_zones {
struct Site {
    const char* name;
    int line;
    uint64_t ticks;
    uint64_t calls;
    Site* next;
    bool registered;
};
Site*& Head() noexcept;  // backend_3ds.cpp

class Scope {
public:
    explicit Scope(Site& site) noexcept : m_site(site), m_start(svcGetSystemTick())
    {
        if (!m_site.registered) {  // first use: register the site
            m_site.registered = true;
            m_site.next = Head();
            Head() = &m_site;
        }
    }
    ~Scope()
    {
        m_site.ticks += svcGetSystemTick() - m_start;
        ++m_site.calls;
    }

private:
    Site& m_site;
    uint64_t m_start;
};
} // namespace brawl3ds_zones

#define BRAWL3DS_ZONE_CAT2(a, b) a##b
#define BRAWL3DS_ZONE_CAT(a, b) BRAWL3DS_ZONE_CAT2(a, b)
#define BRAWL3DS_ZONE(label)                                                                           \
    static brawl3ds_zones::Site BRAWL3DS_ZONE_CAT(brawl3dsZoneSite, __LINE__){label, __LINE__, 0, 0, nullptr, false}; \
    brawl3ds_zones::Scope BRAWL3DS_ZONE_CAT(brawl3dsZoneScope, __LINE__)(BRAWL3DS_ZONE_CAT(brawl3dsZoneSite, __LINE__))

#define ZoneScoped BRAWL3DS_ZONE(__FUNCTION__)
#define ZoneScopedN(name) BRAWL3DS_ZONE(name)
#define TracyPlot(name, value) ((void)0)

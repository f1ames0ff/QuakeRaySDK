#include "RadianceCache.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace
{

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition, ...)                          \
    do                                                 \
    {                                                  \
        g_checks++;                                    \
        if (!(condition))                              \
        {                                              \
            g_failures++;                              \
            std::printf("FAIL: ");                     \
            std::printf(__VA_ARGS__);                  \
            std::printf("\n");                         \
        }                                              \
    } while (0)

void TestEntryLayout()
{
    CHECK(sizeof(qray::RadianceCacheEntry) == 32u, "the cache entry stays at 32 bytes");
    CHECK(qray::RadianceCache::kEntryCount == 262144u, "the cache has 2^18 entries");
    CHECK((qray::RadianceCache::kEntryCount & (qray::RadianceCache::kEntryCount - 1u)) == 0u,
          "the entry count is a power of two");
}

void TestHash()
{
    CHECK(qray::RadianceCache::Hash(0u) == 0u, "hash(0) stays zero");
    CHECK(qray::RadianceCache::Hash(1u) != qray::RadianceCache::Hash(2u), "different inputs differ");
    CHECK(qray::RadianceCache::Hash(1u) == qray::RadianceCache::Hash(1u), "the hash is deterministic");
    CHECK(qray::RadianceCache::Hash(0xdeadbeefu) != qray::RadianceCache::Hash(0xdeadbeeeu),
          "one-bit input changes spread");
}

void TestLookupStability()
{
    const float camera[3] = { 0.0f, 0.0f, 0.0f };
    const float position[3] = { 128.0f, -64.0f, 32.0f };

    const qray::RadianceCache::Lookup a =
        qray::RadianceCache::MakeLookup(position, camera, 16.0f, 1024.0f);
    const qray::RadianceCache::Lookup b =
        qray::RadianceCache::MakeLookup(position, camera, 16.0f, 1024.0f);

    CHECK(a.key == b.key, "the same position and camera produce the same key");
    CHECK(a.index == (a.key & (qray::RadianceCache::kEntryCount - 1u)), "the index masks the key");
    CHECK(a.lod == 1u, "the near field uses the base LOD");

    const float other[3] = { 144.0f, -64.0f, 32.0f };
    const qray::RadianceCache::Lookup c =
        qray::RadianceCache::MakeLookup(other, camera, 16.0f, 1024.0f);
    CHECK(c.key != a.key, "a point in the next cell produces a different key");
}

void TestLodGrowth()
{
    const float camera[3] = { 0.0f, 0.0f, 0.0f };
    const float near[3] = { 0.0f, 0.0f, 0.0f };
    const float one[3] = { 1024.0f, 0.0f, 0.0f };
    const float three[3] = { 3072.0f, 0.0f, 0.0f };

    CHECK(qray::RadianceCache::MakeLookup(near, camera, 16.0f, 1024.0f).lod == 1u,
          "distance zero is LOD 1");
    CHECK(qray::RadianceCache::MakeLookup(one, camera, 16.0f, 1024.0f).lod == 2u,
          "one LOD distance doubles the cell once");
    CHECK(qray::RadianceCache::MakeLookup(three, camera, 16.0f, 1024.0f).lod == 4u,
          "three LOD distances quadruple the cell");
}

void TestPutGetAge()
{
    qray::RadianceCache cache;
    const float camera[3] = { 0.0f, 0.0f, 0.0f };
    const float position[3] = { 100.0f, 200.0f, 300.0f };
    const qray::RadianceCache::Lookup lookup =
        qray::RadianceCache::MakeLookup(position, camera, 16.0f, 1024.0f);
    const float radiance[3] = { 0.5f, 0.25f, 0.125f };
    float out[3] = {};

    CHECK(!cache.Get(lookup, 0u, 5u, out), "an unwritten entry is a miss");

    cache.Put(lookup, 0u, radiance);
    CHECK(cache.Get(lookup, 0u, 5u, out), "the same frame hits");
    CHECK(out[0] == radiance[0] && out[1] == radiance[1] && out[2] == radiance[2],
          "the cached RGB round-trips");

    CHECK(cache.Get(lookup, 5u, 5u, out), "the last allowed age still hits");
    CHECK(!cache.Get(lookup, 6u, 5u, out), "the entry expires after maxAge frames");

    cache.Clear();
    CHECK(!cache.Get(lookup, 0u, 5u, out), "clear drops every entry");
}

void TestKeyMismatch()
{
    qray::RadianceCache cache;
    const float camera[3] = { 0.0f, 0.0f, 0.0f };
    const float a[3] = { 64.0f, 0.0f, 0.0f };
    const float b[3] = { 144.0f, 0.0f, 0.0f };
    const qray::RadianceCache::Lookup la = qray::RadianceCache::MakeLookup(a, camera, 16.0f, 1024.0f);
    const qray::RadianceCache::Lookup lb = qray::RadianceCache::MakeLookup(b, camera, 16.0f, 1024.0f);
    const float radiance[3] = { 1.0f, 0.0f, 0.0f };
    float out[3] = {};

    cache.Put(la, 3u, radiance);

    if (la.index == lb.index)
    {
        CHECK(!cache.Get(lb, 3u, 30u, out), "a colliding key is rejected");
    }
    else
    {
        CHECK(!cache.Get(lb, 3u, 30u, out), "a different slot is a miss");
    }
}

void TestCounters()
{
    qray::RadianceCache cache;
    const float camera[3] = { 0.0f, 0.0f, 0.0f };
    const float position[3] = { 12.0f, 34.0f, 56.0f };
    const qray::RadianceCache::Lookup lookup =
        qray::RadianceCache::MakeLookup(position, camera, 16.0f, 1024.0f);
    const float radiance[3] = { 0.5f, 0.5f, 0.5f };
    float out[3] = {};

    CHECK(cache.Get(lookup, 7u, 5u, out) == false, "the first lookup misses");
    CHECK(cache.GetStats().lookups == 1u && cache.GetStats().misses == 1u,
          "a miss increments lookups and misses");
    CHECK(cache.GetStats().hits == 0u, "a miss does not increment hits");

    cache.Put(lookup, 7u, radiance);
    CHECK(cache.GetStats().puts == 1u && cache.GetStats().activeCells == 1u,
          "the first insert activates one cell");

    cache.Put(lookup, 7u, radiance);
    CHECK(cache.GetStats().activeCells == 1u, "a second insert in the same frame reuses the tag");

    cache.Put(lookup, 8u, radiance);
    CHECK(cache.GetStats().activeCells == 2u, "the next frame activates the cell again");

    CHECK(cache.Get(lookup, 8u, 5u, out), "the inserted entry hits");
    CHECK(cache.GetStats().hits == 1u, "a hit increments hits");

    CHECK(!cache.Get(lookup, 20u, 5u, out), "the expired entry misses");
    CHECK(cache.GetStats().misses == 2u, "the expiry increments misses");

    cache.Clear();
    CHECK(cache.GetStats().lookups == 0u && cache.GetStats().activeCells == 0u,
          "clear resets the counters");
}

void TestOverwrite()
{
    qray::RadianceCache cache;
    const float camera[3] = { 0.0f, 0.0f, 0.0f };
    const float position[3] = { 10.0f, 20.0f, 30.0f };
    const qray::RadianceCache::Lookup lookup =
        qray::RadianceCache::MakeLookup(position, camera, 16.0f, 1024.0f);
    const float first[3] = { 1.0f, 2.0f, 3.0f };
    const float second[3] = { 4.0f, 5.0f, 6.0f };
    float out[3] = {};

    cache.Put(lookup, 10u, first);
    cache.Put(lookup, 11u, second);

    CHECK(cache.Get(lookup, 11u, 30u, out), "the newer write hits");
    CHECK(out[0] == second[0] && out[1] == second[1] && out[2] == second[2],
          "the newer write replaces the older one");
}

}

int main()
{
    TestEntryLayout();
    TestHash();
    TestLookupStability();
    TestLodGrowth();
    TestPutGetAge();
    TestCounters();
    TestKeyMismatch();
    TestOverwrite();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

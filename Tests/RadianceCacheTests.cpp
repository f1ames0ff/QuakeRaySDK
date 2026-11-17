#include "RadianceCache.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

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

    CHECK(cache.Get(lookup, 11u, 30u, out), "the revisited cell hits");
    CHECK(out[0] == 2.5f && out[1] == 3.5f && out[2] == 4.5f,
          "two samples average the cell radiance");
    CHECK(cache.Entry(lookup.index).pad[0] == 2u, "the sample count accumulates");

    const float third[3] = { 7.0f, 8.0f, 9.0f };
    cache.Put(lookup, 12u, third);
    CHECK(cache.Get(lookup, 12u, 30u, out), "the third sample hits");
    CHECK(out[0] == 4.0f && out[1] == 5.0f && out[2] == 6.0f,
          "three samples use the running average");
    CHECK(cache.Entry(lookup.index).pad[0] == 3u, "the sample count reaches three");
}

void TestMetricCellScale()
{
    const float camera[3] = { 0.0f, 0.0f, 0.0f };
    const float p0[3] = { 0.05f, 0.10f, 0.15f };
    const float pSame[3] = { 0.24f, 0.24f, 0.24f };
    const float pNext[3] = { 0.26f, 0.10f, 0.15f };
    const float pLod2[3] = { 25.6f, 0.0f, 0.0f };

    const qray::RadianceCache::Lookup l0 =
        qray::RadianceCache::MakeLookup(p0, camera, 0.25f, 25.6f);
    const qray::RadianceCache::Lookup lSame =
        qray::RadianceCache::MakeLookup(pSame, camera, 0.25f, 25.6f);
    const qray::RadianceCache::Lookup lNext =
        qray::RadianceCache::MakeLookup(pNext, camera, 0.25f, 25.6f);
    const qray::RadianceCache::Lookup lLod2 =
        qray::RadianceCache::MakeLookup(pLod2, camera, 0.25f, 25.6f);

    CHECK(l0.lod == 1u && l0.key == lSame.key, "0.25 m cells group points within 25 cm");
    CHECK(l0.key != lNext.key, "crossing 0.25 m changes the cell key");
    CHECK(lLod2.lod == 2u, "metric 25.6 m LOD threshold doubles the cell size");
}

void TestNormalSeparation()
{
    qray::RadianceCache cache;
    const float camera[3] = { 0.0f, 0.0f, 0.0f };
    const float pos[3] = { 1.0f, 2.0f, 3.0f };
    const float posZ[3] = { 0.0f, 0.0f, 1.0f };
    const float negZ[3] = { 0.0f, 0.0f, -1.0f };
    const float posX[3] = { 1.0f, 0.0f, 0.0f };
    const float unnormalizedZ[3] = { 0.0f, 0.0f, 4.0f };
    const float zeroNormal[3] = { 0.0f, 0.0f, 0.0f };

    const qray::RadianceCache::Lookup lPosZ =
        qray::RadianceCache::MakeLookup(pos, posZ, camera, 0.25f, 25.6f);
    const qray::RadianceCache::Lookup lNegZ =
        qray::RadianceCache::MakeLookup(pos, negZ, camera, 0.25f, 25.6f);
    const qray::RadianceCache::Lookup lPosX =
        qray::RadianceCache::MakeLookup(pos, posX, camera, 0.25f, 25.6f);
    const qray::RadianceCache::Lookup lUnnorm =
        qray::RadianceCache::MakeLookup(pos, unnormalizedZ, camera, 0.25f, 25.6f);
    const qray::RadianceCache::Lookup lZero =
        qray::RadianceCache::MakeLookup(pos, zeroNormal, camera, 0.25f, 25.6f);

    CHECK(lPosZ.key != lNegZ.key, "opposite normals must produce distinct keys");
    CHECK(lPosZ.key != lPosX.key && lNegZ.key != lPosX.key,
          "orthogonal normals must produce distinct keys");
    CHECK(lPosZ.key == lUnnorm.key, "unnormalized normal must match unit normal");
    CHECK(lPosZ.key == lZero.key, "degenerate zero normal must fall back to +Z");

    const float warm[3] = { 2.0f, 1.0f, 0.5f };
    const float cool[3] = { 0.1f, 0.4f, 1.5f };
    const float green[3] = { 0.2f, 1.2f, 0.3f };
    cache.Put(lPosZ, 5u, warm);
    cache.Put(lNegZ, 5u, cool);
    cache.Put(lPosX, 5u, green);

    float out[3] = {};
    CHECK(cache.Get(lPosZ, 5u, 10u, out) && out[0] == warm[0], "+Z wall keeps its own radiance");
    CHECK(cache.Get(lNegZ, 5u, 10u, out) && out[2] == cool[2], "-Z wall keeps its own radiance");
    CHECK(cache.Get(lPosX, 5u, 10u, out) && out[1] == green[1], "+X wall keeps its own radiance");
}

void TestLinearProbing()
{
    const float camera[3] = { 0.0f, 0.0f, 0.0f };
    const float normal[3] = { 0.0f, 0.0f, 1.0f };

    // Collect 5 distinct cells hashing to the same primary bucket (index). Use a large
    // lodDistance so nearby integer cells do not collapse into coarse LOD blocks.
    std::vector<uint8_t> bucketCounts(qray::RadianceCache::kEntryCount, 0u);
    uint32_t targetBucket = 0u;
    uint32_t limit = 0u;
    bool foundBucket = false;

    for (uint32_t i = 0u; i < 1000000u; i++)
    {
        const float pos[3] = { float(i) * 0.25f + 0.1f, 0.1f, 0.1f };
        const qray::RadianceCache::Lookup cand =
            qray::RadianceCache::MakeLookup(pos, normal, camera, 0.25f, 1e9f);
        if (bucketCounts[cand.index] < 255u)
        {
            bucketCounts[cand.index]++;
        }
        if (bucketCounts[cand.index] >= 5u)
        {
            targetBucket = cand.index;
            limit = i + 1u;
            foundBucket = true;
            break;
        }
    }

    std::vector<qray::RadianceCache::Lookup> bucketKeys;
    if (foundBucket)
    {
        for (uint32_t i = 0u; i < limit && bucketKeys.size() < 5u; i++)
        {
            const float pos[3] = { float(i) * 0.25f + 0.1f, 0.1f, 0.1f };
            const qray::RadianceCache::Lookup cand =
                qray::RadianceCache::MakeLookup(pos, normal, camera, 0.25f, 1e9f);
            if (cand.index != targetBucket)
            {
                continue;
            }
            bool unique = true;
            for (const auto &existing : bucketKeys)
            {
                if (existing.key == cand.key)
                {
                    unique = false;
                    break;
                }
            }
            if (unique)
            {
                bucketKeys.push_back(cand);
            }
        }
    }

    CHECK(bucketKeys.size() == 5u, "must find 5 colliding keys in the same bucket");
    if (bucketKeys.size() < 5u)
    {
        return;
    }

    qray::RadianceCache cache;
    const float colors[5][3] = {
        { 1.0f, 0.0f, 0.0f },
        { 2.0f, 0.0f, 0.0f },
        { 3.0f, 0.0f, 0.0f },
        { 4.0f, 0.0f, 0.0f },
        { 5.0f, 0.0f, 0.0f },
    };

    for (uint32_t i = 0u; i < 4u; i++)
    {
        cache.Put(bucketKeys[i], 10u + i, colors[i], 30u);
    }

    float out[3] = {};
    for (uint32_t i = 0u; i < 4u; i++)
    {
        CHECK(cache.Get(bucketKeys[i], 15u, 30u, out) && out[0] == colors[i][0],
              "all four colliding keys in probe window must coexist and hit");
    }

    // In-place update at non-zero probe step (k2 at slot +2) averages instead of duplicating.
    const float k2Second[3] = { 7.0f, 0.0f, 0.0f };
    cache.Put(bucketKeys[2], 16u, k2Second, 30u);
    CHECK(cache.Get(bucketKeys[2], 16u, 30u, out) && out[0] == 5.0f,
          "revisiting a non-zero probe step must average in place");
    CHECK(cache.Entry((targetBucket + 2u) & (qray::RadianceCache::kEntryCount - 1u)).pad[0] == 2u,
          "non-zero probe step update must increment sample count");
    CHECK(cache.Get(bucketKeys[0], 16u, 30u, out) && cache.Get(bucketKeys[1], 16u, 30u, out) &&
              cache.Get(bucketKeys[3], 16u, 30u, out),
          "updating a probed key must preserve sibling keys");

    // Expired slot inside probe window (non-zero offset +1) is reclaimed cleanly with samples = 1.
    qray::RadianceCache expiryCache;
    expiryCache.Put(bucketKeys[0], 6u, colors[0], 30u);
    expiryCache.Put(bucketKeys[1], 7u, colors[1], 30u);
    expiryCache.Put(bucketKeys[0], 9u, colors[0], 30u); // refresh +0 while +1 ages out
    expiryCache.Put(bucketKeys[2], 10u, colors[2], 30u);
    expiryCache.Put(bucketKeys[3], 11u, colors[3], 30u);
    expiryCache.Put(bucketKeys[4], 12u, colors[4], 3u); // +1 age is 5 > 3 (expired); +0,+2,+3 live
    CHECK(expiryCache.Get(bucketKeys[4], 12u, 3u, out) && out[0] == colors[4][0],
          "new key must hit after reclaiming expired slot");
    CHECK(expiryCache.Entry((targetBucket + 1u) & (qray::RadianceCache::kEntryCount - 1u)).pad[0] == 1u,
          "reclaimed expired slot must reset sample count to 1");
    CHECK(!expiryCache.Get(bucketKeys[1], 12u, 3u, out), "reclaimed expired key must miss");
    CHECK(expiryCache.Get(bucketKeys[0], 12u, 3u, out) &&
              expiryCache.Get(bucketKeys[2], 12u, 3u, out) &&
              expiryCache.Get(bucketKeys[3], 12u, 3u, out),
          "reclaiming an expired slot must keep live sibling slots intact");

    // 5th live colliding key evicts the oldest live slot while keeping the newer three.
    qray::RadianceCache evictCache;
    evictCache.Put(bucketKeys[0], 1u, colors[0], 100u);
    evictCache.Put(bucketKeys[1], 2u, colors[1], 100u);
    evictCache.Put(bucketKeys[2], 3u, colors[2], 100u);
    evictCache.Put(bucketKeys[3], 4u, colors[3], 100u);
    evictCache.Put(bucketKeys[4], 5u, colors[4], 100u);
    CHECK(!evictCache.Get(bucketKeys[0], 5u, 100u, out), "5th live key must evict oldest live slot");
    CHECK(evictCache.Get(bucketKeys[1], 5u, 100u, out) &&
              evictCache.Get(bucketKeys[2], 5u, 100u, out) &&
              evictCache.Get(bucketKeys[3], 5u, 100u, out) &&
              evictCache.Get(bucketKeys[4], 5u, 100u, out),
          "newer 3 keys plus 5th key must all remain live after eviction");
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
    TestMetricCellScale();
    TestNormalSeparation();
    TestLinearProbing();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

#include "rt_alias.h"

#include <math.h>
#include <stdlib.h>

typedef struct AliasScratch
{
    double *scaled;
    int *smallIdx;
    int *largeIdx;
} AliasScratch;

static double AliasComputeTotalWeight(const double *weights, int count)
{
    double total = 0.0;
    for (int i = 0; i < count; i++)
    {
        if (isfinite(weights[i]) && weights[i] > 0.0)
            total += weights[i];
    }
    return total;
}

static void AliasPartitionWeights(const double *weights, int count, double total,
                                  const AliasScratch *scratch,
                                  int *outSmallCount, int *outLargeCount)
{
    int smallCount = 0;
    int largeCount = 0;
    const double scale = (double)count / total;

    for (int i = 0; i < count; i++)
    {
        const double w = (isfinite(weights[i]) && weights[i] > 0.0) ? weights[i] : 0.0;
        const double p = w * scale;
        scratch->scaled[i] = p;

        if (p < 1.0)
            scratch->smallIdx[smallCount++] = i;
        else
            scratch->largeIdx[largeCount++] = i;
    }

    *outSmallCount = smallCount;
    *outLargeCount = largeCount;
}

static void AliasCollapseBuckets(int smallCount, int largeCount, uint32_t *outAlias,
                                 const AliasScratch *scratch)
{
    while (smallCount > 0 && largeCount > 0)
    {
        const int s = scratch->smallIdx[--smallCount];
        const int l = scratch->largeIdx[--largeCount];

        outAlias[s] = (uint32_t)l;
        scratch->scaled[l] = (scratch->scaled[l] + scratch->scaled[s]) - 1.0;

        if (scratch->scaled[l] < 1.0)
            scratch->smallIdx[smallCount++] = l;
        else
            scratch->largeIdx[largeCount++] = l;
    }

    while (largeCount > 0)
        scratch->scaled[scratch->largeIdx[--largeCount]] = 1.0;

    while (smallCount > 0)
        scratch->scaled[scratch->smallIdx[--smallCount]] = 1.0;
}

static void AliasWritePrimary(int count, const double *scaledScratch, float *outPrimary)
{
    for (int i = 0; i < count; i++)
    {
        double p = scaledScratch[i];
        if (!isfinite(p) || p < 0.0)
            p = 0.0;
        if (p > 1.0)
            p = 1.0;
        outPrimary[i] = (float)p;
    }
}

int RT_Alias_BuildWithScratch(const double *weights, int count, float *outPrimary, uint32_t *outAlias,
                              double *scaledScratch, int *smallScratch, int *largeScratch)
{
    if (weights == NULL || outPrimary == NULL || outAlias == NULL || count <= 0)
        return 0;

    for (int i = 0; i < count; i++)
    {
        outPrimary[i] = 1.0f;
        outAlias[i] = (uint32_t)i;
    }

    if (count == 1)
        return 1;

    const double total = AliasComputeTotalWeight(weights, count);
    if (!(total > 0.0))
        return 1;

    if (scaledScratch == NULL || smallScratch == NULL || largeScratch == NULL)
        return 0;

    const AliasScratch scratch = { scaledScratch, smallScratch, largeScratch };
    int smallCount = 0;
    int largeCount = 0;
    AliasPartitionWeights(weights, count, total, &scratch, &smallCount, &largeCount);
    AliasCollapseBuckets(smallCount, largeCount, outAlias, &scratch);
    AliasWritePrimary(count, scaledScratch, outPrimary);

    return 1;
}

int RT_Alias_Build(const double *weights, int count, float *outPrimary, uint32_t *outAlias)
{
    if (weights == NULL || outPrimary == NULL || outAlias == NULL || count <= 0)
        return 0;

    if (count == 1)
    {
        outPrimary[0] = 1.0f;
        outAlias[0] = 0u;
        return 1;
    }

    enum { kStackCapacity = 256 };
    if (count <= kStackCapacity)
    {
        double scaledStack[kStackCapacity];
        int smallStack[kStackCapacity];
        int largeStack[kStackCapacity];
        return RT_Alias_BuildWithScratch(weights, count, outPrimary, outAlias,
                                         scaledStack, smallStack, largeStack);
    }

    double *scaled = (double *)malloc(sizeof(double) * (size_t)count);
    int *small = (int *)malloc(sizeof(int) * (size_t)count);
    int *large = (int *)malloc(sizeof(int) * (size_t)count);

    if (scaled == NULL || small == NULL || large == NULL)
    {
        free(scaled);
        free(small);
        free(large);
        return 0;
    }

    const int ok = RT_Alias_BuildWithScratch(weights, count, outPrimary, outAlias,
                                             scaled, small, large);

    free(scaled);
    free(small);
    free(large);

    return ok;
}

void RT_Alias_Marginals(const float *primary, const uint32_t *alias, int count, float *outMarginal)
{
    if (primary == NULL || alias == NULL || outMarginal == NULL || count <= 0)
        return;

    for (int i = 0; i < count; i++)
        outMarginal[i] = primary[i];

    for (int i = 0; i < count; i++)
    {
        const uint32_t chosen = alias[i];

        if (chosen < (uint32_t)count)
        {
            const float secondary = (primary[i] <= 1.0f) ? (1.0f - primary[i]) : 0.0f;

            outMarginal[chosen] += secondary;
        }
    }

    const float inverse = 1.0f / (float)count;

    for (int i = 0; i < count; i++)
    {
        float value = outMarginal[i] * inverse;

        if (!(value >= 0.0f))
            value = 0.0f;

        outMarginal[i] = value;
    }
}

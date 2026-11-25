// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#ifndef RT_ALIAS_H
#define RT_ALIAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_ALIAS_UNIFORM_PRIOR 0.05

int RT_Alias_Build(const double *weights, int count, float *outPrimary, uint32_t *outAlias);
int RT_Alias_BuildWithScratch(const double *weights, int count, float *outPrimary, uint32_t *outAlias,
                              double *scaledScratch, int *smallScratch, int *largeScratch);

void RT_Alias_Marginals(const float *primary, const uint32_t *alias, int count, float *outMarginal);

#ifdef __cplusplus
}
#endif

#endif

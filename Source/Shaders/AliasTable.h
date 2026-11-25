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

#ifndef ALIAS_TABLE_H_
#define ALIAS_TABLE_H_

int aliasColumn(const float u, const int count)
{
    return min(int(u * float(count)), count - 1);
}

uint aliasChoice(const float u, const int count, const int column, const float primary, const uint aliasIndex)
{
    const float fraction = u * float(count) - float(column);

    return (fraction < primary) ? uint(column) : aliasIndex;
}

#endif

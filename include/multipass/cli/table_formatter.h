/*
 * Copyright (C) 2018-2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef MULTIPASS_TABLE_FORMATTER
#define MULTIPASS_TABLE_FORMATTER

#include <multipass/cli/formatter.h>

namespace multipass
{
class TableFormatter final : public Formatter
{
public:
    std::string format(const InfoReply& info) const override;
    std::string format(const ListReply& list) const override;
    std::string format(const NetworksReply& list) const override;
    std::string format(const FindReply& list) const override;
    std::string format(const VersionReply& list) const override;
};
}
#endif // MULTIPASS_TABLE_FORMATTER

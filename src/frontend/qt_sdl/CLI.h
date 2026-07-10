/*
    Copyright 2021-2023 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef CLI_H
#define CLI_H

#include <QApplication>
#include <QStringList>

#include <optional>

namespace CLI {

struct CommandLineOptions
{
    std::optional<QString> dsRomPath;
    std::optional<QString> dsRomArchivePath;
    std::optional<QString> gbaRomPath;
    std::optional<QString> gbaRomArchivePath;
    bool fullscreen;
    bool boot;
};

extern CommandLineOptions* ManageArgs(QApplication& melon);

enum class NetplayAutotestRole
{
    Disabled,
    Host,
    Client,
};

struct NetplayAutotestConfig
{
    NetplayAutotestRole role = NetplayAutotestRole::Disabled;
    QString host = "127.0.0.1";
    int port = 8064;
    unsigned int seed = 1;
    int durationSeconds = 0;
};

extern NetplayAutotestConfig netplayAutotest;

}

#endif

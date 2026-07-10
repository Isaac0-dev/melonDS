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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QStringList>

#include "CLI.h"
#include "Platform.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

bool forceServer = false;
bool forceClient = false;

namespace CLI
{

NetplayAutotestConfig netplayAutotest;

CommandLineOptions* ManageArgs(QApplication& melon)
{
    QCommandLineParser parser;
    parser.addHelpOption();

    parser.addPositionalArgument("nds", "Nintendo DS ROM (or an archive file which contains it) to load into Slot-1");
    parser.addPositionalArgument("gba", "GBA ROM (or an archive file which contains it) to load into Slot-2");

    parser.addOption(QCommandLineOption({"b", "boot"}, "Whether to boot firmware on startup. Defaults to \"auto\" (boot if NDS rom given)", "auto/always/never", "auto"));
    parser.addOption(QCommandLineOption({"f", "fullscreen"}, "Start melonDS in fullscreen mode"));
    parser.addOption(QCommandLineOption({"s", "server"}, "dev mode server netplay"));
    parser.addOption(QCommandLineOption({"c", "client"}, "dev mode client netplay"));
    parser.addOption(QCommandLineOption("netplay-autotest", "Run unattended netplay test mode as host or client", "host/client"));
    parser.addOption(QCommandLineOption("netplay-autotest-host", "Host address for netplay autotest clients", "host", "127.0.0.1"));
    parser.addOption(QCommandLineOption("netplay-autotest-port", "Port for netplay autotest", "port", "8064"));
    parser.addOption(QCommandLineOption("netplay-autotest-seed", "Seed for deterministic netplay autotest inputs", "seed", "1"));
    parser.addOption(QCommandLineOption("netplay-autotest-duration", "Seconds before autotest exits cleanly; 0 disables in-emulator auto-exit", "seconds", "0"));

#ifdef ARCHIVE_SUPPORT_ENABLED
    parser.addOption(QCommandLineOption({"a", "archive-file"}, "Specify file to load inside an archive given (NDS)", "rom"));
    parser.addOption(QCommandLineOption({"A", "archive-file-gba"}, "Specify file to load inside an archive given (GBA)", "rom"));
#endif

    parser.process(melon);

    CommandLineOptions* options = new CommandLineOptions;

    options->fullscreen = parser.isSet("fullscreen");
    forceServer = parser.isSet("server");
    forceClient = parser.isSet("client");

    netplayAutotest = NetplayAutotestConfig();
    if (parser.isSet("netplay-autotest"))
    {
        QString role = parser.value("netplay-autotest");
        if (role == "host")
            netplayAutotest.role = NetplayAutotestRole::Host;
        else if (role == "client")
            netplayAutotest.role = NetplayAutotestRole::Client;
        else
        {
            Log(LogLevel::Error, "ERROR: --netplay-autotest only accepts host/client as arguments\n");
            exit(1);
        }

        bool ok = false;
        netplayAutotest.host = parser.value("netplay-autotest-host");
        netplayAutotest.port = parser.value("netplay-autotest-port").toInt(&ok);
        if (!ok || netplayAutotest.port <= 0 || netplayAutotest.port > 65535)
        {
            Log(LogLevel::Error, "ERROR: --netplay-autotest-port must be a valid TCP/UDP port\n");
            exit(1);
        }

        netplayAutotest.seed = parser.value("netplay-autotest-seed").toUInt(&ok, 0);
        if (!ok)
        {
            Log(LogLevel::Error, "ERROR: --netplay-autotest-seed must be an unsigned integer\n");
            exit(1);
        }

        netplayAutotest.durationSeconds = parser.value("netplay-autotest-duration").toInt(&ok);
        if (!ok || netplayAutotest.durationSeconds < 0)
        {
            Log(LogLevel::Error, "ERROR: --netplay-autotest-duration must be a non-negative integer\n");
            exit(1);
        }
    }

    QStringList posargs = parser.positionalArguments();
    switch (posargs.size())
    {
        default:
            Log(LogLevel::Warn, "Too many positional arguments; ignoring 3 onwards\n");
        case 2:
            options->gbaRomPath = posargs[1];
        case 1:
            options->dsRomPath = posargs[0];
        case 0:
            break;
    }

    QString bootMode = parser.value("boot");
    if (bootMode == "auto")
    {
        options->boot = !posargs.empty();
    }
    else if (bootMode == "always")
    {
        options->boot = true;
    }
    else if (bootMode == "never")
    {
        options->boot = false;
    }
    else
    {
        Log(LogLevel::Error, "ERROR: -b/--boot only accepts auto/always/never as arguments\n");
        exit(1);
    }

#ifdef ARCHIVE_SUPPORT_ENABLED
    if (parser.isSet("archive-file"))
    {
        if (options->dsRomPath.has_value())
        {
            options->dsRomArchivePath = parser.value("archive-file");
        }
        else
        {
            Log(LogLevel::Error, "Option -a/--archive-file given, but no archive specified!");
        }
    }

    if (parser.isSet("archive-file-gba"))
    {
        if (options->gbaRomPath.has_value())
        {
            options->gbaRomArchivePath = parser.value("archive-file-gba");
        }
        else
        {
            Log(LogLevel::Error, "Option -A/--archive-file-gba given, but no archive specified!");
        }
    }
#endif

    return options;
}

}

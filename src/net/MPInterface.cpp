/*
    Copyright 2016-2025 melonDS team

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

#include "MPInterface.h"
#include "LocalMP.h"
#include "LAN.h"
#include "Netplay.h"

namespace melonDS
{

class DummyMP : public MPInterface
{
public:
    MPInterfaceType GetInterfaceType() const override { return MPInterface_Dummy; }

    void Process() override {}

    void Begin(int inst) override {}
    void End(int inst) override {}

    int SendPacket(int inst, u8* data, int len, u64 timestamp) override { return 0; }
    int RecvPacket(int inst, u8* data, u64* timestamp) override { return 0; }
    int SendCmd(int inst, u8* data, int len, u64 timestamp) override { return 0; }
    int SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid) override { return 0; }
    int SendAck(int inst, u8* data, int len, u64 timestamp) override { return 0; }
    int RecvHostPacket(int inst, u8* data, u64* timestamp) override { return 0; }
    u16 RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask) override { return 0; }
};


std::shared_ptr<MPInterface> MPInterface::Current(std::make_shared<DummyMP>());
std::mutex MPInterface::CurrentMutex;


void MPInterface::Set(MPInterfaceType type)
{
    std::lock_guard<std::mutex> lock(CurrentMutex);
    switch (type)
    {
    case MPInterface_Local:
        Current = std::make_shared<LocalMP>();
        break;

    case MPInterface_LAN:
        Current = std::make_shared<LAN>();
        break;

    case MPInterface_Netplay:
        Current = std::make_shared<Netplay>();
        break;

    default:
        Current = std::make_shared<DummyMP>();
        break;
    }
}

}

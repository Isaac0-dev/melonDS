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

#ifndef MPINTERFACE_H
#define MPINTERFACE_H

#include <memory>
#include <mutex>
#include "types.h"

namespace melonDS
{

// TODO: provision for excluding unwanted interfaces at compile time
enum MPInterfaceType
{
    MPInterface_Dummy = -1,
    MPInterface_Local,
    MPInterface_LAN,
    MPInterface_Netplay,
};

struct MPPacketHeader
{
    u32 Magic;
    u32 SenderID;
    u32 Type;       // 0=regular 1=CMD 2=reply 3=ack
    u32 Length;
    u64 Timestamp;
};

class MPInterface
{
public:
    virtual ~MPInterface() = default;

    static MPInterface& Get()
    {
        // Safe only within the full expression; the shared_ptr copy from
        // GetShared() keeps the object alive until the ';'.
        // Long-lived holders must use GetShared() instead.
        return *GetShared();
    }
    static std::shared_ptr<MPInterface> GetShared()
    {
        std::lock_guard<std::mutex> lock(CurrentMutex);
        return Current;
    }
    static MPInterfaceType GetType()
    {
        std::lock_guard<std::mutex> lock(CurrentMutex);
        auto ptr = Current;
        return ptr ? ptr->GetInterfaceType() : MPInterface_Dummy;
    }
    static void Set(MPInterfaceType type);

    virtual MPInterfaceType GetInterfaceType() const = 0;

    [[nodiscard]] int GetRecvTimeout() const noexcept { return RecvTimeout; }
    void SetRecvTimeout(int timeout) noexcept { RecvTimeout = timeout; }

    // function called every video frame
    virtual void Process() = 0;

    virtual void Begin(int inst) = 0;
    virtual void End(int inst) = 0;

    virtual int SendPacket(int inst, u8* data, int len, u64 timestamp) = 0;
    virtual int RecvPacket(int inst, u8* data, u64* timestamp) = 0;
    virtual int SendCmd(int inst, u8* data, int len, u64 timestamp) = 0;
    virtual int SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid) = 0;
    virtual int SendAck(int inst, u8* data, int len, u64 timestamp) = 0;
    virtual int RecvHostPacket(int inst, u8* data, u64* timestamp) = 0;
    virtual u16 RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask) = 0;

protected:
    int RecvTimeout = 25;

private:
    static std::shared_ptr<MPInterface> Current;
    static std::mutex CurrentMutex;
};

}

#endif // MPINTERFACE_H

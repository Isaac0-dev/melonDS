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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <queue>
#include <cassert>
#include <memory>

#include <enet/enet.h>
#include <zlib.h>

#include "NDSCart.h"
#include "Netplay.h"
#include "Savestate.h"
#include "Platform.h"
#include <chrono>


namespace melonDS {

struct TimedLock
{
    Platform::Mutex* M;
    u64 Start;
    const char* LockName;

    TimedLock(Platform::Mutex* m, const char* name)
    {
        M = m;
        LockName = name;
        Start = Platform::GetMSCount();
        Platform::Mutex_Lock(M);
    }

    ~TimedLock()
    {
        u64 held = Platform::GetMSCount() - Start;
        if (held > 5)
        {
            Platform::Log(Platform::LogLevel::Warn, "[PERF] %s Mutex held too long: %llums\n", LockName, held);
        }
        Platform::Mutex_Unlock(M);
    }
};

const u32 kNetplayMagic = 0x5054454E; // NETP

const u32 kProtocolVersion = 1;

const u32 kLocalhost = 0x0100007F;

enum
{
    Chan_Input0 = 0,        // channels 0-15 -- input from players 0-15 resp.
    Chan_Cmd = 16,          // channel 16 -- control commands
    Chan_Blob,              // channel 17 -- blob
    Chan_Max,
};

enum
{
    Cmd_ClientInit = 1,             // 01 -- host->client -- init new client and assign ID
    Cmd_PlayerInfo,                 // 02 -- client->host -- send client player info to host
    Cmd_PlayerList,                 // 03 -- host->client -- broadcast updated player list
    Cmd_StartGame,                  // 04 -- host->client -- start the game on all players
    Cmd_UpdateSettings,             // 05 -- host->client -- game settings changed
};

std::function<void(int)> OnStartEmulatorThread = nullptr;

Netplay::Netplay() noexcept : LocalMP(), Inited(false)
{
    Active = false;
    IsHost = false;
    GameInited = false;
    Host = nullptr;
    StallFrame = false;
    Settings.Delay = 4;

    CurBlobType = -1;
    CurBlobLen = 0;

    for (int i = 0; i < Blob_MAX; i++)
    {
        Blobs[i] = nullptr;
        BlobLens[i] = 0;
    }

    PlayersMutex = Platform::Mutex_Create();
    InstanceMutex = Platform::Mutex_Create();
    NetworkMutex = Platform::Mutex_Create();

    memset(RemotePeers, 0, sizeof(RemotePeers));
    memset(Players, 0, sizeof(Players));
    NumPlayers = 0;
    MaxPlayers = 0;

    memset(PlayerToInstance, 0, sizeof(PlayerToInstance));
    memset(InstanceToPlayer, 0, sizeof(InstanceToPlayer));

    MirrorMode = true;
    NumMirrorClients = 0;
    for (int i = 0; i < 16; ++i)
    {
        PendingFrames[i].Active = false;
        PendingFrames[i].FrameNum = 0;
        PendingFrames[i].SavestateBuffer = nullptr;
        nds_instances[i] = nullptr;
    }
    PacketSequenceCounter = 1;
    DesyncDumped = false;

    // TODO make this somewhat nicer
    if (enet_initialize() != 0)
    {
        Platform::Log(Platform::LogLevel::Error, "Netplay: failed to initialize enet\n");
        return;
    }

    Platform::Log(Platform::LogLevel::Info, "Netplay: enet initialized\n");
    Inited = true;
}

Netplay::~Netplay() noexcept
{
    EndSession();

    Inited = false;
    enet_deinitialize();

    Platform::Mutex_Free(PlayersMutex);
    Platform::Mutex_Free(InstanceMutex);
    Platform::Mutex_Free(NetworkMutex);

    Platform::Log(Platform::LogLevel::Info, "Netplay: enet deinitialized\n");
}

// To be called just before a game starts
// consoleType: -1 = keep current, 0 = DS, 1 = DSi
bool Netplay::InitGame(int consoleType)
{
    if (GameInited) return true;

    if (!OnStartEmulatorThread)
    {
        printf("error, tried to start netplay game with OnStartEmulatorThread null!\n");
        return false;
    }

    OnStartEmulatorThread(consoleType); // Hack to access frontend code

    GameInited = true;
    return true;
}

std::vector<Netplay::Player> Netplay::GetPlayerList()
{
    Platform::Mutex_Lock(PlayersMutex);

    std::vector<Player> ret;
    for (int i = 0; i < 16; i++)
    {
        if (Players[i].Status == Player_None) continue;

        // make a copy of the player entry, fix up the address field
        Player newp = Players[i];
        if (newp.ID == MyPlayer.ID)
        {
            newp.IsLocalPlayer = true;
            newp.Address = kLocalhost;
        }
        else
        {
            newp.IsLocalPlayer = false;
            if (newp.Status == Player_Host)
                newp.Address = HostAddress;
        }

        ret.push_back(newp);
    }

    Platform::Mutex_Unlock(PlayersMutex);
    return ret;
}

// Gets the local index of the player with that port
// helper: make a 64-bit key from host and port
static inline u64 MakeEndpointKey(u32 host, u16 port)
{
    return (static_cast<u64>(host) << 32) | port;
}

int Netplay::GetPlayerIndexFromEndpoint(u32 host, u16 port)
{
    u64 key = MakeEndpointKey(host, port);
    auto it = PortToPlayerIndex.find(key);
    if (it != PortToPlayerIndex.end()) {
        return it->second;
    }
    return -1;
}

bool Netplay::StartHost(const char* playername, int port)
{
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;

    Host = enet_host_create(&addr, 16, Chan_Max, 0, 0);
    if (!Host)
    {
        printf("host shat itself :(\n");
        return false;
    }

    Platform::Mutex_Lock(PlayersMutex);

    Player* player = &Players[0];
    memset(player, 0, sizeof(Player));
    player->ID = 0;
    strncpy(player->Name, playername, 31);
    player->Status = Player_Host;
    player->Address = 0x0100007F;
    NumPlayers = 1;
    MaxPlayers = 16;
    memcpy(&MyPlayer, player, sizeof(Player));

    Platform::Mutex_Unlock(PlayersMutex);

    HostAddress = 0x0100007F;

    Active = true;
    IsHost = true;
    DesyncDumped = false;

    return true;
}

bool Netplay::StartClient(const char* playername, const char* host, int port)
{
    Host = enet_host_create(nullptr, 16, Chan_Max, 0, 0);
    if (!Host)
    {
        printf("connection failed\n");
        return false;
    }

    printf("client created, connecting (%s, %s:%d)\n", playername, host, port);

    ENetAddress addr;
    enet_address_set_host(&addr, host);
    addr.port = port;
    ENetPeer* peer = enet_host_connect(Host, &addr, Chan_Max, 0);
    if (!peer)
    {
        printf("connection failed\n");
        return false;
    }

    Platform::Mutex_Lock(PlayersMutex);

    Player* player = &MyPlayer;
    memset(player, 0, sizeof(Player));
    player->ID = 0;
    strncpy(player->Name, playername, 31);
    player->Status = Player_Connecting;

    Platform::Mutex_Unlock(PlayersMutex);

    ENetEvent event;
    int conn = 0;
    u32 starttick = (u32)Platform::GetMSCount();
    const int conntimeout = 5000;
    for (;;)
    {
        u32 curtick = (u32)Platform::GetMSCount();
        if (curtick < starttick) break;
        int timeout = conntimeout - (int)(curtick - starttick);
        if (timeout < 0) break;
        if (enet_host_service(Host, &event, timeout) > 0)
        {
            if (conn == 0 && event.type == ENET_EVENT_TYPE_CONNECT)
            {
                conn = 1;
            }
            else if (conn == 1 && event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                u8* data = event.packet->data;
                if (event.channelID != Chan_Cmd) continue;
                if (data[0] != Cmd_ClientInit) continue;
                if (event.packet->dataLength != 11 + sizeof(NetworkSettings)) continue;

                u32 magic = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
                u32 version = data[5] | (data[6] << 8) | (data[7] << 16) | (data[8] << 24);
                if (magic != kNetplayMagic) continue;
                if (version != kProtocolVersion) continue;
                if (data[10] > 16) continue;

                MaxPlayers = data[10];
                memcpy(&Settings, &data[11], sizeof(NetworkSettings));

                // send player information
                MyPlayer.ID = data[9];
                u8 cmd[9+sizeof(Player)];
                cmd[0] = Cmd_PlayerInfo;
                cmd[1] = (u8)kNetplayMagic;
                cmd[2] = (u8)(kNetplayMagic >> 8);
                cmd[3] = (u8)(kNetplayMagic >> 16);
                cmd[4] = (u8)(kNetplayMagic >> 24);
                cmd[5] = (u8)kProtocolVersion;
                cmd[6] = (u8)(kProtocolVersion >> 8);
                cmd[7] = (u8)(kProtocolVersion >> 16);
                cmd[8] = (u8)(kProtocolVersion >> 24);
                memcpy(&cmd[9], &MyPlayer, sizeof(Player));
                ENetPacket* pkt = enet_packet_create(cmd, 9+sizeof(Player), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(event.peer, Chan_Cmd, pkt);

                conn = 2;
                break;
            }
            else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                conn = 0;
                break;
            }
        }
        else
            break;
    }

    if (conn != 2)
    {
        enet_peer_reset(peer);
        enet_host_destroy(Host);
        Host = nullptr;
        Platform::Log(Platform::LogLevel::Error, "Netplay: connection failed\n");
        return false;
    }

    HostAddress = addr.host;
    RemotePeers[0] = peer;
    // register the mapping from remote port to our local player index
    PortToPlayerIndex[MakeEndpointKey(peer->address.host, peer->address.port)] = 0;

    Active = true;
    IsHost = false;
    DesyncDumped = false;
    Platform::Log(Platform::LogLevel::Info, "Netplay: connected as client\n");
    return true;
}

void Netplay::EndSession()
{
    Active = false;
    StallFrame = false;
    GameInited = false;
    SyncInProgress = false;

    Platform::Mutex_Lock(NetworkMutex);
    for (int i = 0; i < 16; i++)
    {
        if (i == MyPlayer.ID) continue;

        if (RemotePeers[i])
            enet_peer_disconnect(RemotePeers[i], 0);

        RemotePeers[i] = nullptr;
    }

    if (Host)
    {
        enet_host_destroy(Host);
        Host = nullptr;
    }
    IsHost = false;
    Platform::Mutex_Unlock(NetworkMutex);

    Platform::Mutex_Lock(InstanceMutex);
    for (int i = 0; i < 16; i++)
    {
        PendingFrames[i].Active = false;
        PendingFrames[i].FrameNum = 0;
        PendingFrames[i].SavestateBuffer.reset();
        nds_instances[i] = nullptr;
        InputHistory[i].clear();
        StateSnapshots[i].clear();
        ReusableStates[i].reset();
    }
    Platform::Mutex_Unlock(InstanceMutex);

    for (int i = 0; i < Blob_MAX; i++)
    {
        if (Blobs[i])
        {
            delete[] Blobs[i];
            Blobs[i] = nullptr;
        }
        BlobLens[i] = 0;
    }
    CurBlobType = -1;
    CurBlobLen = 0;
    BlobInProgress = false;

    PortToPlayerIndex.clear();
}

void Netplay::SendNetworkSettings()
{
    if (!IsHost) return;

    u8 cmd[1 + sizeof(NetworkSettings)];
    cmd[0] = Cmd_UpdateSettings;
    memcpy(&cmd[1], &Settings, sizeof(NetworkSettings));
    ENetPacket* pkt = enet_packet_create(cmd, sizeof(cmd), ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast(Host, Chan_Cmd, pkt);
}

void Netplay::SetInputBufferSize(int value)
{
    if (!IsHost) return;
    Settings.Delay = value;

    // tell clients that it changed
    SendNetworkSettings();
}

void Netplay::SetSyncMethod(SyncMethod method)
{
    if (!IsHost) return;
    Settings.SyncMode = (int)method;
    SendNetworkSettings();
}

bool Netplay::SendBlob(int type, u32 len, u8* data)
{
    if (!Host) return false;

    u8* buf = ChunkBuffer;

    buf[0] = 0x01;
    buf[1] = type & 0xFF;
    buf[2] = 0;
    buf[3] = 0;
    *(u32*)&buf[4] = len;

    u32 crc = crc32(0L, Z_NULL, 0);
    if (len > 0)
        crc = crc32(crc, data, len);
    *(u32*)&buf[8] = crc;

    printf("send blob, type %d, len 0x%08X, hash: 0x%08X\n", type, len, crc);

    ENetPacket* pkt = enet_packet_create(buf, 12, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast(Host, Chan_Blob, pkt);

    if (len > 0)
    {
        buf[0] = 0x02;
        *(u32*)&buf[12] = 0;

        for (u32 pos = 0; pos < len; pos += kChunkSize)
        {
            u32 chunklen = kChunkSize;
            if ((pos + chunklen) > len)
                chunklen = len - pos;

            *(u32*)&buf[8] = pos;
            memcpy(&buf[16], &data[pos], chunklen);

            ENetPacket* pkt = enet_packet_create(buf, 16+chunklen, ENET_PACKET_FLAG_RELIABLE);
            enet_host_broadcast(Host, Chan_Blob, pkt);
        }
    }

    buf[0] = 0x03;
    *(u32*)&buf[8] = crc;

    pkt = enet_packet_create(buf, 12, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast(Host, Chan_Blob, pkt);

    return true;
}

void Netplay::RecvBlob(ENetPeer* peer, ENetPacket* pkt, int inst)
{
    u8* buf = pkt->data;
    if (buf[0] == 0x01)
    {
        if (pkt->dataLength != 12) return;

        int type = buf[1];
        if (type >= Blob_MAX) return;

        u32 len = *(u32*)&buf[4];
        if (len > 0x40000000) return;

        // Clean up previous incomplete/stale blob if any
        if (CurBlobType != -1)
        {
            Platform::Log(Platform::LogLevel::Warn, "Netplay: Starting new blob %d while previous blob %d was incomplete, cleaning up.\n", type, CurBlobType);
            if (Blobs[CurBlobType])
            {
                delete[] Blobs[CurBlobType];
                Blobs[CurBlobType] = nullptr;
            }
            BlobLens[CurBlobType] = 0;
            CurBlobType = -1;
            BlobInProgress = false;
        }

        // Clean up existing completed blob of the same type if not already processed/freed
        if (Blobs[type] != nullptr)
        {
            delete[] Blobs[type];
            Blobs[type] = nullptr;
        }
        BlobLens[type] = 0;

        Platform::Log(Platform::LogLevel::Info, "Netplay: starting blob transfer, type %d, len %d\n", type, len);
        if (len) Blobs[type] = new u8[len];
        BlobLens[type] = len;

        CurBlobType = type;
        CurBlobLen = len;
        CurBlobCRC = crc32(0L, Z_NULL, 0);
        BlobCurrSize = 0;
        BlobInProgress = true;
    }
    else if (buf[0] == 0x02)
    {
        if (CurBlobType < 0 || CurBlobType >= Blob_MAX) return;
        if (pkt->dataLength > (16+kChunkSize)) return;

        int type = buf[1];
        if (type != CurBlobType) return;

        u32 len = *(u32*)&buf[4];
        if (len != CurBlobLen) return;

        u32 pos = *(u32*)&buf[8];
        if (pos >= len) return;
        if ((pos + (pkt->dataLength-16)) > len) return;

        u8* dst = Blobs[type];
        if (!dst) return;
        if (BlobLens[type] != len) return;
        memcpy(&dst[pos], &buf[16], pkt->dataLength-16);
        BlobCurrSize += pkt->dataLength-16;
        CurBlobCRC = crc32(CurBlobCRC, &buf[16], pkt->dataLength - 16);
    }
    else if (buf[0] == 0x03)
    {
        if (CurBlobType < 0 || CurBlobType >= Blob_MAX) return;
        if (pkt->dataLength != 12) return;

        int type = buf[1];
        if (type != CurBlobType) return;

        u32 len = *(u32*)&buf[4];
        if (len != CurBlobLen) return;

        u32 ExpectedCRC = *(u32*)&buf[8];
        if (CurBlobCRC != ExpectedCRC) {
            Platform::Log(Platform::LogLevel::Error, "Netplay: blob %d CRC mismatch! expected 0x%08X got 0x%08X\n",
                   CurBlobType, ExpectedCRC, CurBlobCRC);
            // CRC failure - clean up the failed blob
            if (Blobs[type])
            {
                delete[] Blobs[type];
                Blobs[type] = nullptr;
            }
            BlobLens[type] = 0;
            CurBlobType = -1;
            BlobInProgress = false;
            return;
        }

        Platform::Log(Platform::LogLevel::Info, "Netplay: finished blob transfer, type %d, len %d\n", type, len);
        CurBlobType = -1;
        CurBlobLen = 0;
        BlobInProgress = false;
    }
    else if (buf[0] == 0x04)
    {
        if (pkt->dataLength != 2) return;

        // Pass the state's console type so the frontend can create
        // the correct NDS derived type (NDS vs DSi) before loading.
        InitGame(buf[1]);

        // InitGame may have recreated the NDS (via updateConsole).
        // The frontend callback already registered the new pointer at
        // nds_instances[MyPlayer.ID] and nds_instances[0]; scan for it.
        NDS* nds = nullptr;
        for (int i = 0; i < 16; i++)
        {
            if (nds_instances[i])
            {
                nds = nds_instances[i];
                break;
            }
        }
        if (!nds) return;

        // reset
        nds->ConsoleType = buf[1];

        Platform::Log(Platform::LogLevel::Info, "Netplay: loading synced state for instance %d, console type %d, size 0x%08X\n", inst, nds->ConsoleType, BlobCurrSize);

        // load initial state
        Savestate* state = new Savestate(Blobs[Blob_InitState], BlobLens[Blob_InitState], false);
        u32 loadStart = Platform::GetMSCount();
        bool success = nds->DoSavestate(state);
        u32 loadEnd = Platform::GetMSCount();
        delete state;

        for (int i = 0; i < Blob_MAX; i++)
        {
            if (Blobs[i]) delete[] Blobs[i];
            Blobs[i] = nullptr;
            BlobLens[i] = 0;
        }

        if (success) {
            Platform::Log(Platform::LogLevel::Info, "Netplay: state loaded, instance %d, frame: %d, PC=%08X/%08X, load_ms=%u\n", inst, nds->NumFrames, nds->GetPC(0), nds->GetPC(1), (unsigned)(loadEnd - loadStart));
        } else {
            Platform::Log(Platform::LogLevel::Error, "Netplay: failed to load synced state for instance %d, load_ms=%u\n", inst, (unsigned)(loadEnd - loadStart));
        }
        ENetPacket* resp = enet_packet_create(buf, 1, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer, Chan_Blob, resp);
    }
}

bool Netplay::SyncClients()
{
    if (!IsHost) return false;

    SyncInProgress = true;
    bool success = true;

    // Capture the state snapshot outside the lock so
    // enet_host_service can flush queued packets during polling.
    NDS* nds = nds_instances[0];
    if (!nds || !Host)
    {
        SyncInProgress = false;
        return false;
    }

    u32 stateLen;
    u8 *stateBuf;

    std::unique_ptr<Savestate> state = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
    nds->DoSavestate(state.get());
    stateLen = state->Length();
    stateBuf = (u8 *) state->Buffer();
    Platform::Log(Platform::LogLevel::Info, "[HOST] syncing clients. sending save state with size %d\n", stateLen);

    {
        TimedLock lock(NetworkMutex, "SyncClients_Init");
        if (!Host)
        {
            SyncInProgress = false;
            return false;
        }
        SendBlob(Blob_InitState, stateLen, stateBuf);

        u8 data[2];
        data[0] = 0x04;
        data[1] = (u8) nds->ConsoleType;
        ENetPacket* pkt = enet_packet_create(&data, 2, ENET_PACKET_FLAG_RELIABLE);
        enet_host_broadcast(Host, Chan_Blob, pkt);

        // Flush queued packets so clients start receiving data immediately.
        // enet_host_broadcast with RELIABLE only queues; packets aren't
        // sent on the wire until enet_host_service or enet_host_flush runs.
        enet_host_flush(Host);
    }

    int ngood = 0;
    ENetEvent evt;
    u64 startTime = Platform::GetMSCount();
    const u64 timeoutMS = 120000; // 2 minutes for large blobs over tunnels

// Main polling loop runs with granular locking per service turn
    while (ngood < (NumPlayers - 1))
    {
        if (Platform::GetMSCount() - startTime > timeoutMS)
        {
            Platform::Log(Platform::LogLevel::Error, "[HOST] Synchronization timed out waiting for client ACKs.\n");
            success = false;
            break;
        }

        int result = 0;

        // Non-blocking ENet poll: hold mutex only for the instant query
        {
            TimedLock netLock(NetworkMutex, "SyncClients_Service");
            if (!Host)
            {
                success = false;
                break;
            }
            result = enet_host_service(Host, &evt, 0);
        }

        if (result > 0)
        {
            if (evt.type == ENET_EVENT_TYPE_RECEIVE)
            {
                if (evt.channelID == Chan_Blob)
                {
                    if (evt.packet->dataLength == 1 && evt.packet->data[0] == 0x04)
                    {
                        ngood++;
                    }
                    enet_packet_destroy(evt.packet);
                }
                else if (evt.channelID >= Chan_Input0 && evt.channelID < Chan_Cmd)
                {
                    // ReceiveInputs handles its own InstanceMutex locking internally
                    TimedLock netLock(NetworkMutex, "SyncClients_Service");
                    if (Host)
                    {
                        ReceiveInputs(evt, 0);
                    }
                    enet_packet_destroy(evt.packet);
                }
                else
                {
                    enet_packet_destroy(evt.packet);
                }
            }
            else if (evt.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                Platform::Log(Platform::LogLevel::Error, "[HOST] Client disconnected during synchronization handshake.\n");
                success = false;
                break;
            }
        }
        else
        {
            // No event available; yield so the UI thread doesn't busy-spin
            Platform::Sleep(1);
        }

        if (!Active)
        {
            success = false;
            break;
        }
    }

    if (ngood != (NumPlayers - 1))
    {
        Platform::Log(Platform::LogLevel::Error, "!!! SYNC FAILED !! Acknowledged: %d / Expected: %d\n", ngood, NumPlayers - 1);
        success = false;
    }
    else
    {
        Platform::Log(Platform::LogLevel::Info, "[HOST] All clients successfully synchronized.\n");
    }

    SyncInProgress = false;
    return success;
}

void Netplay::StartGame()
{
    if (!IsHost)
    {
        printf("?????\n");
        return;
    }

    InitGame();

    if (NumPlayers > 1)
    {
        if (!SyncClients())
        {
            Platform::Log(Platform::LogLevel::Error, "Netplay: Aborting game start due to synchronization failure.\n");
            EndSession();
            return;
        }

        Platform::Mutex_Lock(NetworkMutex);
        if (!Host)
        {
            Platform::Mutex_Unlock(NetworkMutex);
            return;
        }
        u8 cmd[1] = { Cmd_StartGame };
        ENetPacket* pkt = enet_packet_create(cmd, sizeof(cmd), ENET_PACKET_FLAG_RELIABLE);
        enet_host_broadcast(Host, Chan_Cmd, pkt);
        Platform::Mutex_Unlock(NetworkMutex);
    }

    // start game locally
    StartLocal();
}

void Netplay::StartLocal()
{
    printf("starting netplay game\n");

    // Pre-allocate reusable savestate buffers
    for (int i = 0; i < 16; i++)
        ReusableStates[i] = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);

    // assign local instances to players

    PlayerToInstance[MyPlayer.ID] = 0;
    InstanceToPlayer[0] = MyPlayer.ID;

    for (int p = 0, i = 1; p < 16; p++)
    {
        if (p == MyPlayer.ID) continue;

        Player* player = &Players[p];
        if (player->Status == Player_None) continue;

        PlayerToInstance[p] = i;
        InstanceToPlayer[i] = p;
        i++;
    }

    InitGame();
}


void Netplay::ProcessHost(int inst)
{
    if (!Host || SyncInProgress) return;

    Platform::Mutex_Lock(NetworkMutex);
    if (!Host || SyncInProgress)
    {
        Platform::Mutex_Unlock(NetworkMutex);
        return;
    }

    ENetEvent event;
    while (enet_host_service(Host, &event, 0) > 0)
    {
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            {
                if ((NumPlayers >= MaxPlayers) || (NumPlayers >= 16))
                {
                    // game is full, reject connection
                    enet_peer_disconnect(event.peer, 0);
                    break;
                }

                // TODO: reject connection if game is running

                // client connected; assign player number

                int id;
                for (id = 0; id < 16; id++)
                {
                    if (id >= NumPlayers) break;
                    if (Players[id].Status == Player_None) break;
                }

                if (id < 16)
                {
                    u8 cmd[11 + sizeof(NetworkSettings)];
                    cmd[0] = Cmd_ClientInit;
                    cmd[1] = (u8)kNetplayMagic;
                    cmd[2] = (u8)(kNetplayMagic >> 8);
                    cmd[3] = (u8)(kNetplayMagic >> 16);
                    cmd[4] = (u8)(kNetplayMagic >> 24);
                    cmd[5] = (u8)kProtocolVersion;
                    cmd[6] = (u8)(kProtocolVersion >> 8);
                    cmd[7] = (u8)(kProtocolVersion >> 16);
                    cmd[8] = (u8)(kProtocolVersion >> 24);
                    cmd[9] = (u8)id;
                    cmd[10] = MaxPlayers;
                    memcpy(&cmd[11], &Settings, sizeof(NetworkSettings));
                    Platform::Log(Platform::LogLevel::Info, "Netplay: client connecting with id %d from %08X:%d\n", id, event.peer->address.host, event.peer->address.port);
                    ENetPacket* pkt = enet_packet_create(cmd, sizeof(cmd), ENET_PACKET_FLAG_RELIABLE);
                    enet_peer_send(event.peer, Chan_Cmd, pkt);

                    Platform::Mutex_Lock(PlayersMutex);

                    Players[id].ID = id;
                    Players[id].Status = Player_Connecting;
                    Players[id].Address = event.peer->address.host;
                    event.peer->data = &Players[id];
                    NumPlayers++;

                    Platform::Mutex_Unlock(PlayersMutex);

                    SendNetworkSettings();

                    PortToPlayerIndex[MakeEndpointKey(event.peer->address.host, event.peer->address.port)] = id;
                    RemotePeers[id] = event.peer;
                }
                else
                {
                    // ???
                    enet_peer_disconnect(event.peer, 0);
                }
            }
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
            {
                Player* player = (Player*)event.peer->data;
                if (!player) break;

                int id = player->ID;
                RemotePeers[id] = nullptr;

                player->ID = 0;
                player->Status = Player_None;
                NumPlayers--;

                // todo broadcast updated player list
                // HostUpdatePlayerList();

                Platform::Log(Platform::LogLevel::Info, "Netplay: disconnected player %d\n", player->ID);
            }
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            {
                if (event.packet->dataLength < 1) break;

                if (event.channelID >= Chan_Input0 && event.channelID < Chan_Cmd)
                {
                    ReceiveInputs(event, inst);
                    break;
                }

                if (event.channelID == Chan_Blob)
                {
                    RecvBlob(event.peer, event.packet, inst);
                    break;
                }

                u8* data = (u8*)event.packet->data;
                switch (data[0])
                {
                case Cmd_PlayerInfo: // client sending player info
                    {
                        if (event.packet->dataLength != (9+sizeof(Player))) break;

                        u32 magic = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
                        u32 version = data[5] | (data[6] << 8) | (data[7] << 16) | (data[8] << 24);
                        if ((magic != kNetplayMagic) || (version != kProtocolVersion))
                        {
                            enet_peer_disconnect(event.peer, 0);
                            break;
                        }

                        Player player;
                        memcpy(&player, &data[9], sizeof(Player));
                        player.Name[31] = '\0';

                        Player* hostside = (Player*)event.peer->data;
                        if (player.ID != hostside->ID)
                        {
                            enet_peer_disconnect(event.peer, 0);
                            break;
                        }

                        Platform::Mutex_Lock(PlayersMutex);

                        player.Status = Player_Client;
                        player.Address = event.peer->address.host;
                        player.Port = event.peer->address.port;
                        memcpy(&Players[player.ID], &player, sizeof(Player));

                        Platform::Mutex_Unlock(PlayersMutex);

                        Platform::Log(Platform::LogLevel::Info, "Netplay: updated player list\n");

                        // broadcast updated player list
                        u8 cmd[2+sizeof(Players)];
                        cmd[0] = Cmd_PlayerList;
                        cmd[1] = (u8)NumPlayers;
                        memcpy(&cmd[2], Players, sizeof(Players));
                        ENetPacket* pkt = enet_packet_create(cmd, 2+sizeof(Players), ENET_PACKET_FLAG_RELIABLE);
                        enet_host_broadcast(Host, Chan_Cmd, pkt);
                    }
                    break;
                }
            }
            break;
        }

        if (event.type == ENET_EVENT_TYPE_RECEIVE)
            enet_packet_destroy(event.packet);
    }
    Platform::Mutex_Unlock(NetworkMutex);
}

void Netplay::ProcessClient(int inst)
{
    if (!Host) return;

    Platform::Mutex_Lock(NetworkMutex);
    if (!Host)
    {
        Platform::Mutex_Unlock(NetworkMutex);
        return;
    }

    ENetEvent event;
    while (enet_host_service(Host, &event, 0) > 0)
    {
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            {
                // another client is establishing a direct connection to us

                int localId = -1;
                for (int i = 0; i < 16; i++)
                {
                    Player* player = &Players[i];
                    if (i == MyPlayer.ID) continue;
                    if (player->Status != Player_Client) continue;

                    if (player->Address == event.peer->address.host &&
                        player->Port    == event.peer->address.port)
                    {
                        localId = i;
                        break;
                    }
                }

                if (localId < 0)
                {
                    enet_peer_disconnect(event.peer, 0);
                    break;
                }

                PortToPlayerIndex[MakeEndpointKey(event.peer->address.host, event.peer->address.port)] = localId;
                RemotePeers[localId] = event.peer;
                event.peer->data = &Players[localId];

                Platform::Log(Platform::LogLevel::Info, "Netplay: connected to peer %d from %08X:%d\n", localId, event.peer->address.host, event.peer->address.port);
            }
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
            {
                Platform::Log(Platform::LogLevel::Info, "Netplay: peer disconnected\n");
            }
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            {
                if (event.packet->dataLength < 1) break;

                if (event.channelID >= Chan_Input0 && event.channelID < Chan_Cmd)
                {
                    ReceiveInputs(event, inst);
                    break;
                }

                if (event.channelID == Chan_Blob)
                {
                    RecvBlob(event.peer, event.packet, inst);
                    break;
                }

                u8* data = (u8*)event.packet->data;
                switch (data[0])
                {
                case Cmd_PlayerList: // host sending player list
                    {
                        if (event.packet->dataLength != (2+sizeof(Players))) break;
                        if (data[1] > 16) break;
                        Platform::Log(Platform::LogLevel::Info, "Netplay: received player list from host\n");
                        Platform::Mutex_Lock(PlayersMutex);

                        NumPlayers = data[1];
                        memcpy(Players, &data[2], sizeof(Players));
                        for (int i = 0; i < 16; i++)
                        {
                            Players[i].Name[31] = '\0';
                        }

                        Platform::Mutex_Unlock(PlayersMutex);

                        // establish connections to any new clients
                        for (int i = 0; i < 16; i++)
                        {
                            Player* player = &Players[i];
                            if (i == MyPlayer.ID) continue;
                            if (player->Status != Player_Client) continue;

                            if (!RemotePeers[i])
                            {
                                ENetAddress peeraddr;
                                peeraddr.host = player->Address;
                                peeraddr.port = player->Port;
                                ENetPeer* peer = enet_host_connect(Host, &peeraddr, Chan_Max, 0);
                                if (!peer)
                                {
                                    // TODO deal with this
                                    continue;
                                }
                            }
                        }
                    }
                    break;

                case Cmd_StartGame: // start game
                    {
                        StartLocal();
                    }
                    break;
                case Cmd_UpdateSettings:
                    {
                        memcpy(&Settings, &data[1], sizeof(NetworkSettings));
                    }
                }
            }
            break;
        }

        if (event.type == ENET_EVENT_TYPE_RECEIVE)
            enet_packet_destroy(event.packet);
    }
    Platform::Mutex_Unlock(NetworkMutex);
}

void Netplay::ProcessFrame(int inst)
{
    if (IsHost)
    {
        ProcessHost(inst);
    }
    else
    {
        ProcessClient(inst);
    }
}

Netplay::InputFrame *Netplay::GetInputFrame(u16 playerID, u32 frameNum)
{
    if (playerID >= 16) return nullptr;

    auto &playerHistory = InputHistory[playerID];

    auto it = playerHistory.find(frameNum);
    return (it != playerHistory.end()) ? &it->second : nullptr;
}

u32 Netplay::CaptureStateSnapshot(int inst, NDS* nds, u32 frameNum)
{
    if (!nds || inst < 0 || inst >= 16) return 0;

    std::unique_ptr<Savestate> state = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
    if (!nds->DoSavestate(state.get()) || state->Error)
    {
        Platform::Log(Platform::LogLevel::Error, "Netplay: failed to create savestate for state hash\n");
        return 0;
    }
    state->Finish();

    StateSnapshot snapshot;
    snapshot.Hash = crc32(0L, static_cast<const Bytef*>(state->Buffer()), state->Length());
    snapshot.Buffer = std::make_shared<std::vector<u8>>(state->Length());
    memcpy(snapshot.Buffer->data(), state->Buffer(), state->Length());

    StoreStateSnapshot(inst, frameNum, snapshot);
    for (int i = 0; i < 16; i++)
    {
        if (i != inst && nds_instances[i] == nds)
            StoreStateSnapshot(i, frameNum, snapshot);
    }

    return snapshot.Hash;
}

void Netplay::StoreStateSnapshot(int inst, u32 frameNum, const StateSnapshot& snapshot)
{
    if (inst < 0 || inst >= 16) return;

    auto& snapshots = StateSnapshots[inst];
    snapshots[frameNum] = snapshot;
    while (snapshots.size() > 32)
        snapshots.erase(snapshots.begin());
}

u32 Netplay::ComputeStateHash(NDS* nds)
{
    if (!nds) return 0;

    std::unique_ptr<Savestate> state = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
    if (!nds->DoSavestate(state.get()) || state->Error)
    {
        Platform::Log(Platform::LogLevel::Error, "Netplay: failed to create savestate for state hash\n");
        return 0;
    }
    state->Finish();

    return crc32(0L, static_cast<const Bytef*>(state->Buffer()), state->Length());
}

void Netplay::DumpDesyncState(NDS* nds, u32 frameNum, u32 localHash, u32 remoteHash)
{
    if (!nds || DesyncDumped) return;

    std::unique_ptr<Savestate> state = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
    if (!nds->DoSavestate(state.get()) || state->Error)
    {
        Platform::Log(Platform::LogLevel::Error, "Netplay: failed to create desync dump savestate\n");
        return;
    }
    state->Finish();

    const char* role = IsHost ? "host" : "client";
    char path[256];
    snprintf(path, sizeof(path), "netplay-desync-%s-frame-%u-local-%08X-remote-%08X.mln",
             role, frameNum, localHash, remoteHash);

    FILE* file = fopen(path, "wb");
    if (!file)
    {
        Platform::Log(Platform::LogLevel::Error, "Netplay: failed to open desync dump '%s'\n", path);
        return;
    }

    size_t written = fwrite(state->Buffer(), 1, state->Length(), file);
    fclose(file);

    if (written != state->Length())
    {
        Platform::Log(Platform::LogLevel::Error, "Netplay: incomplete desync dump '%s' (%zu/%u bytes)\n",
                      path, written, state->Length());
        return;
    }

    DesyncDumped = true;
    Platform::Log(Platform::LogLevel::Info, "Netplay: wrote desync savestate dump '%s' (%u bytes)\n",
                  path, state->Length());
}

void Netplay::DumpDesyncState(const std::vector<u8>& state, u32 frameNum, u32 localHash, u32 remoteHash)
{
    if (state.empty() || DesyncDumped) return;

    const char* role = IsHost ? "host" : "client";
    char path[256];
    snprintf(path, sizeof(path), "netplay-desync-%s-frame-%u-local-%08X-remote-%08X.mln",
             role, frameNum, localHash, remoteHash);

    FILE* file = fopen(path, "wb");
    if (!file)
    {
        Platform::Log(Platform::LogLevel::Error, "Netplay: failed to open desync dump '%s'\n", path);
        return;
    }

    size_t written = fwrite(state.data(), 1, state.size(), file);
    fclose(file);

    if (written != state.size())
    {
        Platform::Log(Platform::LogLevel::Error, "Netplay: incomplete desync dump '%s' (%zu/%zu bytes)\n",
                      path, written, state.size());
        return;
    }

    DesyncDumped = true;
    Platform::Log(Platform::LogLevel::Info, "Netplay: wrote cached desync savestate dump '%s' (%zu bytes)\n",
                  path, state.size());
}

void Netplay::ReceiveInputs(ENetEvent &event, int inst)
{
    if (!Active) return;

    Platform::Mutex_Lock(PlayersMutex);
    int index = GetPlayerIndexFromEndpoint(event.peer->address.host, event.peer->address.port);
    Platform::Mutex_Unlock(PlayersMutex);
    if (index == -1) return;

    Platform::Mutex_Lock(InstanceMutex);

    u8* data = (u8*)event.packet->data;
    size_t dataSize = event.packet->dataLength;
    if (dataSize < sizeof(InputReport))
    {
        Platform::Mutex_Unlock(InstanceMutex);
        return;
    }

    // Read packed InputReport fields safely via memcpy to avoid
    // unaligned-access SIGBUS on platforms that enforce alignment.
    u32 seq, latestFrame, lastCompleteFrame, remoteHash;
    memcpy(&seq, data + offsetof(InputReport, seq), 4);
    memcpy(&latestFrame, data + offsetof(InputReport, frameIndex), 4);
    memcpy(&lastCompleteFrame, data + offsetof(InputReport, lastCompleteFrame), 4);
    memcpy(&remoteHash, data + offsetof(InputReport, stateHash), 4);
    seq = ntohl(seq);
    latestFrame = ntohl(latestFrame);
    lastCompleteFrame = ntohl(lastCompleteFrame);
    remoteHash = ntohl(remoteHash);

    int targetInst = MirrorMode ? 0 : PlayerToInstance[index];
    if (targetInst < 0 || targetInst >= 16)
    {
        Platform::Mutex_Unlock(InstanceMutex);
        return;
    }

    InstanceState& pending = PendingFrames[targetInst];
    NDS* nds = nds_instances[targetInst];

    // Check for desync if a hash was provided. Prefer an exact cached
    // snapshot because packets can arrive after this instance has run ahead.
    if (remoteHash != 0 && nds)
    {
        u32 localHash = 0;
        const std::vector<u8>* localState = nullptr;

        auto snapshotIt = StateSnapshots[targetInst].find(latestFrame);
        if (snapshotIt != StateSnapshots[targetInst].end())
        {
            localHash = snapshotIt->second.Hash;
            localState = snapshotIt->second.Buffer.get();
        }
        else if (nds->NumFrames == latestFrame)
        {
            localHash = ComputeStateHash(nds);
        }
        else
        {
            Platform::Log(Platform::LogLevel::Info,
                "Netplay: no cached state hash for remote frame %u (local frame %u, remote hash %08X)\n",
                latestFrame, nds->NumFrames, remoteHash);
        }

        if (localHash != 0 && localHash != remoteHash)
        {
            Platform::Log(Platform::LogLevel::Error, "Netplay: DESYNC DETECTED at frame %u! (local: %08X, remote: %08X)\n",
                          latestFrame, localHash, remoteHash);
            if (localState)
                DumpDesyncState(*localState, latestFrame, localHash, remoteHash);
            else
                DumpDesyncState(nds, latestFrame, localHash, remoteHash);
        }
    }

    // register the last frame this player has completed
    Platform::Mutex_Lock(PlayersMutex);
    Player &player = Players[index];
    player.LastCompletedFrame = lastCompleteFrame;
    Platform::Mutex_Unlock(PlayersMutex);

    const u8* ptr = data + sizeof(InputReport);
    size_t remaining = dataSize - sizeof(InputReport);
    size_t entryCount = remaining / sizeof(InputFrame);

    auto &playerHistory = InputHistory[index];
    for (size_t i = 0; i < entryCount; ++i) {
        InputFrame frame;
        memcpy(&frame.FrameNum, ptr, 4);
        memcpy(&frame.KeyMask, ptr + 4, 4);
        memcpy(&frame.Touching, ptr + 8, 4);
        memcpy(&frame.TouchX, ptr + 12, 4);
        memcpy(&frame.TouchY, ptr + 16, 4);
        frame.FrameNum = ntohl(frame.FrameNum);
        frame.KeyMask  = ntohl(frame.KeyMask);
        frame.Touching = ntohl(frame.Touching);
        frame.TouchX   = ntohl(frame.TouchX);
        frame.TouchY   = ntohl(frame.TouchY);

        playerHistory[frame.FrameNum] = frame;

        ptr += sizeof(InputFrame);
    }

    if (pending.Active && nds)
    {
        auto it = playerHistory.find(pending.FrameNum);
        Platform::Log(Platform::LogLevel::Info,
            "Netplay DEBUG: ReceiveInputs - pending.Active=1, pending.FrameNum=%u, "
            "looking for frame in player %d history: %s, history size=%zu\n",
            pending.FrameNum, index,
            (it != playerHistory.end()) ? "FOUND" : "NOT FOUND",
            playerHistory.size());
        if (it != playerHistory.end())
        {
            u32 prevWaitFrame = pending.FrameNum;
            u32 currFrame = nds->NumFrames;
            pending.Active = false;

            // load the save state (time it)
            u32 loadStartMS = Platform::GetMSCount();
            pending.SavestateBuffer->Rewind(false);
            nds->DoSavestate(pending.SavestateBuffer.get());
            u32 loadEndMS = Platform::GetMSCount();

            nds->NumFrames = pending.FrameNum;

            // iterate over the frames until we reach the point we were at before
            bool missedFrame = false;
            for (u32 i = nds->NumFrames; i < currFrame; ++i)
            {
                ApplyInputInternal(targetInst, nds, i);
                nds->RunFrame();

                if (!missedFrame)
                {
                    // check if we have inputs from every active player for this frame
                    bool allHaveInputs = true;
                    for (int p = 0; p < 16; p++)
                    {
                        if (Players[p].Status == Player_None) continue;
                        if (!GetInputFrame(p, i))
                        {
                            allHaveInputs = false;
                            break;
                        }
                    }
                    if (!allHaveInputs)
                    {
                        missedFrame = true;
                        pending.Active = true;
                        pending.FrameNum = i;
                        if (!ReusableStates[targetInst])
                            ReusableStates[targetInst] = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
                        ReusableStates[targetInst]->Rewind(true);
                        nds->DoSavestate(ReusableStates[targetInst].get());
                        pending.SavestateBuffer = std::move(ReusableStates[targetInst]);
                    }
                }
            }

            Platform::Log(Platform::LogLevel::Info, "Netplay: instance %d caught up %d frames. Current frame: %d, load_ms=%u\n",
                          targetInst, currFrame - prevWaitFrame, nds->NumFrames, (unsigned)(loadEndMS - loadStartMS));

            int depth = (int)(currFrame - prevWaitFrame);
            if (depth > Diag.MaxRollbackDepth)
                Diag.MaxRollbackDepth = depth;

            // After the rollback loop, recycle the now-current
            // SavestateBuffer back to the reusable pool so it is
            // available for the next snapshot.
            if (pending.SavestateBuffer && !ReusableStates[targetInst])
                ReusableStates[targetInst] = std::move(pending.SavestateBuffer);
        }
    }
    else if (!nds && GameInited)
    {
        Platform::Log(Platform::LogLevel::Warn,
            "Netplay: ReceiveInputs - nds is null for inst %d\n", targetInst);
    }

    Platform::Mutex_Unlock(InstanceMutex);
}

void Netplay::ProcessInput(int netplayID, NDS *nds, u32 inputMask, bool isTouching, u16 touchX, u16 touchY)
{
    if (!Active) return;

    StallFrame = false;

    // Register/update NDS pointer for this instance
    nds_instances[netplayID] = nds;
    // In mirror mode, ReceiveInputs hard-codes targetInst=0, so
    // mirror it there too.
    if (MirrorMode) nds_instances[0] = nds;

    if (SyncInProgress)
    {
        StallFrame = true;
        return;
    }

    InstanceState& pending = PendingFrames[netplayID];

    // Record both the immediate frame (current nds->NumFrames) and the delayed frame
    InputFrame immediateFrame;
    immediateFrame.FrameNum = nds->NumFrames;
    immediateFrame.KeyMask = inputMask;
    immediateFrame.Touching = isTouching;
    immediateFrame.TouchX = touchX;
    immediateFrame.TouchY = touchY;

    InputFrame delayedFrame;
    delayedFrame.FrameNum = nds->NumFrames + Settings.Delay;
    delayedFrame.KeyMask = inputMask;
    delayedFrame.Touching = isTouching;
    delayedFrame.TouchX = touchX;
    delayedFrame.TouchY = touchY;

    Platform::Mutex_Lock(InstanceMutex);
    // Keep a startup/immediate fallback, but do not overwrite a delayed
    // canonical input that was recorded Settings.Delay frames earlier.
    InputHistory[MyPlayer.ID].emplace(immediateFrame.FrameNum, immediateFrame);
    InputHistory[MyPlayer.ID][delayedFrame.FrameNum] = delayedFrame;
    Platform::Mutex_Unlock(InstanceMutex);

    // Calculate a full savestate hash for desync detection every 60 frames.
    u32 stateHash = 0;
    if ((nds->NumFrames % 60) == 0)
    {
        Platform::Mutex_Lock(InstanceMutex);
        stateHash = CaptureStateSnapshot(netplayID, nds, nds->NumFrames);
        Platform::Mutex_Unlock(InstanceMutex);
    }

    // Send the inputs to other players
    {
        size_t packetSize = sizeof(InputReport) + InputHistory[MyPlayer.ID].size() * sizeof(InputFrame);

        std::vector<u8> buffer(packetSize);
        u8* ptr = buffer.data();

        InputReport report;
        report.stallFrame = 0;
        report.seq = htonl(PacketSequenceCounter);
        report.frameIndex = htonl(nds->NumFrames);
        report.lastCompleteFrame = htonl(pending.FrameNum > 0 ? pending.FrameNum - 1 : 0);
        report.stateHash = htonl(stateHash);
        // bump sequence counter for next packet
        PacketSequenceCounter++;

        std::memcpy(ptr, &report, sizeof(report));
        ptr += sizeof(report);

        Platform::Mutex_Lock(InstanceMutex);
        for (auto& pair : InputHistory[MyPlayer.ID]) {
            InputFrame tmp = pair.second;
            tmp.FrameNum = htonl(tmp.FrameNum);
            tmp.KeyMask  = htonl(tmp.KeyMask);
            tmp.Touching = htonl(tmp.Touching);
            tmp.TouchX   = htonl(tmp.TouchX);
            tmp.TouchY   = htonl(tmp.TouchY);
            std::memcpy(ptr, &tmp, sizeof(InputFrame));
            ptr += sizeof(InputFrame);
        }
        Platform::Mutex_Unlock(InstanceMutex);

        Platform::Mutex_Lock(NetworkMutex);
        if (Host)
        {
            if (MirrorMode || IsHost)
            {
                ENetPacket* pkt = enet_packet_create(buffer.data(), buffer.size(), ENET_PACKET_FLAG_UNSEQUENCED);
                enet_host_broadcast(Host, Chan_Input0 + MyPlayer.ID, pkt);
            }
            else if (RemotePeers[0])
            {
                ENetPacket* pkt = enet_packet_create(buffer.data(), buffer.size(), ENET_PACKET_FLAG_UNSEQUENCED);
                enet_peer_send(RemotePeers[0], Chan_Input0 + MyPlayer.ID, pkt);
            }
        }
        Platform::Mutex_Unlock(NetworkMutex);
    }

    // also find the last completed frame of all players
    // in other words, the most recent frame that everyone has everyone's inputs for
    Platform::Mutex_Lock(PlayersMutex);
    int lastCompletedFrame = -1;
    bool allOtherInputsReceived = true;
    bool tooFarAhead = false;
    for (int i = 0; i < 16; ++i)
    {
        if (i == MyPlayer.ID) continue;
        Player &player = Players[i];
        if (player.Status != Player_Client && player.Status != Player_Host) continue;

        if (!GetInputFrame(i, nds->NumFrames))
        {
            allOtherInputsReceived = false;
        }
        if (lastCompletedFrame == -1 || player.LastCompletedFrame < (u32)lastCompletedFrame)
        {
            lastCompletedFrame = player.LastCompletedFrame;
        }

        // Clock lock: if we are more than Delay+1 frames ahead of this
        // player's last completed frame, we must stall to let them catch up.
        // This prevents unbounded timeline drift between instances.
        // Only check once we've received at least one frame update from
        // this player (LastCompletedFrame > 0), otherwise we'd stall
        // immediately on game start before any packets are exchanged.
        if (player.LastCompletedFrame > 0 &&
            nds->NumFrames > player.LastCompletedFrame + Settings.Delay + 1)
        {
            tooFarAhead = true;
        }
    }

    // Update telemetry diagnostics
    Diag.LocalFrameNum = nds->NumFrames;
    int minPeerFrame = -1;
    for (int i = 0; i < 16; ++i)
    {
        if (i == MyPlayer.ID) continue;
        Player &player = Players[i];
        if (player.Status != Player_Client && player.Status != Player_Host) continue;
        if (player.LastCompletedFrame > 0)
        {
            int drift = (int)nds->NumFrames - (int)player.LastCompletedFrame;
            if (drift > Diag.TimelineDrift)
                Diag.TimelineDrift = drift;
            if (minPeerFrame == -1 || (int)player.LastCompletedFrame < minPeerFrame)
                minPeerFrame = (int)player.LastCompletedFrame;
        }
    }
    Diag.PeerLastCompletedFrame = (minPeerFrame >= 0) ? (u32)minPeerFrame : 0;
    Platform::Mutex_Unlock(PlayersMutex);

    // delete all the frames that everyone already have
    // to save memory and keep packet sizes down
    if (lastCompletedFrame != -1)
    {
        Platform::Mutex_Lock(InstanceMutex);
        auto& playerHistory = InputHistory[MyPlayer.ID];
        auto cutoff = playerHistory.lower_bound(static_cast<unsigned>(lastCompletedFrame));
        playerHistory.erase(playerHistory.begin(), cutoff);
        Platform::Mutex_Unlock(InstanceMutex);
    }

    // check if this frame has no inputs available from any other active player
    bool missingInputs = false;
    Platform::Mutex_Lock(PlayersMutex);
    for (int i = 0; i < 16; i++)
    {
        if (i == MyPlayer.ID) continue;
        if (Players[i].Status != Player_Client && Players[i].Status != Player_Host) continue;

        if (!GetInputFrame(i, nds->NumFrames))
        {
            missingInputs = true;
            break;
        }
    }
    Platform::Mutex_Unlock(PlayersMutex);

    // Host never stalls -- it runs ahead and lets clients catch up via
    // rollback. Clients stall when missing inputs or clock-locked.
    // In StrictLockstep mode, host also stalls.
    if ((!IsHost || Settings.SyncMode == Sync_StrictLockstep) &&
        (missingInputs || tooFarAhead))
    {
        StallFrame = true;
    }

    // DEBUG: log stall state periodically
    static u32 stallLogCounter = 0;
    if (StallFrame || missingInputs)
    {
        if ((stallLogCounter++ % 500) == 0)
        {
            Platform::Log(Platform::LogLevel::Info,
                "Netplay DEBUG: ProcessInput stall - frame=%u, StallFrame=%d, missingInputs=%d, "
                "allOtherInputsReceived=%d, tooFarAhead=%d, pending.Active=%d, pending.FrameNum=%u\n",
                nds->NumFrames, (int)StallFrame, (int)missingInputs,
                (int)allOtherInputsReceived, (int)tooFarAhead,
                (int)pending.Active, pending.FrameNum);
        }
    }
    else
    {
        stallLogCounter = 0;
    }

    // Determine if this frame requires a synchronization checkpoint
    bool shouldCheckpoint = false;
    if (Settings.SyncMode == Sync_PureRollback)
        shouldCheckpoint = !IsHost;
    else if (Settings.SyncMode == Sync_PredictiveHost)
        shouldCheckpoint = !IsHost || missingInputs;
    else // Sync_StrictLockstep
        shouldCheckpoint = true;

    // For Predictive Host mode at low delay, allow the host to update its
    // pending checkpoint frame forward if it is running ahead blindly,
    // ensuring it doesn't hold an obsolete snapshot.
    bool canCapture = !pending.Active;
    if (IsHost && Settings.SyncMode == Sync_PredictiveHost && missingInputs)
    {
        canCapture = true; // Force override to refresh the rolling window
    }

    if (shouldCheckpoint && (StallFrame || missingInputs) &&
        nds->NumFrames > Settings.Delay && canCapture)
    {
        Platform::Mutex_Lock(InstanceMutex);
        pending.Active = true;
        pending.FrameNum = nds->NumFrames;
        if (!ReusableStates[netplayID])
            ReusableStates[netplayID] = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
        ReusableStates[netplayID]->Rewind(true);
        nds->DoSavestate(ReusableStates[netplayID].get());
        pending.SavestateBuffer = std::move(ReusableStates[netplayID]);
        Platform::Mutex_Unlock(InstanceMutex);
    }
}

void Netplay::ApplyInput(int netplayID, NDS *nds)
{
    if (!Active) return;

    // Update pointer map
    nds_instances[netplayID] = nds;

    ApplyInputInternal(netplayID, nds, nds->NumFrames);
}

void Netplay::ApplyInputInternal(int netplayID, NDS *nds, u32 frameNum)
{
    // clear inputs in case we return
    nds->SetKeyMask(0xFFF);
    nds->ReleaseScreen();

    Platform::Mutex_Lock(InstanceMutex);
    if (MirrorMode)
    {
        u32 keyMask = 0xFFF;
        bool touching = false;
        u16 touchX = 0, touchY = 0;

        InputFrame *frame0 = GetInputFrame(0, frameNum);
        InputFrame *frame1 = GetInputFrame(1, frameNum);

        if (frame0)
        {
            keyMask &= frame0->KeyMask;
            if (frame0->Touching)
            {
                touching = true;
                touchX = frame0->TouchX;
                touchY = frame0->TouchY;
            }
        }
        if (frame1)
        {
            keyMask &= frame1->KeyMask;
            if (frame1->Touching)
            {
                touching = true;
                touchX = frame1->TouchX;
                touchY = frame1->TouchY;
            }
        }

        Platform::Mutex_Unlock(InstanceMutex);

        nds->SetKeyMask(keyMask);
        if (touching) nds->TouchScreen(touchX, touchY);
        else          nds->ReleaseScreen();

        // Track safe frame for this specific instance
        PendingFrames[netplayID].FrameNum = frameNum;
    }
    else
    {
        int globalPlayerID = InstanceToPlayer[netplayID];
        InputFrame *frame = GetInputFrame(globalPlayerID, frameNum);
        if (!frame)
        {
            Platform::Mutex_Unlock(InstanceMutex);
            return;
        }

        InputFrame frameCopy = *frame;
        Platform::Mutex_Unlock(InstanceMutex);

        // Track safe frame for this specific instance
        PendingFrames[netplayID].FrameNum = frameNum;

        // apply this input frame
        nds->SetKeyMask(frameCopy.KeyMask);
        if (frameCopy.Touching) nds->TouchScreen(frameCopy.TouchX, frameCopy.TouchY);
        else                    nds->ReleaseScreen();
    }
}

void Netplay::Process()
{
    if (!Active) return;

    // When a blob transfer is in progress (client receiving savestate),
    // spin-poll aggressively so we receive all chunks quickly instead
    // of waiting for the 1-second timer tick between each chunk.
    if (BlobInProgress)
    {
        Platform::Mutex_Lock(NetworkMutex);
        ENetEvent evt;
        if (Host)
        {
            for (;;)
            {
                if (!BlobInProgress) break;
                int r = enet_host_service(Host, &evt, 0);
                if (r <= 0) break;
                if (evt.channelID == Chan_Blob)
                    RecvBlob(evt.peer, evt.packet, 0);
                else if (evt.channelID >= Chan_Input0 && evt.channelID < Chan_Cmd)
                    ReceiveInputs(evt, 0);
                if (evt.type == ENET_EVENT_TYPE_RECEIVE)
                    enet_packet_destroy(evt.packet);
            }
        }
        Platform::Mutex_Unlock(NetworkMutex);
        return;
    }

    for (int i = 0; i < 16; i++)
    {
        if (!nds_instances[i])
        {
            // Always service network on instance 0 so the host can
            // accept client connections even before a game is loaded.
            if (i != 0) continue;
        }
        ProcessFrame(i);
    }
    LocalMP::Process();

    static u8 FrameCount = 0;
    FrameCount++;
    if (FrameCount >= 60)
    {
        FrameCount = 0;

        Platform::Mutex_Lock(PlayersMutex);

        for (int i = 0; i < 16; i++)
        {
            if (Players[i].Status == Player_None) continue;
            if (i == MyPlayer.ID) continue;
            if (!RemotePeers[i]) continue;

            Players[i].Ping = RemotePeers[i]->roundTripTime;
        }

        Platform::Mutex_Unlock(PlayersMutex);
    }
}

Netplay::Diagnostics Netplay::GetDiagnostics()
{
    return Diag;
}

}

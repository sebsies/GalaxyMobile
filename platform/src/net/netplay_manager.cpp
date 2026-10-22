#include "net/netplay_manager.hpp"
#include "port/port.h"
#include "port/input.h"

#include <cstring>
#include <algorithm>

namespace {

#pragma pack(push, 1)

constexpr uint32_t kNetMagic = 0x474D502B; // "GMP+"
constexpr uint8_t  kNetVersion = 1;
constexpr int      kDefaultPort = 24601;

enum PacketType : uint8_t {
    PKT_BEACON_BROADCAST = 1,
    PKT_DISCOVERY_PROBE  = 2,
    PKT_CONNECT_REQ      = 3,
    PKT_CONNECT_ACK      = 4,
    PKT_DISCONNECT       = 5,
    PKT_PING             = 6,
    PKT_PONG             = 7,
    PKT_COSTAR_STATE     = 8,
    PKT_PLAYER_STATE     = 9,
    PKT_EVENT            = 10
};

struct PacketHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t seq;
};

struct BeaconPayload {
    char     roomName[64];
    uint8_t  mode;
    char     stage[64];
    int32_t  scenario;
    uint8_t  playerCount;
    uint16_t port;
};

struct ConnectReqPayload {
    char    playerName[32];
    uint8_t preferredMode;
};

struct ConnectAckPayload {
    uint8_t accepted;
    uint8_t assignedRole;
    uint8_t mode;
    char    hostName[32];
};

struct PingPongPayload {
    int64_t timestampMs;
};

struct EventPayload {
    uint8_t eventType; // 1: Warp, 2: Spin
    char    stage[64];
    int32_t scenario;
};

#pragma pack(pop)

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

NetplayManager& NetplayManager::get() {
    static NetplayManager sInstance;
    return sInstance;
}

NetplayManager::NetplayManager() {
    mLastPingSent = std::chrono::steady_clock::now();
    mLastPacketReceived = std::chrono::steady_clock::now();
    mLastBeaconSent = std::chrono::steady_clock::now();
}

NetplayManager::~NetplayManager() {
    shutdown();
}

void NetplayManager::init() {
    if (mRunning.load()) return;
    mRunning.store(true);

    // Open dedicated discovery socket on default port for receiving beacons
    mDiscoverySocket.open(kDefaultPort, true);

    mWorkerThread = std::thread(&NetplayManager::workerLoop, this);
    PORT_LOG("NetplayManager: initialized and started worker thread\n");
}

void NetplayManager::shutdown() {
    if (!mRunning.load()) return;
    disconnect();

    mRunning.store(false);
    if (mWorkerThread.joinable()) {
        mWorkerThread.join();
    }

    mSocket.close();
    mDiscoverySocket.close();
    PORT_LOG("NetplayManager: shut down\n");
}

void NetplayManager::update() {
    // Clean up stale LAN hosts older than 4 seconds
    int64_t curMs = nowMs();
    std::lock_guard<std::mutex> lk(mMutex);
    mLanHosts.erase(
        std::remove_if(mLanHosts.begin(), mLanHosts.end(), [curMs](const PortLanHostInfo& h) {
            return (curMs - h.lastSeenMs) > 4000;
        }),
        mLanHosts.end()
    );
}

bool NetplayManager::startHost(int port, int mode, const std::string& roomName) {
    disconnect();

    mHostPort = (port > 0) ? port : kDefaultPort;
    mMode.store(mode);
    mRoomName = roomName.empty() ? "Galaxy Room" : roomName;

    // Close discovery socket temporarily so main socket can bind kDefaultPort if needed
    if (mDiscoverySocket.getPort() == mHostPort) {
        mDiscoverySocket.close();
    }

    if (!mSocket.open(mHostPort, true)) {
        PORT_LOG("NetplayManager: failed to bind host port %d, retrying on ephemeral port\n", mHostPort);
        if (!mSocket.open(0, true)) {
            return false;
        }
        mHostPort = mSocket.getPort();
    }

    mRole.store(PORT_NET_ROLE_HOST);
    mConnected.store(false);
    PORT_LOG("NetplayManager: started hosting '%s' on port %d (mode=%d)\n", mRoomName.c_str(), mHostPort, mode);
    return true;
}

bool NetplayManager::connectTo(const std::string& ip, int port) {
    disconnect();

    int targetPort = (port > 0) ? port : kDefaultPort;
    if (!mSocket.open(0, true)) {
        PORT_LOG("NetplayManager: failed to open client socket\n");
        return false;
    }

    mPeerIp = ip;
    mPeerPort = targetPort;
    mRole.store(PORT_NET_ROLE_CLIENT);
    mConnected.store(false);

    // Send connection request
    ConnectReqPayload req;
    std::memset(&req, 0, sizeof(req));
    std::strncpy(req.playerName, "Player 2", sizeof(req.playerName) - 1);
    req.preferredMode = (uint8_t)mMode.load();

    sendPacket(PKT_CONNECT_REQ, &req, sizeof(req), mPeerIp, mPeerPort);
    PORT_LOG("NetplayManager: sent connect request to %s:%d\n", ip.c_str(), targetPort);
    return true;
}

void NetplayManager::disconnect() {
    if (mRole.load() != PORT_NET_ROLE_NONE && !mPeerIp.empty() && mPeerPort > 0) {
        sendPacket(PKT_DISCONNECT, nullptr, 0, mPeerIp, mPeerPort);
    }

    // Reset Pad 1 (Co-Star) state on disconnect
    PortPadState zeroPad;
    std::memset(&zeroPad, 0, sizeof(zeroPad));
    zeroPad.connected = 0;
    port_input_set(1, &zeroPad);

    mRole.store(PORT_NET_ROLE_NONE);
    mConnected.store(false);
    mPeerIp.clear();
    mPeerPort = 0;
    mPingMs.store(0);

    std::lock_guard<std::mutex> lk(mMutex);
    mHasRemotePlayer = false;
    std::memset(&mLatestRemotePlayer, 0, sizeof(mLatestRemotePlayer));
}

std::string NetplayManager::getPeerIp() const {
    std::lock_guard<std::mutex> lk(mMutex);
    return mPeerIp;
}

std::string NetplayManager::getLocalIp() const {
    return NetSocket::getLocalIpAddress();
}

int NetplayManager::getLanHosts(PortLanHostInfo* outHosts, int maxHosts) {
    std::lock_guard<std::mutex> lk(mMutex);
    int count = std::min((int)mLanHosts.size(), maxHosts);
    for (int i = 0; i < count; i++) {
        outHosts[i] = mLanHosts[i];
    }
    return count;
}

void NetplayManager::sendPlayerState(const PortRemotePlayerState& state) {
    if (!mConnected.load() || mPeerIp.empty() || mPeerPort <= 0) return;
    sendPacket(PKT_PLAYER_STATE, &state, sizeof(state), mPeerIp, mPeerPort);
}

bool NetplayManager::getRemotePlayerState(PortRemotePlayerState& outState) {
    std::lock_guard<std::mutex> lk(mMutex);
    if (!mHasRemotePlayer) return false;
    outState = mLatestRemotePlayer;
    return true;
}

void NetplayManager::sendCostarState(const PortPadState& pad) {
    if (!mConnected.load() || mPeerIp.empty() || mPeerPort <= 0) return;
    sendPacket(PKT_COSTAR_STATE, &pad, sizeof(pad), mPeerIp, mPeerPort);
}

void NetplayManager::sendWarpEvent(const std::string& stage, int scenario) {
    if (!mConnected.load() || mPeerIp.empty() || mPeerPort <= 0) return;
    EventPayload ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.eventType = 1; // Warp
    std::strncpy(ev.stage, stage.c_str(), sizeof(ev.stage) - 1);
    ev.scenario = scenario;
    sendPacket(PKT_EVENT, &ev, sizeof(ev), mPeerIp, mPeerPort);
}

void NetplayManager::sendSpinEvent() {
    if (!mConnected.load() || mPeerIp.empty() || mPeerPort <= 0) return;
    EventPayload ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.eventType = 2; // Spin
    sendPacket(PKT_EVENT, &ev, sizeof(ev), mPeerIp, mPeerPort);
}

bool NetplayManager::takeWarpEvent(std::string& outStage, int& outScenario) {
    std::lock_guard<std::mutex> lk(mMutex);
    if (!mPendingWarp) return false;
    mPendingWarp = false;
    outStage = mPendingWarpStage;
    outScenario = mPendingWarpScenario;
    return true;
}

bool NetplayManager::takeSpinEvent() {
    std::lock_guard<std::mutex> lk(mMutex);
    if (!mPendingSpin) return false;
    mPendingSpin = false;
    return true;
}

void NetplayManager::sendPacket(uint8_t type, const void* payload, size_t payloadSize, const std::string& destIp, int destPort) {
    static std::atomic<uint16_t> sSeq{1};
    uint8_t buffer[1024];
    if (sizeof(PacketHeader) + payloadSize > sizeof(buffer)) return;

    PacketHeader* hdr = (PacketHeader*)buffer;
    hdr->magic = kNetMagic;
    hdr->version = kNetVersion;
    hdr->type = type;
    hdr->seq = sSeq.fetch_add(1);

    if (payload && payloadSize > 0) {
        std::memcpy(buffer + sizeof(PacketHeader), payload, payloadSize);
    }

    mSocket.sendTo(buffer, sizeof(PacketHeader) + payloadSize, destIp, destPort);
}

void NetplayManager::workerLoop() {
    uint8_t recvBuf[2048];

    while (mRunning.load()) {
        auto now = std::chrono::steady_clock::now();

        // 1. Host LAN Beacon Broadcast (every 1.0 s)
        if (mRole.load() == PORT_NET_ROLE_HOST) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastBeaconSent).count() >= 1000) {
                mLastBeaconSent = now;
                BeaconPayload b;
                std::memset(&b, 0, sizeof(b));
                std::strncpy(b.roomName, mRoomName.c_str(), sizeof(b.roomName) - 1);
                b.mode = (uint8_t)mMode.load();
                std::strncpy(b.stage, "Galaxy", sizeof(b.stage) - 1);
                b.scenario = 1;
                b.playerCount = mConnected.load() ? 2 : 1;
                b.port = (uint16_t)mHostPort;

                // Broadcast on default discovery port 24601
                uint8_t bBuf[sizeof(PacketHeader) + sizeof(BeaconPayload)];
                PacketHeader* bHdr = (PacketHeader*)bBuf;
                bHdr->magic = kNetMagic;
                bHdr->version = kNetVersion;
                bHdr->type = PKT_BEACON_BROADCAST;
                bHdr->seq = 0;
                std::memcpy(bBuf + sizeof(PacketHeader), &b, sizeof(b));
                mSocket.broadcast(bBuf, sizeof(bBuf), kDefaultPort);
            }
        }

        // 2. Keepalive Ping / Timeout Check
        if (mConnected.load() && !mPeerIp.empty() && mPeerPort > 0) {
            // Send ping every 1.0 s
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastPingSent).count() >= 1000) {
                mLastPingSent = now;
                PingPongPayload p;
                p.timestampMs = nowMs();
                sendPacket(PKT_PING, &p, sizeof(p), mPeerIp, mPeerPort);
            }

            // Connection timeout after 5.0 s of silence
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastPacketReceived).count() >= 5000) {
                PORT_LOG("NetplayManager: connection to %s:%d timed out\n", mPeerIp.c_str(), mPeerPort);
                disconnect();
            }
        }

        // 3. Receive Packets from Main Socket
        if (mSocket.isOpened()) {
            std::string srcIp;
            int srcPort = 0;
            int bytes = mSocket.receiveFrom(recvBuf, sizeof(recvBuf), srcIp, srcPort);
            if (bytes > 0) {
                handleIncomingPacket(recvBuf, (size_t)bytes, srcIp, srcPort);
            }
        }

        // 4. Receive Packets from Discovery Socket (for detecting hosts while idle)
        if (mDiscoverySocket.isOpened() && mDiscoverySocket.getPort() != mSocket.getPort()) {
            std::string dIp;
            int dPort = 0;
            int bytes = mDiscoverySocket.receiveFrom(recvBuf, sizeof(recvBuf), dIp, dPort);
            if (bytes > 0) {
                handleIncomingPacket(recvBuf, (size_t)bytes, dIp, dPort);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void NetplayManager::handleIncomingPacket(const uint8_t* data, size_t size, const std::string& srcIp, int srcPort) {
    if (size < sizeof(PacketHeader)) return;
    const PacketHeader* hdr = (const PacketHeader*)data;
    if (hdr->magic != kNetMagic || hdr->version != kNetVersion) return;

    const uint8_t* payload = data + sizeof(PacketHeader);
    size_t payloadSize = size - sizeof(PacketHeader);

    // Beacon packet (LAN Auto-Discovery)
    if (hdr->type == PKT_BEACON_BROADCAST && payloadSize >= sizeof(BeaconPayload)) {
        const BeaconPayload* b = (const BeaconPayload*)payload;
        PortLanHostInfo hostInfo;
        std::memset(&hostInfo, 0, sizeof(hostInfo));
        std::strncpy(hostInfo.name, b->roomName, sizeof(hostInfo.name) - 1);
        std::strncpy(hostInfo.ip, srcIp.c_str(), sizeof(hostInfo.ip) - 1);
        hostInfo.port = b->port;
        hostInfo.mode = b->mode;
        std::strncpy(hostInfo.stage, b->stage, sizeof(hostInfo.stage) - 1);
        hostInfo.scenario = b->scenario;
        hostInfo.playerCount = b->playerCount;
        hostInfo.lastSeenMs = nowMs();

        std::lock_guard<std::mutex> lk(mMutex);
        bool found = false;
        for (auto& h : mLanHosts) {
            if (std::strcmp(h.ip, hostInfo.ip) == 0 && h.port == hostInfo.port) {
                h = hostInfo;
                found = true;
                break;
            }
        }
        if (!found) {
            mLanHosts.push_back(hostInfo);
            PORT_LOG("NetplayManager: discovered LAN host '%s' at %s:%d\n", hostInfo.name, hostInfo.ip, hostInfo.port);
        }
        return;
    }

    // Connect Request (Host receives from Client)
    if (hdr->type == PKT_CONNECT_REQ && mRole.load() == PORT_NET_ROLE_HOST) {
        mPeerIp = srcIp;
        mPeerPort = srcPort;
        mConnected.store(true);
        mLastPacketReceived = std::chrono::steady_clock::now();

        ConnectAckPayload ack;
        ack.accepted = 1;
        ack.assignedRole = (uint8_t)PORT_NET_ROLE_CLIENT;
        ack.mode = (uint8_t)mMode.load();
        std::strncpy(ack.hostName, mRoomName.c_str(), sizeof(ack.hostName) - 1);
        sendPacket(PKT_CONNECT_ACK, &ack, sizeof(ack), srcIp, srcPort);
        PORT_LOG("NetplayManager: accepted client connection from %s:%d\n", srcIp.c_str(), srcPort);
        return;
    }

    // Connect Ack (Client receives from Host)
    if (hdr->type == PKT_CONNECT_ACK && mRole.load() == PORT_NET_ROLE_CLIENT) {
        const ConnectAckPayload* ack = (const ConnectAckPayload*)payload;
        if (ack->accepted) {
            mConnected.store(true);
            mMode.store(ack->mode);
            mLastPacketReceived = std::chrono::steady_clock::now();
            PORT_LOG("NetplayManager: successfully joined host %s:%d (mode=%d)\n", srcIp.c_str(), srcPort, ack->mode);
        }
        return;
    }

    // Verify packet comes from connected peer for gameplay replication
    if (!mConnected.load() || srcIp != mPeerIp || srcPort != mPeerPort) return;
    mLastPacketReceived = std::chrono::steady_clock::now();

    // Ping & Pong
    if (hdr->type == PKT_PING && payloadSize >= sizeof(PingPongPayload)) {
        const PingPongPayload* p = (const PingPongPayload*)payload;
        PingPongPayload pong = *p;
        sendPacket(PKT_PONG, &pong, sizeof(pong), srcIp, srcPort);
    } else if (hdr->type == PKT_PONG && payloadSize >= sizeof(PingPongPayload)) {
        const PingPongPayload* p = (const PingPongPayload*)payload;
        int64_t rtt = nowMs() - p->timestampMs;
        mPingMs.store((int)std::max((int64_t)0, rtt));
    }
    // Co-Star State (Host receives from Assist Client)
    else if (hdr->type == PKT_COSTAR_STATE && payloadSize >= sizeof(PortPadState)) {
        const PortPadState* pad = (const PortPadState*)payload;
        PortPadState costarPad = *pad;
        costarPad.connected = 1;
        port_input_set(1, &costarPad);
    }
    // Remote Player State (Free-Roam Co-op)
    else if (hdr->type == PKT_PLAYER_STATE && payloadSize >= sizeof(PortRemotePlayerState)) {
        const PortRemotePlayerState* pState = (const PortRemotePlayerState*)payload;
        std::lock_guard<std::mutex> lk(mMutex);
        mLatestRemotePlayer = *pState;
        mLatestRemotePlayer.valid = 1;
        mHasRemotePlayer = true;
    }
    // Gameplay Events
    else if (hdr->type == PKT_EVENT && payloadSize >= sizeof(EventPayload)) {
        const EventPayload* ev = (const EventPayload*)payload;
        std::lock_guard<std::mutex> lk(mMutex);
        if (ev->eventType == 1) { // Warp
            mPendingWarp = true;
            mPendingWarpStage = ev->stage;
            mPendingWarpScenario = ev->scenario;
            PORT_LOG("NetplayManager: received warp event to stage=%s scenario=%d\n", ev->stage, ev->scenario);
        } else if (ev->eventType == 2) { // Spin
            mPendingSpin = true;
        }
    }
    // Disconnect
    else if (hdr->type == PKT_DISCONNECT) {
        PORT_LOG("NetplayManager: received graceful disconnect from peer %s:%d\n", srcIp.c_str(), srcPort);
        disconnect();
    }
}

// ---------------------------------------------------------------------------
// C Interface (port/net.h)
// ---------------------------------------------------------------------------
extern "C" {

void port_net_init(void) {
    NetplayManager::get().init();
}

void port_net_shutdown(void) {
    NetplayManager::get().shutdown();
}

void port_net_update(void) {
    NetplayManager::get().update();
}

int port_net_start_host(int port, int mode, const char* roomName) {
    return NetplayManager::get().startHost(port, mode, roomName ? roomName : "Galaxy Room") ? 1 : 0;
}

int port_net_connect(const char* ip, int port) {
    if (!ip) return 0;
    return NetplayManager::get().connectTo(ip, port) ? 1 : 0;
}

void port_net_disconnect(void) {
    NetplayManager::get().disconnect();
}

int port_net_is_connected(void) {
    return NetplayManager::get().isConnected() ? 1 : 0;
}

int port_net_get_role(void) {
    return NetplayManager::get().getRole();
}

int port_net_get_mode(void) {
    return NetplayManager::get().getMode();
}

void port_net_set_mode(int mode) {
    NetplayManager::get().setMode(mode);
}

int port_net_get_ping_ms(void) {
    return NetplayManager::get().getPingMs();
}

const char* port_net_get_peer_ip(void) {
    static std::string sIp;
    sIp = NetplayManager::get().getPeerIp();
    return sIp.c_str();
}

const char* port_net_get_local_ip(void) {
    static std::string sLocalIp;
    sLocalIp = NetplayManager::get().getLocalIp();
    return sLocalIp.c_str();
}

int port_net_get_lan_hosts(PortLanHostInfo* outHosts, int maxHosts) {
    return NetplayManager::get().getLanHosts(outHosts, maxHosts);
}

void port_net_send_player_state(const PortRemotePlayerState* state) {
    if (state) NetplayManager::get().sendPlayerState(*state);
}

int port_net_get_remote_player_state(PortRemotePlayerState* outState) {
    if (!outState) return 0;
    return NetplayManager::get().getRemotePlayerState(*outState) ? 1 : 0;
}

void port_net_send_costar_state(const struct PortPadState* pad) {
    if (pad) NetplayManager::get().sendCostarState(*pad);
}

void port_net_send_warp_event(const char* stage, int scenario) {
    NetplayManager::get().sendWarpEvent(stage ? stage : "", scenario);
}

void port_net_send_spin_event(void) {
    NetplayManager::get().sendSpinEvent();
}

int port_net_take_warp_event(char* outStage, size_t maxLen, int* outScenario) {
    std::string stage;
    int scenario = 1;
    if (NetplayManager::get().takeWarpEvent(stage, scenario)) {
        if (outStage && maxLen > 0) {
            std::strncpy(outStage, stage.c_str(), maxLen - 1);
            outStage[maxLen - 1] = '\0';
        }
        if (outScenario) *outScenario = scenario;
        return 1;
    }
    return 0;
}

int port_net_take_spin_event(void) {
    return NetplayManager::get().takeSpinEvent() ? 1 : 0;
}

} // extern "C"

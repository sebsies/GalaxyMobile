#pragma once

#include "port/net.h"
#include "port/input.h"
#include "net/net_socket.hpp"

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

class NetplayManager {
public:
    static NetplayManager& get();

    void init();
    void shutdown();
    void update();

    // Session Management
    bool startHost(int port, int mode, const std::string& roomName);
    bool connectTo(const std::string& ip, int port);
    void disconnect(bool stopHosting = true);

    // Queries
    bool isConnected() const { return mConnected.load(); }
    int getRole() const { return mRole.load(); }
    int getMode() const { return mMode.load(); }
    void setMode(int mode) { mMode.store(mode); }
    int getPingMs() const { return mPingMs.load(); }
    std::string getPeerIp() const;
    std::string getLocalIp() const;

    // LAN Discovery
    int getLanHosts(PortLanHostInfo* outHosts, int maxHosts);
    void refreshDiscovery();

    // State Replication
    void sendPlayerState(const PortRemotePlayerState& state);
    bool getRemotePlayerState(PortRemotePlayerState& outState);

    void sendCostarState(const PortPadState& pad);

    // Gameplay Events
    void sendWarpEvent(const std::string& stage, int scenario);
    void sendSpinEvent();
    bool takeWarpEvent(std::string& outStage, int& outScenario);
    bool takeSpinEvent();

private:
    NetplayManager();
    ~NetplayManager();

    void workerLoop();
    void handleIncomingPacket(const uint8_t* data, size_t size, const std::string& srcIp, int srcPort);
    void sendPacket(uint8_t type, const void* payload, size_t payloadSize, const std::string& destIp, int destPort);

    NetSocket mSocket;
    NetSocket mDiscoverySocket; // Dedicated socket on 24601 for discovery beacons

    std::atomic<bool> mRunning{false};
    std::thread mWorkerThread;

    std::atomic<bool> mConnected{false};
    std::atomic<int> mRole{PORT_NET_ROLE_NONE};
    std::atomic<int> mMode{PORT_NET_MODE_COOP};
    std::atomic<int> mPingMs{0};

    std::string mRoomName{"Galaxy Room"};
    std::string mPeerIp;
    int mPeerPort{0};
    int mHostPort{24601};

    std::chrono::steady_clock::time_point mLastPingSent;
    std::chrono::steady_clock::time_point mLastPacketReceived;
    std::chrono::steady_clock::time_point mLastBeaconSent;

    mutable std::mutex mMutex;
    std::vector<PortLanHostInfo> mLanHosts;

    PortRemotePlayerState mLatestRemotePlayer{};
    bool mHasRemotePlayer{false};

    bool mPendingWarp{false};
    std::string mPendingWarpStage;
    int mPendingWarpScenario{1};

    bool mPendingSpin{false};
};

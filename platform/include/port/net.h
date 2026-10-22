#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Multiplayer game modes
enum PortNetMode {
    PORT_NET_MODE_OFFLINE = 0,
    PORT_NET_MODE_COOP    = 1, // Mario & Luigi simultaneous 3D co-op
    PORT_NET_MODE_COSTAR  = 2  // Assist partner controls 2P Star Pointer & actions
};

// Multiplayer network roles
enum PortNetRole {
    PORT_NET_ROLE_NONE   = 0,
    PORT_NET_ROLE_HOST   = 1,
    PORT_NET_ROLE_CLIENT = 2
};

// Player action flags for state replication
#define PORT_NET_FLAG_GROUNDED  (1 << 0)
#define PORT_NET_FLAG_AIR       (1 << 1)
#define PORT_NET_FLAG_SWIMMING  (1 << 2)
#define PORT_NET_FLAG_SPINNING  (1 << 3)
#define PORT_NET_FLAG_FLYING    (1 << 4)
#define PORT_NET_FLAG_SLIDING   (1 << 5)
#define PORT_NET_FLAG_SQUATTING (1 << 6)
#define PORT_NET_FLAG_DEAD      (1 << 7)

// Information about a detected local network host (LAN Auto-Discovery)
typedef struct PortLanHostInfo {
    char name[64];
    char ip[64];
    int port;
    int mode;        // enum PortNetMode
    char stage[64];
    int scenario;
    int playerCount;
    int pingMs;
    int64_t lastSeenMs;
} PortLanHostInfo;

// Replicated remote player transform, animation and action state
typedef struct PortRemotePlayerState {
    int valid;
    float posX, posY, posZ;
    float velX, velY, velZ;
    float rotX, rotY, rotZ;
    float forwardX, forwardY, forwardZ;
    uint32_t animHash;
    float animFrame;
    float animSpeed;
    uint32_t stateFlags;
    int powerupMode;
    int isLuigi;
    char name[32];
} PortRemotePlayerState;

struct PortPadState;

// Netplay lifecycle & setup
void port_net_init(void);
void port_net_shutdown(void);
void port_net_update(void);

// Session control
int port_net_start_host(int port, int mode, const char* roomName);
int port_net_connect(const char* ip, int port);
void port_net_disconnect(void);

// Status queries
int port_net_is_connected(void);
int port_net_get_role(void);
int port_net_get_mode(void);
void port_net_set_mode(int mode);
int port_net_get_ping_ms(void);
const char* port_net_get_peer_ip(void);
const char* port_net_get_local_ip(void);

// LAN discovery
int port_net_get_lan_hosts(PortLanHostInfo* outHosts, int maxHosts);

// Free-Roam Co-op State Replication
void port_net_send_player_state(const PortRemotePlayerState* state);
int port_net_get_remote_player_state(PortRemotePlayerState* outState);

// Co-Star Assist State Replication
void port_net_send_costar_state(const struct PortPadState* pad);

// Gameplay events
void port_net_send_warp_event(const char* stage, int scenario);
void port_net_send_spin_event(void);
int port_net_take_warp_event(char* outStage, size_t maxLen, int* outScenario);
int port_net_take_spin_event(void);

#ifdef __cplusplus
}
#endif

#include "dfvr_bridge.pb.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#ifndef STANDALONE_MOCK
#include "modules/Units.h"
#include "modules/Maps.h"
#include "df/world.h"
#include "df/unit.h"
#include "df/unit_body_part_status.h"
#include "df/viewscreen_adventure_menust.h"
#include "Core.h"
using namespace DFHack;
using namespace df::enums;
DFHACK_PLUGIN("dfvr_bridge");
#else
namespace DFHack {
    struct color_ostream {
        template<typename... Args>
        void print(const char* fmt, Args... args) { printf(fmt, args...); }
    };
    enum command_result { CR_OK = 0 };
}
static uint64_t mock_frame_counter = 0;
#endif

static std::thread server_thread;
static std::thread udp_broadcast_thread;
static std::mutex state_mutex;
static std::condition_variable state_cv;
static bool state_updated = false;
static bool is_running = false;
static bool udp_running = false;
static SOCKET server_socket = INVALID_SOCKET;
static SOCKET udp_broadcast_socket = INVALID_SOCKET;

// Thread-safe action queue
static std::vector<dfvr::ActionRequest> pending_actions;
static std::mutex actions_mutex;

// Shared state buffer populated by the main thread or mock generator
static dfvr::GameStatePayload shared_game_state;

int GetConfigPort() {
    int port = 9000;
    std::ifstream file("dfhack-config/dfvr_bridge.ini");
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("port=") == 0) {
                try {
                    port = std::stoi(line.substr(5));
                } catch (...) {}
            }
        }
    }
    return port;
}

#ifndef STANDALONE_MOCK
void ScrapeMenuOptions(dfvr::GameStatePayload& payload) {
    auto screen = Gui::getCurViewscreen(true);
    if (auto adv_menu = virtual_cast<df::viewscreen_adventure_menust>(screen)) {
        dfvr::MenuOption* opt = payload.add_current_menu_options();
        opt->set_option_id(1);
        opt->set_text_content("Talk");
    }
}

struct BlockCoord {
    int x, y, z;
    bool operator==(const BlockCoord& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const BlockCoord& o) const { return !(*this == o); }
};
static BlockCoord last_player_block = {-999, -999, -999};

void ScrapeEntities(dfvr::GameStatePayload& payload) {
    for (auto unit : world->units.active) {
        if (!Units::isAlive(unit) || !unit->flags1.bits.active) continue;

        auto entity = payload.add_entities();
        entity->set_entity_id(unit->id);
        
        std::string name = Translation::TranslateName(&unit->name, false);
        if (name.empty()) name = "Unknown Creature";
        entity->set_name(name);

        auto pos = entity->mutable_position();
        pos->set_x(static_cast<float>(unit->pos.x));
        pos->set_y(static_cast<float>(unit->pos.z)); 
        pos->set_z(static_cast<float>(unit->pos.y));

        if (unit->body.body_plan) {
            const auto& body_parts = unit->body.body_plan->body_parts;
            for (int i = 0; i < (int)body_parts.size(); ++i) {
                auto part_raw = body_parts[i];
                if (part_raw->flags.is_set(df::body_part_raw_flags::INTERNAL)) continue;

                dfvr::BodyPart* part = entity->add_body_parts();
                part->set_id(i);
                part->set_name(part_raw->name_singular[0]->value);
                
                float y_offset = 0.0f;
                float x_offset = 0.0f;
                std::string part_name = part_raw->name_singular[0]->value;
                if (part_name.find("head") != std::string::npos || part_name.find("skull") != std::string::npos) {
                    y_offset = 0.6f;
                } else if (part_name.find("foot") != std::string::npos || part_name.find("toe") != std::string::npos) {
                    y_offset = -0.8f;
                } else if (part_name.find("leg") != std::string::npos) {
                    y_offset = -0.4f;
                    x_offset = (part_name.find("right") != std::string::npos) ? 0.2f : -0.2f;
                } else if (part_name.find("arm") != std::string::npos || part_name.find("hand") != std::string::npos) {
                    y_offset = 0.3f;
                    x_offset = (part_name.find("right") != std::string::npos) ? 0.4f : -0.4f;
                }

                part->mutable_relative_position()->set_x(x_offset);
                part->mutable_relative_position()->set_y(y_offset); 
                part->mutable_relative_position()->set_z(0.0f);
                part->set_size_volume(static_cast<float>(part_raw->relsize));
            }
        }
    }
}

bool ScrapeMapBlocks(dfvr::GameStatePayload& payload, bool force_update) {
    df::unit* player = world->units.active.size() > 0 ? world->units.active[0] : nullptr;
    if (!player) return false;

    int player_x = player->pos.x;
    int player_y = player->pos.y;
    int player_z = player->pos.z;

    int bx = (player_x / 16) * 16;
    int by = (player_y / 16) * 16;

    BlockCoord current_block = {bx, by, player_z};
    if (!force_update && current_block == last_player_block) {
        return false;
    }
    last_player_block = current_block;

    for (int obx = -1; obx <= 1; ++obx) {
        for (int oby = -1; oby <= 1; ++oby) {
            int target_bx = bx + (obx * 16);
            int target_by = by + (oby * 16);

            auto block = payload.add_map_blocks();
            block->mutable_position()->set_x((float)target_bx);
            block->mutable_position()->set_y((float)player_z);
            block->mutable_position()->set_z((float)target_by);

            for (int ty = 0; ty < 16; ++ty) {
                for (int tx = 0; tx < 16; ++tx) {
                    df::coord pos(target_bx + tx, target_by + ty, player_z);
                    if (Maps::isValidTilePos(pos)) {
                        auto tile_type = Maps::getTileType(pos);
                        block->add_tiles(static_cast<int32_t>(tile_type));
                    } else {
                        block->add_tiles(0);
                    }
                }
            }
        }
    }
    return true;
}
#else
// Standalone mock state generation
void GenerateMockState(dfvr::GameStatePayload& payload) {
    payload.Clear();
    payload.set_type(dfvr::GameStatePayload_PayloadType_FULL_STATE);
    payload.set_frame_tick(++mock_frame_counter);

    // Mock entity (Goblin)
    auto entity = payload.add_entities();
    entity->set_entity_id(101);
    entity->set_name("Goblin Snatcher");
    entity->mutable_position()->set_x(0.0f);
    entity->mutable_position()->set_y(0.0f);
    entity->mutable_position()->set_z(-3.0f);

    // Mock body parts
    const char* parts[] = {"head", "upper body", "left arm", "right arm", "left leg", "right leg"};
    for (int i = 0; i < 6; ++i) {
        auto part = entity->add_body_parts();
        part->set_id(i + 1);
        part->set_name(parts[i]);
        part->set_size_volume(1000.0f);
        part->mutable_relative_position()->set_x(i == 2 ? -0.4f : (i == 3 ? 0.4f : 0.0f));
        part->mutable_relative_position()->set_y(i == 0 ? 0.6f : (i >= 4 ? -0.6f : 0.2f));
        part->mutable_relative_position()->set_z(0.0f);
    }

    // Mock 3x3 map blocks (16x16)
    for (int obx = -1; obx <= 1; ++obx) {
        for (int oby = -1; oby <= 1; ++oby) {
            auto block = payload.add_map_blocks();
            block->mutable_position()->set_x((float)(obx * 16));
            block->mutable_position()->set_y(0.0f);
            block->mutable_position()->set_z((float)(oby * 16));
            for (int t = 0; t < 256; ++t) {
                block->add_tiles(1); // Solid floor tile
            }
        }
    }
}
#endif

// UDP Auto-Discovery Broadcast Loop
void UdpBroadcastLoop() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    udp_broadcast_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_broadcast_socket == INVALID_SOCKET) return;

    int broadcast = 1;
    setsockopt(udp_broadcast_socket, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

    sockaddr_in broadcast_addr;
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    broadcast_addr.sin_port = htons(9001);

    while (udp_running && is_running) {
        int port = GetConfigPort();
        std::string beacon = "DFVR_HOST:" + std::to_string(port);
        sendto(udp_broadcast_socket, beacon.data(), beacon.size(), 0, (sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    closesocket(udp_broadcast_socket);
#ifdef _WIN32
    WSACleanup();
#endif
}

bool ReadExact(SOCKET sock, char* buffer, size_t length) {
    size_t total_read = 0;
    while (total_read < length && is_running) {
        int bytes_read = recv(sock, buffer + total_read, length - total_read, 0);
        if (bytes_read <= 0) return false;
        total_read += bytes_read;
    }
    return true;
}

bool WriteExact(SOCKET sock, const char* buffer, size_t length) {
    size_t total_written = 0;
    while (total_written < length && is_running) {
        int bytes_written = send(sock, buffer + total_written, length - total_written, 0);
        if (bytes_written <= 0) return false;
        total_written += bytes_written;
    }
    return true;
}

void HandleClient(SOCKET client_socket) {
#ifdef _WIN32
    DWORD timeout = 1000;
#else
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
#endif
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    uint32_t length_prefix = 0;
    if (!ReadExact(client_socket, reinterpret_cast<char*>(&length_prefix), sizeof(length_prefix))) {
        closesocket(client_socket);
        return;
    }
    
    if (length_prefix == 0 || length_prefix > 10 * 1024 * 1024) {
        closesocket(client_socket);
        return;
    }
    
    std::vector<char> request_buffer(length_prefix);
    if (!ReadExact(client_socket, request_buffer.data(), length_prefix)) {
        closesocket(client_socket);
        return;
    }
    
    dfvr::HandshakeRequest request;
    if (request.ParseFromArray(request_buffer.data(), length_prefix)) {
        std::cout << "Received Handshake from Client Version: " << request.client_version() << std::endl;
        
        dfvr::HandshakeResponse response;
#ifndef STANDALONE_MOCK
        std::string df_version = "Unknown";
        if (Core::getInstance().vinfo) {
            df_version = Core::getInstance().vinfo->getVersion();
        }
        response.set_server_version("DFHack " + df_version + " - DFVR Bridge 0.1a");
        response.set_world_time(world->frame_counter);
        int player_id = !world->units.active.empty() ? world->units.active[0]->id : -1;
        response.set_player_id(player_id);
#else
        response.set_server_version("Standalone Mock Server 0.1a");
        response.set_world_time(mock_frame_counter);
        response.set_player_id(101);
#endif
        response.set_connection_status("OK");
        
        std::string serialized_response;
        response.SerializeToString(&serialized_response);
        uint32_t response_length = static_cast<uint32_t>(serialized_response.size());
        
        if (WriteExact(client_socket, reinterpret_cast<const char*>(&response_length), sizeof(response_length))) {
            WriteExact(client_socket, serialized_response.data(), response_length);
        } else {
            closesocket(client_socket);
            return;
        }

        std::cout << "Client handshaked. Entering streaming mode..." << std::endl;
        while (is_running) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(client_socket, &readfds);

            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000;

            int result = select(client_socket + 1, &readfds, NULL, NULL, &tv);
            if (result > 0 && FD_ISSET(client_socket, &readfds)) {
                uint32_t req_length = 0;
                if (ReadExact(client_socket, reinterpret_cast<char*>(&req_length), sizeof(req_length))) {
                    if (req_length > 0 && req_length <= 1024 * 1024) {
                        std::vector<char> req_buf(req_length);
                        if (ReadExact(client_socket, req_buf.data(), req_length)) {
                            dfvr::ActionRequest action;
                            if (action.ParseFromArray(req_buf.data(), req_length)) {
                                std::lock_guard<std::mutex> lock(actions_mutex);
                                pending_actions.push_back(action);
                                std::cout << "Received ActionRequest for Option ID: " << action.selected_option_id() << std::endl;
                            }
                        } else break;
                    }
                } else {
                    break;
                }
            } else if (result < 0) {
                break;
            }

            std::string payload_data;
#ifndef STANDALONE_MOCK
            {
                std::unique_lock<std::mutex> lock(state_mutex);
                if (state_cv.wait_for(lock, std::chrono::milliseconds(10), []{ return state_updated || !is_running; })) {
                    if (!is_running) break;
                    shared_game_state.SerializeToString(&payload_data);
                    state_updated = false;
                }
            }
#else
            {
                GenerateMockState(shared_game_state);
                shared_game_state.SerializeToString(&payload_data);
                std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS mock update
            }
#endif

            if (!payload_data.empty()) {
                uint32_t payload_length = static_cast<uint32_t>(payload_data.size());
                if (!WriteExact(client_socket, reinterpret_cast<const char*>(&payload_length), sizeof(payload_length))) break;
                if (!WriteExact(client_socket, payload_data.data(), payload_length)) break;
            }
        }
    }
    
    std::cout << "Client disconnected." << std::endl;
    closesocket(client_socket);
}

void ServerLoop() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) return;

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(GetConfigPort());

    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        closesocket(server_socket);
        return;
    }

    listen(server_socket, SOMAXCONN);
    std::cout << "DFVR Bridge TCP Server listening on port " << GetConfigPort() << std::endl;

    while (is_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int result = select(server_socket + 1, &readfds, NULL, NULL, &tv);
        if (result > 0 && FD_ISSET(server_socket, &readfds)) {
            SOCKET client_socket = accept(server_socket, NULL, NULL);
            if (client_socket != INVALID_SOCKET) {
                HandleClient(client_socket);
            }
        }
    }

    closesocket(server_socket);
#ifdef _WIN32
    WSACleanup();
#endif
}

#ifndef STANDALONE_MOCK
DFhackCExport command_result plugin_onupdate(color_ostream &out) {
    {
        std::lock_guard<std::mutex> lock(actions_mutex);
        for (const auto& action : pending_actions) {
            auto screen = Gui::getCurViewscreen(true);
            if (auto adv_menu = virtual_cast<df::viewscreen_adventure_menust>(screen)) {
                out.print("DFVR: Injected ActionRequest option %d into adventure menu\n", action.selected_option_id());
                adv_menu->feed_key(interface_key::SELECT);
            }
        }
        pending_actions.clear();
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        dfvr::GameStatePayload map_payload;
        bool map_updated = ScrapeMapBlocks(map_payload, false);
        if (map_updated) {
            map_payload.set_type(dfvr::GameStatePayload_PayloadType_MAP_ONLY);
            map_payload.set_frame_tick(world->frame_counter);
            shared_game_state.CopyFrom(map_payload);
        } else {
            shared_game_state.Clear();
            shared_game_state.set_type(dfvr::GameStatePayload_PayloadType_DYNAMIC_ONLY);
            shared_game_state.set_frame_tick(world->frame_counter);
            ScrapeMenuOptions(shared_game_state);
            ScrapeEntities(shared_game_state);
        }
        state_updated = true;
    }
    state_cv.notify_all();
    return CR_OK;
}

DFhackCExport command_result plugin_init(color_ostream &out, std::vector<PluginCommand> &commands) {
    is_running = true;
    udp_running = true;
    server_thread = std::thread(ServerLoop);
    udp_broadcast_thread = std::thread(UdpBroadcastLoop);
    out.print("DFVR Bridge Server started on port %d, UDP broadcast on 9001.\n", GetConfigPort());
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    is_running = false;
    udp_running = false;
    if (server_thread.joinable()) server_thread.join();
    if (udp_broadcast_thread.joinable()) udp_broadcast_thread.join();
    return CR_OK;
}
#else
int main() {
    std::cout << "Starting DFVR Bridge Standalone Mock Server..." << std::endl;
    is_running = true;
    udp_running = true;
    server_thread = std::thread(ServerLoop);
    udp_broadcast_thread = std::thread(UdpBroadcastLoop);

    std::cout << "Mock server running. Press Enter to exit." << std::endl;
    std::cin.get();

    is_running = false;
    udp_running = false;
    if (server_thread.joinable()) server_thread.join();
    if (udp_broadcast_thread.joinable()) udp_broadcast_thread.join();
    return 0;
}
#endif

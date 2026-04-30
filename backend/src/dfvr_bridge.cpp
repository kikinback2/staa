#include "Core.h"
#include "Console.h"
#include "Export.h"
#include "PluginManager.h"
#include "modules/Gui.h"
#include "modules/Translation.h"
#include "df/viewscreen_adventure_menust.h"
#include "dfvr_bridge.pb.h"

#include <thread>
#include <mutex>
#include <iostream>
// Assume a simple TCP wrapper or standard ASIO for networking
#include "TcpServer.h"

using namespace DFHack;
using namespace df::enums;

DFHACK_PLUGIN("dfvr_bridge");

static std::thread server_thread;
static std::mutex state_mutex;
static bool is_running = false;
static TcpServer tcp_server;

// Shared state buffer populated by the main thread
static dfvr::GameStatePayload shared_game_state;

// Scrapes the current active menu options in Adventure Mode
// MUST BE CALLED FROM THE MAIN THREAD (e.g. plugin_onupdate)
void ScrapeMenuOptions(dfvr::GameStatePayload& payload) {
    auto screen = Gui::getCurViewscreen(true);
    
    // Check if we are in an adventure mode screen (simplified example)
    if (auto adv_menu = virtual_cast<df::viewscreen_adventure_menust>(screen)) {
        // In a real implementation, we would parse adv_menu->menu_options
        // Here we provide a mock example of populating the Protobuf repeated field.
        dfvr::MenuOption* opt = payload.add_current_menu_options();
        opt->set_option_id(1);
        opt->set_text_content("Ask about surrounding area.");
    }
}

// DFHack Update Hook (Main Thread)
DFhackCExport command_result plugin_onupdate(color_ostream &out) {
    std::lock_guard<std::mutex> lock(state_mutex);
    
    // Clear and re-populate the shared state
    shared_game_state.Clear();
    ScrapeMenuOptions(shared_game_state);
    
    // TODO: Populate Entities and BodyParts here
    
    return CR_OK;
}

// Background thread function serving clients
void ServerLoop() {
    tcp_server.listen(9000); // Listen on port 9000

    while (is_running) {
        if (tcp_server.has_client()) {
            std::string serialized_data;
            
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                shared_game_state.SerializeToString(&serialized_data);
            }

            tcp_server.send(serialized_data);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // ~20 TPS
    }
}

DFhackCExport command_result plugin_init(color_ostream &out, std::vector<PluginCommand> &commands) {
    is_running = true;
    server_thread = std::thread(ServerLoop);
    out.print("DFVR Bridge Server started on port 9000.\n");
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    is_running = false;
    if (server_thread.joinable()) {
        server_thread.join();
    }
    tcp_server.close();
    return CR_OK;
}

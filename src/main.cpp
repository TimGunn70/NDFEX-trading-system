#include <thread>
#include <iostream>
#include <fstream>
#include <atomic>
#include <unordered_map>
#include <utility>
#include <chrono>

#include "symbol_manager.h"
#include "oe_client.h"
#include "etf_client.h"
#include "etf_arb.h"
#include "listener.h"
#include "config.h"

//static constexpr uint32_t GOLD_TICK = 10;
static constexpr uint32_t BLUE_TICK = 5;
//static constexpr int32_t  MM_POSITION_LIMIT = 6;

int main() {
    std::string error;
    std::optional<Config> cfg = load_config("config/bot.toml", error);
    if (!cfg) {
        std::cerr << "config/bot.toml: " << error << "\n";
        return 1;
    }
    SymbolManager       sm;
    std::atomic<bool>   global_shutdown{false};

    std::unordered_map<uint64_t, std::pair<uint32_t, SIDE>> mm_order_map;

    OEClient oe(cfg->exchange.host, cfg->exchange.port);
    if (!oe.connect()) {
        std::cerr << "Failed to connect to exchange\n";
        return 1;
    }
    if (!oe.login(cfg->auth.team_name.c_str(), cfg->auth.password.c_str(), cfg->exchange.client_id)) {
        std::cerr << "Login failed\n";
        return 1;
    }

    ETFClient etf(cfg->etf_service.url, cfg->auth.team_name, cfg->auth.password);
    if (!etf.health_check()) {
        std::cerr << "ETF service unreachable\n";
        return 1;
    }

    
    ETFArb arb(sm, oe, etf, global_shutdown, mm_order_map);
    oe.set_on_close([&](uint64_t order_id) {
    mm_order_map.erase(order_id);
    });


    std::thread md_thread([&]() {
        run_listener(sm, cfg->market_data, global_shutdown);
    });

    // Wait for books to populate
    std::cout << "Waiting 3s for market data snapshot...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::thread pnl_thread([&]() {
        std::ofstream pnl_log("pnl_log.txt", std::ios::app);
        std::ofstream pnl_file("pnl.txt", std::ios::trunc);
        pnl_log << "=== PnL Log Started ===\n";
        pnl_log.flush();
        pnl_file << "=== PnL Summary ===\n";
        pnl_file.flush();

        while (!global_shutdown.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            double pnl = sm.get_total_pnl();
            
            // Format output
            std::string output = "[PnL] total=" + std::to_string(pnl) + " | positions: ";
            for (uint32_t id = 1; id <= 13; ++id) {
                int32_t pos = sm.get_position(id);
                if (pos != 0) output += "sym" + std::to_string(id) + "=" + std::to_string(pos) + " ";
            }


            std::cout << output << "\n";
            std::cout.flush();
            
            pnl_log << output << "\n";
            pnl_log.flush();
            
            pnl_file.seekp(0);
            pnl_file.write(output.c_str(), output.length());
            pnl_file << "\n";
            pnl_file.flush();

            if (pnl < -4000.0) {
                std::string warning = "[PnL] WARNING: approaching -5000 floor!";
                std::cerr << warning << "\n";
                pnl_log << warning << "\n";
                pnl_log.flush();
            }
        }
        pnl_log << "=== PnL Log Ended ===\n";
        pnl_log.flush();
        pnl_log.close();
        pnl_file.close();
    });



    md_thread.join();
    pnl_thread.join();
    return 0;
}
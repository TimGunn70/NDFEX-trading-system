#pragma once
#include "symbol_manager.h"
#include "config.h"
#include <atomic>

// Runs the market data listener loop. Returns on fatal error and sets global_shutdown before returning.
// Call this on a dedicated thread from main.cpp. 
// The listener still runs even if global_shutdown is true - TODO(P4)
// All book updates are forwarded into `sm` via the SymbolManager interface.
void run_listener(SymbolManager& sm, const Config::MarketData& md, std::atomic<bool>& global_shutdown);


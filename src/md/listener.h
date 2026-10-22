#pragma once
#include "symbol_manager.h"
#include "config.h"

// Runs the market data listener loop. Blocks forever.
// Call this on a dedicated thread from main.cpp.
// All book updates are forwarded into `sm` via the SymbolManager interface.
void run_listener(SymbolManager& sm, const Config::MarketData& md);


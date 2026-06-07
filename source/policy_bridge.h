#pragma once
#ifndef POLICY_BRIDGE_H
#define POLICY_BRIDGE_H

#include "threads.h"

void init_policy();
bool load_policy(const char* filename);
void evaluate_policy(ThreadData& td, float* output_scores);

// True once a correctly-sized policy .bin has been loaded. Used by the offline
// history-seeding (PolicySeed): skip the forward pass entirely when no net is
// present (weights are zeroed -> logits all 0 -> seed would be a no-op anyway).
bool policy_loaded();

#endif
// Copyright 2020 Farouk Salem <salem@zib.de>

#pragma once

#include "HeartbeatManager.hpp"
#include "HeartbeatMessage.hpp"


namespace tl_libfabric
{
/**
 *  Facade layer of common Scheduler functionalities between DDSs and INs
 */
class SchedulerOrchestrator
{
public:
//// Common Methods
    // Initialize
    static void initialize(HeartbeatManager* heartbeat_manager);

//// ComputeHeartbeatManager Methods

    // Log sent heartbeat message
    static void log_sent_heartbeat_message(uint32_t connection_id, HeartbeatMessage message);

    // get next message id sequence
    static uint64_t get_next_heartbeat_message_id();

    // Acknowledge the arrival of a sent hearbeat message
    static void acknowledge_heartbeat_message(uint64_t message_id);

//// Variables
private:
    static HeartbeatManager* heartbeat_manager_;
};
}
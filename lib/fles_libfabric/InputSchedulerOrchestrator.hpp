// Copyright 2019 Farouk Salem <salem@zib.de>

#pragma once

#include "InputIntervalScheduler.hpp"
#include "InputTimesliceManager.hpp"
#include "HeartbeatManager.hpp"
#include "IntervalMetaData.hpp"

namespace tl_libfabric
{
/**
 * Singleton and Facade layer of the Input Scheduler that could be used in InputChannelSender and InputChannelConnections
 */
class InputSchedulerOrchestrator
{
public:
////Common Methods
    // Initialize the instance scheduler
    static void initialize(uint32_t scheduler_index, uint32_t compute_conn_count,
	    uint32_t interval_length, std::string log_directory, bool enable_logging);


    // update the compute node count which is needed for the initial interval (#0)
    static void update_compute_connection_count(uint32_t);

    // Set the input scheduler index
    static void update_input_scheduler_index(uint32_t);

    // Set the begin time to be used in logging and create the first interval if not there
    static void update_input_begin_time(std::chrono::high_resolution_clock::time_point);

    // Get the number of current compute node connections
    static uint32_t get_compute_connection_count();

    //Generate log files of the stored data
    static void generate_log_files();

//// InputIntervalScheduler Methods
    // Receive proposed interval meta-data from InputChannelConnections
    static void add_proposed_meta_data(const IntervalMetaData);

    // Return the actual interval meta-data to InputChannelConnections
    static const IntervalMetaData* get_actual_meta_data(uint64_t);

    // Get last timeslice to be sent
    static uint64_t get_last_timeslice_to_send();

    // Get the time to start sending more timeslices
    static int64_t get_next_fire_time();

//// InputTimesliceManager Methods

    // Get the timeslice to be sent to specific compute index
    static uint64_t get_connection_next_timeslice(uint32_t compute_index);

    // Log the transmission time of a timeslice
    static void mark_timeslice_transmitted(uint32_t compute_index, uint64_t timeslice);

    // Log the duration to receive the completion event of rdma write operation
    static void mark_timeslice_rdma_write_acked(uint32_t compute_index, uint64_t timeslice);

    // Log the duration of a timeslice until receiving the ack
    static void mark_timeslices_acked(uint32_t compute_index, uint64_t up_to_descriptor_id);

    // Check whether a timeslice is acked
    static bool is_timeslice_rdma_acked(uint32_t compute_index, uint64_t timeslice);

    // Get the timeslice number of a specific descriptor
    static uint64_t get_timeslice_of_not_acked_descriptor(uint32_t compute_index, uint64_t descriptor);

    //
    static void log_timeslice_IB_blocked(uint64_t timeslice, bool sent_completed=false);

    //
    static void log_timeslice_CB_blocked(uint64_t timeslice, bool sent_completed=false);

    //
    static void log_timeslice_MR_blocked(uint64_t timeslice, bool sent_completed=false);


//// HeartbeatManager Methods

    static void log_heartbeat(uint32_t connection_id);


private:
    static InputIntervalScheduler* interval_scheduler_;
    static InputTimesliceManager* timeslice_manager_;
    static HeartbeatManager* heartbeat_manager_;
};
}

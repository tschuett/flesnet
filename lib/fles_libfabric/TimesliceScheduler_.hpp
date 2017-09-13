/*
 * TimesliceScheduler.hpp
 *
 *  Created on: Aug 4, 2017
 *      Author: Farouk Salem
 */

#pragma once

#include "InputSchedulerData.hpp"
#include <algorithm>
#include <set>
#include <vector>
#include <chrono>
#include <assert.h>
#include <log.hpp>
#include <fstream>
#include <iomanip>

namespace tl_libfabric {


class TimesliceScheduler {
public:
	TimesliceScheduler(const uint64_t compute_index,
		const uint32_t input_node_count, const uint32_t interval_length):
			compute_index_(compute_index), input_node_count_(input_node_count), INTERVAL_LENGTH(interval_length),
			ts_duration_(MAX_DURATION_HISTORY), ts_duration_stats_(ConstVariables::SCHEDULER_INTERVAL_LENGTH),
			acked_ts_count_(MAX_DURATION_HISTORY){

		for (uint_fast16_t i=0 ; i<input_node_count_ ; i++)
			sender_info_.push_back(InputSchedulerData());

		alpha_percentage_.resize(input_node_count,0);
		completed_ts_ = false;

	}

	TimesliceScheduler() = delete;
	TimesliceScheduler(const TimesliceScheduler&) = delete;
	TimesliceScheduler& operator=(const TimesliceScheduler&) = delete;

	/// set the MPI barrier time of TimesliceBuilder
	void set_compute_MPI_time(
			std::chrono::high_resolution_clock::time_point compute_MPI_time){
		compute_MPI_time_ = compute_MPI_time;
	}

	/// Init compute info
	void init_compute_time(uint64_t compute_index, uint32_t input_node_count,
			std::chrono::high_resolution_clock::time_point compute_MPI_time){
		compute_index_ = compute_index;
		input_node_count_ = input_node_count;
		compute_MPI_time_ = compute_MPI_time;
	}

	/// This method initializes the required data from each input node such as when the MPI barrier is passed
	void init_input_index_info(uint32_t input_index,
			std::chrono::high_resolution_clock::time_point MPI_time){

		assert(sender_info_.size() == input_node_count_);
		sender_info_[input_index].MPI_Barrier_time = MPI_time;
		sender_info_[input_index].clock_offset = std::chrono::duration_cast
				<std::chrono::microseconds>(compute_MPI_time_ - MPI_time).count();
	}

	/// This method adds the received information from an input node to the scheduler data
	void add_input_ts_info(uint32_t input_index, uint64_t timeslice,
			std::chrono::high_resolution_clock::time_point sent_time,
			std::chrono::high_resolution_clock::time_point proposed_time,
			double duration){

	    if (!sender_info_[input_index].ts_sent_info_.contains(timeslice)) {
		    sender_info_[input_index].ts_sent_info_.add(timeslice, std::pair<std::chrono::high_resolution_clock::time_point,uint64_t>(sent_time, duration));
		    if (sender_info_[input_index].min_duration == ConstVariables::MINUS_ONE || sender_info_[input_index].min_duration > duration){
			sender_info_[input_index].min_duration = duration;
		    }
		    increament_acked_ts(timeslice);

		    /// Logging
		    std::map<uint64_t, std::vector<std::pair<int64_t, int64_t> > >::iterator it = proposed_actual_times_log_.find(timeslice);

		    if (it == proposed_actual_times_log_.end()){
			proposed_actual_times_log_.insert(std::pair<uint64_t,
				std::vector<std::pair<int64_t, int64_t> > >(timeslice, std::vector<std::pair<int64_t, int64_t> >(input_node_count_)));
			it = proposed_actual_times_log_.find(timeslice);
		    }

		    it->second[input_index] = std::make_pair<int64_t, int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(proposed_time - compute_MPI_time_).count() + sender_info_[input_index].clock_offset,
			std::chrono::duration_cast<std::chrono::microseconds>(sent_time - compute_MPI_time_).count() + sender_info_[input_index].clock_offset);

		    if (it->second[input_index].first < 0 )it->second[input_index].first = 0;
		    /// END OF Logging
	    }

	}

	/// This method gets the sent time for a particular input node and timeslice
	std::chrono::high_resolution_clock::time_point get_sent_time(
		    uint32_t input_index, uint64_t timeslice){

	    uint64_t last_complete_ts = get_last_complete_ts();
	    uint64_t last_complete_ts_duration = get_median_ts_duration(last_complete_ts);
	    uint32_t last_input_node = (compute_index_ - 1) % input_node_count_;
	    // get last sent time of the received contribution of the last complete timeslice
	    std::chrono::high_resolution_clock::time_point last_received_contribution_time =
			    sender_info_[last_input_node].ts_sent_info_.get(
					    last_complete_ts).first
					    + std::chrono::microseconds(
							    sender_info_[last_input_node].clock_offset);
	    uint64_t sum_needed_duration = 0;
	    for (uint32_t i = compute_index_; i != input_index;
		    i = ((i+1) % input_node_count_)) {
		    sum_needed_duration += sender_info_[i].min_duration;
	    }
	    sum_needed_duration += (sum_needed_duration * alpha_percentage_[input_index]);

	    std::chrono::high_resolution_clock::time_point sent_time = last_received_contribution_time + std::chrono::microseconds(
			    sum_needed_duration - sender_info_[input_index].clock_offset);

	    for (uint64_t ts = last_complete_ts+input_node_count_ ; ts < timeslice ; ts+=input_node_count_){
		    sent_time += std::chrono::microseconds(last_complete_ts_duration);
	    }

	    //L_(info) << "[get][" <<input_index <<"][" << last_complete_ts << "] sum_dur=" << sum_needed_duration << " offset= " << sender_info_[input_index].clock_offset << " time = " << std::chrono::duration_cast<std::chrono::microseconds>(last_received_contribution_time - compute_MPI_time_).count()
		//     << " sent_time= " << std::chrono::duration_cast<std::chrono::microseconds>(sent_time - compute_MPI_time_).count();

	    /// Logging
	    std::map<uint64_t, std::vector<int64_t> >::iterator it = proposed_times_log_.find(timeslice);
	    if (it == proposed_times_log_.end()){
		proposed_times_log_.insert(std::pair<uint64_t, std::vector<int64_t> >(timeslice, std::vector<int64_t>(input_node_count_)));
		it = proposed_times_log_.find(timeslice);
	    }

	    it->second[input_index] = std::chrono::duration_cast<std::chrono::microseconds>(sent_time - compute_MPI_time_).count() + sender_info_[input_index].clock_offset;
	    /// END OF Logging

	    return sent_time;
	}

	/// This method gets the sent time for a particular input node and timeslice
	std::chrono::high_resolution_clock::time_point get_next_interval_sent_time(
		    uint32_t input_index, uint64_t timeslice){

	    uint64_t last_complete_ts = get_last_complete_ts();
	    uint64_t interval_index = get_timeslice_interval(last_complete_ts);// fraction will be thrown away
	    // TODO INTERVAL_LENGTH * num_input_nodes should be INTERVAL_LENGTH * num_COMPUTE_nodes
	    uint64_t current_interval_start_ts = (interval_index * (INTERVAL_LENGTH * input_node_count_)) + compute_index_; // the first ts in this interval_index
	    uint32_t count_received_ts_in_interval = ((last_complete_ts - current_interval_start_ts) / input_node_count_) + 1;

	    uint32_t next_interval_start_ts = ((interval_index+1) * (INTERVAL_LENGTH * input_node_count_)) + compute_index_; // the first ts in this interval_index
	    assert (timeslice == next_interval_start_ts);
	    uint32_t count_ts_to_next_interval = ((next_interval_start_ts - last_complete_ts) / input_node_count_) - 1;

	    // get first sent time of the received contribution of the interval interval_index
	    std::chrono::high_resolution_clock::time_point first_interval_received_contribution_time =
			    sender_info_[compute_index_].ts_sent_info_.get(current_interval_start_ts).first
					    + std::chrono::microseconds(
							    sender_info_[compute_index_].clock_offset);

	    // get last sent time of the received contribution of the last complete timeslice
	    uint32_t last_input_node = (compute_index_ - 1) % input_node_count_;
	    std::chrono::high_resolution_clock::time_point last_received_contribution_time =
			    sender_info_[last_input_node].ts_sent_info_.get(last_complete_ts).first
					    + std::chrono::microseconds(
							    sender_info_[last_input_node].clock_offset);

	    // Get the average duration per timeslice within a particular interval
	    uint64_t average_duration_per_ts = std::chrono::duration_cast<std::chrono::microseconds>(
		    last_received_contribution_time - first_interval_received_contribution_time).count() / count_received_ts_in_interval;

	    // build the time gap between different input nodes
	    uint64_t sum_needed_duration = 0;
	    for (uint32_t i = compute_index_; i != input_index;
		    i = ((i+1) % input_node_count_)) {
		    sum_needed_duration += sender_info_[i].min_duration;
	    }
	    sum_needed_duration += (sum_needed_duration * alpha_percentage_[input_index]);

	    std::chrono::high_resolution_clock::time_point sent_time = last_received_contribution_time + std::chrono::microseconds(
		    (count_ts_to_next_interval * average_duration_per_ts) +
	    			    sum_needed_duration - sender_info_[input_index].clock_offset);

	    /// Logging
	    std::map<uint64_t, std::vector<int64_t> >::iterator it = proposed_times_log_.find(timeslice);
	    if (it == proposed_times_log_.end()){
		proposed_times_log_.insert(std::pair<uint64_t, std::vector<int64_t> >(timeslice, std::vector<int64_t>(input_node_count_)));
		it = proposed_times_log_.find(timeslice);
	    }

	    it->second[input_index] = std::chrono::duration_cast<std::chrono::microseconds>(sent_time - compute_MPI_time_).count() + sender_info_[input_index].clock_offset;
	    /// END OF Logging

	    return sent_time;
	}

	/// This method gets the duration needed for receiving a complete timeslice after a specific timeslice
	uint64_t get_ts_duration(uint64_t timeslice){

	    if (ts_duration_.contains(timeslice)) return ts_duration_.get(timeslice);

	    return ConstVariables::MINUS_ONE;
	}

	/// This method returns the adjusted duration for receiving a complete timeslice after a specific timeslice. Theta is a factor in the calculations
	uint64_t get_adjusted_ts_duration(uint64_t timeslice){

	    if (!ts_duration_.contains(timeslice)) return ConstVariables::MINUS_ONE;

	    uint64_t interval = get_timeslice_interval(timeslice);
	    std::map<uint64_t, std::pair<uint64_t,uint64_t> >::iterator it = interval_duration_log_.find(interval+1);
	    if (it != interval_duration_log_.end() && it->second.second != ConstVariables::MINUS_ONE) return it->second.second;

	    uint64_t adjusted_duration ;
	    if (min_ts_duration_ == ConstVariables::MINUS_ONE){
		TimeSchedulerStatsData stats_data = calculate_stats_data(timeslice);
		adjusted_duration = (stats_data.median + (stats_data.median * get_adjusted_theta(interval)));
	    }else{
		adjusted_duration = (min_ts_duration_ + (min_ts_duration_ * get_adjusted_theta(interval)));
	    }
	    if (it == interval_duration_log_.end()){
		interval_duration_log_.insert(std::pair<uint64_t, std::pair<uint64_t, uint64_t>>(interval+1, std::pair<uint64_t, uint64_t>(ConstVariables::MINUS_ONE, adjusted_duration)));
	    }else{
		it->second.second = adjusted_duration;

	    }
	    return adjusted_duration;

	}


	/// This method gets the median duration for receiving an interval of timeslices before a specific timeslice
	uint64_t get_median_ts_duration(uint64_t timeslice){

	    if (!ts_duration_.contains(timeslice)) return ConstVariables::MINUS_ONE;

	    return calculate_stats_data(timeslice).median;
	}

	/// This method gets the mean duration for receiving an interval of timeslices before a specific timeslice
	uint64_t get_mean_ts_duration(uint64_t timeslice){

	    if (!ts_duration_.contains(timeslice)) return ConstVariables::MINUS_ONE;

	    return calculate_stats_data(timeslice).mean;
	}

	/// This method gets the variance of receiving an interval of timeslices before a specific timeslice
	uint64_t get_variance_ts_duration(uint64_t timeslice){

	    if (!ts_duration_.contains(timeslice)) return ConstVariables::MINUS_ONE;

	    return calculate_stats_data(timeslice).variance;
	}

	///This method returns the latest completed timeslice
	uint64_t get_last_complete_ts() {

	    if (ts_duration_.size() == 0) {
		    return ConstVariables::MINUS_ONE;
	    }
	    return ts_duration_.get_last_key();
	}

	bool check_new_ts_completed(){

	    if (completed_ts_) {
		    completed_ts_ = false;
		    return true;
	    }
	    return false;
	}

	void build_scheduled_time_file(){

	    std::ofstream log_file;
	    log_file.open(std::to_string(compute_index_)+".compute.proposed_vs_sent_time.out");

	    log_file << std::setw(25) << "Input Index" << std::setw(25) << "Timeslice" << std::setw(25) << "Contribution" << std::setw(25) << "Proposed(t)" << std::setw(25) << "Sent(t)" << std::setw(25) << "Diff" << std::setw(25) << "Duration" << "\n";

	    std::map<uint64_t, std::vector<std::pair<int64_t, int64_t> > >::iterator it = proposed_actual_times_log_.begin();
	    std::vector<std::pair<int64_t, int64_t>> times;

	    while (it != proposed_actual_times_log_.end()){
		times = it->second;

		for (uint32_t i=0 ; i<times.size() ; i++){
		    log_file << std::setw(25) << i << std::setw(25) << it->first << std::setw(25) << (it->first+i) << std::setw(25) << (times[i].first*1.0)/1000.0 << std::setw(25) <<
			    (times[i].second*1.0)/1000.0 << std::setw(25) << ((times[i].second - times[i].first)*1.0)/1000.0 << std::setw(25) <<
			    (durations_log_.find(it->first)->second*1.0)/1000.0 << "\n";
		}

		log_file.flush();
		++it;
	    }


	    log_file.close();
	}

	void build_duration_file(){

	    std::ofstream log_file;
	    log_file.open(std::to_string(compute_index_)+".compute.proposed_vs_taken_duration.out");

	    log_file << std::setw(25) << "Interval" << std::setw(25) << "Duration(proposed)" << std::setw(25) << "Duration(Taken)" << std::setw(25) << "Diff(p-t)" << "\n";

	    std::map<uint64_t, std::pair<uint64_t, uint64_t> >::iterator it = interval_duration_log_.begin();
	    double diff, taken_duration, proposed_duration;
	    while (it != interval_duration_log_.end()){
		taken_duration = it->second.first == ConstVariables::MINUS_ONE ? -1 : (it->second.first*1.0)/1000.0;
		proposed_duration = it->second.second == ConstVariables::MINUS_ONE ? -1 : (it->second.second*INTERVAL_LENGTH*1.0)/1000.0;
		diff = taken_duration != -1 && proposed_duration != -1 ? proposed_duration - taken_duration : -1;
		    log_file << std::setw(25) << it->first << std::setw(25) <<
			    proposed_duration << std::setw(25) <<
			    taken_duration << std::setw(25) <<
			    diff << "\n";

		log_file.flush();
		++it;
	    }


	    log_file.close();
	}

private:

	/// This struct contains the needed data for update theta and alpha. It contains the variance, median, and mean of set of durations
	struct TimeSchedulerStatsData {
	    uint64_t mean = 0;
	    uint64_t median = 0;
	    uint64_t variance = 0;
	};

	uint32_t get_timeslice_interval(uint64_t timeslice){
	    // TODO INTERVAL_LENGTH * num_input_nodes should be INTERVAL_LENGTH * num_COMPUTE_nodes
	    return(timeslice / (INTERVAL_LENGTH * input_node_count_)); // fraction will be thrown away
	}
	/// This increases the counter for the received timeslices to trigger when to start calculate the sent time
	void increament_acked_ts(uint64_t timeslice) {

	    uint32_t count = 1;
	    if (acked_ts_count_.contains(timeslice)) {
		count = acked_ts_count_.get(timeslice)+1;
		acked_ts_count_.update(timeslice,count);
	    } else {
		acked_ts_count_.add(timeslice, count);
	    }
	    if (count == input_node_count_) {
		calculate_total_ts_duration(timeslice);
	    }
	}

	/// This calculates the needed duration to receive a complete timeslice from all input nodes
	void calculate_total_ts_duration(uint64_t timeslice){

	    uint64_t total_duration = 0;
	    for (uint32_t i = 0; i < input_node_count_; i++) {
		    total_duration += sender_info_[i].ts_sent_info_.get(timeslice).second;
	    }
	    //total_duration += (total_duration * theta_percentage_);

	    ts_duration_.add(timeslice, total_duration);
	    if (min_ts_duration_ == ConstVariables::MINUS_ONE || total_duration < min_ts_duration_){
		min_ts_duration_ = total_duration;
	    }
	    completed_ts_ = true;


	    //logging
	    durations_log_.insert(std::pair<uint64_t, uint64_t>(timeslice, total_duration));

	}

	TimeSchedulerStatsData calculate_stats_data(uint64_t timeslice){

	    if (ts_duration_stats_.contains(timeslice)) return ts_duration_stats_.get(timeslice);

	    TimeSchedulerStatsData statsData;
	    std::vector<uint64_t> values;
	    uint64_t sum =0;
	    SizedMap<uint64_t, uint64_t>::iterator start_duration_it = ts_duration_.get_iterator(timeslice);
	    // get values of up to an interval and calculate the sum
	    while (values.size() < ConstVariables::SCHEDULER_INTERVAL_LENGTH){
		values.push_back(start_duration_it->second);
		sum += start_duration_it->second;
		if (start_duration_it == ts_duration_.get_begin_iterator())break;
		--start_duration_it;
	    }
	    std::sort(values.begin(), values.end());

	    statsData.mean = (sum/values.size());
	    statsData.median = values[values.size()/2];

	    // calculate variance
	    statsData.variance = 0;
	    for (int i=0 ; i<values.size() ; i++){
		statsData.variance += (values[i] - statsData.mean);
	    }
	    statsData.variance /=values.size();

	    ts_duration_stats_.add(timeslice,statsData);

	    return statsData;
	}

	uint64_t get_actual_interval_duration(uint32_t interval_index){

	    std::map<uint64_t, std::pair<uint64_t,uint64_t> >::iterator interval_it = interval_duration_log_.find(interval_index);
	    if (interval_it != interval_duration_log_.end() && interval_it->second.first != ConstVariables::MINUS_ONE) return interval_it->second.first;

	    uint32_t start_ts = (interval_index * (INTERVAL_LENGTH * input_node_count_)) + compute_index_; // the first ts in this interval_index
	    uint32_t last_ts = start_ts + (INTERVAL_LENGTH * input_node_count_); // the last ts in this interval_index
	    if (!ts_duration_.contains(start_ts) || !ts_duration_.contains(last_ts)) return ConstVariables::MINUS_ONE;
	    uint64_t sum =0;
	    SizedMap<uint64_t, uint64_t>::iterator start_duration_it = ts_duration_.get_iterator(start_ts);
	    SizedMap<uint64_t, uint64_t>::iterator last_duration_it = ts_duration_.get_iterator(last_ts);
	    // get values of up to an interval and calculate the sum
	    while (true){
		sum += start_duration_it->second;
		if (start_duration_it == last_duration_it)break;
		++start_duration_it;
	    }
	    if (interval_it == interval_duration_log_.end()){
		interval_duration_log_.insert(std::pair<uint64_t, std::pair<uint64_t, uint64_t>>(interval_index, std::pair<uint64_t, uint64_t>(sum,ConstVariables::MINUS_ONE)));
	    }else{
		interval_it->second.first = sum;
	    }
	    if (min_interval_duration_ == ConstVariables::MINUS_ONE || min_interval_duration_ > sum){
		min_interval_duration_ = sum;
	    }
	    return sum;
	}

	// This method finds the theta by the last two completed intervals
	double get_adjusted_theta(uint64_t current_interval_index){

	    if (current_interval_index <= 1) return 0;

	    uint64_t prev_interval_duration = get_actual_interval_duration(current_interval_index-1);

	    // previous interval is incomplete
	    if (prev_interval_duration != ConstVariables::MINUS_ONE){
		return 0;
	    }

	    uint64_t pre_prev_interval_duration = get_actual_interval_duration(current_interval_index - 2);

	    if (prev_interval_duration <= pre_prev_interval_duration) return -0.1;

	    return 0.1;
	}

	/// This const variable limits the number of durations of timeslices to be kept
	const int32_t MAX_DURATION_HISTORY = 100;

	/// This const variable determines the interval length that a new sent_time and duration will be generated
	const uint32_t INTERVAL_LENGTH;

	/// The compute node index. The order of input nodes is based on this index
	uint64_t compute_index_;

	/// The local time of the compute node when the MPI barrier reached
	std::chrono::high_resolution_clock::time_point compute_MPI_time_;

	/// The number of input nodes which the compute receives data from
	uint32_t input_node_count_;

	/// This is a list of input nodes with the history of their data
	std::vector<InputSchedulerData> sender_info_;

	/// A history of the estimated durations <timeslice, duration>
	SizedMap<uint64_t, uint64_t> ts_duration_;

	/// A history of the mean durations till specific timeslice <timeslice, duration>
	SizedMap<uint64_t, TimeSchedulerStatsData> ts_duration_stats_;

	/// Count of the acked contributions from input nodes <timeslice, count>
	SizedMap<uint64_t, uint32_t> acked_ts_count_;

	/// Theta to increase/decrease the duration needed to receive a complete timeslice -- <= 1.0
	double theta_percentage_ = 0;

	/// Alpha to increase/decrease the time to send timeslices -- <= 1.0
	std::vector<double> alpha_percentage_;

	/// Triggers if there are new completed timeslices
	bool completed_ts_ = false;

	uint64_t min_interval_duration_ = ConstVariables::MINUS_ONE;

	uint64_t min_ts_duration_ = ConstVariables::MINUS_ONE;


	/// LOGGING
	// timeslice, [{proposed, actual}]
	std::map<uint64_t, std::vector<std::pair<int64_t, int64_t> > > proposed_actual_times_log_;
	///
	std::map<uint64_t, uint64_t> durations_log_;
	///
	std::map<uint64_t, std::vector<int64_t> > proposed_times_log_;
	/// interval index, <taken duration, proposed duration gap>
	std::map<uint64_t, std::pair<uint64_t, uint64_t> > interval_duration_log_;


};


} // namespace tl_libfabric


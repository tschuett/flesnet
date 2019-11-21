// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>
// Copyright 2016 Thorsten Schuett <schuett@zib.de>, Farouk Salem <salem@zib.de>

#pragma once

#include "ConstVariables.hpp"
#include "SizedMap.hpp"
#include "RequestIdentifier.hpp"
#include "LibfabricContextPool.hpp"
#include "HeartbeatMessage.hpp"
//#include "InfinibandException.hpp"
#include <memory>
//#include <rdma/rdma_cma.h>
#include <vector>

#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>

#include <cstdint>
#include <string>
#include <chrono>

namespace tl_libfabric
{
/// Libfabric connection base class.
/** An Connection object represents the endpoint of a single
    Libfabric connection handled by an rdma connection manager. */

class Connection
{
public:
    /// The Connection constructor. Creates an endpoint.
    Connection(struct fid_eq* eq, uint_fast16_t connection_index,
               uint_fast16_t remote_connection_index);

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// The Connection destructor.
    virtual ~Connection();

    //    /// Retrieve the InfiniBand queue pair associated with the connection.
    //    struct fid_qp* qp() const { return cm_id_->qp; }
    //
    /// Initiate a connection request to target hostname and service.
    /**
       \param hostname The target hostname
       \param service  The target service or port number
    */
    void connect(const std::string& hostname, const std::string& service,
                 struct fid_domain* domain, struct fid_cq* cq,
                 struct fid_av* av);

    void disconnect();

    virtual void on_rejected(struct fi_eq_err_entry* event);

    /// Connection handler function, called on successful connection.
    /**
       \param event RDMA connection manager event structure
       \return      Non-zero if an error occured
    */
    virtual void on_established(struct fi_eq_cm_entry* event);

    /// Handle RDMA_CM_EVENT_DISCONNECTED event for this connection.
    virtual void on_disconnected(struct fi_eq_cm_entry* event);
    //
    //    /// Handle RDMA_CM_EVENT_ADDR_RESOLVED event for this connection.
    //    virtual void on_addr_resolved(struct ibv_pd* pd, struct ibv_cq* cq);
    //
    //    virtual void create_qp(struct ibv_pd* pd, struct ibv_cq* cq);
    //
    // virtual void accept_connect_request();

    /// Handle RDMA_CM_EVENT_CONNECT_REQUEST event for this connection.
    virtual void on_connect_request(struct fi_eq_cm_entry* event,
                                    struct fid_domain* pd, struct fid_cq* cq);

    virtual std::unique_ptr<std::vector<uint8_t>> get_private_data();

    virtual void setup() = 0;
    virtual void setup_mr(struct fid_domain* pd) = 0;

    virtual bool try_sync_buffer_positions() = 0;

    /// Post a receive work request (WR) to the receive queue
    virtual void post_recv_heartbeat_message();

    /// Post a send work request (WR) to the send queue
    virtual void post_send_heartbeat_message();
    //
    //    /// Handle RDMA_CM_EVENT_ROUTE_RESOLVED event for this connection.
    //    virtual void on_route_resolved();
    //
    /// Retrieve index of this connection in the local connection group.
    uint_fast16_t index() const { return index_; }

    /// Retrieve index of this connection in the remote connection group.
    uint_fast16_t remote_index() const { return remote_index_; }

    bool done() const { return done_; }

    /// Retrieve the total number of bytes transmitted.
    uint64_t total_bytes_sent() const { return total_bytes_sent_; }

    /// Retrieve the total number of syn msgs bytes transmitted.
    uint64_t total_sync_bytes_sent() const { return total_sync_bytes_sent_; }

    /// Retrieve the total number of SEND work requests.
    uint64_t total_send_requests() const { return total_send_requests_; }

    /// Retrieve the total number of RECV work requests.
    uint64_t total_recv_requests() const { return total_recv_requests_; }

    std::chrono::high_resolution_clock::time_point time_begin_;

protected:
    //    void dump_send_wr(struct ibv_send_wr* wr);

    /// Post an Libfabric rdma send work request
    void post_send_rdma(struct fi_msg_rma* msg, uint64_t flags);

    /// Post an Libfabric message send work request
    void post_send_msg(const struct fi_msg_tagged* msg);

    /// Post an Libfabric message recveive request.
    void post_recv_msg(const struct fi_msg_tagged* msg);

    void make_endpoint(struct fi_info* info, const std::string& hostname,
                       const std::string& service, struct fid_domain* pd,
                       struct fid_cq* cq, struct fid_av* av);

    /// Message setup of heartbeat messages
    virtual void setup_heartbeat();

    /// Memory registers setup of heartbeat messages
    virtual void setup_heartbeat_mr(struct fid_domain* pd);

    /// Index of this connection in the local group of connections.
    uint_fast16_t index_;

    /// Index of this connection in the remote group of connections.
    uint_fast16_t remote_index_;

    /// Flag indicating connection finished state.
    bool done_ = false;
    //
    //    /// The queue pair capabilities.
    //    struct ibv_qp_cap qp_cap_;
    //
    /// connection configuration
    uint32_t max_send_wr_;
    uint32_t max_send_sge_;
    uint32_t max_recv_wr_;
    uint32_t max_recv_sge_;
    uint32_t max_inline_data_;

    struct fid_ep* ep_ = nullptr;

    bool connection_oriented_ = false;

    /// check if new data should be sent
    bool data_changed_= false;

    /// check if new data is acked and should be sent
    bool data_acked_= false;

    /// To prevent reusing the buffer while injecting sync messages
    bool send_buffer_available_ = true;

    /// Send Heartbeat message buffer
    HeartbeatMessage send_heartbeat_message_ = HeartbeatMessage();

    /// Receive Heartbeat message buffer
    HeartbeatMessage recv_heartbeat_message_ = HeartbeatMessage();

    /// heartbeat recv work request
    struct fi_msg_tagged heartbeat_recv_wr = fi_msg_tagged();
    struct iovec heartbeat_recv_wr_iovec = iovec();
    void* heartbeat_recv_descs[1] = {nullptr};
    fid_mr* mr_heartbeat_recv_ = nullptr;

    /// heartbeat send work request
    struct fi_msg_tagged heartbeat_send_wr = fi_msg_tagged();
    struct iovec heartbeat_send_wr_iovec = iovec();
    void* heartbeat_send_descs[1] = {nullptr};
    fid_mr* mr_heartbeat_send_ = nullptr;
    
private:
    //    /// Low-level communication parameters.
    //    enum {
    //        RESOLVE_TIMEOUT_MS = 5000 ///< Resolve timeout in milliseconds.
    //    };
    //
    //
    /// RDMA connection manager ID.
    //    struct rdma_cm_id* cm_id_ = nullptr;
    struct fid_eq* eq_ = nullptr;
    // struct fid_pep *pep_ = nullptr;

    /// Total number of bytes transmitted.
    uint64_t total_bytes_sent_ = 0;

    /// Total number of sync messages bytes transmitted.
    uint64_t total_sync_bytes_sent_ = 0;

    /// Total number of SEND work requests.
    uint64_t total_send_requests_ = 0;

    /// Total number of RECV work requests.
    uint64_t total_recv_requests_ = 0;

    const uint32_t num_cqe_ = 1000000;
};
}

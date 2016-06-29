// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>

#include "InputChannelSender.hpp"
#include "MicrosliceDescriptor.hpp"
#include "RequestIdentifier.hpp"
#include "Utility.hpp"
#include <cassert>
#include <log.hpp>
#include <rdma/fi_domain.h>

#include <valgrind/memcheck.h>

#include <chrono>

InputChannelSender::InputChannelSender(
    uint64_t input_index, InputBufferReadInterface &data_source,
    const std::vector<std::string> compute_hostnames,
    const std::vector<std::string> compute_services, uint32_t timeslice_size,
    uint32_t overlap_size, uint32_t max_timeslice_number)
    : input_index_(input_index), data_source_(data_source),
      compute_hostnames_(compute_hostnames),
      compute_services_(compute_services), timeslice_size_(timeslice_size),
      overlap_size_(overlap_size), max_timeslice_number_(max_timeslice_number),
      min_acked_desc_(data_source.desc_buffer().size() / 4),
      min_acked_data_(data_source.data_buffer().size() / 4)
{
    size_t min_ack_buffer_size =
        data_source_.desc_buffer().size() / timeslice_size_ + 1;
    ack_.alloc_with_size(min_ack_buffer_size);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    VALGRIND_MAKE_MEM_DEFINED(data_source_.data_buffer().ptr(),
                              data_source_.data_buffer().bytes());
    VALGRIND_MAKE_MEM_DEFINED(data_source_.desc_buffer().ptr(),
                              data_source_.desc_buffer().bytes());
#pragma GCC diagnostic pop
}

InputChannelSender::~InputChannelSender()
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    if (mr_desc_) {
        fi_close((struct fid *)mr_desc_);
        mr_desc_ = nullptr;
    }

    if (mr_data_) {
        fi_close((struct fid *)(mr_data_));
        mr_data_ = nullptr;
    }
#pragma GCC diagnostic pop
}

/// The thread main function.
void InputChannelSender::operator()()
{
    try
    {
        set_cpu(2);

        connect();
        while (connected_ != compute_hostnames_.size()) {
            poll_cm_events();
        }

        data_source_.proceed();
        time_begin_ = std::chrono::high_resolution_clock::now();

        uint64_t timeslice = 0;
        sync_buffer_positions();
        report_status();
        while (timeslice < max_timeslice_number_ && !abort_) {
            if (try_send_timeslice(timeslice)) {
                timeslice++;
            }
            poll_completion();
            data_source_.proceed();
            scheduler_.timer();
        }

        for (auto &c : conn_) {
            c->finalize(abort_);
        }

        L_(debug) << "[i" << input_index_ << "] "
                  << "SENDER loop done";

        while (!all_done_) {
            poll_completion();
            scheduler_.timer();
        }

        time_end_ = std::chrono::high_resolution_clock::now();

        disconnect();
        while (connected_ != 0) {
            poll_cm_events();
        }

        summary();
    }
    catch (std::exception &e)
    {
        L_(error) << "exception in InputChannelSender: " << e.what();
    }
}

void InputChannelSender::report_status()
{
    constexpr auto interval = std::chrono::seconds(1);

    // if data_source.written pointers are lagging behind due to lazy updates,
    // use sent value instead
    uint64_t written_desc = data_source_.get_write_index().desc;
    if (written_desc < sent_desc_) {
        written_desc = sent_desc_;
    }
    uint64_t written_data = data_source_.get_write_index().data;
    if (written_data < sent_data_) {
        written_data = sent_data_;
    }

    std::chrono::system_clock::time_point now =
        std::chrono::system_clock::now();
    SendBufferStatus status_desc{
        now,         data_source_.desc_buffer().size(), cached_acked_desc_,
        acked_desc_, sent_desc_,                        written_desc
    };
    SendBufferStatus status_data{
        now,         data_source_.data_buffer().size(), cached_acked_data_,
        acked_data_, sent_data_,                        written_data
    };

    double delta_t =
        std::chrono::duration<double, std::chrono::seconds::period>(
            status_desc.time - previous_send_buffer_status_desc_.time).count();
    double rate_desc =
        static_cast<double>(status_desc.acked -
                            previous_send_buffer_status_desc_.acked) /
        delta_t;
    double rate_data =
        static_cast<double>(status_data.acked -
                            previous_send_buffer_status_data_.acked) /
        delta_t;

    L_(debug) << "[i" << input_index_ << "] desc " << status_desc.percentages()
              << " (used..free) | "
              << human_readable_count(status_desc.acked, true, "") << " ("
              << human_readable_count(rate_desc, true, "Hz") << ")";

    L_(debug) << "[i" << input_index_ << "] data " << status_data.percentages()
              << " (used..free) | "
              << human_readable_count(status_data.acked, true) << " ("
              << human_readable_count(rate_data, true, "B/s") << ")";

    L_(info) << "[i" << input_index_ << "]   |"
             << bar_graph(status_data.vector(), "#x._", 20) << "|"
             << bar_graph(status_desc.vector(), "#x._", 10) << "| "
             << human_readable_count(rate_data, true, "B/s") << " ("
             << human_readable_count(rate_desc, true, "Hz") << ")";

    previous_send_buffer_status_desc_ = status_desc;
    previous_send_buffer_status_data_ = status_data;

    scheduler_.add(std::bind(&InputChannelSender::report_status, this),
                   now + interval);
}

void InputChannelSender::sync_buffer_positions()
{
    for (auto &c : conn_) {
        c->try_sync_buffer_positions();
    }

    auto now = std::chrono::system_clock::now();
    scheduler_.add(std::bind(&InputChannelSender::sync_buffer_positions, this),
                   now + std::chrono::milliseconds(0));
}

bool InputChannelSender::try_send_timeslice(uint64_t timeslice)
{
    // wait until a complete timeslice is available in the input buffer
    uint64_t desc_offset = timeslice * timeslice_size_;
    uint64_t desc_length = timeslice_size_ + overlap_size_;

    if (write_index_desc_ < desc_offset + desc_length) {
        write_index_desc_ = data_source_.get_write_index().desc;
    }
    // check if microslice no. (desc_offset + desc_length - 1) is avail
    if (write_index_desc_ >= desc_offset + desc_length) {

        uint64_t data_offset =
            data_source_.desc_buffer().at(desc_offset).offset;
        uint64_t data_end =
            data_source_.desc_buffer()
                .at(desc_offset + desc_length - 1)
                .offset +
            data_source_.desc_buffer().at(desc_offset + desc_length - 1).size;
        assert(data_end >= data_offset);

        uint64_t data_length = data_end - data_offset;
        uint64_t total_length =
            data_length + desc_length * sizeof(fles::MicrosliceDescriptor);

        if (false) {
            L_(trace) << "SENDER working on timeslice " << timeslice
                      << ", microslices " << desc_offset << ".."
                      << (desc_offset + desc_length - 1) << ", data bytes "
                      << data_offset << ".." << (data_offset + data_length - 1);
            L_(trace) << get_state_string();
        }

        int cn = target_cn_index(timeslice);

        if (!conn_[cn]->write_request_available())
            return false;

        // number of bytes to skip in advance (to avoid buffer wrap)
        uint64_t skip = conn_[cn]->skip_required(total_length);
        total_length += skip;

        if (conn_[cn]->check_for_buffer_space(total_length, 1)) {

            post_send_data(timeslice, cn, desc_offset, desc_length, data_offset,
                           data_length, skip);

            conn_[cn]->inc_write_pointers(total_length, 1);

            sent_desc_ = desc_offset + desc_length;
            sent_data_ = data_end;

            return true;
        }
    }

    return false;
}

std::unique_ptr<InputChannelConnection>
InputChannelSender::create_input_node_connection(uint_fast16_t index)
{
  //unsigned int max_send_wr = 8000; ???
  unsigned int max_send_wr = 495; // ???

    // limit pending write requests so that send queue and completion queue
    // do not overflow
    unsigned int max_pending_write_requests = std::min(
        static_cast<unsigned int>((max_send_wr - 1) / 3),
        static_cast<unsigned int>((num_cqe_ - 1) / compute_hostnames_.size()));

    std::unique_ptr<InputChannelConnection> connection(
              new InputChannelConnection(eq_, index, input_index_, max_send_wr,
                                         max_pending_write_requests));
    return connection;
}

void InputChannelSender::connect()
{
  if (!pd_)
    init_context(Provider::getInst()->get_info());

  for (unsigned int i = 0; i < compute_hostnames_.size(); ++i) {
        std::unique_ptr<InputChannelConnection> connection =
            create_input_node_connection(i);
        connection->connect(compute_hostnames_[i], compute_services_[i], pd_, cq_);
        conn_.push_back(std::move(connection));
    }
}

int InputChannelSender::target_cn_index(uint64_t timeslice)
{
    return timeslice % conn_.size();
}

void InputChannelSender::on_connected(struct fid_domain* pd)
{
  int requested_key = 1;

    if (!mr_data_) {
      // Register memory regions.
      int err = fi_mr_reg(
                      pd, const_cast<uint8_t*>(data_source_.data_send_buffer().ptr()),
                      data_source_.data_send_buffer().bytes(), FI_WRITE, 0,
                      requested_key++, 0, &mr_data_, nullptr);
      if(err) {
        std::cout << strerror(-err) << std::endl;
        throw LibfabricException("fi_mr_reg failed");
      }

      if (!mr_data_) {
        L_(error) << "fi_mr_reg failed for mr_data: " << strerror(errno);
        throw LibfabricException("registration of memory region failed");
      }

      err = fi_mr_reg(pd, const_cast<fles::MicrosliceDescriptor*>(
                      data_source_.desc_send_buffer().ptr()),
                      data_source_.desc_send_buffer().bytes(),
                      FI_WRITE, 0,
                      requested_key++, 0, &mr_desc_, nullptr);
      if(err) {
        std::cout << strerror(-err) << std::endl;
        throw LibfabricException("fi_mr_reg failed");
      }

      if (!mr_desc_) {
        L_(error) << "fi_mr_reg failed for mr_desc: " << strerror(errno);
        throw LibfabricException("registration of memory region failed");
      }

    }

}

void InputChannelSender::on_rejected(struct fi_eq_err_entry* event)
{
    std::cout << "InputChannelSender:on_rejected" << std::endl;

    InputChannelConnection* conn =
    static_cast<InputChannelConnection*>(event->fid->context);

    conn->on_rejected(event);
    uint_fast16_t i = conn->index();
    conn_.at(i) = nullptr;

    std::cout << "retrying: " << i << std::endl;
    // immediately initiate retry
    std::unique_ptr<InputChannelConnection> connection =
        create_input_node_connection(i);
    connection->connect(compute_hostnames_[i], compute_services_[i], pd_, cq_);
    conn_.at(i) = std::move(connection);
}

std::string InputChannelSender::get_state_string()
{
    std::ostringstream s;

    s << "/--- desc buf ---" << std::endl;
    s << "|";
    for (unsigned int i = 0; i < data_source_.desc_buffer().size(); ++i)
        s << " (" << i << ")" << data_source_.desc_buffer().at(i).offset;
    s << std::endl;
    s << "| acked_desc_ = " << acked_desc_ << std::endl;
    s << "/--- data buf ---" << std::endl;
    s << "|";
    for (unsigned int i = 0; i < data_source_.data_buffer().size(); ++i)
        s << " (" << i << ")" << std::hex << data_source_.data_buffer().at(i)
          << std::dec;
    s << std::endl;
    s << "| acked_data_ = " << acked_data_ << std::endl;
    s << "\\---------";

    return s.str();
}

void InputChannelSender::post_send_data(uint64_t timeslice, int cn,
                                        uint64_t desc_offset,
                                        uint64_t desc_length,
                                        uint64_t data_offset,
                                        uint64_t data_length, uint64_t skip)
{
    int num_sge = 0;
    struct iovec sge[4];
    void *descs[4];
    // descriptors
    if ((desc_offset & data_source_.desc_send_buffer().size_mask()) <=
        ((desc_offset + desc_length - 1) &
         data_source_.desc_send_buffer().size_mask())) {
        // one chunk
        sge[num_sge].iov_base =
            &data_source_.desc_send_buffer().at(desc_offset);
        sge[num_sge].iov_len = sizeof(fles::MicrosliceDescriptor) * desc_length;
        assert(mr_desc_ != nullptr);
        descs[num_sge++] = fi_mr_desc(mr_desc_);
        // sge[num_sge++].lkey = mr_desc_->lkey;
    } else {
        // two chunks
        sge[num_sge].iov_base =
            &data_source_.desc_send_buffer().at(desc_offset);
        sge[num_sge].iov_len =
            sizeof(fles::MicrosliceDescriptor) *
            (data_source_.desc_send_buffer().size() -
             (desc_offset & data_source_.desc_send_buffer().size_mask()));
        descs[num_sge++] = fi_mr_desc(mr_desc_);
        sge[num_sge].iov_base = data_source_.desc_send_buffer().ptr();
        sge[num_sge].iov_len =
            sizeof(fles::MicrosliceDescriptor) *
            (desc_length - data_source_.desc_send_buffer().size() +
             (desc_offset & data_source_.desc_send_buffer().size_mask()));
        descs[num_sge++] = fi_mr_desc(mr_desc_);
    }
    int num_desc_sge = num_sge;
    // data
    if (data_length == 0) {
        // zero chunks
    } else if ((data_offset & data_source_.data_send_buffer().size_mask()) <=
               ((data_offset + data_length - 1) &
                data_source_.data_send_buffer().size_mask())) {
        // one chunk
        sge[num_sge].iov_base =
            &data_source_.data_send_buffer().at(data_offset);
        sge[num_sge].iov_len = data_length;
        descs[num_sge++] = fi_mr_desc(mr_data_);
    } else {
        // two chunks
        sge[num_sge].iov_base =
            &data_source_.data_send_buffer().at(data_offset);
        sge[num_sge].iov_len =
            data_source_.data_send_buffer().size() -
            (data_offset & data_source_.data_send_buffer().size_mask());
        descs[num_sge++] = fi_mr_desc(mr_data_);
        sge[num_sge].iov_base = data_source_.data_send_buffer().ptr();
        sge[num_sge].iov_len =
            data_length - data_source_.data_send_buffer().size() +
            (data_offset & data_source_.data_send_buffer().size_mask());
        descs[num_sge++] = fi_mr_desc(mr_data_);
    }
    // copy between buffers
    for (int i = 0; i < num_sge; ++i) {
        if (i < num_desc_sge) {
            data_source_.copy_to_desc_send_buffer(
                reinterpret_cast<fles::MicrosliceDescriptor *>(
                    sge[i].iov_base) -
                    data_source_.desc_send_buffer().ptr(),
                sge[i].iov_len / sizeof(fles::MicrosliceDescriptor));
        } else {
            data_source_.copy_to_data_send_buffer(
                reinterpret_cast<uint8_t *>(sge[i].iov_base) -
                    data_source_.data_send_buffer().ptr(),
                sge[i].iov_len);
        }
    }

    conn_[cn]->send_data(sge, descs, num_sge, timeslice, desc_length,
                         data_length, skip);
}

void InputChannelSender::on_completion(uint64_t wr_id)
{
    switch (wr_id & 0xFF) {
    case ID_WRITE_DESC: {
        uint64_t ts = wr_id >> 24;

        int cn = (wr_id >> 8) & 0xFFFF;
        conn_[cn]->on_complete_write();

        uint64_t acked_ts = acked_desc_ / timeslice_size_;
        if (ts == acked_ts)
            do
                ++acked_ts;
            while (ack_.at(acked_ts) > ts);
        else
            ack_.at(ts) = ts;
        acked_data_ =
            data_source_.desc_buffer().at(acked_ts * timeslice_size_).offset;
        acked_desc_ = acked_ts * timeslice_size_;
        if (acked_data_ >= cached_acked_data_ + min_acked_data_ ||
            acked_desc_ >= cached_acked_desc_ + min_acked_desc_) {
            cached_acked_data_ = acked_data_;
            cached_acked_desc_ = acked_desc_;
            data_source_.set_read_index(
                { cached_acked_desc_, cached_acked_data_ });
        }
        if (false) {
            L_(trace) << "[i" << input_index_ << "] "
                      << "write timeslice " << ts
                      << " complete, now: acked_data_=" << acked_data_
                      << " acked_desc_=" << acked_desc_;
        }
    } break;

    case ID_RECEIVE_STATUS: {
        int cn = wr_id >> 8;
        conn_[cn]->on_complete_recv();
        if (conn_[cn]->request_abort_flag()) {
            abort_ = true;
        }
        if (conn_[cn]->done()) {
            ++connections_done_;
            all_done_ = (connections_done_ == conn_.size());
            L_(debug) << "[i" << input_index_ << "] "
                      << "ID_RECEIVE_STATUS final for id " << cn
                      << " all_done=" << all_done_;
        }
    } break;

    case ID_SEND_STATUS: {
    } break;

    default:
        L_(error) << "[i" << input_index_ << "] "
                  << "wc for unknown wr_id=" << (wr_id & 0xFF);
        throw LibfabricException("wc for unknown wr_id");
    }
}
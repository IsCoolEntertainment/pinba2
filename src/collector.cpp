#include "auto_config.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h> // setsockopt

#include <stdexcept>
#include <thread>
#include <vector>

#include <nanomsg/nn.h>
#include <nanomsg/pipeline.h>

#include <meow/defer.hpp>
#include <meow/format/format.hpp>
#include <meow/unix/fd_handle.hpp>
#include <meow/unix/socket.hpp>
#include <meow/unix/netdb.hpp>
#include <meow/unix/resource.hpp>

#include "pinba/nmsg_socket.h"
#include "pinba/nmsg_poller.h"
#include "pinba/collector.h"

#include "proto/pinba.pb-c.h"

#include "misc/nmpa.h"
#include "misc/nmpa_pba.h"

////////////////////////////////////////////////////////////////////////////////////////////////

namespace ff = meow::format;

////////////////////////////////////////////////////////////////////////////////////////////////
namespace { namespace aux {
////////////////////////////////////////////////////////////////////////////////////////////////

	struct fd_handle_t
	{
		int fd_;

		fd_handle_t()
			: fd_(-1)
		{
		}

		explicit fd_handle_t(int fd)
			: fd_(fd)
		{
		}

		fd_handle_t(fd_handle_t&& other)
			: fd_(-1)
		{
			std::swap(fd_, other.fd_);
		}

		void operator=(fd_handle_t&& other)
		{
			reset();
			std::swap(fd_, other.fd_);
		}

		int operator*() const
		{
			return fd_;
		}

		void reset()
		{
			while (fd_ >= 0)
			{
				int const r = close(fd_);
				if (0 == r)
				{
					fd_ = -1;
					break;
				}

				assert(r < 0);
				if (errno == EINTR)
					continue;

				// silently swallow the error otherwise, leaking the fd
				break;
			}
		}

		~fd_handle_t()
		{
			reset();
		}
	};

////////////////////////////////////////////////////////////////////////////////////////////////

	struct collector_impl_t : public collector_t
	{
		collector_impl_t(pinba_globals_t *globals, collector_conf_t *conf)
			: globals_(globals)
			, stats_(globals->stats())
			, conf_(conf)
		{
			if (conf_->n_threads == 0 || conf_->n_threads > 1024)
				throw std::runtime_error(ff::fmt_str("collector_conf_t::n_threads must be within [1, 1023]"));

			out_sock_
				.open(AF_SP, NN_PUSH)
				.bind(conf_->nn_output);

			shutdown_sock_
				.open(AF_SP, NN_PULL)
				.bind(conf_->nn_shutdown);

			this->try_bind();
		}

		virtual void startup() override
		{
			if (!threads_.empty())
				throw std::logic_error("collector_t::startup(): already started");

			for (uint32_t i = 0; i < conf_->n_threads; i++)
			{
				stats_->collector_threads.push_back({});

				std::thread t([this, i]()
				{
					std::string const thr_name = ff::fmt_str("udp_reader/{0}", i);
					pthread_setname_np(pthread_self(), thr_name.c_str());

					MEOW_DEFER(
						LOG_DEBUG(globals_->logger(), "{0}; exiting", thr_name);
					);

					this->eat_udp(i);
				});

				// t.detach();
				threads_.push_back(move(t));
			}
		}

		virtual void shutdown() override
		{
			nmsg_socket_t sock;
			sock.open(AF_SP, NN_PUSH).connect(conf_->nn_shutdown);
			sock.send(1); // there is no need to send multiple times, threads exit on poll signal

			for (uint32_t i = 0; i < conf_->n_threads; i++)
			{
				threads_[i].join();
			}
		}

	private:

		void try_bind()
		{
			os_addrinfo_list_ptr ai_list = os_unix::getaddrinfo_ex(conf_->address.c_str(), conf_->port.c_str(), AF_INET, SOCK_DGRAM, 0);
			os_addrinfo_t *ai = ai_list.get(); // take 1st item for now

			auto fd = this->try_bind_to_addr(ai);

			// commit if everything is ok
			ai_list_ = std::move(ai_list);
			ai_      = ai;
			fd_      = std::move(fd);
		}

		fd_handle_t try_bind_to_addr(os_addrinfo_t *ai)
		{
			fd_handle_t fd { os_unix::socket_ex(ai->ai_family, ai->ai_socktype, ai->ai_protocol) };
			os_unix::setsockopt_ex(*fd, SOL_SOCKET, SO_REUSEADDR, 1);
			os_unix::setsockopt_ex(*fd, SOL_SOCKET, SO_REUSEPORT, 1);
			os_unix::bind_ex(*fd, ai->ai_addr, ai->ai_addrlen);

			return std::move(fd);
		}

		void send_current_batch(uint32_t thread_id, raw_request_ptr& req)
		{
			++stats_->udp.batch_send_total;
			bool const success = out_sock_.send_message(req, NN_DONTWAIT);
			if (!success)
				++stats_->udp.batch_send_err;

			req.reset(); // signal the need to reinit
		}

		void eat_udp(uint32_t const thread_id)
		{
			// this->eat_udp_recv(thread_id);
			this->eat_udp_recvmmsg(thread_id);
		}

		void eat_udp_recv(uint32_t const thread_id)
		{
			static constexpr size_t const read_buffer_size = 64 * 1024; // max udp message size
			char buf[read_buffer_size];

			raw_request_ptr req;

			ProtobufCAllocator request_unpack_pba = {
				.alloc = nmpa___pba_alloc,
				.free = nmpa___pba_free,
				.allocator_data = NULL, // changed in progress
			};

			// SO_REUSEPORT bind in thread
			fd_handle_t const fd = this->try_bind_to_addr(ai_);

			nmsg_poller_t poller;

			poller
				.before_poll([this](timeval_t now, duration_t wait_for)
				{
					++stats_->udp.poll_total;
				})
				.ticker(1 * d_second, [&](timeval_t now)
				{
					os_rusage_t const ru = os_unix::getrusage_ex(RUSAGE_THREAD);

					std::lock_guard<std::mutex> lk_(stats_->mtx);
					stats_->collector_threads[thread_id].ru_utime = timeval_from_os_timeval(ru.ru_utime);
					stats_->collector_threads[thread_id].ru_stime = timeval_from_os_timeval(ru.ru_stime);
				})
				.read_nn_socket(shutdown_sock_, [&](timeval_t)
				{
					LOG_INFO(globals_->logger(), "udp_reader/{0}; received shutdown request", thread_id);
					poller.set_shutdown_flag();
				})
				.read_plain_fd(*fd, [&](timeval_t now)
				{
					// try receiving as much as possible without blocking
					while (true)
					{
						++stats_->udp.recv_total;

						int const n = recv(*fd, buf, sizeof(buf), MSG_DONTWAIT);
						if (n > 0)
						{
							++stats_->udp.recv_packets;

							if (!req)
							{
								constexpr size_t nmpa_block_size = 16 * 1024;
								req = meow::make_intrusive<raw_request_t>(conf_->batch_size, nmpa_block_size);
								request_unpack_pba.allocator_data = &req->nmpa;
							}

							stats_->udp.recv_bytes += uint64_t(n);

							Pinba__Request *request = pinba__request__unpack(&request_unpack_pba, n, (uint8_t*)buf);
							if (request == NULL) {
								++stats_->udp.packet_decode_err;
								continue;
							}

							req->requests[req->request_count] = request;
							req->request_count++;

							if (req->request_count >= conf_->batch_size)
							{
								this->send_current_batch(thread_id, req);
							}

							continue;
						}

						if (n < 0) {
							if (errno == EINTR)
								continue;

							if (errno == EAGAIN)
							{
								++stats_->udp.recv_eagain;

								// need to send current batch if we've got anything
								if (req && req->request_count > 0)
								{
									this->send_current_batch(thread_id, req);
								}
								return;
							}

							LOG_ERROR(globals_->logger(), "udp_reader/{0}; recvmmsg() failed, exiting: {1}:{2}", thread_id, errno, strerror(errno));
							poller.set_shutdown_flag();
							return;
						}

						// XXX: this will never happen, even if socket is closed from main thread
						if (n == 0)
						{
							LOG_INFO(globals_->logger(), "udp_reader/{0}; recv socket closed, exiting", thread_id);
							poller.set_shutdown_flag();
							return;
						}
					}
				})
				.loop();
		}

		void eat_udp_recvmmsg(uint32_t const thread_id)
		{
			size_t const max_message_size   = 64 * 1024; // max udp message size
			size_t const max_dgrams_to_recv = conf_->batch_size; // FIXME: make a special setting for this

			struct mmsghdr hdr[max_dgrams_to_recv];  memset(&hdr, 0, sizeof(hdr));
			struct iovec   iov[max_dgrams_to_recv];  memset(&iov, 0, sizeof(iov));

			std::unique_ptr<char[]> recv_buffer_p { new char[max_dgrams_to_recv * max_message_size] };
			char *recv_buffer = recv_buffer_p.get();

			for (unsigned i = 0; i < max_dgrams_to_recv; i++)
			{
				iov[i].iov_base           = recv_buffer + i * max_message_size;
				iov[i].iov_len            = max_message_size;

				hdr[i].msg_hdr.msg_iov    = &iov[i];
				hdr[i].msg_hdr.msg_iovlen = 1;
			}

			raw_request_ptr req;

			ProtobufCAllocator request_unpack_pba = {
				.alloc = nmpa___pba_alloc,
				.free = nmpa___pba_free,
				.allocator_data = NULL, // changed in progress
			};

			// SO_REUSEPORT bind in thread
			fd_handle_t const fd = this->try_bind_to_addr(ai_);

			nmsg_poller_t poller;

			poller
				.before_poll([this](timeval_t now, duration_t wait_for)
				{
					++stats_->udp.poll_total;
				})
				.ticker(1 * d_second, [&](timeval_t now)
				{
					os_rusage_t const ru = os_unix::getrusage_ex(RUSAGE_THREAD);

					std::lock_guard<std::mutex> lk_(stats_->mtx);
					stats_->collector_threads[thread_id].ru_utime = timeval_from_os_timeval(ru.ru_utime);
					stats_->collector_threads[thread_id].ru_stime = timeval_from_os_timeval(ru.ru_stime);
				})
				.read_nn_socket(shutdown_sock_, [&](timeval_t)
				{
					LOG_INFO(globals_->logger(), "udp_reader/{0}; received shutdown request", thread_id);
					poller.set_shutdown_flag();
				})
				.read_plain_fd(*fd, [&](timeval_t now)
				{
					// recv as much as possible without blocking
					// but see comments in EAGAIN handling on sleep() and saving syscalls
					while (true)
					{
						++stats_->udp.recv_total;

						int const n = recvmmsg(*fd, hdr, max_dgrams_to_recv, MSG_DONTWAIT, NULL);
						if (n > 0)
						{
							stats_->udp.recv_packets += uint64_t(n);

							for (int i = 0; i < n; i++)
							{
								if (!req)
								{
									req = meow::make_intrusive<raw_request_t>(conf_->batch_size, 16 * 1024);
									request_unpack_pba.allocator_data = &req->nmpa;
								}

								str_ref const dgram = { (char*)iov[i].iov_base, (size_t)hdr[i].msg_len };

								stats_->udp.recv_bytes += dgram.size();

								Pinba__Request *request = pinba__request__unpack(&request_unpack_pba, dgram.c_length(), (uint8_t*)dgram.data());
								if (request == NULL) {
									++stats_->udp.packet_decode_err;
									continue;
								}

								req->requests[req->request_count] = request;
								req->request_count++;

								if (req->request_count >= conf_->batch_size)
								{
									this->send_current_batch(thread_id, req);
								}
							}

							continue;
						}

						if (n < 0)
						{
							if (errno == EINTR)
								continue;

							if (errno == EAGAIN)
							{
								++stats_->udp.recv_eagain;

								// need to send current batch if we've got anything
								if (req && req->request_count > 0)
								{
									this->send_current_batch(thread_id, req);
								}

								// sleep for at least 1ms, before polling again, to let more packets arrive
								// and save a ton on system calls
								constexpr struct timespec const sleep_for = {
									.tv_sec = 0,
									.tv_nsec = 1 * 1000 * 1000,
								};
								nanosleep(&sleep_for, NULL);

								return;
							}

							LOG_ERROR(globals_->logger(), "udp_reader/{0}; recvmmsg() failed, exiting: {1}:{2}", thread_id, errno, strerror(errno));
							poller.set_shutdown_flag();
							return;
						}

						// XXX: this will never happen, even if socket is closed from main thread
						if (n == 0)
						{
							LOG_INFO(globals_->logger(), "udp_reader/{0}; recv socket closed, exiting", thread_id);
							poller.set_shutdown_flag();
							return;
						}
					} // recv loop
				})
				.loop();
		}

	private:
		fd_handle_t           fd_;
		os_addrinfo_list_ptr  ai_list_;
		os_addrinfo_t         *ai_;

		nmsg_socket_t         out_sock_;
		nmsg_socket_t         shutdown_sock_;

		pinba_globals_t       *globals_;
		pinba_stats_t         *stats_;
		collector_conf_t      *conf_;

		std::vector<std::thread> threads_;
	};

////////////////////////////////////////////////////////////////////////////////////////////////
}} // namespace { namespace aux {
////////////////////////////////////////////////////////////////////////////////////////////////

collector_ptr create_collector(pinba_globals_t *globals, collector_conf_t *conf)
{
	return meow::make_unique<aux::collector_impl_t>(globals, conf);
}
